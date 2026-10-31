#!/usr/bin/env python3
"""Build private output-locked Extend and direct DMA-BUF Mirror capture tools."""
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
REGIONS_PATCH_VERSION = 6
SUBPROJECTS = {
    'aml': '685035c9830aa89df02a43df89b644690bd885f5',  # v1.0.0
    'neatvnc': '783437adc56c2d7ce2cac1ec584acad798594267',  # v1.0.3
}
TOOL_VERSIONS = {'meson': '1.12.1', 'ninja': '1.13.0', 'cmake': '3.31.6'}
PKG_REQUIREMENTS = {
    'wayland-client': None, 'pixman-1': None, 'libdrm': None,
    'xkbcommon': '1.0.0', 'jansson': None, 'zlib': None, 'gbm': None,
    'egl': None, 'glesv2': None, 'libturbojpeg': None,
}


def checkout_dependency(name, revision):
    directory = SOURCE / 'subprojects' / name
    if directory.is_symlink():
        raise SystemExit('Private dependency source must not be a symlink: ' + name)
    if not directory.exists():
        directory.parent.mkdir(parents=True, exist_ok=True)
        run('git', 'clone', '--no-checkout', 'https://github.com/any1/' + name + '.git', str(directory))
        run('git', '-C', str(directory), 'fetch', 'origin', revision)
        run('git', '-C', str(directory), 'checkout', '--detach', revision)
    head = subprocess.check_output(['git', '-C', str(directory), 'rev-parse', 'HEAD'], text=True).strip()
    if head != revision:
        raise SystemExit('Private ' + name + ' checkout moved; refusing to overwrite it')
    return directory


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


def owned_receipt():
    """Only pinned, previously completed private builds can own cached edits."""
    path = DEST / 'capture-build.json'
    if not path.exists() and not path.is_symlink():
        return {}
    if path.is_symlink() or not path.is_file() or path.stat().st_uid != os.getuid():
        raise SystemExit('Capture receipt must be a regular owned file')
    try:
        receipt = json.loads(path.read_text())
    except (OSError, ValueError) as error:
        raise SystemExit('Cannot read capture ownership receipt: ' + str(error))
    if (not isinstance(receipt, dict) or receipt.get('commit') != COMMIT or
            receipt.get('subprojects') != SUBPROJECTS or
            receipt.get('output_lock_patch') != PATCH_VERSION):
        raise SystemExit('Capture ownership receipt has unpinned revisions or output-lock patches')
    legacy = receipt.get('native_gpu_patch') == 2 and 'regions_patch' not in receipt
    regions = receipt.get('regions_patch') in (1, 2, 3, 4, 5, REGIONS_PATCH_VERSION)
    if not legacy and not regions:
        raise SystemExit('Capture ownership receipt has an unknown native patch version')
    expected = {
        'wayvnc': {'src/main.c'},
        'aml': set(),
        'neatvnc': {'meson.build', 'src/enc/h264/ffmpeg-impl.c', 'src/server.c'},
    }
    if regions:
        expected['neatvnc'].update({
            'include/rfb-proto.h', 'src/enc/interface.c', 'src/enc/omarchy-regions.c',
        })
    if receipt.get('regions_patch') in (3, 4, 5, REGIONS_PATCH_VERSION):
        expected['wayvnc'].update({
            'meson.build', 'src/mirror-capture.c', 'include/mirror-wire.h',
        })
    if receipt.get('regions_patch') in (4, 5, REGIONS_PATCH_VERSION):
        expected['aml'].update({
            'include/aml.h', 'include/thread-pool.h', 'src/aml.c', 'src/thread-pool.c',
        })
    hashes = receipt.get('native_source_sha256')
    if not isinstance(hashes, dict) or set(hashes) != set(expected):
        raise SystemExit('Capture receipt is missing its native source ownership hashes')
    for project, names in expected.items():
        entries = hashes[project]
        if not isinstance(entries, dict) or set(entries) != names:
            raise SystemExit('Capture receipt has unknown or incomplete owned paths: ' + project)
        if any(not isinstance(value, str) or len(value) != 64 or
               any(char not in '0123456789abcdef' for char in value)
               for value in entries.values()):
            raise SystemExit('Capture receipt has invalid source hashes: ' + project)
    return hashes


