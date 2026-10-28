#!/usr/bin/env python3
"""Rootless install/update; --uninstall retains pairing, --purge-config removes it."""
import argparse
import copy
import hashlib
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
ID = 'smartest74.omarchy-display'
HOME = Path.home()

def xdg(name, fallback):
    return Path(os.environ.get(name, str(HOME / fallback))).expanduser().absolute()

DATA = xdg('XDG_DATA_HOME', '.local/share') / 'omarchy-display'
STATE = xdg('XDG_STATE_HOME', '.local/state') / 'omarchy-display'
CACHE = xdg('XDG_CACHE_HOME', '.cache') / 'omarchy-display'
CONFIG = xdg('XDG_CONFIG_HOME', '.config')
APP = DATA / 'app'
LAUNCHER = HOME / '.local/bin/omarchy-display'
PLUGIN = CONFIG / 'omarchy/plugins' / ID
SHELL = CONFIG / 'omarchy/shell.json'
MANIFEST = STATE / 'install.json'
MARKER = '# Owned by Omarchy Display installer; do not edit.'


def private(path):
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    if path.is_symlink() or path.stat().st_uid != os.getuid():
        raise RuntimeError('Application directory is not owned by this user: ' + str(path))
    path.chmod(0o700)


def save(path, value):
    temporary = path.with_name(path.name + '.install-' + str(time.time_ns()) + '.tmp')
    with temporary.open('x') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
    temporary.replace(path)


def inventory(directory):
    result = {}
    for path in sorted(directory.rglob('*')):
        if path.is_symlink():
            result[str(path.relative_to(directory))] = {'link': os.readlink(path)}
        elif path.is_file():
            result[str(path.relative_to(directory))] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
    return result


def remove_owned(directory, files):
    if directory.is_symlink():
        raise RuntimeError('Refusing to remove a replaced application directory: ' + str(directory))
    current = inventory(directory) if directory.exists() else {}
    for name, fingerprint in files.items():
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise RuntimeError('Invalid installation ownership record')
        if current.get(name) == fingerprint:
            path = directory / relative
            # Never traverse a parent directory replaced with a symlink.
            if any(parent.is_symlink() for parent in path.parents if parent != directory.parent):
                raise RuntimeError('Refusing to follow a replaced installed directory')
            path.unlink(missing_ok=True)
    for path in sorted(directory.rglob('*'), key=lambda item: len(item.parts), reverse=True):
        if path.is_dir() and not path.is_symlink():
            try:
                path.rmdir()
            except OSError:
                pass
    try:
        directory.rmdir()
    except OSError:
        print('Preserved changed or unrelated files in ' + str(directory))


def load_manifest():
    if not MANIFEST.exists():
        return None
    value = json.loads(MANIFEST.read_text())
    expected = {'app': str(APP), 'launcher': str(LAUNCHER), 'plugin': str(PLUGIN), 'uid': os.getuid()}
    if value.get('schemaVersion') != 1 or any(value.get(key) != item for key, item in expected.items()):
        raise RuntimeError('Install ownership record does not match these XDG paths/user')
    return value


def backup_shell():
    if SHELL.exists():
        private(STATE / 'shell-backups')
        target = STATE / 'shell-backups' / ('shell-' + str(time.time_ns()) + '.json')
        shutil.copyfile(SHELL, target)
        target.chmod(0o600)


