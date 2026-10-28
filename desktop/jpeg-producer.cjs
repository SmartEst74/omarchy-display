'use strict';
// One private Raw-only RFB client and one bounded, native JPEG encoding pipeline.
const net = require('node:net');
const path = require('node:path');
const {spawn} = require('node:child_process');

class Input {
  constructor(limit) { this.limit = limit; this.chunks = []; this.offset = 0; this.length = 0; }
  push(chunk) {
    if (this.length + chunk.length > this.limit) throw new Error('Capture input exceeded its frame budget');
    this.chunks.push(chunk); this.length += chunk.length;
  }
  copy(target, offset, length) {
    let remaining = length;
    while (remaining) {
      const chunk = this.chunks[0];
      const count = Math.min(remaining, chunk.length - this.offset);
      chunk.copy(target, offset, this.offset, this.offset + count);
      this.offset += count; offset += count; remaining -= count; this.length -= count;
      if (this.offset === chunk.length) { this.chunks.shift(); this.offset = 0; }
    }
  }
  read(length) {
    if (this.length < length) return null;
    const result = Buffer.allocUnsafe(length);
    this.copy(result, 0, length);
    return result;
  }
}

class JpegParser {
  constructor(limit, frame) {
    this.bytes = Buffer.allocUnsafe(limit); this.frame = frame;
    this.used = 0; this.position = 0; this.stage = 'start'; this.scanSeen = false;
  }
  push(chunk) {
    if (this.used + chunk.length > this.bytes.length) throw new Error('JPEG exceeded its frame budget');
    chunk.copy(this.bytes, this.used); this.used += chunk.length;
    const available = this.bytes.subarray(0, this.used);
    while (true) {
      if (this.stage === 'start') {
        if (this.used < 2) return;
        if (this.bytes.readUInt16BE(0) !== 0xffd8) throw new Error('Encoder did not produce JPEG');
        this.position = 2; this.stage = 'marker';
      } else if (this.stage === 'marker') {
        if (this.used - this.position < 2) return;
        if (this.bytes[this.position] !== 0xff) throw new Error('Invalid JPEG marker');
        if (this.bytes[this.position + 1] === 0xff) { this.position++; continue; }
        this.marker = this.bytes[this.position + 1]; this.position += 2;
        if (this.marker === 0xd9 || this.marker === 0xd8 || this.marker === 0 ||
            (this.marker >= 0xd0 && this.marker <= 0xd7)) throw new Error('Unexpected JPEG marker');
        this.stage = 'segment';
      } else if (this.stage === 'segment') {
        if (this.used - this.position < 2) return;
        const size = this.bytes.readUInt16BE(this.position);
        if (size < 2) throw new Error('Invalid JPEG segment');
        if (this.used - this.position < size) return;
        this.position += size;
        if (this.marker === 0xda) { this.scanSeen = true; this.stage = 'scan'; }
        else this.stage = 'marker';
      } else {
        const marker = available.indexOf(0xff, this.position);
        if (marker < 0) { this.position = this.used; return; }
        if (marker + 1 >= this.used) { this.position = marker; return; }
        const code = this.bytes[marker + 1];
        if (code === 0 || (code >= 0xd0 && code <= 0xd7)) { this.position = marker + 2; continue; }
        if (code === 0xff) { this.position = marker + 1; continue; }
        if (code !== 0xd9) { this.position = marker; this.stage = 'marker'; continue; }
        const end = marker + 2;
        if (!this.scanSeen || end !== this.used) throw new Error('Unexpected queued JPEG output');
        // The callback must copy this borrowed view before returning. Storage is
        // reused; no growing concat or duplicate full-JPEG allocation is needed.
        this.frame(this.bytes.subarray(0, end));
        this.used = 0; this.position = 0; this.stage = 'start'; this.scanSeen = false;
        return;
      }
    }
  }
}

