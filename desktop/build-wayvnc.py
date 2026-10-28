#!/usr/bin/env python3
"""Build a private, output-locked wayvnc; never install or start it."""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dest', type=Path, default=Path(os.environ.get(
    'XDG_CACHE_HOME', str(Path.home() / '.cache'))) / 'omarchy-display/capture')
DEST = parser.parse_args().dest.expanduser().resolve()
SOURCE = DEST / 'wayvnc-source'
COMMIT = 'bb837459c75fd3c267d48af170ac7ed2566f67e5'  # upstream v0.10.2
PATCH_VERSION = 1
SUBPROJECTS = {
    'aml': '685035c9830aa89df02a43df89b644690bd885f5',  # v1.0.0
    'neatvnc': '783437adc56c2d7ce2cac1ec584acad798594267',  # v1.0.3
}
TOOL_VERSIONS = {'meson': '1.12.1', 'ninja': '1.13.0', 'cmake': '3.31.6'}
PKG_REQUIREMENTS = {
    'wayland-client': None, 'pixman-1': None, 'libdrm': None,
    'xkbcommon': '1.0.0', 'jansson': None, 'zlib': None, 'libturbojpeg': None,
}


def checkout_dependency(name, revision):
    directory = SOURCE / 'subprojects' / name
    if not directory.exists():
        directory.parent.mkdir(parents=True, exist_ok=True)
        run('git', 'clone', '--no-checkout', 'https://github.com/any1/' + name + '.git', str(directory))
        run('git', '-C', str(directory), 'fetch', 'origin', revision)
        run('git', '-C', str(directory), 'checkout', '--detach', revision)
    head = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
    if head != revision:
        raise SystemExit('Private ' + name + ' checkout moved; refusing to overwrite it')
    # Dependency sources are pristine. Never reset/stash/replace another edit.
    if subprocess.run(['git', '-C', str(directory), 'diff', '--quiet', 'HEAD', '--']).returncode:
        raise SystemExit('Private ' + name + ' checkout has edits; refusing to build unpinned source')
    if subprocess.check_output(['git', '-C', str(directory), 'ls-files', '--others', '--exclude-standard'], text=True).strip():
        raise SystemExit('Private ' + name + ' checkout has untracked files; preserve them before building')


def private_tools():
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'amd64'):
        raise SystemExit('Pinned capture-tool wheel hashes currently target Linux x86_64 only')
    directory = DEST / 'build-tools'
    python = directory / 'bin/python'
    env = dict(os.environ)
    env.pop('PYTHONPATH', None)
    env.pop('PYTHONHOME', None)
    env['VIRTUAL_ENV'] = str(directory)
    env['PATH'] = str(directory / 'bin') + os.pathsep + env.get('PATH', '')
    env['TMPDIR'] = str(DEST / 'tmp')
    env['XDG_CACHE_HOME'] = str(DEST / 'cache')
    Path(env['TMPDIR']).mkdir(exist_ok=True)
    Path(env['XDG_CACHE_HOME']).mkdir(exist_ok=True)
    env['NINJA'] = str(directory / 'bin/ninja')
    if not python.exists():
        subprocess.run([sys.executable, '-m', 'venv', str(directory)], check=True, env=env)
    probe = subprocess.run([str(python), '-c',
        'import importlib.metadata as m,json; '
        'print(json.dumps({name:m.version(name) for name in ("meson","ninja","cmake")}))'],
        capture_output=True, text=True, env=env)
    installed = json.loads(probe.stdout) if probe.returncode == 0 else {}
    if installed != TOOL_VERSIONS:
        # ensurepip is part of this host's Python; never use get-pip or system pip.
        subprocess.run([str(python), '-m', 'ensurepip', '--default-pip'], check=True, env=env)
        subprocess.run([str(python), '-m', 'pip', '--isolated', '--disable-pip-version-check',
            '--cache-dir', str(DEST / 'pip-cache'), 'install', '--index-url', 'https://pypi.org/simple',
            '--only-binary=:all:', '--no-deps', '--require-hashes',
            '-r', str(ROOT / 'desktop/capture-tools.txt')], check=True, env=env)
    return [str(python), '-m', 'mesonbuild.mesonmain'], env


def host_libraries():
    versions = {}
    for name, minimum in PKG_REQUIREMENTS.items():
        result = subprocess.run(['pkg-config', '--modversion', name], capture_output=True, text=True)
        if result.returncode:
            raise SystemExit('Missing host pkg-config dependency: ' + name + '; no OS package was installed')
        if minimum and subprocess.run(['pkg-config', '--atleast-version=' + minimum, name]).returncode:
            raise SystemExit('Host dependency ' + name + ' must be >= ' + minimum)
        versions[name] = result.stdout.strip()
    return versions


def run(*args):
    subprocess.run(args, check=True)


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise SystemExit('Pinned wayvnc source differs; refusing an unreviewed patch')
    return text.replace(old, new, 1)