def prepare_owned_patches(directory, revision, patches, receipt_hashes, new_files=None):
    """Validate every source first; defer all writes until all checkouts pass."""
    new_files = new_files or {}
    owned_paths = set(patches) | set(new_files)
    changed = subprocess.check_output(
        ['git', '-C', str(directory), 'diff', '--name-only', '-z', 'HEAD', '--']
    ).decode().split('\0')
    if any(name and name not in owned_paths for name in changed):
        raise SystemExit('Private ' + directory.name + ' source has unrelated edits; refusing unpinned source')
    if subprocess.check_output(
            ['git', '-C', str(directory), 'diff', '--summary', 'HEAD', '--']).strip():
        raise SystemExit('Private ' + directory.name + ' source has mode/path changes; refusing overwrite')
    if subprocess.run(['git', '-C', str(directory), 'diff', '--cached', '--quiet', '--']).returncode:
        raise SystemExit('Private ' + directory.name + ' source has staged edits; refusing overwrite')
    untracked = subprocess.check_output(
        ['git', '-C', str(directory), 'ls-files', '--others', '--exclude-standard', '-z']
    ).decode().split('\0')
    if any(name and name not in new_files for name in untracked):
        raise SystemExit('Private ' + directory.name + ' source has untracked files; refusing unpinned source')
    pending = []
    hashes = {}
    for name, desired in {**patches, **new_files}.items():
        path = directory / name
        if path.is_symlink() or (path.exists() and not path.is_file()):
            raise SystemExit('Private source file is not a regular file: ' + name)
        if any((directory / parent).is_symlink()
               for parent in Path(name).parents):
            raise SystemExit('Private source directory contains a symlink: ' + name)
        if name in patches:
            original = subprocess.check_output(
                ['git', '-C', str(directory), 'show', revision + ':' + name])
            if not path.is_file():
                raise SystemExit('Private source file is missing: ' + name)
        else:
            original = None
        current = path.read_bytes() if path.exists() else None
        previous_hash = receipt_hashes.get(name)
        if (current not in (original, desired) and
                (previous_hash is None or hashlib.sha256(current).hexdigest() != previous_hash)):
            raise SystemExit('Private source hash differs from its ownership receipt: ' + name)
        if current != desired:
            pending.append((path, desired))
        hashes[name] = hashlib.sha256(desired).hexdigest()
    return hashes, pending


