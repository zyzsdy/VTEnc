#!/usr/bin/env python3
"""Explicit P4 integration tests on real Apple Silicon; never part of default CTest."""
import argparse
from fractions import Fraction
import json
import os
from pathlib import Path
import platform
import pty
import select
import signal
import subprocess
import tempfile
import threading
import time

COLOR = ['--vpp-colorspace', 'matrix=bt709:auto,transfer=bt709:auto,colorprim=bt709:auto,range=limited:auto']


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def events(data, terminal=None):
    require(data.endswith(b'\n') and b'\r' not in data, 'JSON must be LF-terminated, never terminal control text')
    result = [json.loads(line) for line in data.splitlines()]
    require([x['sequence'] for x in result] == list(range(len(result))), 'Event order/sequence is not contiguous')
    require(all(x['schema_version'] == 1 for x in result), 'Wrong protocol version')
    if terminal:
        ends = [x for x in result if x['event'] in ['completed', 'failed', 'cancelled']]
        require(len(ends) == 1 and result[-1] == ends[0] and ends[0]['event'] == terminal, 'Wrong terminal event')
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--vtenc', type=Path, default=Path('build/dev/vtenc'))
    parser.add_argument('--vspipe', default='vspipe')
    parser.add_argument('--skip-vspipe', action='store_true', help='Run only non-VSPipe checks; not full P4 acceptance')
    args = parser.parse_args()
    require(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'Requires native Apple Silicon')
    binary = args.vtenc.resolve()
    parent = Path('build/hardware-tests')
    parent.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='p4-', dir=parent)).resolve()
    records = []

    def execute(name, options, data=None, code=0):
        result = subprocess.run([str(binary), '--json-status', *map(str, options)], input=data,
                                capture_output=True, timeout=30)
        require(result.returncode == code, f'{name}: {result.returncode}: {result.stderr.decode()}')
        parsed = events(result.stderr, 'completed' if code == 0 else 'cancelled' if code == 130 else 'failed')
        (directory / (name + '.jsonl')).write_bytes(result.stderr)
        if code == 0:
            require(parsed[-1]['hardware'] is True and parsed[-1]['probe_level'] == 'frames_encoded', 'Hardware result absent')
            sessions = [x for x in parsed if x['event'] == 'session']
            require(sessions and sessions[-1]['encoder_id'].startswith('com.apple.'), 'No actual encoder identity')
        else:
            require(not list(directory.glob('*.part.*')), 'Failed task leaked temporary output')
        records.append({'case': name, 'exit_code': code, 'final': parsed[-1]})
        return result, parsed

    def verify(path, count, depth, origin=Fraction(0), step=Fraction(1001, 30000)):
        probe = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-show_frames', '-select_streams', 'v:0',
                                '-of', 'json', str(path)], capture_output=True, check=True, timeout=30)
        obj = json.loads(probe.stdout)
        stream = obj['streams'][0]
        frames = obj['frames']
        base = Fraction(stream['time_base'])
        require(len(frames) == count, f'Wrong decoded frame count: {len(frames)} != {count}')
        require(stream['pix_fmt'] == ('yuv420p10le' if depth == 10 else 'yuv420p'), 'Wrong bit depth/chroma')
        for i, frame in enumerate(frames):
            require(abs(int(frame['pts']) * base - (origin + i * step)) <= base / 2, f'Wrong PTS at frame {i}')
        subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', str(path), '-f', 'null', '-'],
                       check=True, capture_output=True, timeout=30)

    # Protocol CLI, UTF-8/control escaping, capability scope and OS context.
    version = subprocess.run([binary, '--version', '--json-status'], capture_output=True, check=True)
    require(not version.stdout and events(version.stderr)[0]['event'] == 'version', 'Version protocol')
    for codec, depth in [('h264', '8'), ('hevc', '8'), ('hevc', '10')]:
        cap = subprocess.run([binary, '--check-features', '--codec', codec, '--output-depth', depth, '--json-status'],
                             capture_output=True, check=True)
        item = events(cap.stderr)[0]
        require(not cap.stdout and item['probe_level'] == 'session_prepared' and not item['frames_encoded']
                and not item['property_values_validated'] and item['os_version'], 'Probe exaggerated its scope')
        (directory / f'capabilities-{codec}-{depth}.json').write_bytes(cap.stderr)
    invalid, parsed = execute('invalid', ['--bad-option'], code=2)
    require(not invalid.stdout, 'CLI error contaminated stdout')
    missing = directory / '不存在\n"\\.mp4'
    _, parsed = execute('escaping', ['-i', missing, '-o', directory / 'missing.mp4'], code=1)
    require(parsed[0]['config']['input'] == str(missing), 'UTF-8/control character roundtrip')

    producer = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=30000/1001',
                               '-frames:v', '37', '-pix_fmt', 'yuv420p', '-f', 'yuv4mpegpipe', '-'],
                              capture_output=True, check=True)
    y4m = producer.stdout
    (directory / 'ffmpeg.y4m').write_bytes(y4m)
    output = directory / 'ffmpeg.mp4'
    opts = ['--avsw', '-i', '-', '-o', output, *COLOR]
    result, parsed = execute('ffmpeg', opts, y4m)
    require(not result.stdout, 'File output logs polluted stdout')
    require(all(x['percent'] is None and x['eta_seconds'] is None for x in parsed if 'percent' in x), 'Invented Y4M length')
    require(parsed[-1]['frames'] == 37 and parsed[-1]['media_time'] == {'value': 37037, 'timescale': 30000}, 'Wrong media time')
    verify(output, 37, 8)
    # File and stdin take the same avformat route; filename is deliberately not .y4m.
    disguised = directory / 'source.bin'
    disguised.write_bytes(y4m)
    execute('y4m-file', ['--avsw', '-i', disguised, '-o', directory / 'file.mp4', *COLOR])
    verify(directory / 'file.mp4', 37, 8)
    # Long FRAME extensions would drift FFmpeg's byte-offset-based timestamps.
    first = y4m.index(b'\n') + 1
    frame_size = 320 * 180 * 3 // 2
    extended = y4m[:first]
    cursor = first
    for i in range(37):
        require(y4m[cursor:cursor + 6] == b'FRAME\n', 'Unexpected fixture framing')
        extended += b'FRAME Xtest=' + b'a' * 50 + b'\n' + y4m[cursor + 6:cursor + 6 + frame_size]
        cursor += 6 + frame_size
    execute('frame-extensions', ['--avsw', '-i', '-', '-o', directory / 'extended.mp4', *COLOR], extended)
    verify(directory / 'extended.mp4', 37, 8)

    ten = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'color=c=0x4080c0:size=320x180:rate=30000/1001',
                          '-frames:v', '37', '-pix_fmt', 'yuv420p10le', '-strict', '-1', '-f', 'yuv4mpegpipe', '-'],
                         capture_output=True, check=True).stdout
    (directory/'ffmpeg10.y4m').write_bytes(ten)
    ten_output = directory/'ffmpeg10.mp4'
    execute('ffmpeg10', ['--avsw','-i','-','-o',ten_output,'--codec','hevc','--output-depth','10',
                         '--input-chroma-loc','left',*COLOR], ten)
    verify(ten_output, 37, 10)
    source_pixels = subprocess.run(['ffmpeg','-v','error','-i',str(directory/'ffmpeg10.y4m'),'-frames:v','1',
                                   '-pix_fmt','yuv420p10le','-f','rawvideo','-'], capture_output=True, check=True).stdout
    encoded_pixels = subprocess.run(['ffmpeg','-v','error','-i',str(ten_output),'-frames:v','1',
                                    '-pix_fmt','yuv420p10le','-f','rawvideo','-'], capture_output=True, check=True).stdout
    for offset in [0, 320*180, 320*180*5//4]:
        value = int.from_bytes(encoded_pixels[offset*2:offset*2+2], 'little')
        expected = int.from_bytes(source_pixels[offset*2:offset*2+2], 'little')
        require(abs(value - expected) <= 4, 'Incorrect 10-bit Y/U/V upload')
    execute('missing-chroma-location', ['--avsw','-i','-','-o',directory/'missing-chroma.mp4',*COLOR], ten, code=1)

    # Real VSPipe 8/10-bit producers, also check their exit status separately.
    for depth in ([] if args.skip_vspipe else [8, 10]):
        script = directory / f'vs{depth}.vpy'
        script.write_text(f'import vapoursynth as vs\nc = vs.core.std.BlankClip(width=320, height=180, length=37, '
                          f'fpsnum=30000, fpsden=1001, format=vs.YUV420P{depth}, color={[64 << (depth-8), 100 << (depth-8), 180 << (depth-8)]})\nc.set_output()\n')
        producer = subprocess.run([args.vspipe, '-c', 'y4m', str(script), '-'], capture_output=True, timeout=30)
        require(producer.returncode == 0, producer.stderr.decode())
        path = directory / f'vs{depth}.mp4'
        options = ['--avsw', '-i', '-', '-o', path, '--input-chroma-loc', 'left', *COLOR]
        if depth == 10:
            options += ['--codec', 'hevc', '--output-depth', '10']
        execute(f'vspipe-{depth}', options, producer.stdout)
        verify(path, 37, depth)
        (directory / f'vs{depth}.y4m').write_bytes(producer.stdout)
        # Verify decoded sample values: little-endian planar 10-bit must not be
        # mistaken for P010 high bits or swap U and V on upload.
        raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(path), '-frames:v', '1', '-pix_fmt',
                              'yuv420p10le' if depth == 10 else 'yuv420p', '-f', 'rawvideo', '-'], capture_output=True, check=True).stdout
        for offset, expected in [(0, 64), (320*180, 100), (320*180*5//4, 180)]:
            value = raw[offset] if depth == 8 else int.from_bytes(raw[offset*2:offset*2+2], 'little')
            require(abs(value - (expected << (depth-8))) <= 4, f'Incorrect {depth}-bit planar upload: {value}')

    # Clean EOF cannot communicate producer failure at a complete frame boundary.
    failed_producer = subprocess.run(['sh', '-c', 'cat "$1"; exit 7', 'producer', str(disguised)], capture_output=True)
    require(failed_producer.returncode == 7, 'Producer failure fixture')
    execute('clean-eof-producer-failed', ['--avsw', '-i', '-', '-o', directory / 'producer-failed.mp4', *COLOR], failed_producer.stdout)

    for name, payload in [('truncated-pixels', y4m[:-7]), ('truncated-header', y4m+b'FR'),
                           ('empty', y4m[:first]), ('missing-fps', y4m.replace(b'F30000:1001', b'')),
                           ('zero-fps', y4m.replace(b'F30000:1001', b'F0:1')),
                           ('duplicate-fps', y4m.replace(b'F30000:1001', b'F30000:1001 F0:1')),
                           ('unsupported-chroma', y4m.replace(b'C420jpeg', b'C444'))]:
        path = directory / (name + '.mp4')
        execute(name, ['--avsw', '-i', '-', '-o', path, *COLOR], payload, code=1)
        require(not path.exists(), 'Invalid input published output')
    for name, flags in [('missing-colors', []), ('default-decoder', COLOR), ('forced-hardware', ['--avhw', *COLOR])]:
        path = directory / (name + '.mp4')
        options = ['-i', '-', '-o', path, *flags]
        if name == 'missing-colors': options += ['--avsw']
        result, _ = execute(name, options, y4m, code=1)
        require(b'restarting' not in result.stderr and not path.exists(), 'Non-replayable Y4M retried')

    # Nonzero timeline start: progress is media elapsed, not absolute PTS.
    offset = directory / 'offset.mp4'
    subprocess.run(['ffmpeg', '-v', 'error', '-i', str(output), '-c', 'copy', '-output_ts_offset', '5', str(offset)], check=True)
    _, parsed = execute('nonzero-start', ['--avsw', '-i', offset, '-o', directory / 'offset-out.mp4'])
    final = parsed[-1]
    require(Fraction(final['media_time']['value'], final['media_time']['timescale']) == Fraction(37037, 30000), 'Progress included start offset')
    require(final['percent'] == 100 and final['eta_seconds'] == 0, 'Known duration final progress')
    verify(directory / 'offset-out.mp4', 37, 8, origin=Fraction(5))

    # Known raw frame count, Bob count, overwrite refusal, and stdout media.
    raw_args = ['--synthetic', '--input-res', '320x180', '--frames', '37', '--fps', '30000/1001']
    result, parsed = execute('raw-stdout', [*raw_args, '-o', '-', '-f', 'mpegts'])
    require(result.stdout.startswith(b'\x47') and parsed[-1]['bytes'] == len(result.stdout), 'Contaminated stdout or wrong byte count')
    require(parsed[-1]['percent'] == 100 and parsed[-1]['frames'] == 37, 'Known raw count')
    (directory / 'stdout.ts').write_bytes(result.stdout)
    before = output.read_bytes()
    execute('overwrite', [*raw_args, '-o', output], code=1)
    require(output.read_bytes() == before, 'Overwrote published file')
    execute('muxer-error', [*raw_args, '-o', '-', '-f', 'mp4'], code=1)

    raw = directory / 'bob.nv12'
    subprocess.run(['ffmpeg','-v','error','-i',str(disguised),'-pix_fmt','nv12','-f','rawvideo',str(raw)], check=True)
    path = directory / 'raw-bob.mp4'
    _, parsed = execute('raw-bob', ['-i', raw, '--input-res', '320x180', '--frames', '37', '--fps', '30000/1001',
                                    '--deinterlace', 'bob', '--field-order', 'tff', '-o', path])
    require(parsed[-1]['frames'] == 74 and parsed[-1]['percent'] == 100, 'Bob status did not use output frames')
    verify(path, 74, 8, step=Fraction(1001, 60000))

    # Blocked producer input, both before the header and after frame processing.
    for name, payload in [('idle-input', b''), ('stalled-input', y4m)]:
        path = directory / (name + '.mp4')
        with (directory / (name+'.jsonl')).open('wb') as log:
            proc = subprocess.Popen([binary, '--json-status', '--avsw', '-i', '-', '-o', path, *COLOR], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
            try:
                if payload: proc.stdin.write(payload); proc.stdin.flush()
                time.sleep(0.3)
                proc.send_signal(signal.SIGINT)
                proc.wait(timeout=5)
                require(proc.returncode == 130 and not path.exists(), 'Blocked stdin cancellation failed')
            finally:
                if proc.poll() is None: proc.kill(); proc.wait()
                proc.stdin.close(); proc.stdout.close()
        parsed = events((directory/(name+'.jsonl')).read_bytes(), 'cancelled')
        if not payload: require(parsed[-1].get('hardware') is not True, 'Cancelled before session falsely verified hardware')

    # Backpressure: let output fill without consuming, then release it and verify
    # every frame. A separate stderr file prevents logging from becoming the bottleneck.
    long_args = ['--synthetic', '--input-res', '640x360', '--frames', '600', '--fps', '30000/1001', '-o', '-', '-f', 'mpegts']
    with (directory/'backpressure.jsonl').open('wb') as log:
        proc = subprocess.Popen([binary, '--json-status', *long_args], stdout=subprocess.PIPE, stderr=log)
        try:
            require(select.select([proc.stdout], [], [], 10)[0], 'No initial stdout data')
            time.sleep(0.3)
            require(proc.poll() is None, 'Writer did not experience backpressure')
            payload, _ = proc.communicate(timeout=30)
            require(proc.returncode == 0, 'Backpressure recovery failed')
        finally:
            if proc.poll() is None: proc.kill(); proc.communicate()
    parsed = events((directory/'backpressure.jsonl').read_bytes(), 'completed')
    require(parsed[-1]['frames'] == 600, 'Backpressure lost frames')
    (directory/'backpressure.ts').write_bytes(payload)
    # MPEG-TS may add muxer origin; validate using its first decoded PTS.
    ff = json.loads(subprocess.run(['ffprobe','-v','error','-show_streams','-of','json',str(directory/'backpressure.ts')], capture_output=True, check=True).stdout)
    verify(directory/'backpressure.ts', 600, 8, Fraction(ff['streams'][0]['start_pts']) * Fraction(ff['streams'][0]['time_base']))
    for name, close_pipe in [('broken-output', True), ('blocked-output-cancel', False)]:
        with (directory/(name+'.jsonl')).open('wb') as log:
            proc = subprocess.Popen([binary, '--json-status', *long_args], stdout=subprocess.PIPE, stderr=log)
            try:
                if close_pipe: proc.stdout.close()
                else:
                    require(select.select([proc.stdout], [], [], 10)[0], 'No stdout before cancellation')
                    time.sleep(0.2)
                    proc.send_signal(signal.SIGINT)
                proc.wait(timeout=5)
                require(proc.returncode == (1 if close_pipe else 130), 'Wrong pipe exit code')
            finally:
                if proc.poll() is None: proc.kill(); proc.wait()
                proc.stdout.close()
        events((directory/(name+'.jsonl')).read_bytes(), 'failed' if close_pipe else 'cancelled')

    # Backpressure propagates all the way to a Y4M producer, then resumes at EOF.
    producer_errors = []
    with (directory/'y4m-backpressure.jsonl').open('wb') as log:
        proc = subprocess.Popen([binary, '--json-status', '--avsw', '-i', '-', '-o', '-', '-f', 'mpegts', *COLOR],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
        def feed():
            try:
                proc.stdin.write(y4m[:first])
                for _ in range(10): proc.stdin.write(y4m[first:])
                proc.stdin.close()
            except Exception as error:
                producer_errors.append(str(error))
        writer = threading.Thread(target=feed)
        writer.start()
        try:
            require(select.select([proc.stdout], [], [], 10)[0], 'No Y4M stdout')
            time.sleep(0.3)
            require(writer.is_alive() and proc.poll() is None, 'Backpressure did not reach producer')
            # Read while the producer is active; do not communicate() on its stdin.
            chunks = []
            deadline = time.monotonic() + 30
            while True:
                require(time.monotonic() < deadline, 'Y4M backpressure recovery timed out')
                if not select.select([proc.stdout], [], [], 1)[0]: continue
                chunk = os.read(proc.stdout.fileno(), 65536)
                if not chunk: break
                chunks.append(chunk)
            proc.wait(timeout=5)
            writer.join(timeout=5)
            require(proc.returncode == 0 and not writer.is_alive() and not producer_errors, 'Y4M producer recovery failed')
        finally:
            if proc.poll() is None: proc.kill(); proc.wait()
            writer.join(timeout=5)
            proc.stdout.close()
    parsed = events((directory/'y4m-backpressure.jsonl').read_bytes(), 'completed')
    require(parsed[-1]['frames'] == 370, 'Y4M backpressure lost frames')
    path = directory/'y4m-backpressure.ts'
    path.write_bytes(b''.join(chunks))
    stream = json.loads(subprocess.run(['ffprobe','-v','error','-show_streams','-of','json',str(path)], capture_output=True, check=True).stdout)['streams'][0]
    verify(path, 370, 8, Fraction(stream['start_pts']) * Fraction(stream['time_base']))

    # Plain terminal progress is live, includes percent/fps, and lives on stderr.
    master, slave = pty.openpty()
    proc = subprocess.Popen([binary, *raw_args, '-o', directory/'terminal.mp4'], stdout=subprocess.PIPE, stderr=slave)
    os.close(slave)
    terminal = bytearray()
    while True:
        try:
            data = os.read(master, 4096)
            if not data: break
            terminal.extend(data)
        except OSError:
            break
    os.close(master)
    stdout, _ = proc.communicate(timeout=10)
    require(proc.returncode == 0 and not stdout and b'\r\x1b[2K' in terminal and b'%' in terminal and b' fps' in terminal, 'Missing terminal progress')
    (directory/'terminal.log').write_bytes(terminal)
    require(not list(directory.glob('*.part.*')), 'Leaked temporary output')
    report = {'vspipe_verified': not args.skip_vspipe, 'machine': platform.machine(), 'system': platform.platform(), 'records': records,
              'checks': ['8/10-bit PTS and pixels', 'FFmpeg' if args.skip_vspipe else 'VSPipe and FFmpeg', 'EOF/truncation', 'producer exit status',
                         'stdin SIGINT', 'stdout backpressure/EPIPE/SIGINT', 'terminal progress', 'JSON escaping/capabilities']}
    (directory/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    print(f'PASS P4 status/Y4M (VSPipe verified={not args.skip_vspipe}): {directory}', flush=True)


if __name__ == '__main__':
    main()
