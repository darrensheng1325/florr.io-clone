#include "shared/net/web_channel.h"

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

// The JavaScript half lives here rather than in a .js library for the same
// reason canvas.cpp's does: it is the implementation of the C++ declarations
// directly above it, and splitting the two across files is how they drift.
//
// Everything hangs off one registry, Module.flixNet, whose slots hold either a
// channel or a listener. Slot indices are the handles C++ passes around, so a
// closed channel's slot is nulled rather than spliced out -- reusing an index
// for something else while C++ still holds it would be the one bug this design
// can have.
//
// A listener is one of two things, decided by what the runtime is. Under Node
// it is a real HTTP(S) server with a WebSocket upgrade path -- the deployed
// game server. In a page it is an in-page listener: a port number that
// connect() calls from the SAME page resolve to directly, handing the two ends
// a pair of queues instead of a socket. That is what lets one wasm carry both
// halves of the game and play them against each other with no network at all
// (offline/main.cpp); the Listener and Dialer above cannot tell the difference,
// and neither can the protocol.

EM_JS(void, flix_net_init, (), {
  if (Module.flixNet) return;

  const net = {
    slots: [],
    isNode: typeof process !== "undefined" && process.versions && process.versions.node,
    // port -> slot id of an in-page listener. Only a page has these; Node
    // listens on real ports.
    local: {},
    // The names an in-page listener answers to. What a native Listener would
    // be reachable at from its own machine, and nothing else: any other host
    // is a real server somewhere and goes over the network as before.
    isLoopbackHost(host) {
      return host === "127.0.0.1" || host === "localhost" || host === "::1" || host === "[::1]";
    },

    // How long the transport picker waits for /transport-info, and for a
    // WebTransport handshake, before giving up and using WebSocket. Both are
    // one-off costs on the first connection of a session.
    infoTimeoutMs: 1500,
    webTransportTimeoutMs: 2500,

    alloc(slot) {
      const reuse = this.slots.indexOf(null);
      if (reuse >= 0) { this.slots[reuse] = slot; return reuse; }
      this.slots.push(slot);
      return this.slots.length - 1;
    },
    get(id) { return (id >= 0 && id < this.slots.length) ? this.slots[id] : null; },
    release(id) { if (id >= 0 && id < this.slots.length) this.slots[id] = null; },

    channel() {
      return {
        listener: false,
        state: 0,
        kind: "",
        peer: "",
        error: "",
        chunks: [],
        head: null,
        headAt: 0,
        queued: 0,
        sender: null,
        bufferedFn: null,
        closer: null,
        outstanding: 0,
      };
    },

    // Received bytes, appended whole. The reader never re-slices: recv() walks
    // the chunk list with an offset, so a large message is handed out across
    // as many calls as the caller's buffer needs without copying twice.
    deliver(channel, bytes) {
      if (!channel || channel.state === 2 || !bytes || bytes.length === 0) return;
      channel.chunks.push(bytes);
      channel.queued += bytes.length;
    },

    fail(channel, reason) {
      if (!channel) return;
      if (!channel.error) channel.error = String(reason || "closed");
      channel.state = 2;
    },
  };

  Module.flixNet = net;
});

// --- client ----------------------------------------------------------------

