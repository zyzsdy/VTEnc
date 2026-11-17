#!/usr/bin/env python3
"""Relocate a release ZIP and deny access to Homebrew and the source checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix='vtenc-release-isolation-')).resolve()
    with zipfile.ZipFile(args.archive) as archive:
        archive.extractall(work)
    package = next(p for p in work.iterdir() if p.is_dir())
    manifest = json.loads((package / 'BUILD-MANIFEST.json').read_text())
    for entry in manifest['files']:
        path = package / entry['path']; path.chmod(0o755)
        assert hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256'], entry['path']
        subprocess.run(['codesign', '--verify', '--strict', str(path)], check=True)
    binary = package / 'bin/vtenc'
    # All caller input and output for these checks lives outside the denied paths.
    profile = '(version 1)(allow default)' + ''.join(
        '(deny file-read* (subpath ' + json.dumps(str(p)) + '))'
        for p in (ROOT, Path('/opt/homebrew'), Path('/usr/local')))
    env = {'HOME': str(work), 'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'TMPDIR': str(work)}
    prefix = ['/usr/bin/sandbox-exec', '-p', profile, str(binary)]
    denied = subprocess.run(['/usr/bin/sandbox-exec', '-p', profile, '/bin/cat', str(ROOT / 'README.md')],
                            env=env, capture_output=True)
    assert denied.returncode != 0, 'Isolation policy did not deny the checkout'
    version = subprocess.run([*prefix, '--version'], env={**env, 'DYLD_PRINT_LIBRARIES': '1'},
                             capture_output=True, check=True)
    (work / 'loader.log').write_bytes(version.stderr)
    assert b'/opt/homebrew/' not in version.stderr and str(ROOT).encode() not in version.stderr
    records = []

    def encode(name, options, data=None):
        output = work / (name + '.mp4')
        result = subprocess.run([*prefix, '--json-status', *map(str, options), '-o', str(output)],
                                env=env, input=data, capture_output=True, timeout=90)
        (work / (name + '.jsonl')).write_bytes(result.stderr)
        assert result.returncode == 0, result.stderr.decode(errors='replace')
        assert not result.stdout
        events = [json.loads(x) for x in result.stderr.splitlines()]
        assert events[-1]['event'] == 'completed' and events[-1]['hardware'], events[-1]
        subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', str(output), '-f', 'null', '-'],
                       check=True, capture_output=True, timeout=60)
        records.append({'case': name, 'final': events[-1]})
        return output

    source = encode('synthetic', ['--synthetic', '--input-res', '640x360', '--frames', '60'])
    encode('hardware-file', ['-i', source, '--avhw', '--no-audio'])
    subtitle = work / 'subtitle.ass'; subtitle.write_bytes((ROOT / 'tests/fixtures/p6.ass').read_bytes())
    encode('subtitle', ['-i', source, '--avsw', '--no-audio', '--vpp-subburn', 'filename=' + str(subtitle)])
    y4m = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(source), '-pix_fmt', 'yuv420p10le',
                          '-strict', '-1', '-f', 'yuv4mpegpipe', '-'], capture_output=True, check=True).stdout
    encode('y4m-main10', ['-i', '-', '--avsw', '--no-audio', '--codec', 'hevc', '--output-depth', '10',
                         '--vpp-colorspace', 'matrix=bt709:auto,transfer=bt709:auto,colorprim=bt709:auto,range=limited:auto',
                         '--input-chroma-loc', 'left'], y4m)
    report = work / 'report.json'
    report.write_text(json.dumps({'package': str(args.archive), 'version': manifest['version'],
                                 'denied_paths': [str(ROOT), '/opt/homebrew', '/usr/local'],
                                 'cases': records}, indent=2) + '\n')
    print('PASS relocated release with source/Homebrew reads denied:', report)
    print('Extracted binary:', binary)


if __name__ == '__main__':
    main()
