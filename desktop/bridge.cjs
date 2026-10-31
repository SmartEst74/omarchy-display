'use strict';
// Authenticated GPU-differenced image regions; presentation ACKs drive capture.
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const https = require('node:https');
const crypto = require('node:crypto');
const {WebSocketServer} = require('ws');
const RegionProducer = require('./region-producer.cjs');
const config = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const runtime = process.argv[3];
process.umask(0o077);
const captureIdentity = JSON.parse(process.env.OMARCHY_DISPLAY_CAPTURE);
const captureSize = JSON.parse(process.env.OMARCHY_DISPLAY_CAPTURE_SIZE || 'null');
if (!captureSize || !Number.isInteger(captureSize.width) || !Number.isInteger(captureSize.height) ||
    captureSize.width < 320 || captureSize.height < 320 || captureSize.width > 8192 || captureSize.height > 8192 ||
    captureSize.width * captureSize.height > 3840 * 2160 || ![15, 30, 60].includes(captureSize.fps)) {
  throw new Error('Owned capture dimensions are required');
}
const encoding = JSON.parse(process.env.OMARCHY_DISPLAY_GPU || 'null');
if (!encoding || encoding.codec !== 'gpu-regions' || encoding.capture !== 'dmabuf' ||
    encoding.difference !== 'gpu-exact' || encoding.encoder !== 'gpu-solid+libturbojpeg') {
  throw new Error('The native GPU region backend is required');
}
const mirror = JSON.parse(process.env.OMARCHY_DISPLAY_MIRROR);
const viewers = new Map();
let producerError = false;
const selfStat = fs.readFileSync('/proc/self/stat', 'utf8');
const stat = selfStat.slice(selfStat.lastIndexOf(')') + 1).trim().split(/\s+/);
const bridgeIdentity = {pid: process.pid, start: stat[19]};
const connected = new Map();
let receiver = null;
let receiverPeer = null;
function publish() {
  const report = {session: process.env.OMARCHY_DISPLAY_SESSION,
    bridge: bridgeIdentity, capture: captureIdentity, clients: connected.size,
    updated: Number(process.hrtime.bigint()) / 1e9, receiver, error: producerError, encoding};
  report.performance = [...viewers.values()].filter(c => c.ackedFrames).map(c => ({
    presentedFrames: c.ackedFrames, captureToAckMs: c.captureToAckMs, encodedToAckMs: c.encodedToAckMs
  }));
  const destination = path.join(runtime, 'bridge-status.json');
  fs.writeFileSync(destination + '.tmp', JSON.stringify(report), {mode: 0o600});
  fs.renameSync(destination + '.tmp', destination);
}
const root = path.resolve(__dirname, '../apps/desktop-tv');
const normalize = (value) => value.startsWith('::ffff:') ? value.slice(7) : value;
const peers = new Set([config.tv, '127.0.0.1', '::1']);
const allowedPeer = (request) => peers.has(normalize(request.socket.remoteAddress || ''));
const tls = config.tls ? {
  key: fs.readFileSync(config.tls.key), cert: fs.readFileSync(config.tls.cert),
  minVersion: 'TLSv1.2'
} : null;
const scheme = tls ? 'https:' : 'http:';
const origins = new Set([scheme + '//' + config.listen + ':' + config.port,
  scheme + '//127.0.0.1:' + config.port, scheme + '//localhost:' + config.port,
  'null', 'file://']);
