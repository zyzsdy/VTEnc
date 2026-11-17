#!/usr/bin/env python3
"""Explicit P5 integration checks on Apple Silicon; no software video encoding.
FFmpeg is an independent decoder, audio fixture generator and remuxer only.
"""
import argparse
from array import array
from fractions import Fraction
import json
import math
from pathlib import Path
import platform
import re
import signal
import subprocess
import struct
import tempfile
import time
from transcode_hardware import run, require, probe, packets, verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vtenc', type=Path, default=Path('build/dev/vtenc'))
    parser.add_argument('--samples', type=Path, default=Path('tests/video_samples'))
    args = parser.parse_args()
    require(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'Requires native Apple Silicon')
    binary = args.vtenc.resolve()
    parent = Path('build/hardware-tests'); parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='p5-', dir=parent)).resolve()
    records = []

    def encode(name, source, opts=(), code=0, extension='mp4'):
        output = directory / (name + '.' + extension)
        result = run([binary, '--json-status', '-i', source, '-o', output, *opts], expected=code)
        (directory / (name + '.jsonl')).write_text(result.stderr)
        events = [json.loads(line) for line in result.stderr.splitlines()]
        require(not result.stdout, 'Logs polluted stdout')
        require(events[-1]['event'] == ('completed' if code == 0 else 'failed'), 'Wrong terminal event')
        if code == 0:
            require(events[-1]['hardware'] is True and events[-1]['probe_level'] == 'frames_encoded', 'Hardware encoding not verified')
            run(['ffmpeg', '-v', 'error', '-xerror', '-i', output, '-f', 'null', '-'])
        else:
            require(not output.exists() and not list(directory.glob('*.part.*')), 'Failed job published/leaked output')
        records.append({'case': name, 'exit_code': code, 'final': events[-1]})
        print('PASS', name, flush=True)
        return output, result

    def cfr(source, output, fps, force=False):
        _, before = packets(probe(source), 'video'); _, after = packets(probe(output), 'video')
        before.sort(key=lambda x: x['time'])
        origin = before[0]['time']; end = before[-1]['time'] + before[-1]['length']
        step = 1 / Fraction(fps); count = math.ceil((end - origin) / step)
        require(len(after) == count, 'CFR frame count')
        for i, packet in enumerate(after):
            require(packet['time'] == origin + i * step, 'CFR origin / fractional clock drift')
            expected = step if force else min(step, end - packet['time'])
            require(packet['length'] == expected, 'CFR tail / interval mismatch')
        return len(after)

    vfr = args.samples / 'timing/vfr_h264_aac.mp4'
    # Nonzero video/audio origins and a source with genuinely variable intervals.
    for mode, fps, decoder in [('cfr', '30000/1001', '--avhw'), ('forcecfr', '24', '--avsw'), ('cfr', '7', '--avsw')]:
        output, _ = encode(mode + '-' + fps.replace('/', '-'), vfr, [decoder, '--avsync', mode, '--fps', fps])
        cfr(vfr, output, fps, mode == 'forcecfr')
        _, a = packets(probe(vfr), 'audio'); _, b = packets(probe(output), 'audio')
        require([(x['time'], x['data_hash']) for x in a] == [(x['time'], x['data_hash']) for x in b], 'CFR moved/copied audio incorrectly')
    default, _ = encode('default-vfr', vfr, ['--avhw'])
    verify(vfr, default, 'h264', 8, True)

    # Bob and IVTC precede clock resampling; compare against their actual VFR output.
    for mode, source, options in [
        ('bob', args.samples/'deinterlace/interlaced_tff_h264.mp4', ['--deinterlace', 'bob']),
        ('ivtc', args.samples/'deinterlace/telecine_32_tff_h264.mp4', ['--vpp-ivtc', 'cycle=5'])]:
        opts = ['--avsw', '--field-order', 'tff', '--no-audio', *options]
        baseline, _ = encode(mode + '-baseline', source, opts)
        output, _ = encode(mode + '-cfr', source, [*opts, '--avsync', 'cfr', '--fps', '30000/1001'])
        cfr(baseline, output, '30000/1001')

    # Raw DAR and compressed 8/10-bit DAR with crop/resize and display rotation.
    raw = directory/'video.mp4'
    run([binary, '--synthetic', '--input-res', '320x240', '--frames', '37', '--fps', '30000/1001', '--dar', '16:9', '-o', raw])
    require(probe(raw)['streams'][0]['sample_aspect_ratio'] == '4:3', 'Raw DAR missing from mux')
    for angle, dar, sar in [(0, '16:9', '32:27'), (90, '9:16', '32:27'), (180, '16:9', '32:27'), (270, '9:16', '32:27')]:
        rotated = directory/f'rotate-{angle}.mov'
        run(['ffmpeg', '-v', 'error', '-display_rotation:v:0', str(angle), '-i', raw, '-c', 'copy', rotated])
        out, _ = encode('dar-'+str(angle), rotated, ['--avsw', '--codec', 'hevc', '--output-depth', '10',
                          '--crop', '2,2,2,2', '--output-res', '300x200', '--dar', dar, '--no-audio'])
        stream = probe(out)['streams'][0]
        require(stream['sample_aspect_ratio'] == sar and stream['pix_fmt'] == 'yuv420p10le', 'Derived DAR/depth')
        # Strip the container and independently read SAR from the actual SPS.
        elementary = directory/f'dar-{angle}.hevc'
        run(['ffmpeg', '-v', 'error', '-i', out, '-map', '0:v', '-c', 'copy', elementary])
        require(probe(elementary)['streams'][0]['sample_aspect_ratio'] == sar, 'SPS and container SAR differ')
        rotation = [s.get('rotation') for s in stream.get('side_data_list', []) if 'rotation' in s]
        require((not rotation and angle == 0) or (len(rotation) == 1 and rotation[0] % 360 == angle), 'Rotation metadata changed')

    def pcm(path, rate=None):
        cmd = ['ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:a:0']
        if rate: cmd += ['-ar', str(rate)]
        result = subprocess.run([*cmd, '-f', 'f32le', '-'], capture_output=True, check=True, timeout=60)
        values = array('f'); values.frombytes(result.stdout); return values


    def movie_audio_edit(path):
        def boxes(data):
            offset = 0
            while offset < len(data):
                length, kind = struct.unpack_from('>I4s', data, offset)
                require(length >= 8 and offset+length <= len(data), 'Invalid test movie box')
                yield kind, data[offset+8:offset+length]
                offset += length
        moov = next(data for kind,data in boxes(path.read_bytes()) if kind == b'moov')
        mvhd = next(data for kind,data in boxes(moov) if kind == b'mvhd')
        scale = struct.unpack_from('>I', mvhd, 20 if mvhd[0] else 12)[0]
        for kind, track in boxes(moov):
            if kind != b'trak': continue
            mdia = next(data for key,data in boxes(track) if key == b'mdia')
            handler = next(data for key,data in boxes(mdia) if key == b'hdlr')
            if handler[8:12] != b'soun': continue
            edts = next(data for key,data in boxes(track) if key == b'edts')
            elst = next(data for key,data in boxes(edts) if key == b'elst')
            require(elst[0] == 1, 'Expected exact 64-bit AAC edits')
            count = struct.unpack_from('>I', elst, 4)[0]
            entries = [struct.unpack_from('>QqI', elst, 8+i*20) for i in range(count)]
            require(all(x[2] == 65536 for x in entries), 'AAC edit retimes audio')
            require(count in [1,2] and (count==1 or entries[0][1]==-1), 'AAC edit structure')
            return Fraction(entries[0][0],scale) if count==2 else Fraction(0), Fraction(entries[-1][0],scale), entries[-1][1]
        raise RuntimeError('Missing AAC edit track')

    def audio_check(source, output, rate, channels, quality=True):
        expected = pcm(source, rate)
        if output.suffix in ['.mp4', '.mov']:
            native = output.with_suffix('.wav')
            run(['afconvert', '-f', 'WAVE', '-d', 'F32', output, native])
            actual = pcm(native)
        else:
            actual = pcm(output)
        require(len(actual) == len(expected), f'Valid PCM samples differ: {len(actual)} != {len(expected)}')
        before, src = packets(probe(source), 'audio'); after, dst = packets(probe(output), 'audio')
        require(after['sample_rate'] == str(rate) and after['channels'] == channels and after['profile'] == 'LC', 'AAC output format')
        skip = dst[0].get('side_data_list', [{}])[0].get('skip_samples', 0)
        first = dst[0]['time'] + Fraction(skip, rate)
        if output.suffix in ['.mp4', '.mov']:
            first, duration, media_start = movie_audio_edit(output)
            priming = int(re.search(r'priming=(\d+) samples', output.with_suffix('.jsonl').read_text()).group(1))
            require(duration == Fraction(len(actual), channels*rate) and media_start == priming, 'AAC edit valid interval')
        source_skip = src[0].get('side_data_list', [{}])[0].get('skip_samples', 0)
        source_first = src[0]['time'] + Fraction(source_skip, int(before['sample_rate']))
        require(abs(first - source_first) <= Fraction(1, 2*rate) + Fraction(after['time_base'])/2, 'AAC moved audio origin')
        if quality and len(expected) > rate//2:
            # Ignore only codec boundary transients; verify samples align at both ends.
            for start in [2048*channels, len(expected)-8192*channels]:
                a = expected[start:start+4096*channels]; b = actual[start:start+4096*channels]
                power = sum(x*x for x in a); noise = sum((x-y)**2 for x,y in zip(a,b))
                require(power > 0 and 10*math.log10(power/max(noise, 1e-30)) > 25, 'AAC PCM lost alignment / excessive error')
        return len(actual)//channels

    for rate, channels, count, target, ext in [
        (48000, 1, 59259, 48000, 'mp4'), (44100, 2, 54445, 44100, 'mov'),
        (44100, 2, 54445, 48000, 'mp4'), (48000, 2, 59259, 44100, 'mp4'),
        (48000, 1, 64, 48000, 'mp4'), (48000, 2, 4096, 48000, 'mkv'),
        (44100, 2, 2646013, 48000, 'mp4')]:
        name = f'aac-{rate}-{channels}-{count}-{target}-{ext}'
        source = directory/(name+'-source.mov')
        run(['ffmpeg', '-v', 'error', '-i', raw, '-f', 'lavfi', '-i',
             f'aevalsrc=0.2*sin(2*PI*997*t)'+ ('|0.2*sin(2*PI*1709*t)' if channels==2 else '')+f':s={rate}',
             '-map', '0:v', '-map', '1:a', '-af', f'atrim=end_sample={count}', '-c:v', 'copy', '-c:a', 'pcm_f32le', source])
        out, _ = encode(name, source, ['--avsw', '--audio-codec', 'aac', '--audio-bitrate', '192', '--audio-samplerate', str(target)], extension=ext)
        records[-1]['valid_samples'] = audio_check(source, out, target, channels)
        if count == 59259 and channels == 1:
            pcm_source = source
            # Existing AAC input, including trim metadata, must survive a new decode/encode.
            again, _ = encode('aac-to-aac', out, ['--audio-codec', 'aac'])
            audio_check(out, again, target, channels)
            shifted = directory/'shifted.mov'
            run(['ffmpeg', '-v', 'error', '-copyts', '-itsoffset', '0.2', '-i', source, '-itsoffset', '0.1', '-i', source,
                 '-map', '0:v', '-map', '1:a', '-c', 'copy', shifted])
            offset, _ = encode('aac-offset', shifted, ['--audio-codec', 'aac', '--avsync', 'cfr', '--fps', '30000/1001'])
            cfr(shifted, offset, '30000/1001'); audio_check(shifted, offset, target, channels)
            # Re-transcode positive-origin AAC with native trim metadata supplement.
            repeat, _ = encode('aac-offset-repeat', offset, ['--audio-codec', 'aac'])
            audio_check(shifted, repeat, target, channels)
            early = directory/'early.mov'
            run(['ffmpeg', '-v', 'error', '-copyts', '-itsoffset', '0.01', '-i', source, '-c', 'copy', early])
            early_out, _ = encode('aac-small-offset', early, ['--audio-codec', 'aac'])
            audio_check(early, early_out, target, channels)

    # Failure policy is concrete: unsupported trim containers never publish a file.
    for name, source, opts, ext, diagnostic in [
        ('missing-audio', raw, ['--audio-codec', 'aac'], 'mp4', 'requires an input audio track'),
        ('bad-bitrate', pcm_source, ['--audio-codec', 'aac', '--audio-bitrate', '1'], 'mp4', 'AAC bitrate'),
        ('aac-transport', pcm_source, ['--audio-codec', 'aac'], 'ts', 'priming/padding'),
        ('aac-video-only', pcm_source, ['--audio-codec', 'aac'], 'h264', 'audio'),
        ('bad-sar', raw, ['--dar', '2147483647:1'], 'mp4', '16-bit spacing')]:
        _, result = encode(name, source, opts, 1, ext)
        require(diagnostic in result.stderr, 'Missing expected diagnostic: '+result.stderr)
    result = run([binary, '-i', pcm_source, '--audio-codec', 'aac', '-o', '-', '-f', 'matroska'], expected=1)
    require(not result.stdout and 'requires file output' in result.stderr, 'AAC stdout must reject before bytes')
    surround = directory/'surround.mov'
    run(['ffmpeg', '-v', 'error', '-i', raw, '-f', 'lavfi', '-i', 'anullsrc=r=48000:cl=5.1:d=0.1',
         '-map', '0:v', '-map', '1:a', '-c:v', 'copy', '-c:a', 'pcm_s16le', surround])
    _, result = encode('unsupported-layout', surround, ['--audio-codec', 'aac'], code=1)
    require('mono/stereo' in result.stderr, 'Unsupported layout silently remixed')
    lowrate = directory/'lowrate.mov'
    run(['ffmpeg', '-v', 'error', '-i', pcm_source, '-map', '0', '-c:v', 'copy', '-c:a', 'pcm_s16le', '-ar', '32000', lowrate])
    _, result = encode('unsupported-rate', lowrate, ['--audio-codec', 'aac'], code=1)
    require('44100/48000' in result.stderr, 'Implicit sample-rate change')
    discontinuous = directory/'gap.mkv'
    run(['ffmpeg', '-v', 'error', '-i', pcm_source, '-c:v', 'copy', '-af', r'asetpts=PTS+if(gte(T\,0.5)\,0.1/TB\,0)',
         '-c:a', 'flac', discontinuous])
    _, result = encode('audio-gap', discontinuous, ['--audio-codec', 'aac'], code=1)
    require('Discontinuous decoded audio PTS' in result.stderr, 'Audio gap silently collapsed')
    existing = directory/'existing.mp4'; existing.write_bytes(b'keep')
    run([binary, '-i', pcm_source, '--audio-codec', 'aac', '-o', existing], expected=1)
    require(existing.read_bytes()==b'keep', 'Overwrote output')
    cancel_source = directory/'cancel-source.mov'
    run(['ffmpeg', '-v', 'error', '-stream_loop', '100', '-i', pcm_source, '-c', 'copy', cancel_source])
    cancelled = directory/'cancelled.mp4'
    with (directory/'cancelled.jsonl').open('w+') as log:
        process = subprocess.Popen([str(binary), '--json-status', '-i', str(cancel_source), '--audio-codec', 'aac',
                                    '--avsync', 'forcecfr', '--fps', '120', '-o', str(cancelled)],
                                   stdout=subprocess.PIPE, stderr=log)
        try:
            deadline = time.monotonic()+10
            while time.monotonic()<deadline:
                log.flush(); log.seek(0)
                if '"event":"progress"' in log.read(): break
                require(process.poll() is None, 'Cancellation fixture exited early')
                time.sleep(0.02)
            else: raise RuntimeError('Cancellation fixture made no progress')
            process.send_signal(signal.SIGINT)
            out,_ = process.communicate(timeout=10)
            require(process.returncode==130 and not out and not cancelled.exists(), 'CFR/AAC cancellation did not discard output')
            log.seek(0); terminal = json.loads(log.readlines()[-1])
            require(terminal['event']=='cancelled' and not terminal['output_complete'], 'Wrong cancellation event')
            records.append({'case':'cfr-aac-cancel', 'exit_code':130, 'final':terminal})
        finally:
            if process.poll() is None: process.kill(); process.wait()
    require(not list(directory.glob('*.part.*')), 'Temporary output leaked')
    (directory/'verification.json').write_text(json.dumps({'platform': platform.platform(), 'cases': records}, indent=2))
    print('P5 verification:', directory, flush=True)


if __name__ == '__main__':
    main()
