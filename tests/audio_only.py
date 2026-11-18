#!/usr/bin/env python3
"""Exercise standalone AudioToolbox encoding with generated inputs and ffprobe."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import wave

from transcode_hardware import require, run


def inspect(path, muxer, rate):
    data = json.loads(run(['ffprobe', '-v', 'error', '-show_streams', '-show_format',
                           '-of', 'json', path]).stdout)
    require(muxer in data['format']['format_name'].split(','), 'Wrong output container')
    require(len(data['streams']) == 1, 'Expected exactly one stream')
    stream = data['streams'][0]
    require((stream['codec_type'], stream['codec_name'], stream['profile'], stream['sample_rate'])
            == ('audio', 'aac', 'LC', str(rate)), 'Wrong AAC stream parameters')
    run(['ffmpeg', '-v', 'error', '-xerror', '-i', path, '-f', 'null', '-'])
    return stream


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vtenc', type=Path, default=Path('build/dev/vtenc'))
    vtenc = parser.parse_args().vtenc.resolve()
    with tempfile.TemporaryDirectory(dir='build', prefix='audio-only-') as tmp:
        directory = Path(tmp)
        source = directory / 'input.wav'
        rate, samples = 44100, 88201
        with wave.open(str(source), 'wb') as wav:
            wav.setparams((1, 2, rate, 0, 'NONE', ''))
            wav.writeframes(b'\0\0' * samples)

        command = [vtenc, '--audio-only', '--json-status']
        default = directory / 'default.aac'
        result = run([*command, '-i', source, '-o', default])
        require(not result.stdout, 'File job polluted stdout')
        events = [json.loads(line) for line in result.stderr.splitlines()]
        require(events[0]['config']['audio_only'] and events[0]['config']['output_format'] == 'mp4',
                'Audio-only configuration missing')
        require(all(e['event'] != 'session' for e in events), 'Opened a video session')
        end = events[-1]
        require(end['event'] == 'completed' and end['frames'] == 0 and end['audio_packets'] > 0
                and end['hardware'] is None, 'Wrong terminal status')
        stream = inspect(default, 'mp4', rate)
        require(stream['time_base'] == f'1/{rate}' and stream['duration_ts'] == samples, 'Incorrect AAC trims')

        # Two video streams exercise input that the normal video pipeline rejects.
        video = directory / 'video.y4m'
        video.write_bytes(b'YUV4MPEG2 W2 H2 F1:1 Ip A1:1 C420jpeg\nFRAME\n' + bytes(6))
        mixed = directory / 'mixed.nut'
        run(['ffmpeg', '-v', 'error', '-i', video, '-i', source, '-i', default,
             '-map', '0:v', '-map', '0:v', '-map', '1:a', '-map', '2:a',
             '-c', 'copy', '-c:a:1', 'pcm_s16le', '-ac:a:1', '2', mixed])
        override = directory / 'override.mp4'
        run([*command, '-i', mixed, '-o', override, '--audio-samplerate', '48000',
             '--audio-bitrate', '128', '-f', 'matroska'])
        require(inspect(override, 'matroska', 48000)['channels'] == 1, 'Selected the wrong audio track')

        adts = directory / 'stdout.aac'
        with adts.open('wb') as output:
            subprocess.run([*command, '-i', default, '-o', '-', '-f', 'adts'],
                           stdout=output, stderr=subprocess.PIPE, check=True, timeout=30)
        inspect(adts, 'aac', rate)

        transport = directory / 'input.ts'
        run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'sine=duration=2:sample_rate=44100',
             '-c:a', 'aac', '-f', 'mpegts', transport])
        from_transport = directory / 'from-transport.mp4'
        run([*command, '-i', transport, '-o', from_transport])
        inspect(from_transport, 'mp4', rate)

        failed = directory / 'failed.mp4'
        missing = run([*command, '-i', video, '-o', failed], expected=1)
        require('requires an input audio track' in missing.stderr, 'Missing audio diagnostic')
        run([*command, '-i', source, '-o', failed, '-f', 'mp3'], expected=1)
        require(not failed.exists() and not list(directory.glob('*.part.*')), 'Failed output leaked')


if __name__ == '__main__':
    main()
