#!/usr/bin/env python3
"""Explicit RFF/mixed Metal tests on Apple Silicon; video encoding is VT hardware only.
Inject H.264 picture_timing SEI into hardware-generated all-I rasters, then remux.
FFmpeg independently verifies the RFF flags, decoded pixels, PTS and audio payloads.
"""
import argparse
from fractions import Fraction
import json
import math
from pathlib import Path
import platform
import re
import subprocess
import tempfile
from transcode_hardware import require, run, probe, packets
from bob_hardware import progressive

WIDTH, HEIGHT = 192, 128
FIELD = Fraction(1001, 60000)


def film(index):
    # Vertical features make wrong field matches readily visible.
    line = bytes(40 + ((x // 8 + index * 3) % 20) * 8 for x in range(WIDTH))
    return line * HEIGHT + bytes([128]) * (WIDTH * HEIGHT // 2)


def escape(data):
    result, zeros = bytearray(), 0
    for byte in data:
        if zeros == 2 and byte <= 3:
            result.append(3)
            zeros = 0
        result.append(byte)
        zeros = zeros + 1 if byte == 0 else 0
    return bytes(result)


def fixture(binary, directory, name, first, soft_only=False):
    # (top film ID, bottom film ID, H.264 pic_struct)
    sequence = [] if soft_only else [(i, i, 0) for i in range(5)]
    offset = len(sequence)
    structs = [6, 3, 5, 4] if first else [5, 4, 6, 3]
    sequence += [(offset+i, offset+i, structs[i % 4]) for i in range(8)]
    if not soft_only:
        a, b = [0, 1, 1, 2, 3], [0, 1, 2, 3, 3]
        for i in range(10):
            top, bottom = 13+i//5*4+a[i % 5], 13+i//5*4+b[i % 5]
            sequence.append((bottom if first else top, top if first else bottom, 4 if first else 3))
        sequence += [(i, i, 0) for i in range(21, 26)]
    raw = directory / (name + '.nv12')
    with raw.open('wb') as target:
        for top, bottom, _ in sequence:
            frames = film(top), film(bottom)
            for plane_height, plane_offset in [(HEIGHT, 0), (HEIGHT//2, WIDTH*HEIGHT)]:
                for y in range(plane_height):
                    start = plane_offset + y*WIDTH
                    target.write(frames[y % 2][start:start+WIDTH])
    coded = directory / (name + '-coded.h264')
    result = run([binary, '-i', raw, '-o', coded, '--input-res', f'{WIDTH}x{HEIGHT}',
                  '--frames', len(sequence), '--fps', '30000/1001', '--gop-len', '1', '--bitrate', '12000'])
    require('Hardware verified: yes' in result.stdout+result.stderr, 'Fixture encoder is not verified hardware')
    trace = run(['ffmpeg', '-v', 'verbose', '-i', coded, '-c', 'copy', '-frames:v', '1',
                 '-bsf:v', 'trace_headers', '-f', 'null', '-']).stderr
    match = re.search(r'\] (\d+)\s+vui_parameters_present_flag\s+[01] = [01]', trace)
    require(match, 'Cannot locate hardware SPS VUI')
    position = int(match[1])
    # Replace only SPS VUI, with explicit limited BT.709, 60000/1001 field
    # clock, no HRD, pic_struct_present_flag=1. Slice picture data is untouched.
    vui = '001'+'101'+'0'+'1'+f'{1:08b}{1:08b}{1:08b}'+'0'+'1'+f'{1001:032b}{60000:032b}'+'10010'
    result, index = bytearray(), 0
    for nal in re.split(b'\x00\x00\x00?\x01', coded.read_bytes()):
        if not nal:
            continue
        kind = nal[0] & 31
        if kind == 7:
            bits = ''.join(f'{byte:08b}' for byte in nal.replace(b'\0\0\3', b'\0\0'))
            bits = bits[:position]+'1'+vui+'1'
            bits += '0' * (-len(bits) % 8)
            nal = escape(bytes(int(bits[i:i+8], 2) for i in range(0, len(bits), 8)))
        if kind in (1, 5):
            require(index < len(sequence), 'Fixture requires one slice per hardware picture')
            pic = sequence[index][2]
            index += 1
            payload = (pic << 4) | (1 if pic in (5, 6) else 2 if pic in (3, 4) else 4)
            result += b'\0\0\0\1\6\1\1'+bytes([payload])+b'\x80'
        result += b'\0\0\0\1'+nal
    require(index == len(sequence), 'Hardware picture/slice count mismatch')
    elementary = directory / (name + '-rff.h264')
    elementary.write_bytes(result)
    # Explicit packet timestamps avoid raw H.264 demux frame-rate estimation.
    soft = f'clip(N-{offset},0,8)'
    pts = f'12000+1001*(2*N+floor(({soft}+1)/2))'
    duration = f'1001*(2+between(N,{offset},{offset+7})*eq(mod(N-{offset},2),0))'
    scale = Fraction(1, 60000) / Fraction(probe(elementary)['streams'][0]['time_base'])
    require(scale.denominator == 1, 'Raw input cannot represent field clock')
    pts = f'({pts})*{scale.numerator}'
    duration = f'({duration})*{scale.numerator}'
    bsf = f'setts=pts={pts}:dts={pts}:duration={duration}'.replace(',', r'\,')
    output = directory / (name + '.mp4')
    run(['ffmpeg', '-nostdin', '-v', 'error', '-i', elementary, '-map', '0:v', '-c', 'copy', '-bsf:v', bsf, '-video_track_timescale', '60000', output])
    frames = json.loads(run(['ffprobe', '-v', 'error', '-show_frames', '-select_streams', 'v', '-of', 'json', output]).stdout)['frames']
    require([f['repeat_pict'] for f in frames] == [int(s[2] in (5, 6)) for s in sequence], 'Fixture RFF flags are missing')
    _, source_packets = packets(probe(output), 'video')
    end = Fraction(1, 5)
    for packet, picture in zip(source_packets, sequence):
        length = FIELD * (2 + int(picture[2] in (5, 6)))
        require(packet['time'] == end and packet['length'] == length, 'Fixture packet time drift')
        end += length
    return output, sequence


def check_hevc_metadata_gap(binary, directory):
    # FFmpeg currently loses HEVC pic_struct=5/6 in both public frame/parser
    # APIs. Prove that this stream fails explicitly instead of losing RFF.
    coded = directory/'hevc-coded.hevc'
    result = run([binary, '-i', directory/'soft-tff.nv12', '-o', coded,
                  '--input-res', f'{WIDTH}x{HEIGHT}', '--frames', '8', '--gop-len', '1', '--codec', 'hevc'])
    require('Hardware verified: yes' in result.stdout+result.stderr, 'HEVC fixture is not hardware encoded')
    trace = run(['ffmpeg', '-v', 'verbose', '-i', coded, '-c', 'copy', '-frames:v', '1',
                 '-bsf:v', 'trace_headers', '-f', 'null', '-']).stderr
    match = re.search(r'\] (\d+)\s+frame_field_info_present_flag\s+0 = 0', trace)
    require(match, 'Cannot locate HEVC SPS field info flag')
    position = int(match[1])
    data = bytearray()
    for nal in re.split(b'\x00\x00\x00?\x01', coded.read_bytes()):
        if not nal:
            continue
        kind = (nal[0] >> 1) & 63
        if kind == 33:
            bits = ''.join(f'{byte:08b}' for byte in nal.replace(b'\0\0\3', b'\0\0'))
            bits = bits[:position]+'1'+bits[position+1:]
            nal = escape(bytes(int(bits[i:i+8], 2) for i in range(0, len(bits), 8)))
        if kind < 32:
            data += b'\0\0\0\1'+bytes([39 << 1, 1, 1, 1, 0x50, 0x80])
        data += b'\0\0\0\1'+nal
    elementary = directory/'hevc-rff.hevc'
    elementary.write_bytes(data)
    source = directory/'hevc-rff.mp4'
    run(['ffmpeg', '-nostdin', '-v', 'error', '-i', elementary, '-c', 'copy', source])
    for mode in ['avhw', 'avsw', 'automatic']:
        output = directory/('hevc-rejected-'+mode+'.mp4')
        result = run([binary, '-i', source, '-o', output, '--vpp-ivtc', 'mixed=true',
                      *([] if mode == 'automatic' else ['--'+mode])], expected=1)
        require('cannot reliably read HEVC picture-timing/RFF metadata' in result.stderr,
                'HEVC RFF limitation was hidden')
        require(not output.exists(), 'Rejected HEVC metadata left a completed output')
    return {'case': 'hevc-metadata-rejection', 'decoder_paths': 3}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vtenc', type=Path, default=Path('build/dev/vtenc'))
    parser.add_argument('--samples', type=Path, default=Path('tests/video_samples'))
    args = parser.parse_args()
    require(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'Requires native Apple Silicon')
    parent = Path('build/hardware-tests')
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='rff-', dir=parent))
    report = []
    for first in [0, 1]:
        for soft_only in [False, True]:
            name = ('soft' if soft_only else 'mixed') + ('-bff' if first else '-tff')
            source, sequence = fixture(args.vtenc.resolve(), directory, name, first, soft_only)
            if not soft_only:
                with_audio = directory / (name+'-audio.mp4')
                run(['ffmpeg', '-nostdin', '-v', 'error', '-copyts', '-i', source, '-i',
                     args.samples/'decode/h264_8bit.mp4', '-map', '0:v', '-map', '1:a', '-c', 'copy', with_audio])
                source = with_audio
            for mode in ['avsw', 'avhw']:
                for expand in (['auto', 'off'] if soft_only else ['auto']):
                    case = name+'-'+mode+'-'+expand
                    output = directory/(case+'.mp4')
                    params = f'mixed=true,expand={expand},log=true'
                    result = run([args.vtenc.resolve(), '-i', source, '-o', output, '--'+mode, '--vpp-ivtc', params,
                                  *([] if soft_only else ['--field-order', 'bff' if first else 'tff'])])
                    log = result.stdout+result.stderr
                    (directory/(case+'.log')).write_text(log)
                    require('Hardware verified: yes' in log, 'Unverified hardware encoding')
                    if mode == 'avhw':
                        require('Hardware decoder verified: yes' in log, 'Unverified hardware decoding')
                    expected_count = 8 if soft_only else 26
                    progressive(output, expected_count)
                    _, before = packets(probe(source), 'video')
                    _, after = packets(probe(output), 'video')
                    require(len(after) == expected_count, 'Wrong mixed output count')
                    expected = []
                    if soft_only and expand == 'off':
                        expected = [(p['time'], p['length']) for p in before]
                    else:
                        origin = Fraction(1, 5)
                        lengths = [FIELD*5/2]*8 if soft_only else [FIELD*2]*5+[FIELD*5/2]*16+[FIELD*2]*5
                        for length in lengths:
                            expected.append((origin, length))
                            origin += length
                    require([(p['time'], p['length']) for p in after] == expected, 'Mixed PTS or duration drift')
                    require(after[-1]['time']+after[-1]['length'] == before[-1]['time']+before[-1]['length'], 'Mixed movie end changed')
                    _, a = packets(probe(source), 'audio')
                    _, b = packets(probe(output), 'audio')
                    require([(p['time'], p['length'], p['data_hash']) for p in a] ==
                            [(p['time'], p['length'], p['data_hash']) for p in b], 'Audio payload or timing changed')
                    decoded = subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-i', str(output), '-map', '0:v',
                                              '-fps_mode', 'passthrough', '-pix_fmt', 'nv12', '-f', 'rawvideo', '-'],
                                             capture_output=True, check=True, timeout=60).stdout
                    frame_size = WIDTH*HEIGHT*3//2
                    require(len(decoded) == frame_size*expected_count, 'Decoded frame count mismatch')
                    errors = []
                    for i in range(expected_count):
                        actual = decoded[i*frame_size:(i+1)*frame_size]
                        error = sum((x-y)**2 for x, y in zip(actual, film(i)))/frame_size
                        errors.append(100 if error == 0 else 10*math.log10(255**2/error))
                    require(min(errors) > 40, f'{case}: wrong film exposure or field weave: min PSNR {min(errors)}')
                    report.append({'case': case, 'coded_frames': len(sequence), 'output_frames': expected_count,
                                   'min_psnr': min(errors), 'audio_packets': len(a), 'exact_timeline': True})
    report.append(check_hevc_metadata_gap(args.vtenc.resolve(), directory))
    (directory/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'directory': str(directory), 'cases': report}, indent=2))


if __name__ == '__main__':
    main()