def shell_registration(previous):
    current = json.loads(SHELL.read_text()) if SHELL.exists() else {}
    if not isinstance(current, dict):
        raise RuntimeError('Omarchy shell.json must contain an object')
    original = copy.deepcopy(current)
    seeds = copy.deepcopy((previous or {}).get('shellSeeds', []))
    defaults_path = Path(os.environ.get('OMARCHY_PATH', '/usr/share/omarchy')) / 'config/omarchy/shell.json'
    defaults = json.loads(defaults_path.read_text()) if defaults_path.exists() else {}
    default_layout = defaults.get('bar', {}).get('layout')
    if not isinstance(default_layout, dict):
        raise RuntimeError('Cannot find Omarchy default bar layout; no shell config was changed')
    if 'bar' not in current:
        current['bar'] = {'layout': copy.deepcopy(default_layout)}
        seeds.append({'path': ['bar'], 'value': copy.deepcopy(current['bar'])})
    elif 'layout' not in current['bar']:
        current['bar']['layout'] = copy.deepcopy(default_layout)
        seeds.append({'path': ['bar', 'layout'], 'value': copy.deepcopy(default_layout)})
    layout = current['bar']['layout']
    if not isinstance(layout, dict) or any(not isinstance(entries, list) for entries in layout.values()):
        raise RuntimeError('Unsupported Omarchy bar layout; no shell config was changed')
    existing = any(isinstance(entry, dict) and entry.get('id') == ID
                   for entries in layout.values() for entry in entries)
    existing = existing or any(isinstance(entry, dict) and entry.get('id') == ID
                               for entry in current.get('plugins', []))
    added = (previous or {}).get('shellAdded', False)
    if not existing:
        if 'right' not in layout:
            layout['right'] = copy.deepcopy(default_layout.get('right', []))
            seeds.append({'path': ['bar', 'layout', 'right'], 'value': copy.deepcopy(layout['right'])})
        layout['right'].append({'id': ID})
        added = True
    if current != original:
        SHELL.parent.mkdir(parents=True, exist_ok=True)
        backup_shell()
        save(SHELL, current)
    return added, seeds


def unregister_shell(manifest):
    if not SHELL.exists() or not manifest.get('shellAdded'):
        return
    current = json.loads(SHELL.read_text())
    original = copy.deepcopy(current)
    for entries in current.get('bar', {}).get('layout', {}).values():
        entries[:] = [entry for entry in entries if entry != {'id': ID}]
    if isinstance(current.get('plugins'), list):
        current['plugins'][:] = [entry for entry in current['plugins'] if entry != {'id': ID}]
    for seed in reversed(manifest.get('shellSeeds', [])):
        target = current
        for key in seed['path'][:-1]:
            target = target.get(key, {})
        key = seed['path'][-1]
        if target.get(key) == seed['value']:
            del target[key]
    if current != original:
        backup_shell()
        save(SHELL, current)


def stop_installed():
    controller = APP / 'desktop/display.py'
    if controller.exists():
        subprocess.run([sys.executable, str(controller), 'stop'], check=True)
    elif (STATE / 'state.json').exists():
        raise RuntimeError('Recovery state exists but installed controller is missing; restore it before uninstalling')


def uninstall(purge):
    manifest = load_manifest()
    if manifest:
        stop_installed()
        if manifest.get('keybindingsInstalled') and (APP / 'desktop/keybindings.py').exists():
            subprocess.run([sys.executable, str(APP / 'desktop/keybindings.py'), 'uninstall'], check=True)
        unregister_shell(manifest)
        if manifest.get('pluginFiles'):
            remove_owned(PLUGIN, manifest['pluginFiles'])
        if not LAUNCHER.is_symlink() and LAUNCHER.exists() and \
                hashlib.sha256(LAUNCHER.read_bytes()).hexdigest() == manifest['launcherSha256']:
            LAUNCHER.unlink()
        elif LAUNCHER.exists() or LAUNCHER.is_symlink():
            print('Preserved a changed launcher: ' + str(LAUNCHER))
        remove_owned(APP, manifest['appFiles'])
        MANIFEST.unlink()
    if purge:
        if (STATE / 'state.json').exists():
            raise RuntimeError('Recover the owned running session before deleting private pairing')
        pairing = CONFIG / 'omarchy-display/config.json'
        pairing.unlink(missing_ok=True)
        try:
            pairing.parent.rmdir()
        except OSError:
            pass
    print('Uninstalled owned application/plugin entries. Pairing ' + ('removed.' if purge else 'retained.'))
    print('Build cache and private logs/backups are retained in the application XDG cache/state directories.')


