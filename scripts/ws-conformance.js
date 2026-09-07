#!/usr/bin/env node
/**
 * Conformance tests for the WebSocket server in cpp/shared/net/web_channel.cpp.
 *
 * That server is written out rather than taken from npm -- see the comment on
 * it for why -- and this is the other half of that decision: a handshake and a
 * frame parser we own are a handshake and a frame parser we test. Everything
 * here is spoken over raw TCP, so nothing but the server`s own framing is
 * under test; a WebSocket client library would hide exactly the cases that
 * matter (an unmasked frame, a reserved bit, a 1 GiB length prefix).
 *
 * Run: npm run test:ws        (builds nothing -- tests the dist/ that exists)
 */
const { spawnSync, spawn } = require('child_process');
const os = require('os');
const path = require('path');
const net = require('net');
const fs = require('fs');
const crypto = require('crypto');

const ROOT = path.resolve(__dirname, '..');
const PORT = Number(process.env.WS_TEST_PORT || 3991);
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const PROTOCOL_VERSION = 14;

let failures = 0;
const check = (name, ok, detail) => {
  console.log(`${ok ? '  ok  ' : '  FAIL'} ${name}${ok || !detail ? '' : ' -- ' + detail}`);
  if (!ok) failures++;
};

// --- what a browser sends ---------------------------------------------------
function maskedFrame(opcode, payload, fin = true) {
  const mask = crypto.randomBytes(4);
  const len = payload.length;
  const headerBytes = len < 126 ? 2 : len < 65536 ? 4 : 10;
  const out = Buffer.allocUnsafe(headerBytes + 4 + len);
  out[0] = (fin ? 0x80 : 0) | opcode;
  if (len < 126) out[1] = 0x80 | len;
  else if (len < 65536) { out[1] = 0x80 | 126; out.writeUInt16BE(len, 2); }
  else { out[1] = 0x80 | 127; out.writeUInt32BE(0, 2); out.writeUInt32BE(len, 6); }
  mask.copy(out, headerBytes);
  for (let i = 0; i < len; i++) out[headerBytes + 4 + i] = payload[i] ^ mask[i & 3];
  return out;
}

function unmaskedFrame(opcode, payload, fin = true) {   // what a client must NOT send
  const len = payload.length;
  const out = Buffer.allocUnsafe(2 + len);
  out[0] = (fin ? 0x80 : 0) | opcode;
  out[1] = len;
  payload.copy(out, 2);
  return out;
}

// --- what the server should send back ---------------------------------------
function parseFrames(buffer) {
  const frames = [];
  let at = 0;
  while (at + 2 <= buffer.length) {
    const first = buffer[at], second = buffer[at + 1];
    let len = second & 0x7f, header = 2;
    if (len === 126) { if (buffer.length < at + 4) break; len = buffer.readUInt16BE(at + 2); header = 4; }
    else if (len === 127) { if (buffer.length < at + 10) break; len = Number(buffer.readBigUInt64BE(at + 2)); header = 10; }
    const masked = (second & 0x80) !== 0;
    if (buffer.length < at + header + (masked ? 4 : 0) + len) break;
    frames.push({ fin: (first & 0x80) !== 0, opcode: first & 0x0f, masked,
                  payload: buffer.subarray(at + header, at + header + len), headerBytes: header });
    at += header + len;
  }
  return frames;
}

const helloBody = (padding = 0) => {
  const body = Buffer.alloc(7 + padding);           // u8 opcode, u16 version, u32 content hash
  body[0] = 1;                                       // ClientMessage::Hello
  body.writeUInt16LE(PROTOCOL_VERSION, 1);
  body.writeUInt32LE(0xdeadbeef, 3);                 // a hash that will not match: the reply is
  return body;                                       // a refusing Welcome, which is still a reply
};
const appFrame = (body) => {                         // transport.cpp's [u32 length][payload]
  const out = Buffer.allocUnsafe(4 + body.length);
  out.writeUInt32LE(body.length, 0);
  body.copy(out, 4);
  return out;
};