def native_aml_patches(directory):
    """Build the private terminal worker barrier from immutable AML v1 objects."""
    revision = SUBPROJECTS['aml']

    def pristine(name):
        return subprocess.check_output(
            ['git', '-C', str(directory), 'show', revision + ':' + name], text=True)

    header = replace_once(pristine('include/aml.h'),
        'int aml_require_workers(struct aml*, int n);',
        'int aml_require_workers(struct aml*, int n);\n\n'
        '/* Private terminal barrier, called on the owner thread while it holds\n'
        ' * a loop reference. Joins all default workers, then cancels and retires\n'
        ' * all started/queued work refs on this thread, without callbacks.\n'
        ' * Idempotent. Rejects shared/custom pools; no new work after success. */\n'
        'int aml_stop_workers(struct aml*);')
    pool_header = replace_once(pristine('include/thread-pool.h'),
        'void thread_pool_release_default(struct aml*);',
        'void thread_pool_release_default(struct aml*);\n'
        'int thread_pool_stop_default(struct aml*);\n'
        'int thread_pool_resize_default(struct aml*, int n);')
    loop = replace_once(pristine('src/aml.c'),
        '\tbool have_thread_pool;', '\tbool have_thread_pool;\n\tbool workers_stopped;')
    loop = replace_once(loop, '#include <stdlib.h>',
        '#include <stdlib.h>\n#include <errno.h>')
    loop = replace_once(loop,
        'int aml_require_workers(struct aml* self, int n)\n{',
        'int aml_require_workers(struct aml* self, int n)\n{\n'
        '\tif (self->workers_stopped) {\n'
        '\t\terrno = ESHUTDOWN;\n\t\treturn -1;\n\t}\n')
    loop = replace_once(loop,
        '\tif (self->backend.thread_pool_acquire(self, n) < 0)',
        '\t/* Repeated worker requirements grow the same loop membership rather\n'
        '\t * than acquiring another process-global pool ownership reference. */\n'
        '\tif (self->have_thread_pool &&\n'
        '\t\t\tself->backend.thread_pool_acquire == thread_pool_acquire_default)\n'
        '\t\treturn thread_pool_resize_default(self, n);\n\n'
        '\tif (self->backend.thread_pool_acquire(self, n) < 0)')
    loop = replace_once(loop,
        'static int aml__start_work(struct aml* self, struct aml_work* work)\n{\n'
        '\treturn self->backend.thread_pool_enqueue(self, work);\n}',
        'static int aml__start_work(struct aml* self, struct aml_work* work)\n{\n'
        '\tif (!self->have_thread_pool || self->workers_stopped) {\n'
        '\t\terrno = ESHUTDOWN;\n\t\treturn -1;\n\t}\n'
        '\treturn self->backend.thread_pool_enqueue(self, work);\n}')
    loop = replace_once(loop,
        'static void aml__free(struct aml* self)\n{',
        'EXPORT\nint aml_stop_workers(struct aml* self)\n{\n'
        '\tif (self->workers_stopped)\n\t\treturn 0;\n'
        '\tif (self->have_thread_pool) {\n'
        '\t\tif (self->backend.thread_pool_release != thread_pool_release_default ||\n'
        '\t\t\t\tself->backend.thread_pool_enqueue != thread_pool_enqueue_default) {\n'
        '\t\t\terrno = ENOTSUP;\n\t\t\treturn -1;\n\t\t}\n'
        '\t\tif (thread_pool_stop_default(self) < 0)\n\t\t\treturn -1;\n'
        '\t\tself->have_thread_pool = false;\n\t}\n'
        '\tself->workers_stopped = true;\n'
        '\t/* Keep handlers and the loop alive for capture-resource cleanup.\n'
        '\t * Destructors may touch other objects, so restart each search. */\n'
        '\tstruct aml_obj* obj;\n'
        '\tfor (;;) {\n'
        '\t\tLIST_FOREACH(obj, &self->obj_list, link)\n'
        '\t\t\tif (obj->type == AML_OBJ_WORK)\n\t\t\t\tbreak;\n'
        '\t\tif (!obj)\n\t\t\tbreak;\n'
        '\t\taml_stop_obj(self, obj);\n\t}\n'
        '\tfor (;;) {\n'
        '\t\tpthread_mutex_lock(&self->event_queue_mutex);\n'
        '\t\tTAILQ_FOREACH(obj, &self->event_queue, event_link)\n'
        '\t\t\tif (obj->type == AML_OBJ_WORK)\n\t\t\t\tbreak;\n'
        '\t\tuint32_t refs = 0;\n'
        '\t\tif (obj) {\n'
        '\t\t\tTAILQ_REMOVE(&self->event_queue, obj, event_link);\n'
        '\t\t\trefs = obj->n_events;\n\t\t\tobj->n_events = 0;\n\t\t}\n'
        '\t\tpthread_mutex_unlock(&self->event_queue_mutex);\n'
        '\t\tif (!obj)\n\t\t\tbreak;\n'
        '\t\twhile (refs--)\n\t\t\taml_obj_unref(obj);\n\t}\n'
        '\treturn 0;\n}\n\n'
        'static void aml__free(struct aml* self)\n{')
    pool = replace_once(pristine('src/thread-pool.c'),
        '\tenqueue_work(NULL, NULL, 1);\n\n'
        '\tfor (int i = 0; i < n_threads; ++i)\n'
        '\t\tpthread_join(thread_pool[i], NULL);',
        '\t/* Allocation-free terminal marker: workers leave it queued so every\n'
        '\t * thread exits, after all work preceding it has completed. */\n'
        '\tstruct default_work stop = {0};\n'
        '\tpthread_mutex_lock(&work_queue_mutex);\n'
        '\tTAILQ_INSERT_TAIL(&default_work_queue, &stop, link);\n'
        '\tpthread_cond_broadcast(&work_queue_cond);\n'
        '\tpthread_mutex_unlock(&work_queue_mutex);\n\n'
        '\tfor (int i = 0; i < n_threads; ++i)\n'
        '\t\tif (pthread_join(thread_pool[i], NULL) != 0)\n\t\t\tabort();\n'
        '\tTAILQ_REMOVE(&default_work_queue, &stop, link);\n'
        '\tn_threads = 0;')
    pool = replace_once(pool,
        '\t\tif (work->work)\n\t\t\taml_unref(work->work);\n\t\tfree(work);',
        '\t\taml_weak_ref_del(work->aml_ref);\n'
        '\t\tif (work->work)\n\t\t\taml_unref(work->work);\n\t\tfree(work);')
    pool = replace_once(pool,
        '\tdefault_work->aml_ref = aml ? aml_weak_ref_new(aml) : NULL;',
        '\tdefault_work->aml_ref = aml ? aml_weak_ref_new(aml) : NULL;\n'
        '\tif (aml && !default_work->aml_ref) {\n'
        '\t\tif (work)\n\t\t\taml_unref(work);\n'
        '\t\tfree(default_work);\n\t\treturn -1;\n\t}')
    pool = replace_once(pool,
        'int thread_pool_acquire_default(struct aml* aml, int n)\n{',
        'int thread_pool_resize_default(struct aml* aml, int n)\n{')
    pool = replace_once(pool, '\t++n_thread_pool_users;\n\n\treturn rc;',
        '\treturn rc;')
    pool = replace_once(pool,
        'void thread_pool_release_default(struct aml* aml)\n{',
        'int thread_pool_acquire_default(struct aml* aml, int n)\n{\n'
        '\tint rc = thread_pool_resize_default(aml, n);\n'
        '\tif (rc == 0)\n\t\t++n_thread_pool_users;\n'
        '\treturn rc;\n}\n\n'
        'void thread_pool_release_default(struct aml* aml)\n{')
    pool += '''

/* This private helper owns the only loop using the process-global pool.
 * Never pretend that releasing one user's reference joins a shared pool. */
int thread_pool_stop_default(struct aml* aml)
{
\tif (n_thread_pool_users != 1) {
\t\terrno = EBUSY;
\t\treturn -1;
\t}
\tthread_pool_release_default(aml);
\treturn 0;
}
'''
    return {name: text.encode() for name, text in {
        'include/aml.h': header, 'include/thread-pool.h': pool_header,
        'src/aml.c': loop, 'src/thread-pool.c': pool,
    }.items()}