EM_JS(int, flix_ch_connect, (const char* hostPtr, int port), {
  const net = Module.flixNet;
  const host = UTF8ToString(hostPtr);

  // A listener in this very page takes precedence over the network, exactly
  // as a native connect() to 127.0.0.1 reaches whatever is bound on that port
  // locally. The two ends are made together, the sender of each feeding the
  // receive queue of the other, and both are open at once: there is no
  // handshake to wait for when the peer is a function call away. The Dialer
  // still sees Connecting on this call and Open on its next poll, which is
  // the same sequence the network path gives it, only faster.
  if (net.local[port] !== undefined && net.isLoopbackHost(host)) {
    const listener = net.get(net.local[port]);
    if (!listener || !listener.listener) return -1;
    const client = net.channel();
    const server = net.channel();
    const wire = (from, to, peer) => {
      from.kind = "loopback";
      from.peer = peer;
      from.state = 1;
      // Handed over whole and unshared: send() already sliced the bytes out of
      // the heap, so the receiving queue may keep them as they are.
      from.sender = (bytes) => net.deliver(to, bytes);
      // Nothing is ever in flight between two queues in one thread.
      from.bufferedFn = () => 0;
      from.closer = () => net.fail(to, "peer closed");
    };
    // The peer string of the server end is what the account limiter keys on.
    // A page has exactly one player, so the name only has to be stable and
    // not collide with a real address.
    wire(client, server, "loopback:" + port);
    wire(server, client, "loopback");
    const clientId = net.alloc(client);
    listener.pending.push(net.alloc(server));
    return clientId;
  }

  // A page dials its own scheme: a document served over https may not open a
  // ws:// socket, and https is also the only context WebTransport exists in.
  // Node has no page, so it asks for the plain one.
  const secure = typeof location !== "undefined" && location.protocol === "https:";
  const origin = (secure ? "https://" : "http://") + host + ":" + port;
  const channel = net.channel();
  const id = net.alloc(channel);


  const withTimeout = (promise, ms, what) => Promise.race([
    promise,
    new Promise((_, reject) => setTimeout(() => reject(new Error(what + " timed out")), ms)),
  ]);

  const openWebSocket = () => {
    const url = origin.replace(/^http/, "ws") + "/ws";
    // The page has always had WebSocket; Node has had it as a global since
    // 22, which is the floor the deployment already sets for other reasons.
    // There is deliberately no npm fallback -- see the server half below.
    const socket = new WebSocket(url, "binary");
    socket.binaryType = "arraybuffer";
    channel.sender = (bytes) => socket.send(bytes);
    channel.bufferedFn = () => socket.bufferedAmount || 0;
    channel.closer = () => { try { socket.close(); } catch (e) { } };
    socket.onopen = () => { channel.kind = "websocket"; channel.peer = origin; channel.state = 1; };
    socket.onmessage = (event) => {
      const data = event.data;
      if (data instanceof ArrayBuffer) net.deliver(channel, new Uint8Array(data));
      else if (data && data.byteLength !== undefined) net.deliver(channel, new Uint8Array(data.buffer || data));
    };
    socket.onerror = () => { if (!channel.error) channel.error = "websocket error"; };
    socket.onclose = () => net.fail(channel, channel.error || "connection closed");
  };

  const openWebTransport = async (info) => {
    const base = new URL(origin);
    const host = info.host || base.hostname;
    const port = info.port || base.port;
    const path = info.path || "/wt";
    const options = {};
    if (Array.isArray(info.certHashes) && info.certHashes.length > 0) {
      // A development certificate no public CA vouches for is pinned by
      // digest, which is the only way a self-signed localhost setup works
      // without launching the browser with special flags.
      options.serverCertificateHashes = info.certHashes.map((hash) => ({
        algorithm: "sha-256",
        value: Uint8Array.from(atob(hash), (c) => c.charCodeAt(0)),
      }));
    }
    const session = new WebTransport("https://" + host + ":" + port + path, options);
    await withTimeout(session.ready, net.webTransportTimeoutMs, "webtransport handshake");
    const stream = await session.createBidirectionalStream();
    const writer = stream.writable.getWriter();
    const reader = stream.readable.getReader();

    channel.sender = (bytes) => {
      // Tracked by hand because a stream writer has no bufferedAmount: the
      // count goes up when the write is handed over and down when the
      // transport says it took it, which is the same signal.
      channel.outstanding += bytes.length;
      writer.write(bytes).then(
        () => { channel.outstanding -= bytes.length; },
        (e) => { channel.outstanding -= bytes.length; net.fail(channel, e); });
    };
    channel.bufferedFn = () => channel.outstanding;
    channel.closer = () => { try { session.close(); } catch (e) { } };
    channel.kind = "webtransport";
    channel.peer = origin;
    channel.state = 1;

    (async () => {
      try {
        for (;;) {
          const result = await reader.read();
          if (result.done) break;
          net.deliver(channel, result.value);
        }
      } catch (e) {
        net.fail(channel, e);
        return;
      }
      net.fail(channel, "stream ended");
    })();
  };

  (async () => {
    let info = null;
    try {
      const controller = new AbortController();
      const timer = setTimeout(() => controller.abort(), net.infoTimeoutMs);
      const response = await fetch(origin + "/transport-info",
                                  { signal: controller.signal, cache: "no-store" });
      clearTimeout(timer);
      if (response.ok) info = await response.json();
    } catch (e) {
      // No answer means WebSocket only, which is the safe assumption anyway.
    }

    // Secure context only, because that is what the API requires -- an http
    // origin can never use it however willing the server is.
    const eligible = info && info.webtransport && typeof WebTransport !== "undefined" &&
                     origin.startsWith("https://");
    if (eligible) {
      try {
        await openWebTransport(info);
        return;
      } catch (e) {
        // Anything at all: no UDP path, an untrusted certificate, a timeout.
        // One wasted round trip is the whole cost of trying.
        if (typeof console !== "undefined") {
          console.warn("[net] webtransport unavailable (" + e + "); using websocket");
        }
      }
    }
    try {
      openWebSocket();
    } catch (e) {
      net.fail(channel, e);
    }
  })();

  return id;
});

