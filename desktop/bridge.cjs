'use strict';
// Authenticated view-only JPEG frames. Client messages are exact display ACKs only.
const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const https = require('node:https');
const crypto = require('node:crypto');
const {WebSocketServer} = require('ws');
const JpegProducer = require('./jpeg-producer.cjs');
const config = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const runtime = process.argv[3];
process.umask(0o077);
const captureIdentity = JSON.parse(process.env.OMARCHY_DISPLAY_CAPTURE);
const captureSize = JSON.parse(process.env.OMARCHY_DISPLAY_CAPTURE_SIZE || 'null');
if (!captureSize || !Number.isInteger(captureSize.width) || !Number.isInteger(captureSize.height) ||
    captureSize.width < 320 || captureSize.height < 320 || captureSize.width > 8192 || captureSize.height > 8192 ||
    captureSize.width * captureSize.height > 3840 * 2160 || ![15, 30, 60].includes(captureSize.fps) ||
    !path.isAbsolute(process.env.OMARCHY_DISPLAY_PYTHON || '')) {
  throw new Error('Owned capture dimensions and Python runtime are required');
}
const viewers = new Map();
let producer = null;
let retiring = false;
let producerReady = false;
let producerError = false;
let latest = null;
let sequence = 0;
const selfStat = fs.readFileSync('/proc/self/stat', 'utf8');
const stat = selfStat.slice(selfStat.lastIndexOf(')') + 1).trim().split(/\s+/);
const bridgeIdentity = {pid: process.pid, start: stat[19]};
const connected = new Map();
let receiver = null;
let receiverPeer = null;
function publish() {
  const report = {session: process.env.OMARCHY_DISPLAY_SESSION,
    bridge: bridgeIdentity, capture: captureIdentity, clients: connected.size,
    updated: Number(process.hrtime.bigint()) / 1e9, receiver, error: producerError};
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
  maxPayload: 128, handleProtocols: (protocols) => protocols.has('binary') ? 'binary' : false});
function stopProducer() {
  if (producer && !retiring) {
    const previous = producer;
    retiring = true;
    previous.stop();
    previous.closed.then(() => {
      if (producer === previous) producer = null;
      retiring = false;
    });
  }
  producerReady = false; latest = null;
  connected.clear(); receiver = null; receiverPeer = null;
}
function disconnect(ws) {
  if (!viewers.delete(ws)) return;
  connected.delete(ws);
  if (![...connected.values()].includes(receiverPeer)) { receiver = null; receiverPeer = null; }
  if (!viewers.size) stopProducer();
  publish();
}
function drop(ws) {
  disconnect(ws);
  ws.terminate();
}
function sendLatest(ws, client) {
  if (!client.helloSent || client.writing || client.outstanding !== null ||
      !latest || latest.sequence === client.lastSent || ws.readyState !== 1) return;
  if (ws.bufferedAmount) { drop(ws); return; }
  const frame = latest;
  client.outstanding = frame.sequence;
  client.lastSent = frame.sequence;
  client.sentAt = Number(process.hrtime.bigint()) / 1e9;
  client.writing = true;
  try {
    ws.send(frame.packet, {binary: true}, (error) => {
      client.writing = false;
      if (error) { drop(ws); return; }
      if (viewers.has(ws)) sendLatest(ws, client);
    });
  } catch {
    drop(ws);
  }
}
function frame(jpeg) {
  if (!producer || !viewers.size) return;
  if (jpeg.length + 20 > 16 * 1024 * 1024) throw new Error('JPEG transport frame budget exceeded');
  const packet = Buffer.allocUnsafe(20 + jpeg.length);
  packet.write('ODJ1', 0, 4, 'ascii');
  sequence = (sequence + 1) >>> 0;
  packet.writeUInt32BE(sequence, 4);
  packet.writeDoubleBE(Date.now(), 8);
  packet.writeUInt16BE(captureSize.width, 16);
  packet.writeUInt16BE(captureSize.height, 18);
  jpeg.copy(packet, 20);
  latest = {sequence, packet};
  const first = !producerReady;
  producerReady = true;
  for (const [ws, client] of viewers) {
    if (ws.readyState !== 1) continue;
    connected.set(ws, client.peer);
    sendLatest(ws, client);
  }
  if (first) publish();
}
function sourceFailure(error) {
  producerError = true;
  console.error('JPEG producer stopped: ' + error.message);
  stopProducer();
  for (const ws of [...viewers.keys()]) drop(ws);
  publish();
}
function startProducer() {
  if (producer || !viewers.size) return;
  producerError = false;
  try {
    producer = new JpegProducer({runtime, ...captureSize, python: process.env.OMARCHY_DISPLAY_PYTHON,
      onFrame: frame, onError: sourceFailure});
  } catch (error) {
    sourceFailure(error);
  }
  publish();
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
  if (!protocols.includes('binary') || supplied.length !== expected.length ||
      !crypto.timingSafeEqual(supplied, expected)) {
    reject(socket, '401 Unauthorized'); return;
  }
  if (wss.clients.size >= 2 || retiring || (producer && producer.stopped)) {
    reject(socket, '503 Service Unavailable'); return;
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    const client = {peer: normalize(req.socket.remoteAddress || ''), helloSent: false,
      writing: false, outstanding: null, lastSent: null, sentAt: 0};
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
      client.outstanding = null;
      sendLatest(ws, client);
    });
    if (producerReady) connected.set(ws, client.peer);
    ws.send(JSON.stringify({type: 'hello', width: captureSize.width, height: captureSize.height,
      fps: captureSize.fps, viewOnly: true, audio: false}), {binary: false}, (error) => {
      if (error) { drop(ws); return; }
      client.helloSent = true;
      if (viewers.has(ws)) sendLatest(ws, client);
    });
    startProducer();
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
  const now = Number(process.hrtime.bigint()) / 1e9;
  for (const [ws, client] of viewers) {
    if (client.outstanding !== null && now - client.sentAt > 5) drop(ws);
  }
  publish();
}, 1000);
function stop() {
  clearInterval(heartbeat);
  stopProducer();
  for (const ws of [...viewers.keys()]) drop(ws);
  publish();
  for (const server of servers) server.close();
  process.exit(0);
}
process.on('SIGTERM', stop);
process.on('SIGINT', stop);