def native_regions_patches(directory):
    revision = SUBPROJECTS['neatvnc']

    def pristine(name):
        return subprocess.check_output(
            ['git', '-C', str(directory), 'show', revision + ':' + name], text=True)

    meson_build = replace_once(pristine('meson.build'),
        "libdrm = dependency('libdrm', required: get_option('h264'))",
        "libdrm = dependency('libdrm', required: true)\n"
        "egl = dependency('egl', required: true)\n"
        "glesv2 = dependency('glesv2', required: true)")
    meson_build = replace_once(meson_build, "\t\t'src/enc/interface.c',",
        "\t\t'src/enc/interface.c',\n\t\t'src/enc/omarchy-regions.c',")
    meson_build = replace_once(meson_build, '\tlibdrm_inc,\n',
        '\tlibdrm_inc,\n\tlibdrm,\n\tegl,\n\tglesv2,\n')
    meson_build = replace_once(meson_build,
        'have_ffmpeg = gbm.found() and libdrm.found() and libavcodec.found() and libavfilter.found() and libavutil.found()',
        'have_ffmpeg = false # Omarchy regions never use native video encoders')
    meson_build = replace_once(meson_build,
        "have_v4l2 = gbm.found() and libdrm.found() and cc.check_header('linux/videodev2.h')",
        'have_v4l2 = false # Omarchy regions require GBM and cropped JPEG only')

    protocol = replace_once(pristine('include/rfb-proto.h'),
        '\tRFB_ENCODING_OPEN_H264 = 50,',
        '\tRFB_ENCODING_OPEN_H264 = 50,\n'
        '\tRFB_ENCODING_OMARCHY_REGIONS = 0x4f445233,')
    interface = replace_once(pristine('src/enc/interface.c'),
        'struct encoder* raw_encoder_new(void);',
        'struct encoder* od_regions_new(uint16_t width, uint16_t height);\n'
        'struct encoder* raw_encoder_new(void);')
    interface = replace_once(interface,
        'extern struct encoder_impl encoder_impl_raw;',
        'extern struct encoder_impl encoder_impl_od_regions;\n'
        'extern struct encoder_impl encoder_impl_raw;')
    interface = replace_once(interface,
        '\tcase RFB_ENCODING_RAW: return raw_encoder_new();',
        '\tcase RFB_ENCODING_OMARCHY_REGIONS: return od_regions_new(width, height);\n'
        '\tcase RFB_ENCODING_RAW: return raw_encoder_new();')
    interface = replace_once(interface,
        '\tif (self->impl == &encoder_impl_raw)',
        '\tif (self->impl == &encoder_impl_od_regions)\n'
        '\t\treturn RFB_ENCODING_OMARCHY_REGIONS;\n'
        '\tif (self->impl == &encoder_impl_raw)')

    server = replace_once(pristine('src/server.c'),
        '\t\tcase RFB_ENCODING_OPEN_H264:\n\t\tcase RFB_ENCODING_CURSOR:',
        '\t\tcase RFB_ENCODING_OMARCHY_REGIONS:\n'
        '\t\tcase RFB_ENCODING_OPEN_H264:\n\t\tcase RFB_ENCODING_CURSOR:')
    server = replace_once(server,
        '\tcase RFB_ENCODING_OPEN_H264: return "open-h264";',
        '\tcase RFB_ENCODING_OMARCHY_REGIONS: return "omarchy-regions";\n'
        '\tcase RFB_ENCODING_OPEN_H264: return "open-h264";')
    server = replace_once(server,
        '\tfor (size_t i = 0; i < client->n_encodings; ++i) {\n'
        '\t\tswitch (client->encodings[i]) {',
        '\t/* An ODR3 client must never silently receive a Raw/JPEG fallback. */\n'
        '\tif (client_has_encoding(client, RFB_ENCODING_OMARCHY_REGIONS))\n'
        '\t\treturn RFB_ENCODING_OMARCHY_REGIONS;\n\n'
        '\tfor (size_t i = 0; i < client->n_encodings; ++i) {\n'
        '\t\tswitch (client->encodings[i]) {')
    # The software placeholder only advertises initial dimensions. Preserve the
    # request until the first DMA-BUF capture instead of choosing Raw.
    server = replace_once(server,
        '\tif (!have_display_buffers(server))\n\t\treturn;\n',
        '\tif (!have_display_buffers(server))\n\t\treturn;\n\n'
        '\tif (client_has_encoding(client, RFB_ENCODING_OMARCHY_REGIONS)) {\n'
        '\t\tfor (int i = 0; i < server->n_displays; ++i) {\n'
        '\t\t\tif (server->displays[i]->buffer->buffer->type == NVNC_BUFFER_GBM_BO)\n'
        '\t\t\t\tcontinue;\n'
        '\t\t\tif (client->encoder) {\n'
        '\t\t\t\tnvnc_log(NVNC_LOG_ERROR, "Omarchy regions lost DMA-BUF capture");\n'
        '\t\t\t\t_exit(EXIT_FAILURE);\n'
        '\t\t\t}\n'
        '\t\t\treturn;\n'
        '\t\t}\n'
        '\t}\n')
    server = replace_once(server,
        '\t\tnvnc_log(NVNC_LOG_ERROR, "Failed to set up encoder");\n',
        '\t\tnvnc_log(NVNC_LOG_ERROR, "Failed to set up encoder");\n'
        '\t\tif (client_has_encoding(client, RFB_ENCODING_OMARCHY_REGIONS))\n'
        '\t\t\t_exit(EXIT_FAILURE);\n')
    server = replace_once(server,
        '\t\tnvnc_log(NVNC_LOG_ERROR, "Failed to encode current frame");\n',
        '\t\tnvnc_log(NVNC_LOG_ERROR, "Failed to encode current frame");\n'
        '\t\tif (client_has_encoding(client, RFB_ENCODING_OMARCHY_REGIONS))\n'
        '\t\t\t_exit(EXIT_FAILURE);\n')
    # Both backends use private Unix sockets. Authenticate the peer before any
    # RFB handshake; TCP behavior is unchanged for the pinned upstream server.
    server = replace_once(server,
        '\tint one = 1;\n\tsetsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));',
        '\tstruct sockaddr_storage local_address;\n'
        '\tsocklen_t local_size = sizeof(local_address);\n'
        '\tif (getsockname(fd, (struct sockaddr*)&local_address, &local_size) < 0)\n'
        '\t\tgoto stream_failure;\n'
        '\tif (local_address.ss_family == AF_UNIX) {\n'
        '\t\tstruct ucred peer;\n'
        '\t\tsocklen_t peer_size = sizeof(peer);\n'
        '\t\tif (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) < 0 ||\n'
        '\t\t\t\tpeer_size != sizeof(peer) || peer.uid != getuid())\n'
        '\t\t\tgoto stream_failure;\n'
        '\t}\n\n'
        '\tint one = 1;\n\tsetsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));')
    # Only the standalone helper sets this process-local policy. Reject a
    # missing ODR3 negotiation before a Raw encoder/damage scan can be created.
    server = replace_once(server,
        '\tif (!client->continuous_updates_enabled &&\n'
        '\t    client->n_pending_requests == 0)\n\t\treturn;\n',
        '\tif (!client->continuous_updates_enabled &&\n'
        '\t    client->n_pending_requests == 0)\n\t\treturn;\n\n'
        '\tif (getenv("OMARCHY_TV_MIRROR_DMABUF") &&\n'
        '\t\t\t!client_has_encoding(client, RFB_ENCODING_OMARCHY_REGIONS)) {\n'
        '\t\tnvnc_log(NVNC_LOG_ERROR, "Direct GPU Mirror requires ODR3");\n'
        '\t\t_exit(EXIT_FAILURE);\n'
        '\t}\n')
    # n_damage_clients already excludes ENCODER_IMPL_FLAG_IGNORES_DAMAGE;
    # the custom encoder sets that flag and performs exact GPU comparison.
    return {name: text.encode() for name, text in {
        'meson.build': meson_build,
        'include/rfb-proto.h': protocol,
        'src/enc/interface.c': interface,
        'src/server.c': server,
        # Migrate the receipt-verified old owned GOP patch back to pinned source.
        'src/enc/h264/ffmpeg-impl.c': pristine('src/enc/h264/ffmpeg-impl.c'),
    }.items()}