// `trailing` is appended to the same write as the handshake, which is how a
// frame ends up in the upgrade event`s `head` rather than in a data event.
function connect(trailing) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ port: PORT, host: '127.0.0.1' });
    socket.on('error', reject);
    const key = crypto.randomBytes(16).toString('base64');
    const chunks = [];
    let handshake = null;
    const waiters = [];
    socket.on('data', (chunk) => {
      chunks.push(chunk);
      const all = Buffer.concat(chunks);
      if (!handshake) {
        const end = all.indexOf('\r\n\r\n');
        if (end < 0) return;
        handshake = all.subarray(0, end).toString();
        chunks.length = 0;
        chunks.push(all.subarray(end + 4));
        resolve({ socket, key, handshake, frames: () => parseFrames(Buffer.concat(chunks)),
                  raw: () => Buffer.concat(chunks), waiters });
      }
      for (const waiter of waiters.splice(0)) waiter();
    });
    socket.on('close', () => { for (const waiter of waiters.splice(0)) waiter(); });
    socket.write(
      'GET /ws HTTP/1.1\r\n' +
      'Host: 127.0.0.1\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      'Sec-WebSocket-Key: ' + key + '\r\n' +
      'Sec-WebSocket-Protocol: binary\r\n' +
      'Sec-WebSocket-Version: 13\r\n\r\n' +
      (trailing ? trailing.toString('binary') : ''), trailing ? 'binary' : 'utf8');
  });
}

const settle = (session, ms = 700) => new Promise((done) => {
  session.waiters.push(() => setTimeout(done, 120));
  setTimeout(done, ms);
});

const welcomeIn = (frames) => frames.some((f) => {
  if (f.opcode !== 0x2 || f.masked || !f.fin) return false;
  const body = f.payload;
  if (body.length < 4) return false;
  const length = body.readUInt32LE(0);
  return length + 4 <= body.length && body[4] === 1 &&      // ServerMessage::Welcome
         body.readUInt16LE(5) === PROTOCOL_VERSION;
});

// The server runs from a scratch directory on purpose: it looks for cert.crt
// beside itself, and finding one would put TLS between these tests and the
// framing they are about.
function startServer() {
  const server = path.join(ROOT, 'dist', 'server.js');
  if (!fs.existsSync(server)) {
    console.error('dist/server.js is missing -- run `npm run build:server` first.');
    process.exit(2);
  }
  const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'ws-conformance-'));
  fs.writeFileSync(path.join(scratch, 'inventory.json'), '{}');
  const child = spawn(process.execPath,
                      [server, '--port', String(PORT), '--db', path.join(scratch, 'inventory.json'),
                       '--web-root', scratch],
                      { cwd: scratch, stdio: ['ignore', 'pipe', 'pipe'] });
  return new Promise((resolve, reject) => {
    let output = '';
    const onData = (chunk) => {
      output += chunk;
      if (output.includes('listening on port')) resolve({ child, scratch });
    };
    child.stdout.on('data', onData);
    child.stderr.on('data', onData);
    child.on('exit', (code) => reject(new Error('server exited with ' + code + ':\n' + output)));
    setTimeout(() => reject(new Error('server did not start:\n' + output)), 30000);
  });
}

