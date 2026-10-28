#!/usr/bin/env python3
"""Own one temporary Hyprland output and its private, view-only capture path."""
import argparse
import ctypes
import fcntl
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
def xdg(name, fallback):
    return Path(os.environ.get(name, str(Path.home() / fallback))).expanduser()


CONFIG_DIR = xdg('XDG_CONFIG_HOME', '.config') / 'omarchy-display'
PRIVATE = xdg('XDG_STATE_HOME', '.local/state') / 'omarchy-display'
CONFIG = CONFIG_DIR / 'config.json'
STATE = PRIVATE / 'state.json'
CAPTURE = ROOT / 'capture/wayvnc'
COMMIT = 'bb837459c75fd3c267d48af170ac7ed2566f67e5'
MIRROR_COMMIT = 'efb50993780079460b0cbed1363e2166a2de1d9f'
MIRROR_NAME = 'omarchy-workspace-mirror'
MIRROR_LIBRARY = ROOT / 'mirror-plugin/libomarchy-workspace-mirror.so'


def configured_mode(value=None):
    if value is None and not CONFIG.exists():
        return 'mirror'
    mode = (config() if value is None else value).get('mode', 'extend')
    if mode not in ('mirror', 'extend'):
        raise RuntimeError('Display mode must be mirror or extend')
    return mode


def session_mode(state):
    # v1.1 sessions have no mode and really did transfer workspaces.
    mode = state.get('mode', 'extend')
    if mode not in ('mirror', 'extend'):
        raise RuntimeError('Invalid running display mode')
    return mode


def mode(selected):
    if selected not in ('mirror', 'extend'):
        raise RuntimeError('Choose mode mirror or extend')
    value = config()
    value['mode'] = selected
    save(CONFIG, value)
    print('Saved %s for the next start. The current display was not changed; explicitly stop, then start to apply.' %
          selected.capitalize())


def mirror_receipt(state=None):
    try:
        receipt = load(ROOT / 'mirror-plugin/mirror-build.json')
        valid = isinstance(receipt, dict) and not MIRROR_LIBRARY.is_symlink() and MIRROR_LIBRARY.is_file() and \
            MIRROR_LIBRARY.stat().st_uid == os.getuid() and not MIRROR_LIBRARY.stat().st_mode & 0o022 and \
            receipt.get('commit') == MIRROR_COMMIT and receipt.get('name') == MIRROR_NAME and \
            receipt.get('sha256') == hashlib.sha256(MIRROR_LIBRARY.read_bytes()).hexdigest()
    except (OSError, ValueError):
        valid = False
    if not valid:
        raise RuntimeError('Mirror unavailable: plugin is missing or not the verified owned build; rerun ./install.sh')
    version = hypr('version', state=state, json_output=True)
    if not isinstance(version, dict) or version.get('commit') != MIRROR_COMMIT or \
            version.get('version') != '0.56.2' or version.get('dirty') is not False:
        raise RuntimeError('Mirror requires the exact clean Hyprland 0.56.2 build ' + MIRROR_COMMIT)
    return receipt


def mirror_status(state):
    report = hypr('omarchy-display-mirror', state=state, json_output=True, timeout=2)
    if not isinstance(report, dict) or type(report.get('active')) is not bool or \
            not {'active', 'workspace', 'output', 'locked'} <= report.keys() or \
            type(report.get('locked')) is not bool or \
            not (report.get('workspace') is None or type(report.get('workspace')) is int and report['workspace'] > 0) or \
            not (report.get('output') is None or isinstance(report.get('output'), str)):
        raise RuntimeError('Mirror plugin status is unavailable or incompatible')
    if report['active'] and (report['workspace'] is None or report['output'] is None):
        raise RuntimeError('Mirror plugin reported an invalid active selection')
    return report


def require_mirror_health(state):
    report = mirror_status(state)
    if report['locked']:
        raise RuntimeError('Computer session locked; Mirror stopped. Unlock and start the display again.')
    if not report['active'] or report['output'] != state['output'] or \
            report['workspace'] != state.get('mirrorWorkspace'):
        raise RuntimeError('Mirror plugin lost the selected workspace; stream stopped')
    return report

def mirror_plugins(state):
    plugins = hypr('plugin', 'list', state=state, json_output=True)
    if not isinstance(plugins, list) or any(not isinstance(plugin, dict) for plugin in plugins):
        raise RuntimeError('Mirror plugin inventory is unavailable or incompatible')
    return [plugin for plugin in plugins if plugin.get('name') == MIRROR_NAME]



def load_mirror(state):
    receipt = mirror_receipt(state)
    if mirror_plugins(state):
        raise RuntimeError('Mirror plugin is already loaded outside this session; stop its owner before starting')
    # Persist ownership before IPC so cleanup can recover a partially completed load.
    state['mirrorPlugin'] = {'path': str(MIRROR_LIBRARY), 'sha256': receipt['sha256']}
    save(STATE, state)
    hypr('plugin', 'load', str(MIRROR_LIBRARY), state=state)
    report = mirror_status(state)
    if report['active']:
        raise RuntimeError('Newly loaded Mirror plugin unexpectedly has an active selection')


def clear_mirror(state):
    if not state.get('mirrorPlugin'):
        return
    if not mirror_plugins(state):
        return
    hypr('eval', 'assert(hl.plugin.omarchy_display.stop())', state=state)
    report = mirror_status(state)
    if report['active'] or report['workspace'] is not None or report['output'] is not None:
        raise RuntimeError('Mirror plugin did not clear its selection; stream closed for recovery')


def unload_mirror(state):
    owned = state.get('mirrorPlugin')
    if not owned:
        return
    if not mirror_plugins(state):
        return
    receipt = mirror_receipt(state)
    if owned != {'path': str(MIRROR_LIBRARY), 'sha256': receipt['sha256']}:
        raise RuntimeError('Mirror plugin ownership changed; refusing to unload any plugin')
    hypr('plugin', 'unload', owned['path'], state=state)


def private_directory(path):
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    if path.is_symlink() or path.stat().st_uid != os.getuid():
        raise RuntimeError('Private application directory must be owned by this user')
    path.chmod(0o700)


