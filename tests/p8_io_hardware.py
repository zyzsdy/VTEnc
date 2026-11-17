#!/usr/bin/env python3
"""Explicit P8 scheduler and output checks; standalone or supplementary to p8_hardware.py."""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import subprocess
import tempfile
from p8_hardware import run, require, inspect, fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vtenc', type=Path, default=Path('build/dev/vtenc'))
    args = parser.parse_args(); binary = args.vtenc.resolve()
    parent = Path('build/hardware-tests'); parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='p8-io-', dir=parent)).resolve()
    records = []
    def encode(name, source, options, stdin=None, stdout_format=None, code=0):
        output = root / (name + ('.' + stdout_format if stdout_format else '.mp4'))
        command = [str(binary), '--json-status', *map(str, source), *map(str, options), '-o', '-' if stdout_format else str(output)]
        if stdout_format:
            command += ['-f', stdout_format]
        result = subprocess.run(command, input=stdin, capture_output=True, timeout=120)
        (root / (name + '.jsonl')).write_bytes(result.stderr)
        require(result.returncode == code, result.stderr.decode(errors='replace'))
        events = [json.loads(x) for x in result.stderr.splitlines()]
        require(events[-1]['event'] == ('completed' if not code else 'failed'), 'Wrong final state')
        require(events[-1]['attempt'] == 1, 'Unexpected retry')
        if stdout_format:
            require(result.stdout, 'Missing stdout stream'); output.write_bytes(result.stdout)
        else:
            require(not result.stdout, 'stdout contaminated')
            if code: require(not output.exists() and not list(root.glob('*.part.*')), 'Failed file published or temporary file leaked')
        records.append({'name': name, 'command': command, 'final': events[-1]})
        return output
    source = encode('source', ['--synthetic', '--input-res', '640x360', '--frames', '90'], ['--bitrate', '1000', '-u', 'default'])
    keyfile = root / 'keys.txt'; keyfile.write_text('0\n37\n179\n')
    for codec, depth in [('h264', 8), ('hevc', 10)]:
        output = encode(f'cfr-{codec}', ['-i', source, '--avhw', '--avsync', 'forcecfr', '--fps', '60', '--no-audio'],
                        ['--codec', codec, '--output-depth', depth, '--cbr', '1000', '--keyfile', keyfile])
        stats, _ = inspect(output, codec, depth, 180, Fraction(60), keys=[0, 37, 179]); records[-1]['bitstream'] = stats
    raw = root / 'bob.nv12'
    raw.write_bytes(run(['ffmpeg', '-v', 'error', '-i', source, '-pix_fmt', 'nv12', '-f', 'rawvideo', '-']).stdout)
    output = encode('raw-bob', ['-i', raw, '--input-res', '640x360', '--frames', '90', '--deinterlace', 'bob', '--field-order', 'tff'],
                    ['--cbr', '1000', '--keyfile', keyfile])
    records[-1]['bitstream'], _ = inspect(output, 'h264', 8, 180, Fraction(60), keys=[0, 37, 179])
    keyfile.write_text('0\n37\n89\n')
    y4m = run(['ffmpeg', '-v', 'error', '-i', source, '-pix_fmt', 'yuv420p', '-f', 'yuv4mpegpipe', '-']).stdout
    for codec, depth in [('h264', 8), ('hevc', 10)]:
        output = encode(f'pipe-{codec}', ['-i', '-', '--avsw', '--no-audio', '--vpp-colorspace', 'matrix=bt709:auto,transfer=bt709:auto,colorprim=bt709:auto,range=limited:auto', '--input-chroma-loc', 'left'],
                        ['--codec', codec, '--output-depth', depth, '--vbr', '1000', '--max-bitrate', '2000', '--vbv-bufsize', '4000', '--keyfile', keyfile],
                        stdin=y4m, stdout_format='matroska')
        # Matroska rounds timestamps to milliseconds; inspect decoded keyframes/count separately.
        data = json.loads(run(['ffprobe', '-v', 'error', '-show_frames', '-show_streams', '-of', 'json', output]).stdout)
        frames = data['frames']; require(len(frames) == 90, 'Pipe frame count')
        require(all(frames[i]['key_frame'] for i in (0, 37, 89)), 'Pipe keyframe indices')
        require(data['streams'][0]['pix_fmt'] == ('yuv420p10le' if depth == 10 else 'yuv420p'), 'Pipe depth')
        run(['ffmpeg', '-v', 'error', '-xerror', '-i', output, '-f', 'null', '-'])
    output = encode('vbr-default-buffer', ['--synthetic', '--input-res', '640x360', '--frames', '90'],
                    ['--vbr', '1000', '--max-bitrate', '2000'])
    settings = next(json.loads(line)['settings'] for line in (root / 'vbr-default-buffer.jsonl').read_text().splitlines() if json.loads(line)['event'] == 'encoder_settings')
    require(settings['VBVBufferDuration'] == 2.5, 'Default VBV buffer not set/read back')
    records[-1]['bitstream'], _ = inspect(output, 'h264', 8, 90, Fraction(30))
    output = encode('fractional-cbr', ['--synthetic', '--input-res', '640x360', '--frames', '90', '--fps', '30000/1001'],
                    ['--cbr', '1000'])
    events = [json.loads(line) for line in (root / 'fractional-cbr.jsonl').read_text().splitlines()]
    settings = next(x for x in events if x['event'] == 'encoder_settings')
    require(abs(settings['requested_settings']['ExpectedFrameRate'] - 30000/1001) < 1e-9, 'Fractional frame-rate request lost')
    records[-1]['settings'] = settings
    records[-1]['bitstream'], _ = inspect(output, 'h264', 8, 90, Fraction(30000, 1001))
    noise = root / 'noise.nv12'; fixture(noise, 8, count=420, noise=True)
    encode('stdout-cap-rejected', ['-i', noise, '--input-res', '640x360', '--frames', '420'],
           ['--bitrate', '2000', '--max-bitrate', '3000'], stdout_format='h264', code=1)
    require('DataRateLimits violated' in records[-1]['final']['message'], 'Missing bound failure')
    (root / 'report.json').write_text(json.dumps(records, indent=2) + '\n')
    print('PASS P8 I/O:', root / 'report.json')

if __name__ == '__main__':
    main()