def dependencies():
    missing = [name for name in ('cc', 'git', 'node', 'npm', 'ffmpeg', 'pkg-config', 'wayland-scanner', 'hyprctl')
               if not shutil.which(name)]
    if sys.version_info < (3, 10):
        missing.append('Python >= 3.10')
    if not importlib.util.find_spec('venv') or not importlib.util.find_spec('ensurepip'):
        missing.append('Python venv + ensurepip')
    libraries = ('wayland-client', 'wayland-protocols', 'pixman-1', 'libdrm', 'xkbcommon', 'jansson', 'zlib', 'libturbojpeg')
    if shutil.which('pkg-config'):
        for name in libraries:
            if subprocess.run(['pkg-config', '--exists', name]).returncode:
                missing.append('pkg-config: ' + name)
    if missing:
        raise RuntimeError('Missing native dependencies:\n  ' + '\n  '.join(missing) +
                           '\nInstall the corresponding OS packages yourself; no packages or firewall rules were changed.')
    node = subprocess.check_output(['node', '-p', 'process.versions.node.split(".")[0]'], text=True)
    if int(node.strip()) < 20:
        raise RuntimeError('Node.js >= 20 is required by the desktop bridge')
    if os.geteuid() == 0:
        raise RuntimeError('Run the installer as your desktop user, never with sudo')