def save(path, value):
    temporary = path.with_name(path.name + '.' + secrets.token_hex(8) + '.tmp')
    with temporary.open('w') as output:
        os.chmod(temporary, 0o600)
        json.dump(value, output, indent=2)
        output.write('\n')
    temporary.replace(path)


def load(path):
    return json.loads(path.read_text())


def identity(pid):
    try:
        fields = Path('/proc/%d/stat' % pid).read_text().rsplit(')', 1)[1].split()
        if fields[0] in ('Z', 'X', 'x'):
            return None
        return {'pid': pid, 'start': fields[19]}
    except (FileNotFoundError, ProcessLookupError):
        return None


def alive(process):
    return process and identity(process['pid']) == process


def kill_owned(process, sig=signal.SIGTERM):
    if not process:
        return
    try:
        # Bind the signal to this process lifetime, not a recyclable PID.
        descriptor = os.pidfd_open(process['pid'], 0)
        try:
            if alive(process):
                signal.pidfd_send_signal(descriptor, sig)
        finally:
            os.close(descriptor)
    except ProcessLookupError:
        pass


def wait_owned(processes, timeout):
    deadline = time.monotonic() + timeout
    while any(alive(process) for process in processes):
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.1)
    return True


def hypr(*args, state=None, json_output=False, timeout=10):
    command = ['hyprctl']
    if state:
        command += ['-i', state['instance']]
    if json_output:
        command += ['-j']
    result = subprocess.run(command + list(args), capture_output=True, text=True, timeout=timeout)
    if result.returncode or (not json_output and result.stdout.strip() not in ('ok', '')):
        # Lua dispatcher result tables may contain diagnostics; fail rather than
        # claiming that a window move occurred. Never echo private raw IPC logs.
        raise RuntimeError('Hyprland command failed: ' + args[0] + '; inspect private session locally')
    return json.loads(result.stdout) if json_output else result.stdout


def monitors(state=None):
    return hypr('monitors', state=state, json_output=True)


def require_existing_geometry(before, current):
    fields = ('x', 'y', 'width', 'height', 'scale', 'transform', 'refreshRate', 'mirrorOf')
    by_name = {monitor['name']: monitor for monitor in current}
    for original in before:
        present = by_name.get(original['name'])
        if present is None or any(present.get(field) != original.get(field) for field in fields):
            raise RuntimeError('Existing monitor geometry changed; refusing capture and recovering the owned output')


PRESETS = {'720p': (1280, 720), '1080p': (1920, 1080)}


def valid_dimensions(width, height):
    return type(width) is int and type(height) is int and 320 <= width <= 8192 and 320 <= height <= 8192


def capture_size(value):
    selected = value.get('capture', {'preset': '1080p', 'width': 1920, 'height': 1080, 'fps': 30})
    width, height, fps = selected['width'], selected['height'], selected['fps']
    if not valid_dimensions(width, height) or width * height > 3840 * 2160 or \
            type(fps) is not int or fps not in (15, 30, 60) or selected.get('preset') not in (*PRESETS, 'native'):
        raise RuntimeError('Capture size must be a supported preset or bounded detected panel size at 15/30/60 fps')
    if selected['preset'] in PRESETS and (width, height) != PRESETS[selected['preset']]:
        raise RuntimeError('Capture dimensions do not match the selected preset')
    return {'preset': selected['preset'], 'width': width, 'height': height, 'fps': fps}


def receiver_info(value):
    if not isinstance(value, dict) or not valid_dimensions(value.get('viewportWidth'), value.get('viewportHeight')):
        return None
    panel_width, panel_height = value.get('panelWidth'), value.get('panelHeight')
    if (panel_width is not None or panel_height is not None) and not valid_dimensions(panel_width, panel_height):
        return None
    return {key: value.get(key) for key in ('viewportWidth', 'viewportHeight', 'panelWidth', 'panelHeight')}


def config():
    if CONFIG.is_symlink() or CONFIG.stat().st_uid != os.getuid() or CONFIG.stat().st_mode & 0o077:
        raise RuntimeError('Private config must be owned by this user with mode 0600')
    value = load(CONFIG)
    for key in ('listen', 'tv'):
        ip = ipaddress.ip_address(value[key])
        if ip.version != 4 or ip.is_unspecified or ip.is_multicast or not ip.is_private:
            raise RuntimeError('listen and tv must be explicit private IPv4 addresses')
    if not re.fullmatch('[0-9a-f]{32}', value['secret']):
        raise RuntimeError('Pairing secret must be 128 random bits (32 lowercase hex digits)')
    if type(value['port']) is not int or not 1024 <= value['port'] <= 65535:
        raise RuntimeError('Port must be an unprivileged TCP port')
    capture_size(value)
    configured_mode(value)
    return value


def panel_path(value):
    # Bind cached reports to the configured peer without persisting another
    # address or any device identifiers returned by Samsung's API.
    digest = hashlib.sha256(value['tv'].encode('ascii')).hexdigest()
    return PRIVATE / ('panel-' + digest + '.json')


def panel_report(value=None):
    try:
        path = panel_path(value or config())
        if path.is_symlink() or path.stat().st_uid != os.getuid() or path.stat().st_mode & 0o077:
            return None
        report = load(path)
        if set(report) != {'width', 'height', 'source'} or report['source'] != 'samsung-device-api' or \
                not valid_dimensions(report['width'], report['height']):
            return None
        return {key: report[key] for key in ('width', 'height', 'source')}
    except (OSError, ValueError, KeyError, TypeError, RuntimeError):
        return None


