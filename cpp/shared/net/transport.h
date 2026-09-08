#pragma once
// Length-prefixed message transport over TCP.
//
// One poll() loop drives every connection. At this game's scale -- a few
// hundred sockets on one listener -- poll() costs less than the complexity of
// keeping an epoll and a kqueue path honest, and it is the same code on Linux
// and macOS.
//
// Both sides speak the same framing: [u32 length][payload]. Reads accumulate
// until a whole frame is present; writes queue and drain as the socket allows,
// so a slow client backs up in its own buffer instead of blocking the tick.
//
// There are two backends under all of this, and only the byte movement differs
// -- the framing, the backlog rule and the TransportHandler contract are one
// implementation shared by both:
//
//   native       a TCP socket and poll(), as described above.
//   emscripten   whatever the JavaScript runtime has, via net/web_channel.h:
//                WebTransport when the runtime and the server both offer it,
//                WebSocket otherwise, and an in-page loopback when the server
//                is in the same page as the client. `fd()` there is a channel
//                handle rather than a descriptor, and poll()'s timeout is
//                ignored, because neither a browser tab nor a Node event loop
//                may be blocked.

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "shared/net/bytebuffer.h"
#include "shared/net/protocol.h"
#include "shared/net/web_channel.h"

namespace flix::net {

/// One accepted socket and its buffers.
class Connection {
public:
    Connection(int fd, ConnectionId id, std::string peer);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    ConnectionId id() const { return id_; }
    const std::string& peer() const { return peer_; }
    int fd() const { return fd_; }
    bool open() const { return fd_ >= 0; }

    /// Queues a frame. Never blocks; the bytes leave in later drains.
    void send(const ByteWriter& message);
    void send(const std::byte* data, std::size_t size);

    /// Bytes queued but not yet handed to the kernel. The server drops a
    /// connection whose backlog grows without bound rather than buying memory
    /// on behalf of a client that has stopped reading.
    std::size_t pendingBytes() const { return outbound_.size() - outboundSent_; }

    /// Marks the connection for close once its queued bytes have drained.
    void closeGracefully() { closing_ = true; }
    bool closing() const { return closing_; }

    /// Arbitrary owner state (the session, the player entity). The transport
    /// never looks inside it.
    void* user = nullptr;

private:
    friend class Listener;
    friend class Dialer;

    /// Reads what the socket has. Returns false when the peer hung up or the
    /// connection broke its framing contract.
    bool readAvailable(std::string& errorOut);
    /// Writes what the socket will take. Returns false on a fatal error.
    bool writeAvailable(std::string& errorOut);
    /// Pops one complete frame into `out`. False when none is buffered yet.
    bool nextFrame(std::vector<std::byte>& out);
    /// Bytes the transport itself still holds, on top of what this object has
    /// queued. Zero on a socket, where the kernel's buffer is not ours to see;
    /// the web backend's real backpressure signal.
    std::size_t transportBuffered() const;
    bool wantsWrite() const { return outboundSent_ < outbound_.size(); }
    /// Drops what has already gone out. Called at the end of every write
    /// drain, by both backends.
    void compactOutbound();
    void shutdownNow();

    int fd_;
    ConnectionId id_;
    std::string peer_;
    bool closing_ = false;

    std::vector<std::byte> inbound_;
    std::size_t inboundConsumed_ = 0;

    std::vector<std::byte> outbound_;
    std::size_t outboundSent_ = 0;
};

/// Callbacks a transport owner implements. All are invoked from poll().
struct TransportHandler {
    virtual ~TransportHandler() = default;
    virtual void onConnect(Connection& c) {}
    virtual void onMessage(Connection& c, ByteReader& reader) {}
    virtual void onDisconnect(Connection& c, const std::string& reason) {}
};

/// Accepts connections on a TCP port.
class Listener {
public:
    Listener();
    ~Listener();

    /// Binds and listens. Returns false with `errorOut` set on failure.
    bool start(std::uint16_t port, std::string& errorOut);
    void stop();
    bool listening() const { return listenFd_ >= 0; }
    std::uint16_t port() const { return port_; }

    /// Services sockets for up to `timeoutMillis`, dispatching to `handler`.
    /// Returns the number of frames delivered.
    int poll(TransportHandler& handler, int timeoutMillis);

    /// Pushes queued bytes out without waiting for readability. Called at the
    /// end of a tick so a snapshot leaves immediately rather than after the
    /// next poll timeout.
    void flush();

    Connection* find(ConnectionId id);
    std::size_t connectionCount() const { return connections_.size(); }

    /// Runs `fn` for every live connection.
    void each(const std::function<void(Connection&)>& fn);

    /// Disconnects `id` after its queued bytes drain.
    void close(ConnectionId id);

    /// A connection queuing more than this is dropped: it has stopped reading,
    /// and buffering a snapshot stream for it costs the server unbounded memory.
    std::size_t maxPendingBytes = 4u << 20;   // 4 MiB

    /// TLS material for the web backend, set before start(). Both empty means
    /// the conventional pair names are looked for in the working directory;
    /// finding none leaves the listener on plain HTTP and WebSocket only,
    /// because WebTransport is secure-context only and there would be nothing
    /// to offer. Ignored natively, which speaks TCP and does no TLS at all.
    std::string certPath;
    std::string keyPath;

    /// Directory the web backend serves over the same HTTP(S) listener. Empty
    /// means the directory the program was loaded from, which is where the
    /// client build sits. Ignored natively, which serves no files at all.
    std::string webRoot;

private:
    void acceptPending(TransportHandler& handler);
    void drop(Connection& c, TransportHandler& handler, const std::string& reason);
    /// Everything that happens to one connection once the transport says it
    /// may read and/or write: the write drain, the read, the frame loop, and
    /// the three conditions that end a connection. Returns false when the
    /// connection should be dropped, with `error` saying why.
    ///
    /// Shared by both backends deliberately. They differ in how they learn a
    /// connection is ready -- poll() revents against a JavaScript queue -- and
    /// in nothing else; a copy of these rules per backend is how they would
    /// come to differ in more.
    bool service(Connection& c, bool readable, bool writable, TransportHandler& handler,
                 int& delivered, std::string& error);

    int listenFd_ = -1;
    std::uint16_t port_ = 0;
    ConnectionId nextId_ = 1;
    std::vector<std::unique_ptr<Connection>> connections_;
    std::vector<std::byte> frameScratch_;
};

/// The client end: one outgoing connection.
class Dialer {
public:
    Dialer();
    ~Dialer();

    enum class State { Idle, Connecting, Connected, Failed };

    /// Starts a non-blocking connect. Progress is made inside poll(), so the
    /// caller's frame loop never stalls on a slow or dead host.
    bool connect(const std::string& host, std::uint16_t port, std::string& errorOut);
    void disconnect();

    State state() const { return state_; }
    bool connected() const { return state_ == State::Connected; }
    const std::string& error() const { return error_; }

    /// Queues a frame. Safe before the connect completes; it drains after.
    void send(const ByteWriter& message);

    /// Services the socket for up to `timeoutMillis` and dispatches frames.
    /// Returns the number delivered.
    int poll(TransportHandler& handler, int timeoutMillis);

    void flush();

private:
    void fail(const std::string& reason, TransportHandler* handler);

    std::unique_ptr<Connection> connection_;
    State state_ = State::Idle;
    std::string error_;
    std::vector<std::byte> frameScratch_;
    bool announced_ = false;
};

} // namespace flix::net