const files = {
  '/': ['index.html', 'text/html'], '/index.html': ['index.html', 'text/html'],
  '/main.js': ['main.js', 'application/javascript'],
  '/style.css': ['style.css', 'text/css'], '/icon.png': ['icon.png', 'image/png']
};
function receiverInfo(req, res) {
  if (req.headers.origin && !origins.has(req.headers.origin)) {
    res.writeHead(403); res.end(); return;
  }
  if (req.headers.origin) {
    res.setHeader('Access-Control-Allow-Origin', req.headers.origin);
    res.setHeader('Vary', 'Origin');
  }
  res.setHeader('Cache-Control', 'no-store');
  if (req.method === 'OPTIONS') {
    if (req.headers['access-control-request-method'] !== 'POST') {
      res.writeHead(405); res.end(); return;
    }
    res.writeHead(204, {'Access-Control-Allow-Methods': 'POST',
      'Access-Control-Allow-Headers': 'Authorization, Content-Type'});
    res.end(); return;
  }
  if (req.method !== 'POST') { res.writeHead(405); res.end(); return; }
  const supplied = Buffer.from(String(req.headers.authorization || ''));
  const expected = Buffer.from('Bearer ' + config.secret);
  if (supplied.length !== expected.length || !crypto.timingSafeEqual(supplied, expected)) {
    res.writeHead(401); res.end(); return;
  }
  const peer = normalize(req.socket.remoteAddress || '');
  if (![...connected.values()].includes(peer)) { res.writeHead(409); res.end(); return; }
  if (String(req.headers['content-type'] || '').split(';')[0].trim().toLowerCase() !== 'application/json') {
    res.writeHead(415); res.end(); return;
  }
  const chunks = [];
  let length = 0;
  let rejected = false;
  req.setTimeout(5000, () => req.destroy());
  req.on('error', () => {});
  req.on('data', (chunk) => {
    if (rejected) return;
    length += chunk.length;
    if (length > 2048) {
      rejected = true;
      chunks.length = 0;
      res.writeHead(413, {'Connection': 'close'}); res.end(); return;
    }
    chunks.push(chunk);
  });
  req.on('end', () => {
    if (rejected) return;
    let info;
    try { info = JSON.parse(Buffer.concat(chunks).toString('utf8')); } catch {
      res.writeHead(400); res.end(); return;
    }
    const dimension = (value) => Number.isInteger(value) && value >= 320 && value <= 8192;
    if (!info || Array.isArray(info) || typeof info !== 'object' ||
        !dimension(info.viewportWidth) || !dimension(info.viewportHeight) ||
        !((info.panelWidth === null && info.panelHeight === null) ||
          (dimension(info.panelWidth) && dimension(info.panelHeight)))) {
      res.writeHead(400); res.end(); return;
    }
    if (![...connected.values()].includes(peer)) { res.writeHead(409); res.end(); return; }
    receiver = {viewportWidth: info.viewportWidth, viewportHeight: info.viewportHeight,
      panelWidth: info.panelWidth, panelHeight: info.panelHeight};
    receiverPeer = peer;
    publish();
    res.writeHead(204); res.end();
  });
}
function requestHandler(req, res) {
  if (!allowedPeer(req)) { res.writeHead(403); res.end('Peer not allowed'); return; }
  if (req.url === '/receiver-info') { receiverInfo(req, res); return; }
  const file = files[req.url];
  if (req.method !== 'GET' || !file) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, {'Content-Type': file[1], 'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer'});
  const stream = fs.createReadStream(path.join(root, file[0]));
  stream.on('error', () => res.destroy());
  stream.pipe(res);
}
const wss = new WebSocketServer({noServer: true, perMessageDeflate: false,
  maxPayload: 128, handleProtocols: protocols => protocols.has('regions') ? 'regions' : false});