def probe_panel(value):
    def timed_out(_signum, _frame):
        raise TimeoutError('Device API deadline')

    # This synchronous CLI probe owns no other alarm. Enforce a total deadline,
    # including dribbled HTTP headers, rather than resetting a per-read timeout.
    started = time.monotonic()
    previous_handler = signal.signal(signal.SIGALRM, timed_out)
    previous_timer = signal.setitimer(signal.ITIMER_REAL, 2)
    connection = http.client.HTTPConnection(value['tv'], 8001, timeout=2)
    deadline = started + 2
    try:
        # Exact configured peer, fixed read-only path, no proxy, redirect,
        # discovery, credentials, device control, or persistence of raw JSON.
        connection.connect()
        socket = connection.sock
        connection.request('GET', '/api/v2/', headers={'Accept': 'application/json', 'Connection': 'close'})
        with connection.getresponse() as response:
            if response.status != 200:
                return None
            length = response.getheader('Content-Length')
            if length is not None and not 0 <= int(length) <= 65536:
                return None
            body = bytearray()
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                socket.settimeout(remaining)
                chunk = response.read1(min(8192, 65537 - len(body)))
                if not chunk:
                    break
                body.extend(chunk)
                if len(body) > 65536:
                    return None
                if response.isclosed():
                    break
        payload = json.loads(body)
        device = payload.get('device') if isinstance(payload, dict) else None
        resolution = device.get('resolution') if isinstance(device, dict) else None
        match = re.fullmatch(r'([0-9]{3,4})x([0-9]{3,4})', resolution) if isinstance(resolution, str) else None
        if not match:
            return None
        width, height = map(int, match.groups())
        if not valid_dimensions(width, height):
            return None
        return {'width': width, 'height': height, 'source': 'samsung-device-api'}
    except (OSError, ValueError, http.client.HTTPException):
        return None
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_handler)
        if previous_timer[0]:
            signal.setitimer(signal.ITIMER_REAL,
                             max(0.001, previous_timer[0] - (time.monotonic() - started)), previous_timer[1])
        connection.close()


def detect(quiet=False):
    value = config()
    report = probe_panel(value)
    if report:
        save(panel_path(value), report)
        if not quiet:
            print('Samsung device API reported %dx%d; this is not a physical panel measurement.' %
                  (report['width'], report['height']))
    elif not quiet:
        print('No new Samsung device API resolution was available.' +
              (' A previous verified report is retained.' if panel_report(value) else ' Native size is unknown.'))
    return report


def preflight():
    config()
    for program in ('hyprctl', 'node', 'ffmpeg'):
        if not shutil.which(program):
            raise RuntimeError('Missing dependency: ' + program)
    if not (ROOT / 'desktop/node_modules/ws/package.json').exists():
        raise RuntimeError('Installed WebSocket dependency is missing; rerun ./install.sh')
    for name in ('desktop/jpeg-producer.cjs', 'desktop/jpeg-encoder.py',
                 'apps/desktop-tv/index.html', 'apps/desktop-tv/main.js'):
        if not (ROOT / name).is_file():
            raise RuntimeError('Installed JPEG transport payload is incomplete; rerun ./install.sh')
    receipt = load(ROOT / 'capture/capture-build.json')
    if receipt.get('commit') != COMMIT or receipt.get('output_lock_patch') != 1 or \
            receipt.get('sha256') != hashlib.sha256(CAPTURE.read_bytes()).hexdigest():
        raise RuntimeError('Capture binary is not the pinned output-locked build')
    if not os.environ.get('HYPRLAND_INSTANCE_SIGNATURE') or not os.environ.get('WAYLAND_DISPLAY'):
        raise RuntimeError('Run inside the target Hyprland desktop session')


def init():
    if CONFIG.exists():
        raise RuntimeError('Config already exists; edit the private file locally or use rotate')
    value = {'listen': input('Host private IPv4 address (explicit interface): ').strip(),
             'tv': input('Authorized TV IPv4 address: ').strip(),
             'port': 6080, 'secret': secrets.token_hex(16), 'mode': 'mirror'}
    save(CONFIG, value)
    config()
    print('Private config created; use pair locally for the endpoint and pairing secret.')
    detect()


def child_setup(parent):
    # Children must not keep streaming after their supervisor dies (even SIGKILL).
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(1, signal.SIGTERM, 0, 0, 0) != 0:
        os._exit(126)
    if os.getppid() != parent:
        os._exit(126)


def spawn(args, logfile, env=None):
    parent = os.getpid()
    return subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=logfile,
                            stderr=subprocess.STDOUT, env=env,
                            preexec_fn=lambda: child_setup(parent))


def reserve_output(state, workspaces):
    token = state['session']
    if not re.fullmatch('[0-9a-f]{32}', token):
        raise RuntimeError('Invalid workspace reservation ownership')
    name = 'OMARCHY-DISPLAY-' + token
    existing = next((item for item in workspaces if item['name'] == name), None)
    if existing and existing['monitor'] != state['output']:
        raise RuntimeError('Reserved TV workspace was moved elsewhere; stop to recover without moving it')
    key = json.dumps('omarchy_display_' + token)
    hypr('eval', 'local rule=_G[%s]; if not rule or not rule:is_enabled() then '
         'rule=hl.workspace_rule({workspace=%s,monitor=%s,default=true,persistent=true}); '
         '_G[%s]=rule end; assert(rule and rule:is_enabled(),"TV workspace reservation failed")' %
         (key, json.dumps('name:' + name), json.dumps(state['output']), key), state=state)


def release_reservation(state):
    token = state['session']
    if not re.fullmatch('[0-9a-f]{32}', token):
        raise RuntimeError('Invalid workspace reservation ownership')
    key = json.dumps('omarchy_display_' + token)
    # The Lua API exposes disable, not deletion. Drop only our unique handle;
    # no workspace rule or Hyprland configuration is written to disk.
    hypr('eval', 'local rule=_G[%s]; if rule then rule:set_enabled(false); _G[%s]=nil end' %
         (key, key), state=state)