class JpegProducer {
  constructor({runtime, width, height, fps, python, onFrame, onError}) {
    this.width = width; this.height = height; this.fps = fps;
    this.onFrame = onFrame; this.onError = onError;
    this.stopped = false; this.stage = 'version'; this.pending = false; this.first = true;
    this.input = new Input(width * height * 4 + 65536);
    this.pixels = Buffer.alloc(width * height * 4);
    this.jpeg = new JpegParser(Math.min(this.pixels.length + 65536, 16 * 1024 * 1024 - 20),
      (jpeg) => this.encoded(jpeg));
    this.lastRequest = 0; this.flight = null;
    this.socket = net.createConnection({path: path.join(runtime, 'capture.sock')});
    this.encoder = spawn(python, [path.join(__dirname, 'jpeg-encoder.py'), String(width), String(height), String(fps)], {
      env: {...process.env, OMARCHY_DISPLAY_PARENT_PID: String(process.pid)},
      stdio: ['pipe', 'pipe', 'pipe', 'pipe']
    });
    this.encoder.stderr.pipe(process.stderr, {end: false});
    this.encoder.on('error', (error) => this.fail(error));
    this.encoder.on('exit', () => this.fail(new Error('Native JPEG encoder exited')));
    this.encoder.stdin.on('error', (error) => this.fail(error));
    this.encoder.stdout.on('error', (error) => this.fail(error));
    this.encoder.stdout.on('data', (chunk) => {
      if (this.stopped) return;
      try { this.jpeg.push(chunk); } catch (error) { this.fail(error); }
    });
    this.closed = new Promise((resolve) => this.encoder.once('close', resolve));
    this.socket.on('data', (chunk) => {
      if (this.stopped) return;
      try { this.input.push(chunk); this.parse(); } catch (error) { this.fail(error); }
    });
    this.socket.on('error', (error) => this.fail(error));
    this.socket.on('close', () => this.fail(new Error('Private capture stream ended')));
    this.deadline = setTimeout(() => this.fail(new Error('Capture startup timed out')), 10000);
  }
  fail(error) {
    if (this.stopped) return;
    this.stop();
    this.onError(error);
  }
  stop() {
    if (this.stopped) return;
    this.stopped = true;
    clearTimeout(this.deadline); clearTimeout(this.next);
    this.socket.destroy();
    // Closing this inherited control descriptor asks the Python owner to signal
    // its exact encoder lifetime with a pidfd. Never signal a recyclable PID.
    this.encoder.stdio[3].destroy();
    this.encoder.stdin.destroy(); this.encoder.stdout.destroy();
    this.input.chunks = []; this.input.length = 0;
  }
  send(value) {
    this.socket.write(value, (error) => { if (error) this.fail(error); });
  }
  configure() {
    const format = Buffer.alloc(20);
    format[4] = 32; format[5] = 24; format[7] = 1;
    format.writeUInt16BE(255, 8); format.writeUInt16BE(255, 10); format.writeUInt16BE(255, 12);
    format[14] = 16; format[15] = 8;
    this.send(format);
    const encodings = Buffer.alloc(8); encodings[0] = 2; encodings.writeUInt16BE(1, 2);
    this.send(encodings); // The sole advertised encoding is Raw (signed int32 0).
    this.stage = 'message'; this.request();
  }
  request() {
    if (this.stopped || this.pending || this.flight) return;
    const delay = Math.max(0, 1000 / this.fps - (performance.now() - this.lastRequest));
    this.next = setTimeout(() => {
      if (this.stopped) return;
      const request = Buffer.alloc(10); request[0] = 3; request[1] = this.first ? 0 : 1;
      request.writeUInt16BE(this.width, 6); request.writeUInt16BE(this.height, 8);
      this.pending = true; this.lastRequest = performance.now();
      this.send(request);
    }, delay);
  }
  parse() {
    while (!this.stopped) {
      let value;
      if (this.stage === 'version') {
        if (!(value = this.input.read(12))) return;
        if (value.toString('ascii') !== 'RFB 003.008\n') throw new Error('Capture requires RFB 3.8');
        this.send(Buffer.from('RFB 003.008\n')); this.stage = 'security-count';
      } else if (this.stage === 'security-count') {
        if (!(value = this.input.read(1))) return;
        this.securityCount = value[0];
        if (!this.securityCount) throw new Error('Capture rejected private authentication');
        this.stage = 'security-types';
      } else if (this.stage === 'security-types') {
        if (!(value = this.input.read(this.securityCount))) return;
        if (!value.includes(1)) throw new Error('Private capture does not offer None authentication');
        this.send(Buffer.from([1])); this.stage = 'security-result';
      } else if (this.stage === 'security-result') {
        if (!(value = this.input.read(4))) return;
        if (value.readUInt32BE(0) !== 0) throw new Error('Private capture authentication failed');
        this.send(Buffer.from([1])); this.stage = 'init';
      } else if (this.stage === 'init') {
        if (!(value = this.input.read(24))) return;
        if (value.readUInt16BE(0) !== this.width || value.readUInt16BE(2) !== this.height)
          throw new Error('Capture dimensions differ from the owned output');
        this.nameLength = value.readUInt32BE(20);
        if (this.nameLength > 4096) throw new Error('Capture name exceeds its budget');
        this.stage = 'name';
      } else if (this.stage === 'name') {
        if (this.input.length < this.nameLength) return;
        this.input.read(this.nameLength); this.configure();
      } else if (this.stage === 'message') {
        if (!(value = this.input.read(1))) return;
        if (value[0] !== 0 || !this.pending || this.flight) throw new Error('Unexpected capture message');
        this.stage = 'update';
      } else if (this.stage === 'update') {
        if (!(value = this.input.read(3))) return;
        this.rectangles = value.readUInt16BE(1); this.rawBytes = 0;
        if (this.rectangles > 4096) throw new Error('Capture rectangle budget exceeded');
        if (!this.rectangles) { this.pending = false; this.stage = 'message'; this.request(); }
        else this.stage = 'rectangle';
      } else if (this.stage === 'rectangle') {
        if (!(value = this.input.read(12))) return;
        const x = value.readUInt16BE(0), y = value.readUInt16BE(2);
        const width = value.readUInt16BE(4), height = value.readUInt16BE(6);
        if (value.readInt32BE(8) !== 0 || !width || !height || x + width > this.width || y + height > this.height)
          throw new Error('Capture rectangle is not bounded Raw encoding');
        const length = width * height * 4;
        this.rawBytes += length;
        if (this.rawBytes > this.pixels.length) throw new Error('Raw update exceeded its frame budget');
        this.rectangle = {x, y, width, height, length, copied: 0}; this.stage = 'pixels';
      } else {
        const rect = this.rectangle;
        while (rect.copied < rect.length && this.input.length) {
          const rowBytes = rect.width * 4;
          const row = Math.floor(rect.copied / rowBytes), column = rect.copied % rowBytes;
          const count = Math.min(this.input.length, rowBytes - column, rect.length - rect.copied);
          this.input.copy(this.pixels, ((rect.y + row) * this.width + rect.x) * 4 + column, count);
          rect.copied += count;
        }
        if (rect.copied < rect.length) return;
        if (--this.rectangles) this.stage = 'rectangle';
        else {
          this.stage = 'message'; this.pending = false;
          if (this.input.length) throw new Error('Capture queued an unsolicited update');
          this.submit();
          return;
        }
      }
    }
  }
  submit() {
    if (this.flight) throw new Error('Encoder already has a pending raw frame');
    this.flight = {written: false, encoded: false};
    clearTimeout(this.deadline);
    this.deadline = setTimeout(() => this.fail(new Error('Native JPEG encoding timed out')), 10000);
    this.encoder.stdin.write(this.pixels, (error) => {
      if (this.stopped) return;
      if (error) { this.fail(error); return; }
      this.flight.written = true; this.advance();
    });
  }
  encoded(jpeg) {
    if (!this.flight || this.flight.encoded) throw new Error('Encoder produced an unsolicited frame');
    this.flight.encoded = true;
    this.onFrame(jpeg);
    this.advance();
  }
  advance() {
    if (this.stopped || !this.flight || !this.flight.written || !this.flight.encoded) return;
    clearTimeout(this.deadline);
    this.flight = null; this.first = false;
    this.request();
  }
}

module.exports = JpegProducer;