os.umask(0o077)
DEST.mkdir(parents=True, exist_ok=True, mode=0o700)
if DEST.stat().st_uid != os.getuid() or DEST.is_symlink():
    raise SystemExit('Capture build directory must be owned by this user')
DEST.chmod(0o700)
previous_hashes = owned_receipt()
libraries = host_libraries()
meson, build_env = private_tools()
if SOURCE.is_symlink():
    raise SystemExit('Private capture source must not be a symlink')
if not SOURCE.exists():
    run('git', 'clone', '--no-checkout', 'https://github.com/any1/wayvnc.git', str(SOURCE))
    run('git', '-C', str(SOURCE), 'fetch', 'origin', COMMIT)
    run('git', '-C', str(SOURCE), 'checkout', '--detach', COMMIT)
# Never reset someone else's source work. This builder owns only its private
# checkout and writes a patched file from the immutable upstream git object.
head = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
if head != COMMIT:
    raise SystemExit('Private capture checkout moved; refusing to overwrite it')
source_hashes = {}
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
# Retire worker-owned frames while Wayland's capture pools, observers and GBM
# devices still exist. Joining alone leaves undelivered AML work/event refs.
patched = replace_once(patched,
    'static int init_nvnc(struct wayvnc* self)\n{',
    'static void stop_capture_workers(struct aml* loop)\n{\n'
    '\tif (aml_stop_workers(loop) < 0) {\n'
    '\t\tnvnc_log(NVNC_LOG_ERROR, "Cannot quiesce private capture workers");\n'
    '\t\t_exit(EXIT_FAILURE);\n\t}\n}\n\n'
    'static int init_nvnc(struct wayvnc* self)\n{')