def cleanup(state):
    mirror_error = None
    try:
        original_live = any(item['instance'] == state['instance'] for item in hypr('instances', json_output=True))
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        original_live = False
        mirror_error = error
    if original_live and session_mode(state) == 'mirror':
        try:
            if os.environ.get('HYPRLAND_INSTANCE_SIGNATURE') != state['instance']:
                raise RuntimeError('Recover Mirror from the original Hyprland session')
            clear_mirror(state)
        except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
            mirror_error = error
    # Clear/blacken Mirror before closing capture. Even failed plugin IPC must
    # close the LAN bridge and capture; retain ownership state for recovery.
    processes = [state.get(key) for key in ('bridge', 'capture')]
    for process in processes:
        kill_owned(process)
    if not wait_owned(processes, 5):
        for process in processes:
            kill_owned(process, signal.SIGKILL)
        if not wait_owned(processes, 5):
            raise RuntimeError('Owned bridge/capture has not exited; recovery state retained')
    if mirror_error:
        raise RuntimeError('Mirror could not be cleared; streaming stopped and recovery state retained') from mirror_error
    if original_live and os.environ.get('HYPRLAND_INSTANCE_SIGNATURE') != state['instance']:
        raise RuntimeError('Recovery requires the original Hyprland session; no other output was touched')
    # An exited compositor cannot retain its virtual outputs. Never query/remove
    # an identically named output in the replacement desktop session.
    current = monitors(state) if original_live else []
    target = next((m for m in current if m['name'] == state['output']), None)
    if target:
        if state['output'] in [m['name'] for m in state['before']] or not re.fullmatch('OMARCHY-TV-[0-9a-f]{8}', state['output']):
            raise RuntimeError('Output ownership record is invalid; refusing removal')
        clients = hypr('clients', state=state, json_output=True)
        workspaces = hypr('workspaces', state=state, json_output=True)
        workspace_ids = {w['id'] for w in workspaces}
        for address, previous in (state.get('moves', {}) if session_mode(state) == 'extend' else {}).items():
            window = next((w for w in clients if w['address'] == address), None)
            if window and window['monitor'] == target['id'] and previous in workspace_ids:
                hypr('eval', 'hl.dispatch(hl.dsp.window.move({window="address:%s",workspace=%d,follow=false}))' %
                     (address, previous), state=state)
        for number, previous in (state.get('workspaceMoves', {}) if session_mode(state) == 'extend' else {}).items():
            workspace = next((w for w in workspaces if w['id'] == int(number)), None)
            if workspace and workspace['monitor'] == state['output']:
                if previous not in {monitor['name'] for monitor in current}:
                    raise RuntimeError('Original workspace monitor is unavailable; recovery state retained')
                dispatch_workspace(int(number), previous, state)
        release_reservation(state)
        hypr('output', 'remove', state['output'], state=state)
        deadline = time.monotonic() + 5
        while any(m['name'] == state['output'] for m in monitors(state)):
            if time.monotonic() >= deadline:
                raise RuntimeError('Owned output has not disappeared; recovery state retained')
            time.sleep(0.1)
    elif original_live:
        release_reservation(state)
    if original_live and session_mode(state) == 'mirror':
        unload_mirror(state)
    # Hyprland migrates any remaining windows/workspaces; never close user apps.
    for name in ('capture.sock', 'capture-control.sock', 'bridge-ready', 'bridge-status.json', 'wayvnc.conf'):
        (Path(state['runtime']) / name).unlink(missing_ok=True)
    try:
        Path(state['runtime']).rmdir()
    except OSError:
        pass
    STATE.unlink(missing_ok=True)