def install():
    dependencies()
    previous = load_manifest()
    if APP.is_symlink() or LAUNCHER.is_symlink() or PLUGIN.is_symlink():
        raise RuntimeError('Refusing to replace a symlink at an owned installation path')
    if APP.exists() and (not previous or inventory(APP) != previous['appFiles']):
        raise RuntimeError('Installed application contains unowned/changed files; preserve them before updating')
    if LAUNCHER.exists() and (not previous or hashlib.sha256(LAUNCHER.read_bytes()).hexdigest() != previous['launcherSha256']):
        raise RuntimeError('Refusing to replace an unowned/changed omarchy-display launcher')
    has_shell = bool(shutil.which('omarchy-shell'))
    if has_shell:
        subprocess.run([sys.executable, str(ROOT / 'desktop/keybindings.py'), 'check'], check=True)
    if has_shell and PLUGIN.exists() and (not previous or inventory(PLUGIN) != previous.get('pluginFiles')):
        raise RuntimeError('Refusing to replace an unowned/changed Omarchy Display plugin')
    private(DATA)
    private(CACHE)
    stage = DATA / ('install-' + str(time.time_ns()))
    private(stage)
    try:
        (stage / 'desktop').mkdir()
        # Deliberately enumerate public source: never copy checkout state or credentials.
        for name in ('display.py', 'bridge.cjs', 'jpeg-producer.cjs', 'jpeg-encoder.py',
                     'build-wayvnc.py', 'capture-tools.txt', 'package.json', 'keybindings.py', 'package-lock.json'):
            shutil.copyfile(ROOT / 'desktop' / name, stage / 'desktop' / name)
        (stage / 'apps/desktop-tv').mkdir(parents=True)
        for name in ('index.html', 'main.js', 'style.css', 'icon.png'):
            source = ROOT / 'apps/desktop-tv' / name
            shutil.copyfile(source, stage / 'apps/desktop-tv' / name)
        for name in ('install.py', 'install.sh', 'LICENSE', 'README.md', 'THIRD_PARTY_NOTICES.md'):
            if (ROOT / name).exists():
                shutil.copyfile(ROOT / name, stage / name)
        (stage / 'install.sh').chmod(0o700)
        env = dict(os.environ, npm_config_cache=str(CACHE / 'npm'))
        subprocess.run(['npm', 'ci', '--no-audit', '--no-fund'], cwd=stage / 'desktop', env=env, check=True)
        subprocess.run([sys.executable, str(stage / 'desktop/build-wayvnc.py'), '--dest', str(CACHE / 'capture')], check=True)
        (stage / 'capture').mkdir()
        shutil.copyfile(CACHE / 'capture/wayvnc-build/wayvnc', stage / 'capture/wayvnc')
        (stage / 'capture/wayvnc').chmod(0o700)
        shutil.copyfile(CACHE / 'capture/capture-build.json', stage / 'capture/capture-build.json')
        # Bytecode goes in the installed payload, never into the source checkout.
        subprocess.run([sys.executable, '-m', 'compileall', '-q', str(stage / 'desktop/display.py'),
                        str(stage / 'desktop/build-wayvnc.py'), str(stage / 'desktop/keybindings.py'),
                        str(stage / 'desktop/jpeg-encoder.py'), str(stage / 'install.py')], check=True)
        plugin_files = (previous or {}).get('pluginFiles', {})
        plugin_stage = stage / 'omarchy-plugin'
        shutil.copytree(ROOT / 'omarchy-plugin', plugin_stage)
        plugin_stage.chmod(0o700)
        for path in plugin_stage.rglob('*'):
            if path.is_symlink():
                raise RuntimeError('Plugin source may not contain symbolic links')
            path.chmod(0o700 if path.is_dir() else 0o600)
        if has_shell:
            plugin_files = inventory(plugin_stage)
        added, seeds = ((previous or {}).get('shellAdded', False), (previous or {}).get('shellSeeds', []))
        if previous:
            stop_installed()
        if has_shell:
            added, seeds = shell_registration(previous)
        if APP.exists():
            shutil.rmtree(APP)
        stage.replace(APP)
        if has_shell:
            if PLUGIN.exists():
                shutil.rmtree(PLUGIN)
            PLUGIN.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(APP / 'omarchy-plugin', PLUGIN)
        LAUNCHER.parent.mkdir(parents=True, exist_ok=True)
        launcher = ('#!/bin/sh\n' + MARKER + '\n' +
                    'if [ "${1-}" = uninstall ]; then\n  shift\n  exec ' + shlex.quote(sys.executable) + ' ' +
                    shlex.quote(str(APP / 'install.py')) + ' --uninstall "$@"\nfi\nexec ' +
                    shlex.quote(sys.executable) + ' ' + shlex.quote(str(APP / 'desktop/display.py')) + ' "$@"\n')
        temporary = LAUNCHER.with_name('.omarchy-display-install-' + str(time.time_ns()))
        temporary.write_text(launcher)
        temporary.chmod(0o700)
        temporary.replace(LAUNCHER)
        manifest = {'schemaVersion': 1, 'uid': os.getuid(), 'app': str(APP), 'launcher': str(LAUNCHER),
                    'plugin': str(PLUGIN), 'launcherSha256': hashlib.sha256(launcher.encode()).hexdigest(),
                    'appFiles': inventory(APP), 'pluginFiles': plugin_files, 'shellAdded': added, 'shellSeeds': seeds,
                    'keybindingsInstalled': (previous or {}).get('keybindingsInstalled', False)}
        save(MANIFEST, manifest)
        if has_shell:
            subprocess.run([sys.executable, str(APP / 'desktop/keybindings.py'), 'install'], check=True)
            manifest['keybindingsInstalled'] = True
            save(MANIFEST, manifest)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    print('Installed: ' + str(LAUNCHER))
    print('No autostart, OS packages or firewall rules were changed. Only owned keyboard shortcuts and shell entries were registered.')
    print('Next: omarchy-display init; omarchy-display menu')
    print('Uninstall: omarchy-display uninstall (add --purge-config to delete private pairing).')


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--uninstall', action='store_true')
    parser.add_argument('--purge-config', action='store_true', help='With --uninstall, also delete private pairing config')
    args = parser.parse_args()
    if args.purge_config and not args.uninstall:
        parser.error('--purge-config requires --uninstall')
    if os.geteuid() == 0:
        raise RuntimeError('Run the installer as your desktop user, never with sudo')
    private(STATE)
    lock = (STATE / 'install.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if args.uninstall:
        uninstall(args.purge_config)
    else:
        install()


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        print('omarchy-display installer: ' + str(error), file=sys.stderr)
        sys.exit(1)
