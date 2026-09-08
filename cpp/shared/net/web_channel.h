#pragma once
// The transports available inside a JavaScript runtime, as a byte channel.
//
// Natively, transport.cpp moves bytes with socket(), connect(), accept() and
// recv(). Neither half of an emscripten build can: a browser tab has no TCP,
// and while emscripten can emulate BSD sockets over a WebSocket, that emulation
// offers WebSocket and nothing else. This is the seam that lets the same
// Connection/Listener/Dialer speak whatever the runtime actually has:
//
//   WebTransport   HTTP/3 over QUIC, when the runtime implements it, the page
//                  is a secure context, and the server advertises it. One
//                  bidirectional stream carries everything.
//   WebSocket      always available, and the fallback whenever anything about
//                  the WebTransport attempt does not work out.
//   loopback       a pair of queues, when the server is in the same page as
//                  the client. listen() in a page registers an in-page
//                  listener on its port, and a connect() to 127.0.0.1 or
//                  localhost on that port reaches it without touching the
//                  network. This is how the single-file offline build plays:
//                  one wasm carrying both halves, talking to itself.
//
// All three are presented here as an ordered byte stream, because that is what the
// layer above already expects: transport.cpp's `[u32 length][payload]` framing
// is what finds message boundaries, and it does so identically whether the
// bytes arrived as discrete WebSocket messages or as a QUIC stream that split
// and coalesced them however it liked.
//
// Everything is non-blocking. connect() and listen() return immediately and
// the handshake finishes later; state() is how the caller finds out, polled
// once a frame, because neither runtime lets us block and wait.

#include <cstddef>
#include <cstdint>
#include <string>

namespace flix::net::web {

/// No channel. Returned by every call that could not produce one.
inline constexpr int kInvalid = -1;

enum class State : int {
    Connecting = 0,
    Open = 1,
    Closed = 2,
};

/// Whether this build has a JavaScript runtime under it at all. False
/// natively, where every call below is a stub that fails.
bool available();

/// Starts a client connection to `host`:`port`. Deliberately not a URL: which
/// scheme and which transport are finally used is this layer's decision, not
/// the caller's, and a page inherits its own -- a client served over https
/// must not dial ws://, which the browser would refuse anyway.
///
/// Returns a channel in State::Connecting, or kInvalid. The transport is
/// chosen while it connects: /transport-info is asked what the server offers,
/// WebTransport is tried when everything allows it, and WebSocket is used
/// otherwise or after any failure.
int connect(const std::string& host, std::uint16_t port);

/// Starts a server listener on `port`.
///
/// `certPath`/`keyPath` name the TLS material. Both empty means "find it": the
/// conventional pair names are looked for in the working directory, exactly as
/// the TypeScript server does, and a real certificate is preferred to a
/// development one. Finding none leaves the listener on plain HTTP and
/// WebSocket only -- WebTransport is secure-context only, so without a
/// certificate there is nothing to offer.
///
/// `webRoot` is the directory served over that same HTTP(S) listener; empty
/// means the directory the program was loaded from, which is where the client
/// build sits. One port serves the page, the WebSocket and the QUIC listener,
/// so a client is same-origin with its server and needs nothing else running.
///
/// In a page there is nothing to serve and nobody to bind for; the listener
/// is an in-page one that connect() calls from the same page resolve to, and
/// the certificate and web root are ignored.
///
/// Returns a listener handle, or kInvalid when the port is already listened
/// on in this runtime.
int listen(std::uint16_t port, const std::string& certPath, const std::string& keyPath,
           const std::string& webRoot);

/// The next accepted channel, or kInvalid when none is waiting.
int accept(int listener);

State state(int channel);

/// Copies up to `capacity` received bytes. Returns the count, 0 when nothing
/// has arrived, and -1 when the channel is closed and drained.
int recv(int channel, void* buffer, int capacity);

/// Queues `size` bytes. False when the channel is not open.
bool send(int channel, const void* data, int size);

/// Bytes handed to the transport that are not yet on the wire. This is the
/// backpressure signal: the caller stops feeding a channel whose transport is
/// already behind, exactly as a native socket's EAGAIN makes it stop.
std::size_t buffered(int channel);

void close(int channel);

/// The peer as the transport saw it, for logging. Never client-supplied.
std::string peer(int channel);

/// "websocket", "webtransport" or "loopback", once the channel is open.
std::string kind(int channel);

/// Why a channel closed, when it closed for a reason worth reporting.
std::string error(int channel);

} // namespace flix::net::web