def serve():
    lock = (PRIVATE / 'supervisor.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if STATE.exists():
        raise RuntimeError('Recovery state exists; run stop before starting again')
    preflight()
    value = config()
    selected = capture_size(value)
    selected_mode = configured_mode(value)
    width, height, fps = selected['width'], selected['height'], selected['fps']
    before = monitors()
    if not before:
        raise RuntimeError('At least one existing desktop output is required')
    output = 'OMARCHY-TV-' + secrets.token_hex(4)
    # Put only our output to the left; never pin/reload any existing monitor.
    left = min(m['x'] for m in before) - width
    top = min(m['y'] for m in before)
    runtime_base = os.environ.get('XDG_RUNTIME_DIR')
    if not runtime_base:
        raise RuntimeError('XDG_RUNTIME_DIR is required for private capture sockets')
    runtime_parent = Path(runtime_base)
    if runtime_parent.stat().st_uid != os.getuid() or runtime_parent.stat().st_mode & 0o077:
        raise RuntimeError('XDG_RUNTIME_DIR must be private and owned by this user')
    runtime_root = runtime_parent / ('omarchy-display-' + secrets.token_hex(8))
    runtime_root.mkdir(mode=0o700)
    state = {'supervisor': identity(os.getpid()), 'instance': os.environ['HYPRLAND_INSTANCE_SIGNATURE'],
             'output': output, 'runtime': str(runtime_root), 'session': secrets.token_hex(16),
             'before': before, 'moves': {}, 'workspaceMoves': {}, 'captureSize': selected,
             'mode': selected_mode, 'mirrorWorkspace': None, 'ready': False}
    state['parkingWorkspace'] = 'OMARCHY-DISPLAY-' + state['session']
    save(STATE, state)
    stopped = False
    def stop_signal(_signum, _frame):
        nonlocal stopped
        stopped = True
    signal.signal(signal.SIGTERM, stop_signal)
    signal.signal(signal.SIGINT, stop_signal)
    try:
        if selected_mode == 'mirror':
            load_mirror(state)
        reserve_output(state, hypr('workspaces', state=state, json_output=True))
        hypr('eval', 'hl.monitor({output="%s",mode="%dx%d@%d",position="%dx%d",scale=1,mirror=""})' %
             (output, width, height, fps, left, top), state=state)
        hypr('output', 'create', 'headless', output, state=state)
        deadline = time.monotonic() + 10
        while True:
            current = monitors(state)
            target = next((m for m in current if m['name'] == output), None)
            if target and target['width'] == width and target['height'] == height and target['scale'] == 1 and \
                    target['x'] == left and target['y'] == top and target['mirrorOf'] == 'none' and \
                    target.get('activeWorkspace', {}).get('name') == state['parkingWorkspace']:
                require_existing_geometry(before, current)
                break
            if stopped or time.monotonic() > deadline:
                raise RuntimeError('New headless output did not reach the requested independent geometry')
            time.sleep(0.1)
        require_existing_geometry(before, monitors(state))
        computer = next((monitor for monitor in before if monitor.get('focused')), before[0])
        hypr('eval', 'hl.dispatch(hl.dsp.focus({monitor=%s}))' %
             json.dumps(computer['name'], ensure_ascii=False), state=state)
        if selected_mode == 'mirror':
            workspaces = hypr('workspaces', state=state, json_output=True)
            candidates = [item for item in workspaces if item['id'] > 0 and item['monitor'] in
                          {monitor['name'] for monitor in before}]
            initial = next((item for item in candidates
                            if item['id'] == computer.get('activeWorkspace', {}).get('id')), None)
            initial = initial or next(iter(candidates), None)
            if not initial:
                raise RuntimeError('Mirror needs an existing numbered workspace on the computer')
            select_mirror(state, initial['id'])
        env = dict(os.environ, OMARCHY_TV_OUTPUT=output)
        capture_config = runtime_root / 'wayvnc.conf'
        capture_config.write_text('enable_auth=false\n')
        os.chmod(capture_config, 0o600)
        with (PRIVATE / 'capture.log').open('w') as log:
            capture = spawn([str(CAPTURE), '--config', str(capture_config), '--disable-input',
                '--disable-resizing', '--render-cursor', '--max-fps=' + str(fps), '--output', output,
                '--socket', str(runtime_root / 'capture-control.sock'),
                '--unix-socket', str(runtime_root / 'capture.sock')], log, env)
        state['capture'] = identity(capture.pid)
        save(STATE, state)
        deadline = time.monotonic() + 10
        while not (runtime_root / 'capture.sock').exists():
            if stopped or capture.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Capture did not start; inspect the private capture.log locally')
            time.sleep(0.1)
        with (PRIVATE / 'bridge.log').open('w') as log:
            bridge_env = dict(os.environ, OMARCHY_DISPLAY_SESSION=state['session'],
                              OMARCHY_DISPLAY_CAPTURE=json.dumps(state['capture']),
                              OMARCHY_DISPLAY_CAPTURE_SIZE=json.dumps(selected),
                              OMARCHY_DISPLAY_PYTHON=sys.executable)
            bridge = spawn(['node', str(ROOT / 'desktop/bridge.cjs'), str(CONFIG), str(runtime_root)],
                           log, bridge_env)
        state['bridge'] = identity(bridge.pid)
        save(STATE, state)
        deadline = time.monotonic() + 10
        while not (runtime_root / 'bridge-ready').exists():
            if stopped or bridge.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Bridge did not start; inspect the private bridge.log locally')
            time.sleep(0.1)
        state['ready'] = True
        save(STATE, state)
        while not stopped:
            if capture.poll() is not None or bridge.poll() is not None:
                raise RuntimeError('Capture or bridge exited; stream stopped')
            current = next((m for m in monitors(state) if m['name'] == output), None)
            if not current or current['mirrorOf'] != 'none' or current['width'] != width or \
                    current['height'] != height or current['scale'] != 1:
                raise RuntimeError('Owned independent output disappeared, resized or became mirrored')
            if selected_mode == 'mirror':
                with action_lock():
                    latest = load(STATE)
                    require_mirror_health(latest)
            time.sleep(0.5)
    finally:
        with action_lock():
            # Commands persist only restoration maps while the supervisor runs.
            if STATE.exists():
                latest = load(STATE)
                state['moves'] = latest.get('moves', {})
                state['workspaceMoves'] = latest.get('workspaceMoves', {})
                state['mirrorWorkspace'] = latest.get('mirrorWorkspace')
            cleanup(state)
        (runtime_root / 'wayvnc.conf').unlink(missing_ok=True)
        try:
            runtime_root.rmdir()
        except OSError:
            pass


def start():
    preflight()
    if STATE.exists():
        raise RuntimeError('Desktop already active or recovery pending; use status / stop')
    if configured_mode() == 'mirror':
        mirror_receipt()
    detect(quiet=True)
    with (PRIVATE / 'supervisor.log').open('w') as log:
        daemon = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '_serve'],
             stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    daemon_identity = identity(daemon.pid)
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        if daemon.poll() is not None:
            raise RuntimeError('Desktop start failed; inspect the private supervisor.log locally')
        if STATE.exists():
            state = load(STATE)
            if state['supervisor']['pid'] == daemon.pid and state.get('ready'):
                selected = state['captureSize']
                print('%s %dx%d display prepared; frame cap %d fps. Use pair locally, workspace, status and stop.' %
                      (session_mode(state).capitalize(), selected['width'], selected['height'], selected['fps']))
                return
        time.sleep(0.1)
    kill_owned(daemon_identity)
    raise RuntimeError('Desktop start timed out; run stop to recover')


def stop():
    if not STATE.exists():
        print('Desktop is stopped; no output or process touched.')
        return
    state = load(STATE)
    if alive(state['supervisor']):
        kill_owned(state['supervisor'])
        # Cleanup may unlink STATE just before its supervisor exits. Waiting on
        # file existence misreported that successful shutdown as "still stopping".
        if not wait_owned([state['supervisor']], 20):
            kill_owned(state['supervisor'], signal.SIGKILL)
            if not wait_owned([state['supervisor']], 5):
                raise RuntimeError('Owned supervisor has not exited; no unrelated process was signalled')
    with action_lock():
        if STATE.exists():
            cleanup(load(STATE))
    print('Owned capture, bridge and headless output removed; existing monitors/apps preserved.')