// --- server ----------------------------------------------------------------

EM_JS(int, flix_ch_listen,
      (int port, const char* certPtr, const char* keyPtr, const char* rootPtr), {
  const net = Module.flixNet;

  if (!net.isNode) {
    // A page cannot bind a socket, but it can be the other end of its own
    // connect() calls -- see flix_ch_connect. One listener per port, as with
    // a real bind; the certificate and the web root mean nothing here, since
    // there is no HTTP to serve and nobody outside the page to serve it to.
    if (net.local[port] !== undefined) return -1;
    const listener = { listener: true, state: 1, pending: [], closer: null, error: "", port: port };
    const id = net.alloc(listener);
    net.local[port] = id;
    listener.closer = () => { if (net.local[port] === id) delete net.local[port]; };
    return id;
  }

  const fs = require("fs");

  const path = require("path");
  const crypto = require("crypto");

  // Where the client build sits, unless told otherwise: the emitted server
  // module and the client's page land in the same build directory, so the
  // default needs no argument and no guessing.
  const webRoot = path.resolve(UTF8ToString(rootPtr) || __dirname);

  const readPair = (certPath, keyPath) => {
    try {
      const pair = {
        cert: fs.readFileSync(certPath),
        key: fs.readFileSync(keyPath),
        name: path.basename(certPath),
        validTo: null,
        expired: false,
      };
      try {
        const parsed = new crypto.X509Certificate(pair.cert);
        pair.validTo = parsed.validTo;
        pair.expired = new Date(parsed.validTo) < new Date();
        // Only a short-lived certificate may be pinned by digest, which is
        // what lets a self-signed one work with no trust-store setup.
        const days = (new Date(parsed.validTo) - new Date(parsed.validFrom)) / 86400000;
        pair.digest = days <= 14
          ? crypto.createHash("sha256").update(parsed.raw).digest("base64") : null;
      } catch (e) {
        // Unparseable is still servable; TLS will say so if it is not.
      }
      return pair;
    } catch (e) {
      return null;
    }
  };

  const certPath = UTF8ToString(certPtr);
  const keyPath = UTF8ToString(keyPtr);
  let credentials = null;
  if (certPath && keyPath) {
    credentials = readPair(certPath, keyPath);
    if (!credentials) {
      console.warn("[net] cannot read " + certPath + "/" + keyPath + "; http only");
    }
  } else {
    // The named pairs, in the working directory. VALIDITY decides, not order:
    // the same rule the TypeScript server follows, and the reason it matters
    // here is that the committed cert.crt outlives its own dates while the
    // generated dev pair is refreshed. Serving the dead one would cost every
    // browser the connection and cost WebTransport its pinnable digest.
    const found = [readPair("cert.crt", "cert.key"),
                   readPair("dev-cert.crt", "dev-cert.key")].filter(Boolean);
    credentials = found.find((pair) => !pair.expired) || found[0] || null;
  }

  if (credentials && credentials.expired) {
    // Reported, not repaired: regenerating is the TypeScript server`s job --
    // these files are shared, and two writers is how they come to disagree.
    console.warn("[net] " + credentials.name + " expired on " + credentials.validTo +
                 "; browsers will refuse it. `npm start` regenerates it.");
  }
  console.log("[net] serving " + (credentials ? "https" : "http") + " from " + webRoot +
              (credentials ? " (" + credentials.name + ")" : ""));

  const listener = { listener: true, state: 1, pending: [], closer: null, error: "" };
  const id = net.alloc(listener);

  // Published at /transport-info, and the only thing a client needs to decide
  // between the two transports. Absent hashes mean ordinary CA validation.
  const advertisement = { webtransport: false, port: port, path: "/wt" };
  // A digest is published only for a certificate short-lived enough to be
  // pinned. A long-lived one is either publicly trusted already or will not be
  // accepted pinned, and ordinary CA validation is what applies to it.
  if (credentials && credentials.digest) advertisement.certHashes = [credentials.digest];

  // The extensions this actually serves. `application/wasm` is the one that
  // matters: without it the browser will not stream-compile the module and
  // falls back to buffering the whole thing, or refuses it outright.
  const mimeTypes = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript",
    ".mjs": "application/javascript",
    ".wasm": "application/wasm",
    ".json": "application/json",
    ".css": "text/css",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".ttf": "font/ttf",
    ".woff2": "font/woff2",
    ".map": "application/json",
    ".data": "application/octet-stream",
    ".txt": "text/plain; charset=utf-8",
  };

  const sendFile = (response, file, head) => {
    let body;
    try {
      const stat = fs.statSync(file);
      if (!stat.isFile()) return false;
      body = head ? null : fs.readFileSync(file);
      response.writeHead(200, {
        "Content-Type": mimeTypes[path.extname(file).toLowerCase()] || "application/octet-stream",
        "Content-Length": head ? stat.size : body.length,
        // Revalidate every time. This serves a build directory, and the one
        // failure worth designing against is a page still running the wasm
        // from before the last rebuild.
        "Cache-Control": "no-cache",
      });
      response.end(body === null ? undefined : body);
      return true;
    } catch (e) {
      return false;
    }
  };

  const onRequest = (request, response) => {
    const url = (request.url || "/").split("?")[0];
    const head = request.method === "HEAD";
    if (request.method !== "GET" && !head) {
      response.writeHead(405, { "Content-Type": "text/plain", "Allow": "GET, HEAD" });
      response.end("method not allowed\n");
      return;
    }

    if (url === "/transport-info") {
      const body = JSON.stringify(advertisement);
      response.writeHead(200, {
        "Content-Type": "application/json",
        "Content-Length": Buffer.byteLength(body),
        "Cache-Control": "no-store",
        // The page is normally served from this very origin, so this is only
        // for a split setup -- a client build served from somewhere else.
        "Access-Control-Allow-Origin": "*",
      });
      response.end(head ? undefined : body);
      return;
    }

    // Decoded, normalised, and required to stay under the root: `..` and an
    // encoded `%2e%2e` are the same request, and neither may leave.
    let decoded;
    try {
      decoded = decodeURIComponent(url);
    } catch (e) {
      response.writeHead(400, { "Content-Type": "text/plain" });
      response.end("bad request\n");
      return;
    }
    const target = path.resolve(webRoot, "." + path.posix.normalize(decoded));
    if (target !== webRoot && !target.startsWith(webRoot + path.sep)) {
      response.writeHead(403, { "Content-Type": "text/plain" });
      response.end("forbidden\n");
      return;
    }

    // A directory means its index. In cpp/build-web the client's page is
    // named after its output (bundle.html) rather than index.html, which is
    // the name it is staged into dist/ under, so both are tried.
    let candidates = [target];
    let isDirectory = false;
    try { isDirectory = fs.statSync(target).isDirectory(); } catch (e) { }
    if (isDirectory) {
      candidates = [path.join(target, "index.html"), path.join(target, "bundle.html")];
    }
    for (const candidate of candidates) {
      if (sendFile(response, candidate, head)) return;
    }

    response.writeHead(404, { "Content-Type": "text/plain" });
    response.end(head ? undefined : "not found\n");
  };

  const http = credentials ? require("https") : require("http");
  const server = credentials ? http.createServer(credentials, onRequest) : http.createServer(onRequest);

  // --- the WebSocket server --------------------------------------------------
  // Written out rather than required from npm, because a dependency here is a
  // dependency no deployed box has. The update ships dist/ and preserves
  // node_modules -- which is the right call, since node_modules holds native
  // builds for that host -- so an npm package this file reaches for is missing
  // on every box until somebody ssh`s in and installs it, and the failure lands
  // as a server that will not start. RFC 6455`s server half is a sha1 in the
  // handshake and a header of at most fourteen bytes per frame; owning that is
  // smaller than living with that failure mode. dist/ now needs nothing
  // installed beside it at all.
  //
  // Deliberately absent: permessage-deflate, which is never negotiated because
  // no extension is ever echoed -- these payloads are already-packed binary and
  // compressing them again would cost CPU per tick to save nothing.

  // The RFC`s constant. Hashed with the client`s key, it is the whole of the
  // handshake`s proof that a WebSocket server -- rather than something that
  // echoes headers -- read the request.
  const kAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  // The ceiling on one message, so that a corrupt or hostile length prefix
  // costs a dropped connection rather than the heap. protocol.h caps an
  // application frame at 1 MiB and transport.cpp hands the transport at most
  // 16 KiB at a time, so nothing legitimate comes close.
  const kMaxMessageBytes = 1 << 20;

  // A frame this side sends: FIN always set, never masked (the RFC forbids a
  // server masking), and header and payload in one allocation so they leave as
  // one write -- two would be two segments on a NODELAY socket.
  const encode = (opcode, payload) => {
    const length = payload.length;
    const headerBytes = length < 126 ? 2 : length < 65536 ? 4 : 10;
    const out = Buffer.allocUnsafe(headerBytes + length);
    out[0] = 0x80 | opcode;
    if (length < 126) {
      out[1] = length;
    } else if (length < 65536) {
      out[1] = 126;
      out.writeUInt16BE(length, 2);
    } else {
      out[1] = 127;
      // The high half of a 64-bit length. Zero for anything this sends, and
      // kMaxMessageBytes is what keeps that true.
      out.writeUInt32BE(0, 2);
      out.writeUInt32BE(length, 6);
    }
    out.set(payload, headerBytes);
    return out;
  };

  // Every upgraded socket, so that closing the listener closes them too rather
  // than leaving a process alive on sockets nobody will read again.
  const open = new Set();

  server.on("upgrade", (request, socket, head) => {
    const refuse = (status, reason, extra) => {
      socket.write("HTTP/1.1 " + status + " " + reason + "\r\n" +
                   (extra || "") + "Connection: close\r\n\r\n");
      socket.destroy();
    };

    const headers = request.headers;
    if (String(headers["upgrade"] || "").toLowerCase() !== "websocket") {
      return refuse(400, "Bad Request");
    }
    // 13 is the only version there has ever been, and the RFC`s answer to any
    // other is to say so rather than to guess.
    if (headers["sec-websocket-version"] !== "13") {
      return refuse(426, "Upgrade Required", "Sec-WebSocket-Version: 13\r\n");
    }
    const key = headers["sec-websocket-key"];
    if (!key) return refuse(400, "Bad Request");

    const response = [
      "HTTP/1.1 101 Switching Protocols",
      "Upgrade: websocket",
      "Connection: Upgrade",
      "Sec-WebSocket-Accept: " +
          crypto.createHash("sha1").update(key + kAcceptGuid).digest("base64"),
    ];
    // Echo the subprotocol when one is offered. Saying nothing means "no
    // subprotocol", which is a valid answer rather than a refusal.
    const offered = String(headers["sec-websocket-protocol"] || "")
                        .split(",").map((name) => name.trim());
    if (offered.indexOf("binary") >= 0) response.push("Sec-WebSocket-Protocol: binary");
    socket.write(response.join("\r\n") + "\r\n\r\n");
    // A tick`s worth of state is a few hundred bytes; waiting 40ms for a
    // fuller segment is the whole latency budget.
    socket.setNoDelay(true);

    const channel = net.channel();
    channel.kind = "websocket";
    channel.peer = (socket.remoteAddress || "") + ":" + (socket.remotePort || 0);
    channel.state = 1;
    channel.sender = (bytes) => socket.write(encode(0x2, bytes));
    // What Node is holding and has not handed to the kernel: the same question
    // bufferedAmount answers in the page, and the backpressure signal
    // writeAvailable() stops feeding.
    channel.bufferedFn = () => socket.writableLength || 0;
    channel.closer = () => { try { socket.end(encode(0x8, Buffer.alloc(0))); } catch (e) { } };
    open.add(socket);

    // A close this side starts: say why in the frame, then stop reading. The
    // socket is ended rather than destroyed so the close frame is actually
    // flushed, and paused so a peer that keeps talking cannot keep this
    // connection`s buffer growing after it has been given up on.
    const drop = (reason, code) => {
      const payload = Buffer.allocUnsafe(2);
      payload.writeUInt16BE(code, 0);
      try { socket.pause(); socket.end(encode(0x8, payload)); } catch (e) { }
      net.fail(channel, reason);
    };

    // `held` is what has arrived and has not parsed into a whole frame yet.
    // `fragments` is a message still being delivered across continuation
    // frames -- which no browser sends, and which the protocol allows.
    let held = head && head.length ? Buffer.from(head) : Buffer.alloc(0);
    let fragments = null;
    let fragmentBytes = 0;

    const consume = (chunk) => {
      if (channel.state === 2) return;
      if (chunk && chunk.length) {
        held = held.length === 0 ? chunk : Buffer.concat([held, chunk]);
      }
      for (;;) {
        if (held.length < 2) return;
        const first = held[0];
        const second = held[1];
        // No extension was negotiated, so a reserved bit means the peer is
        // speaking something this cannot read.
        if ((first & 0x70) !== 0) return drop("reserved bits set", 1002);
        const fin = (first & 0x80) !== 0;
        const opcode = first & 0x0f;
        const control = opcode >= 0x8;
        let length = second & 0x7f;
        let at = 2;
        if (length === 126) {
          if (held.length < 4) return;
          length = held.readUInt16BE(2);
          at = 4;
        } else if (length === 127) {
          if (held.length < 10) return;
          // The high half of the 64-bit length: refused rather than read,
          // since anything in it is orders of magnitude past the cap.
          if (held.readUInt32BE(2) !== 0) return drop("frame too large", 1009);
          length = held.readUInt32BE(6);
          at = 10;
        }
        // Three of the RFC`s rules, and the three a corrupt stream breaks
        // first: a client frame is always masked, a control frame is short and
        // never fragmented, and nothing may claim more than the cap.
        if ((second & 0x80) === 0) return drop("client frame is not masked", 1002);
        if (control && (length > 125 || !fin)) return drop("bad control frame", 1002);
        if (length > kMaxMessageBytes) return drop("frame too large", 1009);
        if (held.length < at + 4 + length) return;

        // Unmasked into a buffer of its own. What is left of `held` is a view
        // onto the same memory the next frame will be read from, so the
        // payload cannot be one.
        const mask = held.subarray(at, at + 4);
        at += 4;
        const payload = Buffer.allocUnsafe(length);
        for (let i = 0; i < length; ++i) payload[i] = held[at + i] ^ mask[i & 3];
        held = held.subarray(at + length);

        if (opcode === 0x8) {                  // close: echo the status back
          try { socket.end(encode(0x8, payload.subarray(0, 2))); } catch (e) { }
          net.fail(channel, "peer closed");
          return;
        }
        if (opcode === 0x9) {                  // ping: the pong carries it back
          try { socket.write(encode(0xa, payload)); } catch (e) { }
          continue;
        }
        if (opcode === 0xa) continue;          // pong, to a ping this never sends

        if (opcode === 0x0) {                  // continuation
          if (!fragments) return drop("continuation without a start", 1002);
          fragmentBytes += length;
          if (fragmentBytes > kMaxMessageBytes) return drop("message too large", 1009);
          fragments.push(payload);
          if (fin) {
            net.deliver(channel, Buffer.concat(fragments, fragmentBytes));
            fragments = null;
            fragmentBytes = 0;
          }
          continue;
        }
        if (opcode !== 0x1 && opcode !== 0x2) return drop("unknown opcode", 1002);
        if (fragments) return drop("message inside a message", 1002);
        // Text and binary arrive the same way: transport.cpp`s own length
        // prefix is what finds message boundaries, not the opcode.
        if (fin) { net.deliver(channel, payload); continue; }
        fragments = [payload];
        fragmentBytes = length;
      }
    };

    socket.on("data", (chunk) => {
      // A throw in here would be an uncaught exception on an event handler,
      // which takes the whole server down over one peer`s bytes.
      try { consume(chunk); } catch (e) { drop(e, 1011); }
    });
    socket.on("error", () => net.fail(channel, "websocket error"));
    socket.on("close", () => { open.delete(socket); net.fail(channel, "peer closed"); });

    listener.pending.push(net.alloc(channel));
    // The HTTP parser may have read past the handshake into the first frame.
    if (held.length) { try { consume(null); } catch (e) { drop(e, 1011); } }
  });

  server.listen(port);
  listener.closer = () => {
    for (const socket of open) { try { socket.destroy(); } catch (e) { } }
    open.clear();
    try { server.close(); } catch (e) { }
  };

  // WebTransport is best-effort and entirely optional: the QUIC stack is a
  // native dependency that may not be installed or may not match this host,
  // and it needs the certificate the plain listener can do without. Failing
  // any of that costs the deployment WebTransport and nothing else.
  if (credentials) {
    const esmImport = new Function("specifier", "return import(specifier)");
    esmImport("@fails-components/webtransport").then((module) => {
      const quic = new module.Http3Server({
        port: port,
        host: "::",
        // Signs the QUIC stack's address-validation tokens, which is what
        // stops a spoofed source address from getting a session. It never
        // leaves the process, so a fresh value per boot is right.
        secret: crypto.randomBytes(32).toString("hex"),
        cert: credentials.cert.toString(),
        privKey: credentials.key.toString(),
      });
      quic.startServer();
      return quic.ready.then(() => quic);
    }).then((quic) => {
      advertisement.webtransport = true;
      const previous = listener.closer;
      listener.closer = () => { previous(); try { quic.stopServer(); } catch (e) { } };
      console.log("[net] webtransport listening on udp/" + port);

      (async () => {
        const sessions = quic.sessionStream("/wt").getReader();
        for (;;) {
          const next = await sessions.read();
          if (next.done) break;
          const session = next.value;
          (async () => {
            const channel = net.channel();
            let slot = -1;
            try {
              await session.ready;
              const streams = session.incomingBidirectionalStreams.getReader();
              const first = await streams.read();
              if (first.done || !first.value) throw new Error("no stream");
              const writer = first.value.writable.getWriter();
              const reader = first.value.readable.getReader();
              channel.kind = "webtransport";
              channel.peer = String(session.peerAddress || "");
              channel.state = 1;
              channel.sender = (bytes) => {
                channel.outstanding += bytes.length;
                writer.write(bytes).then(
                  () => { channel.outstanding -= bytes.length; },
                  (e) => { channel.outstanding -= bytes.length; net.fail(channel, e); });
              };
              channel.bufferedFn = () => channel.outstanding;
              channel.closer = () => { try { session.close(); } catch (e) { } };
              slot = net.alloc(channel);
              listener.pending.push(slot);
              for (;;) {
                const result = await reader.read();
                if (result.done) break;
                net.deliver(channel, result.value);
              }
              net.fail(channel, "stream ended");
            } catch (e) {
              net.fail(channel, e);
              try { session.close(); } catch (ignored) { }
            }
          })();
        }
      })();
    }).catch((e) => {
      console.log("[net] webtransport unavailable (" + e + "); websocket only");
    });
  }

  return id;
});

