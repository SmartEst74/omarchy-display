'use strict';
// Pull one GPU-differenced update only after the viewer presents the previous one.
const net = require('node:net');
const path = require('node:path');
const ENCODING = 0x4f445233;
const MAX_BATCH = 32 * 1024 * 1024;

function jpegDimensions(bytes, width, height) {
  if (bytes.length < 4 || bytes.readUInt16BE(0) !== 0xffd8 ||
      bytes.readUInt16BE(bytes.length - 2) !== 0xffd9) return false;
  let offset = 2;
  while (offset < bytes.length - 2) {
    if (bytes[offset++] !== 255) return false;
    while (offset < bytes.length && bytes[offset] === 255) offset++;
    if (offset >= bytes.length) return false;
    const marker = bytes[offset++];
    if (marker === 218 || marker === 217 || marker === 0 || marker === 216) return false;
    if (marker === 1 || (marker >= 208 && marker <= 215)) continue;
    if (offset + 2 > bytes.length) return false;
    const length = bytes.readUInt16BE(offset);
    if (length < 2 || offset + length > bytes.length) return false;
    if (marker === 192 || marker === 193 || marker === 194) {
      return length >= 8 && bytes[offset + 2] === 8 &&
        bytes.readUInt16BE(offset + 3) === height && bytes.readUInt16BE(offset + 5) === width &&
        (bytes[offset + 7] === 1 || bytes[offset + 7] === 3) && length === 8 + 3 * bytes[offset + 7];
    }
    offset += length;
  }
  return false;
}

function overlaps(a, b) {
  return a.x < b.x + b.width && b.x < a.x + a.width &&
    a.y < b.y + b.height && b.y < a.y + a.height;
}

class Input {
  constructor(limit) { this.limit = limit; this.chunks = []; this.offset = 0; this.length = 0; }
  push(chunk) {
    if (this.length + chunk.length > this.limit) throw new Error('Region capture exceeded its bounded packet budget');
    this.chunks.push(chunk); this.length += chunk.length;
  }
  read(length) {
    if (this.length < length) return null;
    if (!length) return Buffer.alloc(0);
    const first = this.chunks[0];
    if (first.length - this.offset >= length) {
      const value = first.subarray(this.offset, this.offset + length);
      this.offset += length; this.length -= length;
      if (this.offset === first.length) { this.chunks.shift(); this.offset = 0; }
      return value;
    }
    const value = Buffer.allocUnsafe(length);
    let written = 0;
    while (written < length) {
      const chunk = this.chunks[0];
      const count = Math.min(length - written, chunk.length - this.offset);
      chunk.copy(value, written, this.offset, this.offset + count);
      written += count; this.offset += count; this.length -= count;
      if (this.offset === chunk.length) { this.chunks.shift(); this.offset = 0; }
    }
    return value;
  }
}