def action_lock():
    lock = (PRIVATE / 'actions.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX)
    return lock


def move(address):
    state = load(STATE)
    if session_mode(state) != 'extend':
        raise RuntimeError('Moving windows is only available in Extend mode; Mirror never moves windows')
    if not alive(state['supervisor']) or not state.get('ready'):
        raise RuntimeError('Start the desktop before moving a window')
    if not re.fullmatch('0x[0-9a-fA-F]+', address):
        raise RuntimeError('Supply a window address from hyprctl clients -j, not a PID')
    client = next((w for w in hypr('clients', state=state, json_output=True) if w['address'] == address), None)
    if not client:
        raise RuntimeError('Selected window does not exist')
    state['moves'].setdefault(address, client['workspace']['id'])
    save(STATE, state)
    hypr('eval', 'hl.dispatch(hl.dsp.window.move({window="address:%s",monitor="%s",follow=false}))' %
         (address, state['output']), state=state)
    print('Selected real window moved to the independent TV output.')


def dispatch_workspace(number, monitor, state):
    hypr('eval', 'hl.dispatch(hl.dsp.workspace.move({workspace=%d,monitor=%s}))' %
         (number, json.dumps(monitor, ensure_ascii=False)), state=state)


def select_mirror(state, number):
    current = next((item for item in hypr('workspaces', state=state, json_output=True)
                    if item['id'] == number), None)
    if not current or number <= 0 or current['monitor'] == state['output'] or \
            current['monitor'] not in {monitor['name'] for monitor in state['before']}:
        raise RuntimeError('Mirror requires a numbered workspace on its original computer monitor')
    hypr('eval', 'assert(hl.plugin.omarchy_display.mirror(%s,%d,%d))' %
         (json.dumps(state['output']), number, state['captureSize']['fps']), state=state)
    state['mirrorWorkspace'] = number
    require_mirror_health(state)
    save(STATE, state)
    print('Workspace %d mirrored live on TV; its windows and computer workspace remain in place.' % number)


def workspace(number, recall=False, explicit_mode=None):
    if not re.fullmatch('[1-9][0-9]{0,8}', number or ''):
        raise RuntimeError('Workspace must be a positive numeric ID')
    number = int(number)
    state = load(STATE)
    if not alive(state['supervisor']) or not state.get('ready'):
        raise RuntimeError('Start the desktop before sending or recalling a workspace')
    active_mode = session_mode(state)
    if explicit_mode is not None and explicit_mode != active_mode:
        raise RuntimeError('This command requires running %s mode; save mode and explicitly stop/start first' %
                           explicit_mode.capitalize())
    if active_mode == 'mirror':
        if recall:
            raise RuntimeError('Recall is only available in Extend mode; mirrored workspaces were never moved')
        require_mirror_health(state)
        select_mirror(state, number)
        return
    workspaces = hypr('workspaces', state=state, json_output=True)
    current = next((item for item in workspaces if item['id'] == number), None)
    if not current:
        raise RuntimeError('The numbered workspace does not exist')
    records = state.setdefault('workspaceMoves', {})
    key = str(number)
    current_monitors = monitors(state)
    names = {monitor['name'] for monitor in current_monitors}
    target = next((monitor for monitor in current_monitors if monitor['name'] == state['output']), None)
    computer = next((monitor for monitor in current_monitors
                     if monitor.get('focused') and monitor['name'] != state['output']), None)
    computer = computer or next((monitor for monitor in current_monitors if monitor['name'] != state['output']), None)
    if not target or not computer:
        raise RuntimeError('The independent TV output and a computer monitor must remain available')
    commands = []
    if recall:
        if key not in records:
            raise RuntimeError('This session did not send that workspace')
        if current['monitor'] == state['output']:
            if records[key] not in names:
                raise RuntimeError('The original workspace monitor is no longer available')
            reserve_output(state, workspaces)
            if target.get('activeWorkspace', {}).get('id') == number:
                commands.append('hl.dispatch(hl.dsp.focus({workspace=%s,on_current_monitor=false}))' %
                                json.dumps('name:' + state['parkingWorkspace']))
            commands.append('hl.dispatch(hl.dsp.workspace.move({workspace=%d,monitor=%s}))' %
                            (number, json.dumps(records[key], ensure_ascii=False)))
    else:
        # At most one numbered workspace may reside on TV. Otherwise ordinary
        # computer shortcuts to a previously sent number could switch the TV.
        for item in workspaces:
            if item['id'] > 0 and item['monitor'] == state['output'] and str(item['id']) not in records:
                raise RuntimeError('An unrecorded numbered workspace is on TV; move it locally or stop to recover')
        returning = []
        for previous, origin in records.items():
            if previous == key:
                continue
            old = next((item for item in workspaces if item['id'] == int(previous)), None)
            if old and old['monitor'] == state['output']:
                if origin not in names:
                    raise RuntimeError('A previous workspace monitor is unavailable; no selection was changed')
                returning.append((int(previous), origin))
        reserve_output(state, workspaces)
        commands.append('hl.dispatch(hl.dsp.focus({workspace=%s,on_current_monitor=false}))' %
                        json.dumps('name:' + state['parkingWorkspace']))
        for previous, origin in returning:
            commands.append('hl.dispatch(hl.dsp.workspace.move({workspace=%d,monitor=%s}))' %
                            (previous, json.dumps(origin, ensure_ascii=False)))
        if current['monitor'] != state['output']:
            records[key] = current['monitor']
        # Persist all restoration records before dispatching. Partial IPC failure
        # can then recover every still-owned TV resident without touching others.
        save(STATE, state)
        if current['monitor'] != state['output']:
            commands.append('hl.dispatch(hl.dsp.workspace.move({workspace=%d,monitor=%s}))' %
                            (number, json.dumps(state['output'])))
        commands.append('hl.dispatch(hl.dsp.focus({workspace=%d,on_current_monitor=false}))' % number)
    if commands:
        previous_focus = computer.get('activeWorkspace', {}).get('id')
        if type(previous_focus) is int and previous_focus > 0 and (recall or previous_focus != number):
            commands.append('hl.dispatch(hl.dsp.focus({workspace=%d,on_current_monitor=false}))' % previous_focus)
        commands.append('hl.dispatch(hl.dsp.focus({monitor=%s}))' %
                        json.dumps(computer['name'], ensure_ascii=False))
        hypr('eval', '; '.join(commands), state=state)
    after = hypr('workspaces', state=state, json_output=True)
    if recall:
        if any(item['id'] == number and item['monitor'] == state['output'] for item in after):
            raise RuntimeError('Workspace recall did not complete; restoration record retained')
        del records[key]
        print('Workspace recalled; any workspace moved elsewhere by you was left untouched.')
    else:
        resident = [item['id'] for item in after if item['id'] > 0 and item['monitor'] == state['output']]
        if resident != [number] or tv_workspace(state) != number:
            raise RuntimeError('TV selection did not complete exclusively; restoration records retained')
        state['workspaceMoves'] = {key: records[key]}
        print('Workspace %d selected on TV; the previous selection returned and computer focus was preserved.' % number)
    save(STATE, state)


def workspace_summary(state=None):
    if not os.environ.get('HYPRLAND_INSTANCE_SIGNATURE'):
        return []
    if state and os.environ['HYPRLAND_INSTANCE_SIGNATURE'] != state['instance']:
        return []
    try:
        items = hypr('workspaces', state=state, json_output=True, timeout=1)
        return sorted([{'id': item['id'], 'windows': item['windows'],
                        'onTV': bool(state and item['monitor'] == state['output']),
                        'mirrored': bool(state and session_mode(state) == 'mirror' and
                                         state.get('mirrorWorkspace') == item['id']),
                        'recallAvailable': bool(state and session_mode(state) == 'extend' and
                                                str(item['id']) in state.get('workspaceMoves', {}))}
                       for item in items if type(item.get('id')) is int and item['id'] > 0 and
                       type(item.get('windows')) is int and item['windows'] >= 0], key=lambda item: item['id'])
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError):
        return []


