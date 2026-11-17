#!/usr/bin/env python3
"""Build pinned, shared distribution dependencies; outputs stay under build/."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'build/distribution'
PREFIX = WORK / 'prefix'
SOURCES = WORK / 'src'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 12))
    parser.add_argument('--only', choices=['freetype', 'harfbuzz', 'fribidi', 'libunibreak', 'libass', 'ffmpeg'],
                        help='Build one dependency (its prerequisites must already be installed)')
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64' or args.jobs < 1:
        parser.error('Requires native macOS arm64 and a positive job count')
    for folder in ('downloads', 'src', 'logs', 'receipts'):
        (WORK / folder).mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    # No auto-discovered Homebrew libraries or caller compiler flags in release dependencies.
    for key in ('CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'CPATH', 'LIBRARY_PATH',
                'CMAKE_PREFIX_PATH', 'PKG_CONFIG_PATH'):
        env.pop(key, None)
    env.update(CC='/usr/bin/clang', CXX='/usr/bin/clang++', MACOSX_DEPLOYMENT_TARGET='13.0',
               PKG_CONFIG_LIBDIR=str(PREFIX / 'lib/pkgconfig'),
               CFLAGS='-O2 -arch arm64 -mmacosx-version-min=13.0',
               CXXFLAGS='-O2 -arch arm64 -mmacosx-version-min=13.0',
               LDFLAGS='-arch arm64 -mmacosx-version-min=13.0 -Wl,-headerpad_max_install_names')
    packages = json.loads((ROOT / 'scripts/dependencies.json').read_text())
    order = ['freetype', 'harfbuzz', 'fribidi', 'libunibreak', 'libass', 'ffmpeg']
    for name in order:
        if args.only and name != args.only:
            continue
        item = next(x for x in packages if x['name'] == name)
        archive = WORK / 'downloads' / item['url'].split('/')[-1]
        if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != item['sha256']:
            partial = archive.with_name(archive.name + '.partial')
            subprocess.run(['curl', '-4', '--fail', '--location', '--retry', '3', '--connect-timeout', '30',
                            '--max-time', '1800', item['url'], '-o', str(partial)], check=True)
            if hashlib.sha256(partial.read_bytes()).hexdigest() != item['sha256']:
                raise RuntimeError(f'SHA256 mismatch: {partial}')
            partial.replace(archive)
        source = SOURCES / f"{name}-{item['version']}"
        if not source.exists():
            subprocess.run(['tar', '-xf', str(archive), '-C', str(SOURCES)], check=True)
        build = WORK / ('build-' + name)
        build.mkdir(exist_ok=True)
        commands = []
        if name == 'freetype':
            commands = [['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
                         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_OSX_ARCHITECTURES=arm64',
                         '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0', '-DCMAKE_C_COMPILER=/usr/bin/clang',
                         '-DCMAKE_INSTALL_PREFIX=' + str(PREFIX), '-DBUILD_SHARED_LIBS=ON',
                         '-DFT_DISABLE_HVF=OFF', '-DHVF_LIBRARY=-weak-lhvf',
                         *['-DFT_DISABLE_' + x + '=ON' for x in
                           ('ZLIB', 'BZIP2', 'PNG', 'HARFBUZZ', 'BROTLI')]],
                        ['cmake', '--build', str(build), '-j', str(args.jobs)],
                        ['cmake', '--install', str(build)]]
        elif name in ('harfbuzz', 'fribidi'):
            options = (['-Dglib=disabled', '-Dgobject=disabled', '-Dcairo=disabled', '-Dicu=disabled',
                        '-Dgraphite2=disabled', '-Dfreetype=enabled', '-Dcoretext=enabled',
                        '-Dauto_features=disabled', '-Draster=disabled', '-Dvector=disabled',
                        '-Dgpu=disabled', '-Dsubset=disabled',
                        '-Dtests=disabled', '-Ddocs=disabled', '-Dutilities=disabled'] if name == 'harfbuzz'
                       else ['-Ddocs=false', '-Dbin=false', '-Dtests=false'])
            commands = [['meson', 'setup', *(['--reconfigure'] if (build / 'meson-private').exists() else []),
                         str(build), str(source), '--prefix=' + str(PREFIX), '--libdir=lib',
                         '--buildtype=release', '--default-library=shared', '--wrap-mode=nodownload', *options],
                        ['meson', 'compile', '-C', str(build), '-j', str(args.jobs)],
                        ['meson', 'install', '-C', str(build)]]
        elif name in ('libunibreak', 'libass'):
            options = ['--disable-static', '--enable-shared']
            if name == 'libass':
                options += ['--disable-fontconfig', '--enable-coretext', '--disable-test', '--disable-profile']
            commands = [[str(source / 'configure'), '--prefix=' + str(PREFIX), *options],
                        ['make', '-j', str(args.jobs)], ['make', 'install']]
        else:
            commands = [[str(source / 'configure'), '--prefix=' + str(PREFIX), '--cc=/usr/bin/clang',
                         '--cxx=/usr/bin/clang++', '--arch=arm64', '--target-os=darwin',
                         '--enable-shared', '--disable-static', '--disable-autodetect',
                         '--disable-gpl', '--disable-nonfree', '--disable-version3',
                         '--disable-programs', '--disable-doc', '--disable-debug',
                         '--disable-encoders', '--disable-avdevice', '--disable-avfilter', '--disable-swscale',
                         '--enable-videotoolbox', '--enable-audiotoolbox', '--enable-zlib',
                         '--extra-cflags=-mmacosx-version-min=13.0',
                         '--extra-ldflags=-mmacosx-version-min=13.0 -Wl,-headerpad_max_install_names'],
                        ['make', '-j', str(args.jobs)], ['make', 'install']]
        recipe = {'source': item, 'commands': [[x for x in cmd if x != '--reconfigure'] for cmd in commands],
                  'environment': {k: env[k] for k in ('CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS',
                                                     'MACOSX_DEPLOYMENT_TARGET', 'PKG_CONFIG_LIBDIR')}}
        receipt = WORK / 'receipts' / (name + '.json')
        if receipt.exists() and json.loads(receipt.read_text()) == recipe:
            print('Already built:', name, flush=True)
            continue
        print('Building:', name, flush=True)
        with (WORK / 'logs' / (name + '.log')).open('w') as log:
            for command in commands:
                log.write(shlex.join(command) + '\n'); log.flush()
                subprocess.run(command, cwd=build, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
        receipt.write_text(json.dumps(recipe, indent=2) + '\n')
    print('Dependencies ready:', PREFIX)


if __name__ == '__main__':
    main()