function disconnect(ws) {
  const client = viewers.get(ws);
  if (!client) return;
  viewers.delete(ws);
  if (client.producer) client.producer.stop();
  connected.delete(ws);
  if (![...connected.values()].includes(receiverPeer)) { receiver = null; receiverPeer = null; }
  publish();
}
function drop(ws) {
  disconnect(ws);
  ws.terminate();
}
function advance(client) {
  if (!client.writing && client.outstanding === null && client.producer) client.producer.acknowledge();
}
function sendFrame(ws, client) {
  if (!client.helloSent || client.writing || client.outstanding !== null || !client.packet || ws.readyState !== 1) return;
  if (ws.bufferedAmount) { drop(ws); return; }
  const packet = client.packet;
  client.packet = null;
  client.outstanding = client.sequence;
  client.sentAtUs = Number(process.hrtime.bigint() / 1000n);
  client.writing = true;
  ws.send(packet, {binary: true}, error => {
    client.writing = false;
    if (error) { drop(ws); return; }
    if (viewers.has(ws)) advance(client);
  });
}
function frame(ws, client, update) {
  if (!viewers.has(ws)) return;
  if (client.packet || client.outstanding !== null) {
    sourceFailure(ws, new Error('Capture produced an unacknowledged region batch')); return;
  }
  const packet = Buffer.allocUnsafe(update.bytes);
  packet.writeUInt32BE(0x4f445233, 0);
  client.sequence = (client.sequence + 1) >>> 0;
  packet.writeUInt32BE(client.sequence, 4);
  packet.writeDoubleBE(update.capturePtsUs, 8);
  packet.writeDoubleBE(Date.now(), 16);
  packet.writeUInt16BE(captureSize.width, 24);
  packet.writeUInt16BE(captureSize.height, 26);
  packet.writeUInt16BE(update.regions.length, 28);
  packet.writeUInt16BE(update.full ? 1 : 0, 30);
  let table = 32, offset = 32 + update.regions.length * 16;
  for (const region of update.regions) {
    packet.writeUInt16BE(region.x, table); packet.writeUInt16BE(region.y, table + 2);
    packet.writeUInt16BE(region.width, table + 4); packet.writeUInt16BE(region.height, table + 6);
    packet.writeUInt32BE(region.kind, table + 8);
    packet.writeUInt32BE(region.data.length, table + 12);
    region.data.copy(packet, offset);
    table += 16; offset += region.data.length;
  }
  client.capturePtsUs = update.capturePtsUs;
  client.encodedAtUs = Number(process.hrtime.bigint() / 1000n);
  client.packet = packet;
  sendFrame(ws, client);
}
function sourceFailure(ws, error) {
  producerError = true;
  console.error('GPU region capture stopped: ' + error.message);
  drop(ws);
}
function reject(socket, status) {
  socket.end('HTTP/1.1 ' + status + '\r\nConnection: close\r\nContent-Length: 0\r\n\r\n');
}
function upgrade(req, socket, head) {
  socket.on('error', () => {});
  if (!allowedPeer(req)) { reject(socket, '403 Forbidden'); return; }
  if (req.url !== '/desktop' || (req.headers.origin && !origins.has(req.headers.origin))) {
    reject(socket, '403 Forbidden'); return;
  }
  const protocols = String(req.headers['sec-websocket-protocol'] || '').split(',').map((s) => s.trim());
  const provided = protocols.filter((s) => s.startsWith('pair.'));
  const expected = Buffer.from('pair.' + config.secret);
  const supplied = Buffer.from(provided.length === 1 ? provided[0] : '');
  if (!protocols.includes('regions') || supplied.length !== expected.length ||
      !crypto.timingSafeEqual(supplied, expected)) {
    reject(socket, '401 Unauthorized'); return;
  }
  if (stopping || wss.clients.size >= 2) {
    reject(socket, '503 Service Unavailable'); return;
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    const client = {peer: normalize(req.socket.remoteAddress || ''), helloSent: false,
      openedAtUs: Number(process.hrtime.bigint() / 1000n), producer: null, packet: null,
      sequence: 0, outstanding: null, writing: false, sentAtUs: 0, ackedFrames: 0,
      captureToAckMs: null, encodedToAckMs: null};
    viewers.set(ws, client);
    ws.on('error', () => drop(ws));
    ws.on('close', () => disconnect(ws));
    ws.on('message', (data, binary) => {
      if (binary || data.length > 128) { drop(ws); return; }
      let ack;
      try { ack = JSON.parse(data.toString('utf8')); } catch { drop(ws); return; }
      if (!ack || Array.isArray(ack) || typeof ack !== 'object' || Object.keys(ack).length !== 2 ||
          ack.type !== 'ack' || !Number.isInteger(ack.sequence) || ack.sequence < 0 || ack.sequence > 0xffffffff ||
          client.outstanding === null || ack.sequence !== client.outstanding) {
        drop(ws); return;
      }
      const now = Number(process.hrtime.bigint() / 1000n);
      const captureToAck = (now - client.capturePtsUs) / 1000;
      client.captureToAckMs = captureToAck >= 0 && captureToAck < 60000 ? captureToAck : null;
      client.encodedToAckMs = (now - client.encodedAtUs) / 1000;
      client.outstanding = null; client.ackedFrames++;
      producerError = false;
      if (!connected.has(ws)) { connected.set(ws, client.peer); publish(); }
      advance(client);
    });
    ws.send(JSON.stringify({type: 'hello', codec: 'gpu-regions',
      width: captureSize.width, height: captureSize.height, fps: captureSize.fps,
      viewOnly: true, audio: false}), {binary: false}, error => {
      if (error) { drop(ws); return; }
      client.helloSent = true; sendFrame(ws, client);
    });
    try {
      client.producer = new RegionProducer({runtime, mirror, ...captureSize,
        onFrame: update => frame(ws, client, update), onError: error => sourceFailure(ws, error)});
    } catch (error) { sourceFailure(ws, error); }
    publish();
  });
}
const servers = [];
let ready = 0;
for (const host of new Set([config.listen, '127.0.0.1'])) {
  const server = tls ? https.createServer(tls, requestHandler) : http.createServer(requestHandler);
  server.headersTimeout = 10000;
  server.requestTimeout = 5000;
  server.on('upgrade', upgrade);
  server.on('error', () => { console.error('Bridge listener failed; stopping.'); process.exit(1); });
  server.listen(config.port, host, () => {
    if (++ready === new Set([config.listen, '127.0.0.1']).size) {
      publish();
      fs.writeFileSync(path.join(runtime, 'bridge-ready'), '', {mode: 0o600});
    }
  });
  servers.push(server);
}
const heartbeat = setInterval(() => {
  const now = Number(process.hrtime.bigint() / 1000n);
  for (const [ws, client] of viewers) {
    if (client.outstanding !== null ? now - client.sentAtUs > 5e6 :
        !connected.has(ws) && now - client.openedAtUs > 15e6) drop(ws);
  }
  publish();
}, 1000);
let stopping = false;
async function stop() {
  if (stopping) return;
  stopping = true;
  clearInterval(heartbeat);
  const closing = [...viewers.values()].filter(c => c.producer).map(c => c.producer.closed);
  for (const ws of [...viewers.keys()]) drop(ws);
  for (const server of servers) server.close();
  try {
    await Promise.all(closing);
    publish();
    process.exit(0);
  } catch (error) {
    console.error('GPU region cleanup failed: ' + error.message);
    process.exit(1);
  }
}
process.on('SIGTERM', stop);
process.on('SIGINT', stop);