os.umask(0o077)
DEST.mkdir(parents=True, exist_ok=True, mode=0o700)
if DEST.stat().st_uid != os.getuid() or DEST.is_symlink():
    raise SystemExit('Capture build directory must be owned by this user')
DEST.chmod(0o700)
libraries = host_libraries()
meson, build_env = private_tools()
if not SOURCE.exists():
    run('git', 'clone', '--no-checkout', 'https://github.com/any1/wayvnc.git', str(SOURCE))
    run('git', '-C', str(SOURCE), 'fetch', 'origin', COMMIT)
    run('git', '-C', str(SOURCE), 'checkout', '--detach', COMMIT)
# Never reset someone else's source work. This builder owns only its private
# checkout and writes a patched file from the immutable upstream git object.
head = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
if head != COMMIT:
    raise SystemExit('Private capture checkout moved; refusing to overwrite it')
changed = subprocess.check_output(
    ['git', '-C', str(SOURCE), 'diff', '--name-only', 'HEAD', '--'], text=True).splitlines()
if any(name != 'src/main.c' for name in changed):
    raise SystemExit('Private capture source has unrelated edits; refusing unpinned source')
original = subprocess.check_output(['git', '-C', str(SOURCE), 'show', COMMIT + ':src/main.c'], text=True)
patched = replace_once(original, '\t\tswitch_to_prev_output(self);',
    '\t\t/* Omarchy: never fall back to a physical display. */\n'
    '\t\t_exit(EXIT_FAILURE);')
patched = replace_once(patched,
    'static void on_image_source_destroyed(struct observer* observer, void* arg)\n{\n'
    '\tstruct wayvnc* self = wl_container_of(observer, self,\n'
    '\t\t\tdestruction_observer);\n\tself->image_source = NULL;\n}',
    'static void on_image_source_destroyed(struct observer* observer, void* arg)\n{\n'
    '\t/* The selected capture source disappeared: fail closed immediately. */\n'
    '\t_exit(EXIT_FAILURE);\n}')
guard = '''
\t/* Omarchy output lock applies to initial capture, IPC switches and fallback.
\t * An unset lock is an error, not permission to capture the whole desktop. */
\tconst char* locked = getenv("OMARCHY_TV_OUTPUT");
\tif (!locked || !image_source_is_output(image_source) ||
\t\t\tstrcmp(output_get_name(output_from_image_source(image_source)), locked)) {
\t\tnvnc_log(NVNC_LOG_ERROR, "Omarchy output lock rejected capture source");
\t\t_exit(EXIT_FAILURE);
\t}
'''
patched = replace_once(patched,
    'void set_image_source(struct wayvnc* self, struct image_source* image_source)\n{',
    'void set_image_source(struct wayvnc* self, struct image_source* image_source)\n{' + guard)
# switch_to_output modifies the existing display before set_image_source; reject
# before it can touch any new source (including a control-socket switch).
patched = replace_once(patched,
    'void switch_to_output(struct wayvnc* self, struct output* output)\n{',
    'void switch_to_output(struct wayvnc* self, struct output* output)\n{\n'
    '\tconst char* locked = getenv("OMARCHY_TV_OUTPUT");\n'
    '\tif (!locked || !output || strcmp(output_get_name(output), locked))\n'
    '\t\t_exit(EXIT_FAILURE);\n')
main = SOURCE / 'src/main.c'
if main.read_text() not in (original, patched):
    raise SystemExit('Private capture source has unrelated edits; refusing overwrite')
if main.read_text() != patched:
    main.write_text(patched)
for name, revision in SUBPROJECTS.items():
    checkout_dependency(name, revision)
build = DEST / 'wayvnc-build'
args = meson + ['setup', str(build), str(SOURCE), '--buildtype=release',
        '--wrap-mode=nofallback', '-Ddefault_library=static', '-Dprefer_static=false',
        '-Dtests=false', '-Dpam=disabled', '-Dman-pages=disabled',
        '-Dscreencopy-dmabuf=disabled', '-Daml:examples=false',
        '-Dneatvnc:tests=false', '-Dneatvnc:examples=false', '-Dneatvnc:benchmarks=false',
        '-Dneatvnc:jpeg=enabled', '-Dneatvnc:gbm=disabled', '-Dneatvnc:h264=disabled',
        '-Dneatvnc:tls=disabled', '-Dneatvnc:nettle=disabled']
if (build / 'build.ninja').exists():
    args.append('--reconfigure')
subprocess.run(args, check=True, env=build_env)
subprocess.run(meson + ['compile', '-C', str(build)], check=True, env=build_env)
binary = build / 'wayvnc'
receipt = {'commit': COMMIT, 'output_lock_patch': PATCH_VERSION,
           'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
           'subprojects': SUBPROJECTS, 'build_tools': TOOL_VERSIONS,
           'host_pkg_config': libraries, 'default_library': 'static',
           'features': {'capture': 'shm', 'jpeg': True, 'h264': False,
                        'backend_tls': False, 'backend_crypto': False}}
(DEST / 'capture-build.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('Private output-locked capture build ready; no capture process started.')