EM_JS(int, flix_ch_accept, (int listenerId), {
  const listener = Module.flixNet.get(listenerId);
  if (!listener || !listener.listener || listener.pending.length === 0) return -1;
  return listener.pending.shift();
});

// --- both ------------------------------------------------------------------

EM_JS(int, flix_ch_state, (int id), {
  const slot = Module.flixNet.get(id);
  return slot ? slot.state : 2;
});

EM_JS(int, flix_ch_recv, (int id, char* out, int capacity), {
  const channel = Module.flixNet.get(id);
  if (!channel || channel.listener) return -1;
  let written = 0;
  while (written < capacity && channel.chunks.length > 0) {
    const chunk = channel.chunks[0];
    const take = Math.min(capacity - written, chunk.length - channel.headAt);
    HEAPU8.set(chunk.subarray(channel.headAt, channel.headAt + take), out + written);
    written += take;
    channel.headAt += take;
    channel.queued -= take;
    if (channel.headAt >= chunk.length) { channel.chunks.shift(); channel.headAt = 0; }
  }
  // Closed AND drained is the only end-of-stream: bytes that arrived before
  // the close still belong to the caller.
  if (written === 0 && channel.state === 2) return -1;
  return written;
});

EM_JS(int, flix_ch_send, (int id, const char* data, int size), {
  const channel = Module.flixNet.get(id);
  if (!channel || channel.listener || channel.state !== 1 || !channel.sender) return 0;
  try {
    // Copied out of the heap, not a view into it: the transport keeps the
    // bytes past this call and the heap can move under it when memory grows.
    channel.sender(HEAPU8.slice(data, data + size));
    return 1;
  } catch (e) {
    Module.flixNet.fail(channel, e);
    return 0;
  }
});