class RegionProducer {
  constructor({runtime, mirror, width, height, fps, onFrame, onError}) {
    this.width = width; this.height = height; this.fps = fps;
    this.mirror = mirror;
    this.onFrame = onFrame; this.onError = onError;
    this.stage = 'version'; this.stopped = false; this.ready = false;
    this.started = false; this.pending = false; this.awaitingAck = false; this.lastRequest = 0;
    this.input = new Input(MAX_BATCH + 65536);
    this.socket = net.createConnection({path: path.join(runtime, 'capture.sock')});
    this.closed = new Promise(resolve => this.socket.once('close', resolve));
    this.socket.on('error', error => this.fail(error));
    this.socket.on('close', () => this.fail(new Error('Private GPU region capture ended')));
    this.socket.on('data', chunk => {
      if (this.stopped) return;
      try { this.input.push(chunk); this.parse(); } catch (error) { this.fail(error); }
    });
    this.deadline = setTimeout(() => this.fail(new Error('GPU region capture startup timed out')), 10000);
  }
  fail(error) {
    if (this.stopped) return;
    this.stop(); this.onError(error);
  }
  stop() {
    if (this.stopped) return;
    this.stopped = true;
    clearTimeout(this.deadline); clearTimeout(this.next);
    if (this.control) this.control.destroy();
    this.socket.destroy(); this.input.chunks = []; this.input.length = 0;
    this.regions = this.sourceBoxes = null;
  }
  send(value) { this.socket.write(value, error => { if (error) this.fail(error); }); }
  configure() {
    const encodings = Buffer.alloc(8);
    encodings[0] = 2; encodings.writeUInt16BE(1, 2); encodings.writeInt32BE(ENCODING, 4);
    this.send(encodings);
    this.ready = true; this.stage = 'message'; this.request();
  }
  refreshMirror(done) {
    const control = this.control = net.createConnection({path: this.mirror.socket});
    let reply = '', finished = false;
    control.setEncoding('utf8');
    control.setTimeout(2000, () => control.destroy(new Error('Mirror capture demand timed out')));
    control.once('connect', () => control.write('j/omarchy-display-frame'));
    control.on('data', chunk => {
      reply += chunk;
      if (reply.length > 4096) control.destroy(new Error('Mirror capture reply exceeded its budget'));
    });
    control.once('error', error => this.fail(error));
    control.once('end', () => {
      finished = true;
      if (this.control === control) this.control = null;
      if (this.stopped) return;
      try {
        const state = JSON.parse(reply);
        if (state.active !== true || state.locked !== false || state.source !== this.mirror.source ||
            state.width !== this.width || state.height !== this.height ||
            !Number.isSafeInteger(state.minimumPtsUs) || state.minimumPtsUs < 1)
          throw new Error('The selected offscreen Mirror source is no longer available');
        this.waitMirrorFrame(state.minimumPtsUs, done);
      } catch (error) { this.fail(error); }
    });
    control.once('close', () => {
      if (!finished && !this.stopped) this.fail(new Error('Mirror capture demand closed without a reply'));
    });
  }
  waitMirrorFrame(minimumPtsUs, done) {
    const control = this.control = net.createConnection({path: this.mirror.control});
    let received = false, finished = false;
    control.setTimeout(2000, () => control.destroy(new Error('Fresh GPU mirror frame timed out')));
    control.once('connect', () => {
      const request = Buffer.allocUnsafe(8);
      request.writeBigUInt64BE(BigInt(minimumPtsUs));
      control.write(request);
    });
    control.on('data', chunk => {
      if (received || chunk.length !== 1 || chunk[0] !== 1)
        control.destroy(new Error('Invalid native mirror frame acknowledgement'));
      else received = true;
    });
    control.once('error', error => this.fail(error));
    control.once('end', () => {
      finished = true;
      if (this.control === control) this.control = null;
      if (this.stopped) return;
      if (!received) { this.fail(new Error('Native mirror frame was not installed')); return; }
      done();
    });
    control.once('close', () => {
      if (!finished && !this.stopped) this.fail(new Error('Native mirror readiness connection ended'));
    });
  }
  request() {
    if (this.stopped || !this.ready || this.pending || this.awaitingAck || this.next) return;
    const delay = Math.max(0, 1000 / this.fps - (performance.now() - this.lastRequest));
    this.next = setTimeout(() => {
      this.next = null;
      if (this.stopped || this.pending || this.awaitingAck) return;
      const request = Buffer.alloc(10);
      request[0] = 3; request[1] = this.started ? 1 : 0;
      request.writeUInt16BE(this.width, 6); request.writeUInt16BE(this.height, 8);
      this.fullRequest = !this.started;
      this.pending = true; this.lastRequest = performance.now();
      if (this.mirror) this.refreshMirror(() => this.send(request));
      else this.send(request);
    }, delay);
  }
  acknowledge() {
    if (this.stopped || !this.awaitingAck) return;
    this.awaitingAck = false; this.request();
  }
  addSource(box) {
    if (!box.width || !box.height || box.x + box.width > this.width || box.y + box.height > this.height ||
        this.sourceBoxes.length >= 64 || this.sourceBoxes.some(other => overlaps(box, other)))
      throw new Error('GPU source regions exceed their bounds, count or non-overlap budget');
    this.sourceBoxes.push(box);
    this.area += box.width * box.height;
  }
  validateRegion(data) {
    const region = this.rectangle;
    let jpeg = data, jpegWidth = region.width, jpegHeight = region.height;
    if (region.kind === 2) {
      const atlasWidth = data.readUInt16BE(0), atlasHeight = data.readUInt16BE(2);
      const count = data.readUInt16BE(4), header = 8 + count * 12;
      if (!atlasWidth || !atlasHeight || atlasWidth > Math.min(4096, Math.ceil(this.width / 8) * 8) ||
          atlasHeight > Math.min(4096, Math.ceil(this.height / 8) * 8) ||
          !count || count > 64 - this.sourceBoxes.length || data.readUInt16BE(6) !== 0 ||
          header + 4 > data.length) throw new Error('Invalid GPU atlas metadata');
      const atlasBoxes = [];
      let left = this.width, top = this.height, right = 0, bottom = 0;
      for (let i = 0; i < count; i++) {
        const offset = 8 + i * 12;
        const box = {x: data.readUInt16BE(offset), y: data.readUInt16BE(offset + 2),
          width: data.readUInt16BE(offset + 4), height: data.readUInt16BE(offset + 6)};
        const atlas = {x: data.readUInt16BE(offset + 8), y: data.readUInt16BE(offset + 10),
          width: box.width, height: box.height};
        if (atlas.x + atlas.width > atlasWidth || atlas.y + atlas.height > atlasHeight ||
            atlasBoxes.some(other => overlaps(atlas, other))) throw new Error('GPU atlas maps overlap or exceed their image');
        this.addSource(box); atlasBoxes.push(atlas);
        left = Math.min(left, box.x); top = Math.min(top, box.y);
        right = Math.max(right, box.x + box.width); bottom = Math.max(bottom, box.y + box.height);
      }
      if (region.x !== left || region.y !== top || region.width !== right - left || region.height !== bottom - top)
        throw new Error('GPU atlas source bounding box is not exact');
      jpeg = data.subarray(header); jpegWidth = atlasWidth; jpegHeight = atlasHeight;
    } else {
      this.addSource(region);
      if (region.kind === 1) {
        if (data[3] !== 255) throw new Error('Native solid region is not opaque');
        return;
      }
    }
    this.decodeArea += jpegWidth * jpegHeight;
    if (this.decodeArea > Math.floor(this.width * this.height * 5 / 4) ||
        !jpegDimensions(jpeg, jpegWidth, jpegHeight)) throw new Error('GPU JPEG dimensions or decode budget are invalid');
  }
  parse() {
    while (!this.stopped) {
      let value;
      if (this.stage === 'version') {
        if (!(value = this.input.read(12))) return;
        if (value.toString('ascii') !== 'RFB 003.008\n') throw new Error('GPU capture requires RFB 3.8');
        this.send(Buffer.from('RFB 003.008\n')); this.stage = 'security-count';
      } else if (this.stage === 'security-count') {
        if (!(value = this.input.read(1))) return;
        this.securityCount = value[0];
        if (!this.securityCount) throw new Error('Private capture rejected authentication');
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
          throw new Error('GPU capture dimensions differ from the owned output');
        this.nameLength = value.readUInt32BE(20);
        if (this.nameLength > 4096) throw new Error('Private capture name exceeded its budget');
        this.stage = 'name';
      } else if (this.stage === 'name') {
        if (this.input.length < this.nameLength) return;
        this.input.read(this.nameLength); this.configure();
      } else if (this.stage === 'message') {
        if (!(value = this.input.read(4))) return;
        if (value[0] !== 0 || value[1] !== 0 || !this.pending || this.awaitingAck)
          throw new Error('Unexpected GPU capture update');
        this.rectangles = value.readUInt16BE(2);
        if (this.rectangles > 64) throw new Error('GPU region count exceeded its budget');
        this.regions = []; this.sourceBoxes = []; this.capturePtsUs = null; this.area = 0; this.decodeArea = 0;
        this.batchBytes = 32 + this.rectangles * 16;
        if (!this.rectangles) {
          if (!this.started) throw new Error('GPU capture did not provide an initial full image');
          this.pending = false; this.request();
        } else this.stage = 'rectangle';
      } else if (this.stage === 'rectangle') {
        if (!(value = this.input.read(12))) return;
        const x = value.readUInt16BE(0), y = value.readUInt16BE(2);
        const width = value.readUInt16BE(4), height = value.readUInt16BE(6);
        if (value.readInt32BE(8) !== ENCODING || !width || !height ||
            x + width > this.width || y + height > this.height)
          throw new Error('Capture did not provide bounded GPU image regions');
        this.rectangle = {x, y, width, height};
        this.stage = 'region-header';
      } else if (this.stage === 'region-header') {
        if (!(value = this.input.read(16))) return;
        const timestamp = value.readBigUInt64BE(0);
        if (timestamp === 0n || timestamp > BigInt(Number.MAX_SAFE_INTEGER)) throw new Error('Invalid native capture timestamp');
        const capturePtsUs = Number(timestamp);
        if (this.capturePtsUs !== null && capturePtsUs !== this.capturePtsUs) throw new Error('GPU regions are from different captures');
        this.capturePtsUs = capturePtsUs;
        this.rectangle.kind = value.readUInt32BE(8);
        this.regionLength = value.readUInt32BE(12);
        this.batchBytes += this.regionLength;
        if ((this.rectangle.kind !== 0 && this.rectangle.kind !== 1 && this.rectangle.kind !== 2) ||
            this.regionLength < (this.rectangle.kind === 2 ? 24 : 4) || (this.rectangle.kind === 1 && this.regionLength !== 4) ||
            this.batchBytes > MAX_BATCH) throw new Error('Native image region type or size is invalid');
        this.stage = 'payload';
      } else if (this.stage === 'payload') {
        if (!(value = this.input.read(this.regionLength))) return;
        this.validateRegion(value);
        this.regions.push({...this.rectangle, data: value});
        if (--this.rectangles) this.stage = 'rectangle';
        else {
          this.stage = 'message'; this.pending = false;
          if (this.input.length) throw new Error('GPU capture queued an unsolicited frame');
          if (this.fullRequest && this.area !== this.width * this.height) throw new Error('Initial image does not cover the complete output');
          this.started = true; this.awaitingAck = true; clearTimeout(this.deadline);
          const regions = this.regions; this.regions = this.sourceBoxes = null;
          this.onFrame({regions, capturePtsUs: this.capturePtsUs, full: this.fullRequest, bytes: this.batchBytes});
          return;
        }
      } else throw new Error('Invalid GPU region parser state');
    }
  }
}
module.exports = RegionProducer;
