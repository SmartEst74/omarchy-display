#!/usr/bin/env python3
"""Own one C JPEG encoder; stream descriptors directly, without Python frame copies."""
import ctypes
import os
import select
import signal
import subprocess
import sys


def parent_death(parent):
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(1, signal.SIGKILL, 0, 0, 0) != 0 or os.getppid() != parent:
        os._exit(126)


def main():
    os.umask(0o077)
    parent_death(int(os.environ['OMARCHY_DISPLAY_PARENT_PID']))
    width, height, fps = map(int, sys.argv[1:])
    if not 320 <= width <= 8192 or not 320 <= height <= 8192 or width * height > 3840 * 2160 or fps not in (15, 30, 60):
        raise RuntimeError('Unsupported encoder dimensions or frame cap')
    os.fstat(3)  # The bridge owns this control descriptor, independent of raw input.
    parent = os.getpid()
    encoder = subprocess.Popen([
        'ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin',
        '-f', 'rawvideo', '-pixel_format', 'bgr0', '-video_size', '%dx%d' % (width, height),
        '-framerate', str(fps), '-probesize', '32', '-analyzeduration', '0', '-i', 'pipe:0',
        '-an', '-c:v', 'mjpeg', '-threads', '1', '-filter_threads', '1',
        '-q:v', '5', '-pix_fmt', 'yuvj420p', '-fps_mode', 'passthrough',
        '-f', 'image2pipe', '-flush_packets', '1', 'pipe:1'],
        stdin=sys.stdin.buffer, stdout=sys.stdout.buffer, stderr=sys.stderr,
        preexec_fn=lambda: parent_death(parent))
    descriptor = os.pidfd_open(encoder.pid)
    try:
        # No user command protocol: EOF on the bridge-owned descriptor means stop.
        while True:
            ready, _, _ = select.select([3, descriptor], [], [])
            if descriptor in ready:
                return encoder.wait()
            if 3 in ready:
                os.read(3, 1)
                signal.pidfd_send_signal(descriptor, signal.SIGTERM)
                try:
                    return encoder.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    signal.pidfd_send_signal(descriptor, signal.SIGKILL)
                    return encoder.wait()
    finally:
        os.close(descriptor)


if __name__ == '__main__':
    try:
        result = main()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        print('Native JPEG encoder: ' + str(error), file=sys.stderr)
        result = 1
    sys.exit(result if result >= 0 else 1)