patched = replace_once(patched,
    '\tnvnc_log(NVNC_LOG_INFO, "Exiting...");\n',
    '\tnvnc_log(NVNC_LOG_INFO, "Exiting...");\n'
    '\tstop_capture_workers(aml);\n')
for label, loop in (('auth_failure', 'aml_get_default()'),
                    ('nvnc_failure', 'aml'), ('ctl_server_failure', 'aml'),
                    ('rate_limiter_failure', 'aml')):
    patched = replace_once(patched, label + ':\n',
        label + ':\n\tstop_capture_workers(' + loop + ');\n')
mirror_sources = {}
for source, target in (('mirror-capture.c', 'src/mirror-capture.c'),
                       ('mirror-wire.h', 'include/mirror-wire.h')):
    path = ROOT / 'desktop' / source
    if path.is_symlink() or not path.is_file():
        raise SystemExit('Missing regular desktop/' + source + ' native mirror source')
    mirror_sources[target] = path.read_bytes()
wayvnc_meson = subprocess.check_output(
    ['git', '-C', str(SOURCE), 'show', COMMIT + ':meson.build'], text=True)
wayvnc_meson += '''

# Omarchy Mirror imports GPU buffers directly; it never connects to Wayland.
executable(
    'mirror-capture',
    'src/mirror-capture.c',
    dependencies: [aml, neatvnc, gbm, drm, pixman, dependency('threads')],
    include_directories: inc,
    install: false,
)
'''
source_hashes['wayvnc'], pending = prepare_owned_patches(
    SOURCE, COMMIT, {'src/main.c': patched.encode(), 'meson.build': wayvnc_meson.encode()},
    previous_hashes.get('wayvnc', {}), mirror_sources)