def sizing_status(receiver=None, current=None):
    try:
        selected = capture_size(config()) if CONFIG.exists() else capture_size({})
    except (OSError, ValueError, KeyError, TypeError, RuntimeError):
        selected = capture_size({})
    panel = panel_report()
    native_width = panel['width'] if panel else receiver.get('panelWidth') if receiver else None
    native_height = panel['height'] if panel else receiver.get('panelHeight') if receiver else None
    native = valid_dimensions(native_width, native_height) and native_width * native_height <= 3840 * 2160
    return dict(selected, presets=list(PRESETS), nativeAvailable=bool(native),
                nativeWidth=native_width if native else None, nativeHeight=native_height if native else None,
                requiresRestart=bool(current and current != selected))


def size(preset, fps=None):
    value = config()
    if preset in PRESETS:
        width, height = PRESETS[preset]
    elif preset == 'native':
        detected = status()['sizing']
        if not detected['nativeAvailable']:
            raise RuntimeError('No supported native panel dimensions reported by a connected receiver')
        width, height = detected['nativeWidth'], detected['nativeHeight']
    else:
        raise RuntimeError('Choose size 720p, 1080p or native')
    value['capture'] = {'preset': preset, 'width': width, 'height': height,
                        'fps': fps if fps is not None else capture_size(value)['fps']}
    capture_size(value)
    save(CONFIG, value)
    print('Selected %dx%d with frame cap %d fps for the next start; an active display is unchanged.' %
          (width, height, value['capture']['fps']))


def tv_workspace(state):
    if not state or os.environ.get('HYPRLAND_INSTANCE_SIGNATURE') != state['instance']:
        return None
    try:
        if session_mode(state) == 'mirror':
            return require_mirror_health(state)['workspace']
        target = next((item for item in hypr('monitors', state=state, json_output=True, timeout=1)
                       if item['name'] == state['output']), None)
        number = target.get('activeWorkspace', {}).get('id') if target else None
        return number if type(number) is int and number > 0 else None
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError):
        return None


def status():
    result = {'schemaVersion': 1, 'state': 'stopped', 'running': False, 'clients': 0,
              'output': None, 'message': 'Display is stopped.', 'viewOnly': True, 'audio': False,
              'width': None, 'height': None, 'fps': None, 'workspaces': [], 'receiver': None,
              'sizing': sizing_status(), 'tvWorkspace': None, 'tvWorkspaceMirror': None, 'panel': panel_report(),
              'mode': None, 'configuredMode': configured_mode(), 'modeRequiresRestart': False}
    if not STATE.exists():
        result['workspaces'] = workspace_summary()
        return result
    try:
        state = load(STATE)
        running = bool(alive(state.get('supervisor')))
        result['running'] = running
        active_mode = session_mode(state)
        result.update(mode=active_mode, modeRequiresRestart=active_mode != result['configuredMode'])
        if active_mode == 'mirror' and state.get('ready'):
            result['tvWorkspaceMirror'] = require_mirror_health(state)['workspace']
            result['tvWorkspace'] = result['tvWorkspaceMirror']
        selected = capture_size({'capture': state['captureSize']})
        result.update(width=selected['width'], height=selected['height'], fps=selected['fps'],
                      workspaces=workspace_summary(state), sizing=sizing_status(current=selected),
                      tvWorkspace=result['tvWorkspaceMirror'] if active_mode == 'mirror' else tv_workspace(state))
        output = state.get('output', '')
        if re.fullmatch('OMARCHY-TV-[0-9a-f]{8}', output):
            result['output'] = output
        else:
            raise RuntimeError('Invalid owned output record')
        if not running:
            raise RuntimeError('Recovery required')
        if not state.get('ready'):
            result.update(state='starting', message='Preparing the independent display.')
            return result
        if not alive(state.get('capture')) or not alive(state.get('bridge')):
            raise RuntimeError('Backend exited')
        report = load(Path(state['runtime']) / 'bridge-status.json')
        age = time.monotonic() - report['updated']
        if report.get('session') != state['session'] or report.get('bridge') != state['bridge'] or \
                report.get('capture') != state['capture'] or not 0 <= age <= 3 or \
                type(report.get('clients')) is not int or not 0 <= report['clients'] <= 2 or \
                type(report.get('error')) is not bool:
            raise RuntimeError('Stale backend report')
        if report['error']:
            raise RuntimeError('JPEG producer failed')
        clients = report['clients']
        receiver = receiver_info(report.get('receiver')) if clients else None
        if receiver and result['panel']:
            receiver.update(panelWidth=result['panel']['width'], panelHeight=result['panel']['height'])
        result.update(receiver=receiver, sizing=sizing_status(receiver, selected))
        result.update(state='sharing' if clients else 'ready', clients=clients,
                      message='Sharing the independent display.' if clients else 'Ready; no viewers connected.')
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError):
        result.update(state='error', clients=0, message='Display needs attention; run stop to recover.')
    return result


