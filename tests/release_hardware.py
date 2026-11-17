#!/usr/bin/env python3
"""Explicit release smoke test: file/ASS/audio workflows on real Apple Silicon."""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import subprocess
import tempfile


def run(args):
    result = subprocess.run([str(x) for x in args], capture_output=True, timeout=90)
    if result.returncode:
        raise RuntimeError(f'{args}: {result.stderr.decode(errors="replace")}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vtenc', type=Path, required=True)
    args = parser.parse_args()
    binary = args.vtenc.resolve()
    parent = Path('build/hardware-tests'); parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='release-', dir=parent)).resolve()
    records = []

    def encode(name, options):
        output = root / (name + '.mp4')
        result = run([binary, '--json-status', *options, '-o', output])
        assert not result.stdout, 'File encode contaminated stdout'
        (root / (name + '.jsonl')).write_bytes(result.stderr)
        events = [json.loads(x) for x in result.stderr.splitlines()]
        end = events[-1]
        assert end['event'] == 'completed' and end['hardware'] and end['probe_level'] == 'frames_encoded', end
        assert any(x['event'] == 'progress' for x in events), 'Missing progress'
        data = json.loads(run(['ffprobe', '-v', 'error', '-show_streams', '-show_frames', '-of', 'json', output]).stdout)
        video = next(x for x in data['streams'] if x['codec_type'] == 'video')
        frames = [x for x in data['frames'] if x['media_type'] == 'video']
        assert len(frames) == 60, len(frames)
        base = Fraction(video['time_base'])
        for i, frame in enumerate(frames):
            assert abs(int(frame['pts']) * base - Fraction(i, 30)) <= base
        run(['ffmpeg', '-v', 'error', '-xerror', '-i', output, '-f', 'null', '-'])
        records.append({'name': name, 'final': end, 'streams': data['streams']})
        return output, data

    video, _ = encode('source', ['--synthetic', '--input-res', '640x360', '--frames', '60', '--fps', '30'])
    source = root / 'with-audio.mov'
    run(['ffmpeg', '-v', 'error', '-i', video, '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
         '-t', '2', '-c:v', 'copy', '-c:a', 'pcm_f32le', source])
    outputs = {}
    for workflow in ('file', 'ass'):
        for audio in ('off', 'copy', 'aac'):
            # MP4 does not support PCM in all muxers: use an AAC fixture for stream copy.
            input_ = source
            if audio == 'copy':
                input_, _ = encode(workflow + '-audio-fixture', ['-i', source, '--avsw', '--audio-codec', 'aac'])
            options = ['-i', input_, '--avsw']
            if audio == 'off': options += ['--no-audio']
            if audio == 'aac': options += ['--audio-codec', 'aac', '--audio-bitrate', '128', '--audio-samplerate', '48000']
            if workflow == 'ass':
                options += ['--vpp-subburn', 'filename=' + str(Path('tests/fixtures/p6.ass').resolve())]
            output, data = encode(workflow + '-' + audio, options)
            outputs[(workflow, audio)] = output
            streams = [x for x in data['streams'] if x['codec_type'] == 'audio']
            assert len(streams) == (0 if audio == 'off' else 1), streams
            if streams:
                assert streams[0]['codec_name'] == 'aac' and streams[0]['sample_rate'] == '48000'
                assert abs(Fraction(streams[0]['duration']) - 2) <= Fraction(1, 48000)
            if audio == 'copy':
                def audio_bytes(path):
                    return run(['ffmpeg', '-v', 'error', '-i', path, '-map', '0:a:0', '-c', 'copy', '-f', 'data', '-']).stdout
                assert audio_bytes(output) == audio_bytes(input_), 'Audio-copy packets changed'
            # A repeated destination must fail without modifying the published result.
            before = output.read_bytes()
            result = subprocess.run([str(binary), '--json-status', *map(str, options), '-o', str(output)], capture_output=True)
            assert result.returncode != 0 and output.read_bytes() == before
            assert json.loads(result.stderr.splitlines()[-1])['event'] == 'failed'
    def picture(path):
        return run(['ffmpeg', '-v', 'error', '-i', path, '-vf', 'select=eq(n\\,12)',
                    '-frames:v', '1', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-']).stdout
    clean = picture(outputs[('file', 'off')]); burned = picture(outputs[('ass', 'off')])
    assert len(clean) == len(burned) == 640 * 360 * 3
    assert sum(abs(a - b) > 20 for a, b in zip(clean, burned)) > 100, 'Subtitle did not alter visible pixels'
    rejected = subprocess.run([str(binary), '--json-status', '--codec', 'av1', '-i', str(source),
                               '-o', str(root / 'unsupported.mp4')], capture_output=True)
    assert rejected.returncode == 2 and json.loads(rejected.stderr.splitlines()[-1])['event'] == 'failed'
    assert not (root / 'unsupported.mp4').exists()
    assert not list(root.glob('*.part.*'))
    report = root / 'report.json'; report.write_text(json.dumps(records, indent=2) + '\n')
    print('PASS release file/ASS/audio workflows:', report)


if __name__ == '__main__':
    main()
