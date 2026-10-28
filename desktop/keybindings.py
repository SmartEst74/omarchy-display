#!/usr/bin/env python3
"""Own only ten conflict-checked current-mode TV workspace shortcuts."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import secrets
import shlex
import stat
import subprocess
import sys
import time

HOME = Path.home()
CONFIG = Path(os.environ.get('XDG_CONFIG_HOME', str(HOME / '.config')))
STATE = Path(os.environ.get('XDG_STATE_HOME', str(HOME / '.local/state'))) / 'omarchy-display'
BINDINGS = CONFIG / 'hypr/bindings.lua'
RECORD = STATE / 'keybindings.json'
BEGIN = '-- BEGIN smartest74.omarchy-display workspace shortcuts'
END = '-- END smartest74.omarchy-display workspace shortcuts'
MASK = 64 | 4 | 8  # SUPER, CTRL, ALT in Hyprland's modifier mask.
LEGACY_DESCRIPTIONS = {number: 'Omarchy Display: send workspace %d to TV' % number for number in range(1, 11)}
DESCRIPTIONS = {number: 'Omarchy Display: select workspace %d on TV (current mode)' % number for number in range(1, 11)}


def block(descriptions=DESCRIPTIONS):
    launcher = shlex.quote(str(HOME / '.local/bin/omarchy-display'))
    lines = [BEGIN]
    for number in range(1, 11):
        # Match Omarchy's physical number-row convention: code 19 is 0.
        lines.append('o.bind("SUPER + CTRL + ALT + code:%d", %s, %s)' %
                     (number + 9, json.dumps(descriptions[number]),
                      json.dumps(launcher + ' workspace ' + str(number), ensure_ascii=False)))
    lines.append(END)
    return ('\n' + '\n'.join(lines) + '\n').encode()


def private(path):
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    if path.is_symlink() or path.stat().st_uid != os.getuid():
        raise RuntimeError('Shortcut state must be owned by this user')
    path.chmod(0o700)


def read_bindings():
    if BINDINGS.is_symlink():
        raise RuntimeError('Refusing to replace a symlinked bindings.lua; manage shortcuts locally instead')
    if BINDINGS.exists() and (not BINDINGS.is_file() or BINDINGS.stat().st_uid != os.getuid()):
        raise RuntimeError('bindings.lua must be a regular file owned by this user')
    return BINDINGS.read_bytes() if BINDINGS.exists() else b''


def ownership():
    if not RECORD.exists():
        return None
    if RECORD.is_symlink() or RECORD.stat().st_uid != os.getuid() or RECORD.stat().st_mode & 0o077:
        raise RuntimeError('Shortcut ownership state must be private and owned by this user')
    value = json.loads(RECORD.read_text())
    if value.get('schemaVersion') != 1 or value.get('uid') != os.getuid() or \
            value.get('bindings') != str(BINDINGS) or \
            value.get('block') not in (block().decode(), block(LEGACY_DESCRIPTIONS).decode()):
        raise RuntimeError('Shortcut ownership record does not match this installation')
    return value


def hypr(*args):
    if not os.environ.get('HYPRLAND_INSTANCE_SIGNATURE'):
        raise RuntimeError('Shortcut registration requires the target Hyprland desktop session')
    result = subprocess.run(['hyprctl', *args], capture_output=True, text=True, timeout=10)
    if result.returncode:
        raise RuntimeError('Hyprland shortcut command failed; no raw IPC output is displayed')
    return result.stdout.strip()


def config_clean():
    if hypr('configerrors'):
        raise RuntimeError('Hyprland reports configuration errors; fix them locally before changing shortcuts')


def check():
    data = read_bindings()
    owned = ownership()
    managed = owned['block'].encode() if owned else block()
    descriptions = LEGACY_DESCRIPTIONS if managed == block(LEGACY_DESCRIPTIONS) else DESCRIPTIONS
    exact = bool(owned and data.count(managed) == 1 and data.count(BEGIN.encode()) == 1 and
                 data.count(END.encode()) == 1)
    if (BEGIN.encode() in data or END.encode() in data) and not exact:
        raise RuntimeError('An unowned or edited Display shortcut block exists; it was left untouched')
    config_clean()
    rows = json.loads(hypr('-j', 'binds'))
    if not isinstance(rows, list):
        raise RuntimeError('Unsupported Hyprland binding schema; no shortcuts changed')
    seen = set()
    for row in rows:
        if not isinstance(row, dict) or type(row.get('modmask')) is not int:
            raise RuntimeError('Unsupported Hyprland binding schema; no shortcuts changed')
        if row['modmask'] != MASK and not row.get('ignore_mods'):
            continue
        key = row.get('key', '')
        code = row.get('keycode', 0)
        if not isinstance(key, str) or type(code) is not int:
            raise RuntimeError('Unsupported numeric key representation; no shortcuts changed')
        number = None
        if key in tuple(str(number) for number in range(10)):
            number = int(key) or 10
        elif key.startswith('code:') and key[5:].isdigit() and 10 <= int(key[5:]) <= 19:
            number = int(key[5:]) - 9
        elif 10 <= code <= 19:
            number = code - 9
        unknown = not key and not code
        if number is None and not unknown and not row.get('catch_all'):
            continue
        description = row.get('description', '')
        ours = next((item for item, text in descriptions.items() if text == description), None)
        if exact and not row.get('catch_all') and not row.get('ignore_mods') and ours and \
                (number is None or number == ours) and ours not in seen:
            seen.add(ours)
            continue
        raise RuntimeError('SUPER+CTRL+ALT number-row shortcut conflict (including unknown/catch-all binds); no shortcuts changed')
    return data, owned


def replace(data, expected, mode):
    if read_bindings() != expected:
        raise RuntimeError('bindings.lua changed concurrently; no unrelated content was overwritten')
    BINDINGS.parent.mkdir(parents=True, exist_ok=True)
    temporary = BINDINGS.with_name('.bindings-display-' + secrets.token_hex(8))
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    try:
        with os.fdopen(descriptor, 'wb') as stream:
            os.fchmod(stream.fileno(), mode)
            stream.write(data)
        temporary.replace(BINDINGS)
    finally:
        temporary.unlink(missing_ok=True)


def reload():
    if hypr('reload') != 'ok':
        raise RuntimeError('Hyprland did not acknowledge the shortcut reload')
    config_clean()


def registered():
    rows = json.loads(hypr('-j', 'binds'))
    if not isinstance(rows, list):
        raise RuntimeError('Cannot verify the registered workspace shortcuts')
    descriptions = [row.get('description') for row in rows if isinstance(row, dict) and row.get('modmask') == MASK]
    if any(descriptions.count(description) != 1 for description in DESCRIPTIONS.values()):
        raise RuntimeError('Workspace shortcuts were not registered exactly once; check that Omarchy sources bindings.lua')


def apply(before, after, existed, mode, register=False):
    private(STATE / 'keybindings-backups')
    backup = STATE / 'keybindings-backups' / ('bindings-' + str(time.time_ns()) + '.lua')
    backup.write_bytes(before)
    backup.chmod(0o600)
    replace(after, before, mode)
    try:
        reload()
        if register:
            registered()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        if read_bindings() != after:
            raise RuntimeError('Reload failed and bindings.lua changed concurrently; private backup retained for local recovery') from error
        if existed:
            replace(before, after, mode)
        else:
            BINDINGS.unlink()
        try:
            reload()
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as rollback_error:
            raise RuntimeError('Prior bindings file restored, but rollback reload failed; inspect the private backup locally') from rollback_error
        raise RuntimeError('Shortcut reload failed; prior bindings and compositor configuration restored') from error


def install():
    before, owned = check()
    managed = block()
    if managed in before:
        reload()
        registered()
        print('Owned workspace shortcuts already installed.')
        return
    existed = BINDINGS.exists()
    mode = stat.S_IMODE(BINDINGS.stat().st_mode) if existed else 0o600
    previous = owned['block'].encode() if owned else None
    after = before.replace(previous, managed, 1) if previous and previous in before else before + managed
    apply(before, after, existed, mode, register=True)
    value = {'schemaVersion': 1, 'uid': os.getuid(), 'bindings': str(BINDINGS),
             'block': managed.decode(), 'bindingsExisted': owned['bindingsExisted'] if owned else existed}
    temporary = RECORD.with_name('keybindings-' + secrets.token_hex(8) + '.tmp')
    try:
        with temporary.open('x') as stream:
            json.dump(value, stream, indent=2)
            stream.write('\n')
        temporary.replace(RECORD)
    except OSError:
        # An unrecorded config edit is not an owned installation. Undo it.
        apply(after, before, True, mode)
        if not existed and not before:
            BINDINGS.unlink(missing_ok=True)
        raise
    finally:
        temporary.unlink(missing_ok=True)
    print('Installed SUPER+CTRL+ALT+1..9/0 current-mode workspace shortcuts (0 selects workspace 10).')


def uninstall():
    owned = ownership()
    if not owned:
        print('No owned workspace shortcuts to remove.')
        return
    before = read_bindings()
    managed = owned['block'].encode()
    if before.count(managed) != 1:
        if BEGIN.encode() in before or END.encode() in before:
            raise RuntimeError('Owned shortcut block was edited; no user configuration was removed')
        config_clean()
        reload()
        RECORD.unlink()
        print('Owned shortcut block was already removed; unrelated bindings retained.')
        return
    config_clean()
    mode = stat.S_IMODE(BINDINGS.stat().st_mode)
    after = before.replace(managed, b'', 1)
    apply(before, after, True, mode)
    if not after and not owned['bindingsExisted']:
        BINDINGS.unlink()
    RECORD.unlink()
    print('Removed only the exact owned workspace shortcut block; unrelated bindings retained.')


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('check', 'install', 'uninstall'))
    args = parser.parse_args()
    if args.command == 'check':
        check()
        print('Workspace shortcut conflict check passed; no configuration changed.')
        return
    private(STATE)
    with (STATE / 'keybindings.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        {'install': install, 'uninstall': uninstall}[args.command]()


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError) as error:
        print('omarchy-display shortcuts: ' + str(error), file=sys.stderr)
        sys.exit(1)