def show_status(as_json=False):
    value = status()
    if as_json:
        print(json.dumps(value, separators=(',', ':')))
    else:
        print(value['state'] + ': ' + value['message'])
        print('Mode: %s; next start: %s%s' %
              ((value['mode'] or 'stopped').capitalize(), value['configuredMode'].capitalize(),
               ' (explicit stop/start required)' if value['modeRequiresRestart'] else ''))
        if value['output']:
            print('Output: ' + value['output'] + '; viewers: ' + str(value['clients']))
        if value['width']:
            print('Capture: %dx%d; frame cap %d fps' % (value['width'], value['height'], value['fps']))
        print('Displayed TV workspace: ' + str(value['tvWorkspace'] or 'none'))
        for item in value['workspaces']:
            print('Workspace %d: %d windows%s' %
                  (item['id'], item['windows'], ' (mirrored live)' if item['mirrored'] else
                   ' (on TV)' if item['onTV'] else ''))
        selected = value['sizing']
        print('Next start: %dx%d; frame cap %d fps (%s)' %
              (selected['width'], selected['height'], selected['fps'], selected['preset']))
        if value['panel']:
            print('Samsung device API reported: %dx%d (not physically measured)' %
                  (value['panel']['width'], value['panel']['height']))
        if value['receiver']:
            receiver = value['receiver']
            print('Receiver viewport: %dx%d; panel: %s' %
                  (receiver['viewportWidth'], receiver['viewportHeight'],
                   ('%dx%d' % (receiver['panelWidth'], receiver['panelHeight'])) if receiver['panelWidth'] else 'not reported'))


def pair():
    value = config()
    print(('wss' if value.get('tls') else 'ws') + '://' + value['listen'] + ':' + str(value['port']) + '/desktop')
    print('Pairing secret (private; do not paste into chat): ' + value['secret'])


def menu_window():
    # The floating terminal steals focus; exclude its process ancestry and
    # retain the previously focused real window for the lifetime of this menu.
    ancestors = set()
    pid = os.getpid()
    while pid > 1 and pid not in ancestors:
        ancestors.add(pid)
        try:
            pid = int(Path('/proc/%d/stat' % pid).read_text().rsplit(')', 1)[1].split()[1])
        except (OSError, ValueError):
            break
    clients = hypr('clients', json_output=True)
    candidates = [client for client in clients if client.get('pid') not in ancestors and
                  client.get('focusHistoryID', -1) >= 0]
    return min(candidates, key=lambda client: client['focusHistoryID'])['address'] if candidates else None


def menu(address=None):
    if not sys.stdin.isatty():
        raise RuntimeError('menu requires an interactive terminal')
    address = address or menu_window()
    while True:
        print('\nOmarchy Display — view only, no audio')
        with action_lock():
            show_status()
        print('\n1  Start display\n2  Stop display\n3  Show private pairing details'
              '\n4  Move previously focused window (Extend only)\n5  Status\n6  Select capture size for next start'
              '\n7  Select numbered workspace (current mode)\n8  Recall numbered workspace (Extend only)'
              '\n9  Save Mirror/Extend mode for next start\nq  Close')
        choice = input('Choose: ').strip().lower()
        if choice in ('q', ''):
            return
        try:
            if choice == '1':
                start()
            elif choice == '2':
                stop()
            elif choice == '3':
                pair()
                input('Press Enter to hide pairing details.')
                print('\033[2J\033[H', end='', flush=True)
            elif choice == '4':
                if not address:
                    raise RuntimeError('No previously focused window was found')
                with action_lock():
                    move(address)
            elif choice == '5':
                show_status()
            elif choice == '6':
                preset = input('Preset (720p, 1080p, native): ').strip()
                frame_rate = input('FPS (15, 30, 60; Enter keeps current): ').strip()
                size(preset, int(frame_rate) if frame_rate else None)
            elif choice in ('7', '8'):
                number = input('Workspace number: ').strip()
                with action_lock():
                    workspace(number, recall=choice == '8')
            elif choice == '9':
                mode(input('Next-start mode (mirror, extend): ').strip().lower())
        except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError):
            print('Action failed. Check private configuration/session logs locally.')


def main():
    os.umask(0o077)
    private_directory(PRIVATE)
    private_directory(CONFIG_DIR)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['init', 'pair', 'rotate', 'start', 'stop', 'status', 'move',
                                          'workspace', 'mirror', 'extend', 'mode', 'recall', 'menu', 'size', 'detect', '_serve'])
    parser.add_argument('address', nargs='?')
    parser.add_argument('--json', action='store_true', help='Machine-readable status, never pairing details')
    parser.add_argument('--window', help='Window address captured before opening the menu')
    parser.add_argument('--fps', type=int, choices=(15, 30, 60), help='Capture frame cap for size selection')
    args = parser.parse_args()
    if args.json and args.command != 'status':
        parser.error('--json is only supported by status')
    if args.fps is not None and args.command != 'size':
        parser.error('--fps is only supported by size')
    if args.command == 'init':
        init()
    elif args.command == 'detect':
        detect()
    elif args.command == 'pair':
        pair()
    elif args.command == 'rotate':
        if STATE.exists():
            raise RuntimeError('Stop the desktop before rotating its secret')
        value = config()
        value['secret'] = secrets.token_hex(16)
        save(CONFIG, value)
        print('Pairing secret rotated; enter the new secret on the receiver.')
    elif args.command == 'status':
        with action_lock():
            show_status(args.json)
    elif args.command == 'menu':
        menu(args.window)
    elif args.command == 'size':
        size(args.address, args.fps)
    elif args.command == 'mode':
        with action_lock():
            mode(args.address)
    elif args.command == 'move':
        if not args.address:
            raise RuntimeError('move requires a real window address from hyprctl clients -j')
        with action_lock():
            move(args.address)
    elif args.command in ('workspace', 'mirror', 'extend', 'recall'):
        with action_lock():
            workspace(args.address, recall=args.command == 'recall',
                      explicit_mode=args.command if args.command in ('mirror', 'extend') else None)
    else:
        {'start': start, 'stop': stop, '_serve': serve}[args.command]()


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