EM_JS(double, flix_ch_buffered, (int id), {
  const channel = Module.flixNet.get(id);
  if (!channel || channel.listener || !channel.bufferedFn) return 0;
  try { return channel.bufferedFn(); } catch (e) { return 0; }
});

EM_JS(void, flix_ch_close, (int id), {
  const net = Module.flixNet;
  const slot = net.get(id);
  if (!slot) return;
  if (slot.listener) {
    // Anything accepted but never handed to C++ is closed too, rather than
    // left holding a socket nobody will ever read.
    for (const pending of slot.pending) {
      const channel = net.get(pending);
      if (channel && channel.closer) { try { channel.closer(); } catch (e) { } }
      net.release(pending);
    }
    slot.pending.length = 0;
  }
  if (slot.closer) { try { slot.closer(); } catch (e) { } }
  slot.state = 2;
  net.release(id);
});

// The caller owns the buffer. The obvious shape -- return a malloc'd string --
// needs _malloc exported to JavaScript, which is a link setting a caller of
// this header would have no reason to expect it imposes.
EM_JS(void, flix_ch_text, (int id, int which, char* out, int capacity), {
  const slot = Module.flixNet.get(id);
  const value = !slot ? "" : (which === 0 ? (slot.peer || "")
                            : which === 1 ? (slot.kind || "")
                                          : String(slot.error || ""));
  stringToUTF8(value, out, capacity);
});