region_source = ROOT / 'desktop/region-encoder.c'
if region_source.is_symlink() or not region_source.is_file():
    raise SystemExit('Missing regular desktop/region-encoder.c native encoder source')
for name, revision in SUBPROJECTS.items():
    directory = checkout_dependency(name, revision)
    patches = native_regions_patches(directory) if name == 'neatvnc' else native_aml_patches(directory)
    new_files = ({'src/enc/omarchy-regions.c': region_source.read_bytes()}
                 if name == 'neatvnc' else {})
    source_hashes[name], changes = prepare_owned_patches(
        directory, revision, patches, previous_hashes.get(name, {}), new_files)
    pending.extend(changes)
# No checkout is mutated until every cached edit passes ownership validation.
for path, desired in pending:
    path.write_bytes(desired)
build = DEST / 'wayvnc-build'
args = meson + ['setup', str(build), str(SOURCE), '--buildtype=release',
        '--wrap-mode=nofallback', '-Ddefault_library=static', '-Dprefer_static=false',
        '-Dtests=false', '-Dpam=disabled', '-Dman-pages=disabled',
        '-Dscreencopy-dmabuf=enabled', '-Daml:examples=false',
        '-Dneatvnc:tests=false', '-Dneatvnc:examples=false', '-Dneatvnc:benchmarks=false',
        '-Dneatvnc:jpeg=enabled', '-Dneatvnc:gbm=enabled', '-Dneatvnc:h264=disabled',
        '-Dneatvnc:tls=disabled', '-Dneatvnc:nettle=disabled']
if (build / 'build.ninja').exists():
    args.append('--reconfigure')
subprocess.run(args, check=True, env=build_env)
subprocess.run(meson + ['compile', '-C', str(build)], check=True, env=build_env)
binary = build / 'wayvnc'
receipt = {'commit': COMMIT, 'output_lock_patch': PATCH_VERSION,
           'regions_patch': REGIONS_PATCH_VERSION,
           'native_source_sha256': source_hashes,
           'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
           'mirror_sha256': hashlib.sha256((build / 'mirror-capture').read_bytes()).hexdigest(),
           'subprojects': SUBPROJECTS, 'build_tools': TOOL_VERSIONS,
           'host_pkg_config': libraries, 'default_library': 'static',
           'features': {'capture': 'dmabuf', 'gpu_regions': True, 'mirror_dmabuf': True,
                        'gpu_jpeg_atlas': True,
                        'jpeg': True, 'h264': False, 'backend_tls': False, 'backend_crypto': False}}
(DEST / 'capture-build.json').write_text(json.dumps(receipt, indent=2) + '\n')
print('Private Extend and direct GPU Mirror capture build ready; no capture process started.')
