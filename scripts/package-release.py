#!/usr/bin/env python3
"""Package and audit the distribution preset. Never bundles Homebrew libraries."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'build/distribution'
PREFIX = WORK / 'prefix'


def run(*args):
    return subprocess.check_output([str(x) for x in args], text=True).strip()


def dependencies(path):
    return [line.strip().split(' (compatibility version')[0]
            for line in run('otool', '-L', path).splitlines()[1:]]


def system(path):
    return path.startswith(('/usr/lib/', '/System/Library/'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--identity', default='-', help='codesign identity; default: explicit ad-hoc signing')
    parser.add_argument('--notary-profile', help='Existing notarytool keychain profile; requires Developer ID')
    args = parser.parse_args()
    if args.notary_profile and args.identity == '-':
        parser.error('Notarization requires a Developer ID Application identity')
    # Verify the effective FFmpeg runtime, not just the requested configure flags.
    codec = ctypes.CDLL(str(PREFIX / 'lib/libavcodec.dylib'))
    codec.avcodec_license.restype = ctypes.c_char_p
    codec.avcodec_configuration.restype = ctypes.c_char_p
    license_ = codec.avcodec_license().decode()
    configuration = codec.avcodec_configuration().decode()
    if license_ != 'LGPL version 2.1 or later':
        raise RuntimeError('Unexpected FFmpeg license: ' + license_)
    codec.av_codec_iterate.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    codec.av_codec_iterate.restype = ctypes.c_void_p
    codec.av_codec_is_encoder.argtypes = [ctypes.c_void_p]
    codec.av_codec_is_encoder.restype = ctypes.c_int
    opaque = ctypes.c_void_p()
    while entry := codec.av_codec_iterate(ctypes.byref(opaque)):
        if codec.av_codec_is_encoder(entry):
            raise RuntimeError('The distribution FFmpeg must contain no encoders')
    binary = WORK / 'vtenc'
    version = run(binary, '--version').split()[1]
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[a-zA-Z0-9.]+)?', version):
        raise RuntimeError('Unsafe release version: ' + version)
    name = f'vtenc-{version}-macos-arm64'
    out = ROOT / 'build/packages'
    stage = out / name
    if stage.exists():
        raise RuntimeError(f'Refusing to overwrite {stage}; move the previous package aside first')
    (stage / 'bin').mkdir(parents=True)
    (stage / 'lib').mkdir()
    docs = stage / 'share/doc/vtenc'
    docs.mkdir(parents=True)
    shutil.copy2(binary, stage / 'bin/vtenc')
    # Resolve the complete linked closure before any install-name rewriting.
    queue = [binary]
    libraries = {}
    edges = {}
    while queue:
        current = queue.pop()
        refs = dependencies(current)
        edges[current] = refs
        for dep in refs:
            if system(dep):
                continue
            resolved = PREFIX / 'lib' / Path(dep).name if dep.startswith('@rpath/') else Path(dep)
            resolved = resolved.resolve()
            if not resolved.is_relative_to(PREFIX.resolve()) or not resolved.is_file():
                raise RuntimeError(f'Unbundled/non-release dependency: {current}: {dep}')
            if resolved.name in libraries and libraries[resolved.name] != resolved:
                raise RuntimeError('Dependency basename collision: ' + resolved.name)
            if resolved not in edges and resolved not in queue:
                queue.append(resolved)
            libraries[resolved.name] = resolved
    for source in libraries.values():
        shutil.copy2(source, stage / 'lib' / source.name)
    files = [(binary, stage / 'bin/vtenc')] + [(p, stage / 'lib' / p.name) for p in libraries.values()]
    records = []
    for source, dest in files:
        dest.chmod(0o755)
        is_library = source != binary
        changes = []
        if is_library:
            changes += ['-id', '@rpath/' + dest.name]
        for dep in edges[source]:
            if system(dep):
                continue
            resolved = (PREFIX / 'lib' / Path(dep).name if dep.startswith('@rpath/') else Path(dep)).resolve()
            changes += ['-change', dep, ('@loader_path/' if is_library else '@executable_path/../lib/') + resolved.name]
        # Remove build-machine rpaths, including inherited ones in third-party libraries.
        loads = run('otool', '-l', dest)
        rpaths = re.findall(r'cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset', loads)
        for path in dict.fromkeys(rpaths):
            changes += ['-delete_rpath', path]
        if changes:
            run('install_name_tool', *changes, dest)
        arch = run('lipo', '-archs', dest)
        if arch != 'arm64':
            raise RuntimeError(f'Unexpected architecture: {dest}: {arch}')
        loads = run('otool', '-l', dest)
        minimum = re.findall(r'\bminos ([0-9.]+)', loads) or re.findall(r'\bversion ([0-9.]+)', loads)
        if len(minimum) != 1:
            raise RuntimeError(f'Cannot establish deployment target: {dest}')
        weak_libraries = re.findall(r'cmd LC_LOAD_WEAK_DYLIB\s+cmdsize \d+\s+name (.*?) \(offset', loads)
        if '/usr/lib/libhvf.dylib' in dependencies(dest) and '/usr/lib/libhvf.dylib' not in weak_libraries:
            raise RuntimeError('HVF must be weak linked for systems older than macOS 15.4')
        for dep in dependencies(dest):
            if not system(dep) and not dep.startswith(('@loader_path/', '@executable_path/', '@rpath/')):
                raise RuntimeError(f'Non-relocatable load command: {dep}')
        sign = ['codesign', '--force', '--sign', args.identity]
        if args.identity != '-':
            sign += ['--options', 'runtime', '--timestamp']
        run(*sign, dest)
        run('codesign', '--verify', '--strict', dest)
        records.append({'path': str(dest.relative_to(stage)), 'architecture': arch, 'minimum_macos': minimum[0], 'weak_system_libraries': weak_libraries,
                        'sha256': hashlib.sha256(dest.read_bytes()).hexdigest(), 'dependencies': dependencies(dest)})
    minimum = max((x['minimum_macos'] for x in records), key=lambda v: tuple(map(int, v.split('.'))))
    manifest = {'version': version, 'minimum_macos_load_commands': minimum,
                'signing': 'ad-hoc' if args.identity == '-' else 'Developer ID',
                'notarization': 'see adjacent notary result' if args.notary_profile else 'not notarized',
                'source_revision': run('git', '-C', ROOT, 'rev-parse', 'HEAD'),
                'source_dirty': bool(run('git', '-C', ROOT, 'status', '--porcelain')),
                'build_os': run('sw_vers', '-productVersion'),
                'compiler': run('xcrun', 'clang++', '--version').splitlines()[0],
                'ffmpeg': {'license': license_, 'configuration': configuration.replace(str(ROOT), '<VTENC_SOURCE>'),
                           'encoder_count': 0}, 'files': records,
                'dependencies': json.loads((ROOT / 'scripts/dependencies.json').read_text())}
    for file in ('README.md', 'THIRD_PARTY_NOTICES.md'):
        shutil.copy2(ROOT / file, docs / file)
    # Keep the repository's development documents online, not in the binary ZIP.
    readme = docs / 'README.md'
    readme.write_text(re.sub(
        r'\]\((docs/[^)]+)\)',
        lambda match: '](' + 'https://github.com/zyzsdy/VTEnc/blob/' +
        manifest['source_revision'] + '/' + quote(match.group(1), safe='/') + ')',
        readme.read_text()))
    licenses = docs / 'licenses'; licenses.mkdir()
    license_files = {
        'ffmpeg': ['COPYING.LGPLv2.1', 'LICENSE.md'],
        'freetype': ['docs/FTL.TXT', 'LICENSE.TXT'],
        'harfbuzz': ['COPYING'], 'fribidi': ['COPYING'],
        'libunibreak': ['LICENCE'], 'libass': ['COPYING'],
    }
    for item in manifest['dependencies']:
        name_ = item['name']; src = WORK / 'src' / f"{name_}-{item['version']}"
        for file in license_files[name_]:
            target = licenses / name_ / Path(file).name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src / file, target)
    recipes = docs / 'build-recipes'; recipes.mkdir()
    for receipt in (WORK / 'receipts').glob('*.json'):
        (recipes / receipt.name).write_text(receipt.read_text().replace(str(ROOT), '<VTENC_SOURCE>'))
    (stage / 'BUILD-MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (stage / 'README.txt').write_text(
        f'VTEnc {version} / macOS arm64\n'
        f'Mach-O deployment floor: macOS {minimum}; feature availability depends on the system and hardware.\n'
        'Run bin/vtenc; retain the sibling lib directory. Homebrew is not needed at runtime.\n'
        f'Signing: {manifest["signing"]}. '
        + ('Not notarized; Gatekeeper may block downloaded files.\n' if not args.notary_profile else
           'Notarization result is published in the adjacent notary JSON asset.\n')
        + 'See share/doc/vtenc/README.md and THIRD_PARTY_NOTICES.md.\n'
        'This software uses FFmpeg under LGPL-2.1-or-later. Exact dependency sources and build\n'
        f'recipes are provided in vtenc-{version}-dependency-sources.tar.gz in the same Release.\n'
        'LGPL libraries may be replaced with compatible modified builds. You may re-sign your\n'
        'local modified executable/libraries ad-hoc; reverse engineering for debugging those\n'
        'library modifications is permitted. No fonts or FFmpeg command-line tools are bundled.\n')
    archive = out / (name + '.zip')
    run('ditto', '-c', '-k', '--keepParent', stage, archive)
    if args.notary_profile:
        result = run('xcrun', 'notarytool', 'submit', archive, '--keychain-profile', args.notary_profile,
                     '--wait', '--output-format', 'json')
        notary = out / (name + '-notary.json'); notary.write_text(result + '\n')
        if json.loads(result)['status'] != 'Accepted':
            raise RuntimeError(f'Notarization not accepted; see {notary}')
    source_archive = out / f'vtenc-{version}-dependency-sources.tar.gz'
    with tarfile.open(source_archive, 'w:gz') as tar:
        for item in manifest['dependencies']:
            archive_ = WORK / 'downloads' / item['url'].split('/')[-1]
            if hashlib.sha256(archive_.read_bytes()).hexdigest() != item['sha256']:
                raise RuntimeError(f'Dependency source mismatch: {archive_}')
            tar.add(archive_, arcname='sources/' + archive_.name)
        for file in ('build-dependencies.py', 'dependencies.json', 'package-release.py'):
            tar.add(ROOT / 'scripts' / file, arcname='scripts/' + file)
        tar.add(recipes, arcname='build-recipes')
    artifacts = [archive, source_archive]
    if args.notary_profile:
        artifacts.append(notary)
    checksum = out / f'vtenc-{version}-SHA256SUMS.txt'
    checksum.write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n' for p in artifacts))
    print(json.dumps({'package': str(archive), 'sources': str(source_archive), 'checksums': str(checksum),
                      'minimum_macos': minimum, 'libraries': len(libraries)}, indent=2))


if __name__ == '__main__':
    main()