(async () => {
  const server = await startServer();
  process.on('exit', () => { try { server.child.kill(); } catch (e) { } });

  // 1. the handshake itself
  {
    const s = await connect();
    const accept = crypto.createHash('sha1').update(s.key + GUID).digest('base64');
    check('101 Switching Protocols', /^HTTP\/1\.1 101 /.test(s.handshake), s.handshake.split('\r\n')[0]);
    check('Sec-WebSocket-Accept is sha1(key + guid)',
          new RegExp('Sec-WebSocket-Accept: ' + accept.replace(/\+/g, '\\+'), 'i').test(s.handshake));
    check('subprotocol echoed', /Sec-WebSocket-Protocol: binary/i.test(s.handshake));
    check('no extension negotiated', !/Sec-WebSocket-Extensions/i.test(s.handshake));
    s.socket.destroy();
  }

  // 2. an unsupported version is refused, and told which one is supported
  {
    const socket = net.createConnection({ port: PORT, host: '127.0.0.1' });
    const reply = await new Promise((resolve) => {
      let text = '';
      socket.on('data', (c) => { text += c; if (text.includes('\r\n\r\n')) resolve(text); });
      socket.on('close', () => resolve(text));
      socket.write('GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
                   'Sec-WebSocket-Key: ' + crypto.randomBytes(16).toString('base64') +
                   '\r\nSec-WebSocket-Version: 8\r\n\r\n');
    });
    check('version 8 refused with 426', /^HTTP\/1\.1 426 /.test(reply), reply.split('\r\n')[0]);
    check('426 names version 13', /Sec-WebSocket-Version: 13/i.test(reply));
    socket.destroy();
  }

  // 3. a small frame, and the reply's shape
  {
    const s = await connect();
    s.socket.write(maskedFrame(0x2, appFrame(helloBody())));
    await settle(s);
    const frames = s.frames();
    check('small frame answered', welcomeIn(frames), JSON.stringify(frames.map((f) => f.opcode)));
    check('server frames are unmasked', frames.every((f) => !f.masked));
    check('server frames set FIN', frames.every((f) => f.fin));
    s.socket.destroy();
  }

  // 4. the 16-bit and 64-bit length forms, received
  for (const [name, padding] of [['16-bit length', 1000], ['64-bit length', 70000]]) {
    const s = await connect();
    // The padding is trailing bytes of the SAME application frame, so a reply
    // proves the whole message was reassembled and the length prefix agreed.
    s.socket.write(maskedFrame(0x2, appFrame(helloBody(padding))));
    await settle(s, 1500);
    check(name + ' received whole', welcomeIn(s.frames()));
    s.socket.destroy();
  }

  // 5. fragmentation, with a ping interleaved (which the RFC allows)
  {
    const s = await connect();
    const body = appFrame(helloBody());
    const a = body.subarray(0, 3), b = body.subarray(3, 6), c = body.subarray(6);
    s.socket.write(maskedFrame(0x2, a, false));
    s.socket.write(maskedFrame(0x0, b, false));
    s.socket.write(maskedFrame(0x9, Buffer.from('mid')));       // control frame mid-message
    s.socket.write(maskedFrame(0x0, c, true));
    await settle(s);
    const frames = s.frames();
    check('fragmented message reassembled', welcomeIn(frames));
    check('ping inside a fragmented message ponged',
          frames.some((f) => f.opcode === 0xa && f.payload.toString() === 'mid'));
    s.socket.destroy();
  }

  // 6. ping/pong, and two frames arriving in one write
  {
    const s = await connect();
    s.socket.write(Buffer.concat([maskedFrame(0x9, Buffer.from('one')),
                                  maskedFrame(0x9, Buffer.from('two'))]));
    await settle(s);
    const pongs = s.frames().filter((f) => f.opcode === 0xa).map((f) => f.payload.toString());
    check('both pings in one write ponged', pongs.join(',') === 'one,two', pongs.join(','));
    s.socket.destroy();
  }

  // 7. a frame split across many TCP writes
  {
    const s = await connect();
    const frame = maskedFrame(0x2, appFrame(helloBody()));
    for (const byte of frame) { s.socket.write(Buffer.from([byte])); }
    await settle(s);
    check('byte-at-a-time frame reassembled', welcomeIn(s.frames()));
    s.socket.destroy();
  }

  // 8. a frame arriving in the same write as the handshake, which the HTTP
  //    parser reads past and hands over as `head`
  {
    const s = await connect(maskedFrame(0x2, appFrame(helloBody())));
    await settle(s);
    check('frame in the handshake write answered', welcomeIn(s.frames()));
    s.socket.destroy();
  }

  // 9. an unmasked client frame is a protocol error
  {
    const s = await connect();
    s.socket.write(unmaskedFrame(0x2, appFrame(helloBody())));
    await settle(s);
    const close = s.frames().find((f) => f.opcode === 0x8);
    check('unmasked client frame closed with 1002',
          !!close && close.payload.readUInt16BE(0) === 1002,
          close ? String(close.payload.readUInt16BE(0)) : 'no close frame');
    s.socket.destroy();
  }

  // 10. a reserved bit is a protocol error (no extension was negotiated)
  {
    const s = await connect();
    const frame = maskedFrame(0x2, appFrame(helloBody()));
    frame[0] |= 0x40;                                            // RSV1
    s.socket.write(frame);
    await settle(s);
    const close = s.frames().find((f) => f.opcode === 0x8);
    check('reserved bit closed with 1002', !!close && close.payload.readUInt16BE(0) === 1002);
    s.socket.destroy();
  }

  // 11. an over-long frame is refused rather than allocated for
  {
    const s = await connect();
    const header = Buffer.alloc(14);
    header[0] = 0x82; header[1] = 0x80 | 127;
    header.writeUInt32BE(0, 2); header.writeUInt32BE(0x40000000, 6);   // 1 GiB
    s.socket.write(header);
    await settle(s);
    const close = s.frames().find((f) => f.opcode === 0x8);
    check('1 GiB frame closed with 1009', !!close && close.payload.readUInt16BE(0) === 1009);
    s.socket.destroy();
  }

  // 12. a close from the client is echoed
  {
    const s = await connect();
    const payload = Buffer.alloc(2); payload.writeUInt16BE(1000, 0);
    s.socket.write(maskedFrame(0x8, payload));
    await settle(s);
    const close = s.frames().find((f) => f.opcode === 0x8);
    check('close echoed with the same status',
          !!close && close.payload.length >= 2 && close.payload.readUInt16BE(0) === 1000);
    s.socket.destroy();
  }

  console.log(`\n${failures} failed`);
  server.child.kill();
  fs.rmSync(server.scratch, { recursive: true, force: true });
  process.exit(failures ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