namespace flix::net::web {

namespace {

/// One init, lazily, so nothing has to be sequenced against startup order.
void ensureInit() {
    static bool done = false;
    if (done) return;
    flix_net_init();
    done = true;
}

/// Peers, transport names and close reasons are all short; a caller that
/// wanted more would be logging a JavaScript exception, which is worth
/// truncating rather than allocating for.
std::string text(int channel, int which) {
    char buffer[256] = {0};
    flix_ch_text(channel, which, buffer, static_cast<int>(sizeof buffer));
    return std::string(buffer);
}

} // namespace

bool available() { return true; }

int connect(const std::string& host, std::uint16_t port) {
    ensureInit();
    return flix_ch_connect(host.c_str(), static_cast<int>(port));
}

int listen(std::uint16_t port, const std::string& certPath, const std::string& keyPath,
           const std::string& webRoot) {
    ensureInit();
    return flix_ch_listen(static_cast<int>(port), certPath.c_str(), keyPath.c_str(),
                         webRoot.c_str());
}

int accept(int listener) { return flix_ch_accept(listener); }

State state(int channel) { return static_cast<State>(flix_ch_state(channel)); }

int recv(int channel, void* buffer, int capacity) {
    return flix_ch_recv(channel, static_cast<char*>(buffer), capacity);
}

bool send(int channel, const void* data, int size) {
    return flix_ch_send(channel, static_cast<const char*>(data), size) != 0;
}

std::size_t buffered(int channel) {
    const double bytes = flix_ch_buffered(channel);
    return bytes > 0 ? static_cast<std::size_t>(bytes) : 0;
}

void close(int channel) { flix_ch_close(channel); }

std::string peer(int channel) { return text(channel, 0); }
std::string kind(int channel) { return text(channel, 1); }
std::string error(int channel) { return text(channel, 2); }

} // namespace flix::net::web

#else   // !__EMSCRIPTEN__

namespace flix::net::web {

// Natively there is no JavaScript runtime and transport.cpp uses real sockets;
// these exist so a caller can ask without an #ifdef of its own.
bool available() { return false; }
int connect(const std::string&, std::uint16_t) { return kInvalid; }
int listen(std::uint16_t, const std::string&, const std::string&, const std::string&) {
    return kInvalid;
}
int accept(int) { return kInvalid; }
State state(int) { return State::Closed; }
int recv(int, void*, int) { return -1; }
bool send(int, const void*, int) { return false; }
std::size_t buffered(int) { return 0; }
void close(int) {}
std::string peer(int) { return {}; }
std::string kind(int) { return {}; }
std::string error(int) { return {}; }

} // namespace flix::net::web

#endif
