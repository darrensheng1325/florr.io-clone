#include "shared/net/transport.h"
#include "shared/net/protocol.h"

#include <cerrno>
#include <cstring>

#ifndef __EMSCRIPTEN__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>

namespace flix::net {

namespace {

constexpr std::size_t kFrameHeaderBytes = 4;
/// Read chunk. Big enough that a burst of client input arrives in one call,
/// small enough that an idle connection is not holding a page per socket.
constexpr std::size_t kReadChunk = 16 * 1024;
/// Once this many consumed bytes pile up at the front of the inbound buffer,
/// compact it. Erasing after every frame would memmove the remainder each
/// time; waiting forever would grow the buffer without bound.
constexpr std::size_t kCompactThreshold = 64 * 1024;

#ifndef __EMSCRIPTEN__
bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void setNoDelay(int fd) {
    // Snapshots are small and latency-critical; Nagle would hold a partial
    // one back waiting for company it will not get.
    int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

std::string describePeer(const sockaddr_storage& addr) {
    char host[INET6_ADDRSTRLEN] = {0};
    std::uint16_t port = 0;
    if (addr.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&addr);
        ::inet_ntop(AF_INET, &v4->sin_addr, host, sizeof host);
        port = ntohs(v4->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        ::inet_ntop(AF_INET6, &v6->sin6_addr, host, sizeof host);
        port = ntohs(v6->sin6_port);
    }
    return std::string(host) + ":" + std::to_string(port);
}

bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
#else
/// How far ahead of the transport this side will run before it stops feeding a
/// channel. A socket says "not now" with EAGAIN; a WebSocket or a QUIC stream
/// says it by holding bytes, so the stop has to be a number. One read chunk is
/// enough to keep the wire busy without handing an unbounded queue to a peer
/// that has stopped reading -- the connection's own backlog cap is what
/// finally drops it.
constexpr std::size_t kWebSendHighWater = 256 * 1024;
#endif

} // namespace

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

Connection::Connection(int fd, ConnectionId id, std::string peer)
    : fd_(fd), id_(id), peer_(std::move(peer)) {}

Connection::~Connection() { shutdownNow(); }

void Connection::shutdownNow() {
    if (fd_ < 0) return;
#ifdef __EMSCRIPTEN__
    web::close(fd_);
#else
    ::close(fd_);
#endif
    fd_ = -1;
}

void Connection::send(const std::byte* data, std::size_t size) {
    if (fd_ < 0 || closing_) return;
    const std::uint32_t length = static_cast<std::uint32_t>(size);
    const std::size_t at = outbound_.size();
    outbound_.resize(at + kFrameHeaderBytes + size);
    std::memcpy(outbound_.data() + at, &length, sizeof length);
    if (size) std::memcpy(outbound_.data() + at + kFrameHeaderBytes, data, size);
}

void Connection::send(const ByteWriter& message) { send(message.data(), message.size()); }

#ifdef __EMSCRIPTEN__

bool Connection::readAvailable(std::string& errorOut) {
    while (true) {
        const std::size_t at = inbound_.size();
        inbound_.resize(at + kReadChunk);
        const int n = web::recv(fd_, inbound_.data() + at, static_cast<int>(kReadChunk));
        if (n > 0) {
            inbound_.resize(at + static_cast<std::size_t>(n));
            if (static_cast<std::size_t>(n) < kReadChunk) return true;
            continue;
        }
        inbound_.resize(at);
        if (n == 0) return true;   // nothing waiting, which is not an end
        const std::string reason = web::error(fd_);
        errorOut = reason.empty() ? "peer closed" : reason;
        return false;
    }
}

bool Connection::writeAvailable(std::string& errorOut) {
    while (outboundSent_ < outbound_.size()) {
        // The transport's own queue is the EAGAIN here: past the high water
        // mark this side stops feeding it and the rest waits for the next
        // pass, which is what keeps a slow peer's backlog on this object --
        // where maxPendingBytes can see and cap it -- rather than inside a
        // JavaScript object that grows without anybody counting.
        if (web::buffered(fd_) >= kWebSendHighWater) break;
        const std::size_t remaining = outbound_.size() - outboundSent_;
        const std::size_t chunk = std::min(remaining, kReadChunk);
        if (!web::send(fd_, outbound_.data() + outboundSent_, static_cast<int>(chunk))) {
            const std::string reason = web::error(fd_);
            errorOut = reason.empty() ? "send failed" : reason;
            return false;
        }
        outboundSent_ += chunk;
    }
    compactOutbound();
    return true;
}

std::size_t Connection::transportBuffered() const {
    return fd_ >= 0 ? web::buffered(fd_) : 0;
}

#else

bool Connection::readAvailable(std::string& errorOut) {
    while (true) {
        const std::size_t at = inbound_.size();
        inbound_.resize(at + kReadChunk);
        const ssize_t n = ::recv(fd_, inbound_.data() + at, kReadChunk, 0);
        if (n > 0) {
            inbound_.resize(at + static_cast<std::size_t>(n));
            // A short read means the socket is drained; going round again
            // would only earn an EAGAIN.
            if (static_cast<std::size_t>(n) < kReadChunk) return true;
            continue;
        }
        inbound_.resize(at);
        if (n == 0) { errorOut = "peer closed"; return false; }
        if (wouldBlock()) return true;
        errorOut = std::strerror(errno);
        return false;
    }
}

bool Connection::writeAvailable(std::string& errorOut) {
    while (outboundSent_ < outbound_.size()) {
        const std::size_t remaining = outbound_.size() - outboundSent_;
        const ssize_t n = ::send(fd_, outbound_.data() + outboundSent_, remaining,
#ifdef MSG_NOSIGNAL
                                 MSG_NOSIGNAL
#else
                                 0
#endif
        );
        if (n > 0) {
            outboundSent_ += static_cast<std::size_t>(n);
            continue;
        }
        if (wouldBlock()) break;
        errorOut = std::strerror(errno);
        return false;
    }

    compactOutbound();
    return true;
}

std::size_t Connection::transportBuffered() const { return 0; }

#endif  // __EMSCRIPTEN__

void Connection::compactOutbound() {
    if (outboundSent_ == outbound_.size()) {
        outbound_.clear();
        outboundSent_ = 0;
    } else if (outboundSent_ > kCompactThreshold) {
        outbound_.erase(outbound_.begin(),
                        outbound_.begin() + static_cast<std::ptrdiff_t>(outboundSent_));
        outboundSent_ = 0;
    }
}

bool Connection::nextFrame(std::vector<std::byte>& out) {
    const std::size_t available = inbound_.size() - inboundConsumed_;
    if (available < kFrameHeaderBytes) return false;

    std::uint32_t length = 0;
    std::memcpy(&length, inbound_.data() + inboundConsumed_, sizeof length);
    if (length > kMaxFrameBytes) {
        // Refuse before allocating: a corrupt or hostile prefix must cost a
        // dropped connection, not a gigabyte.
        closing_ = true;
        return false;
    }
    if (available < kFrameHeaderBytes + length) return false;

    const std::byte* body = inbound_.data() + inboundConsumed_ + kFrameHeaderBytes;
    out.assign(body, body + length);
    inboundConsumed_ += kFrameHeaderBytes + length;

    if (inboundConsumed_ == inbound_.size()) {
        inbound_.clear();
        inboundConsumed_ = 0;
    } else if (inboundConsumed_ > kCompactThreshold) {
        inbound_.erase(inbound_.begin(),
                       inbound_.begin() + static_cast<std::ptrdiff_t>(inboundConsumed_));
        inboundConsumed_ = 0;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

Listener::Listener() = default;
Listener::~Listener() { stop(); }

#ifdef __EMSCRIPTEN__

bool Listener::start(std::uint16_t port, std::string& errorOut) {
    stop();
    listenFd_ = web::listen(port, certPath, keyPath, webRoot);
    if (listenFd_ < 0) {
        // Node binds a real port; a page keeps an in-page listener per port
        // for connections made from the same page. Either way one port is
        // one listener, so this is the bind() failure of the web backend.
        errorOut = "port " + std::to_string(port) + " is already listened on in this runtime";
        return false;
    }
    port_ = port;
    return true;
}

void Listener::stop() {
    connections_.clear();
    if (listenFd_ >= 0) {
        web::close(listenFd_);
        listenFd_ = -1;
    }
}

void Listener::acceptPending(TransportHandler& handler) {
    while (true) {
        const int channel = web::accept(listenFd_);
        if (channel < 0) break;
        auto connection =
            std::make_unique<Connection>(channel, nextId_++, web::peer(channel));
        Connection& ref = *connection;
        connections_.push_back(std::move(connection));
        handler.onConnect(ref);
    }
}

#else

bool Listener::start(std::uint16_t port, std::string& errorOut) {
    stop();

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) { errorOut = std::strerror(errno); return false; }

    int on = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        errorOut = "bind: " + std::string(std::strerror(errno));
        stop();
        return false;
    }
    if (::listen(listenFd_, 128) != 0) {
        errorOut = "listen: " + std::string(std::strerror(errno));
        stop();
        return false;
    }
    if (!setNonBlocking(listenFd_)) {
        errorOut = "fcntl: " + std::string(std::strerror(errno));
        stop();
        return false;
    }

    port_ = port;
    return true;
}

void Listener::stop() {
    connections_.clear();
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

void Listener::acceptPending(TransportHandler& handler) {
    while (true) {
        sockaddr_storage addr{};
        socklen_t addrLen = sizeof addr;
        const int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&addr), &addrLen);
        if (fd < 0) {
            // EMFILE means the process is out of descriptors. Stopping the
            // accept loop leaves the pending connection queued rather than
            // spinning on the same error for the rest of the tick.
            break;
        }
        if (!setNonBlocking(fd)) { ::close(fd); continue; }
        setNoDelay(fd);

        auto connection = std::make_unique<Connection>(fd, nextId_++, describePeer(addr));
        Connection& ref = *connection;
        connections_.push_back(std::move(connection));
        handler.onConnect(ref);
    }
}

#endif  // __EMSCRIPTEN__

Connection* Listener::find(ConnectionId id) {
    for (auto& c : connections_) {
        if (c->id() == id) return c.get();
    }
    return nullptr;
}

void Listener::each(const std::function<void(Connection&)>& fn) {
    // Index rather than iterate: a callback may queue a close, and while that
    // does not remove entries here, keeping this index-based makes it safe if
    // it ever does.
    for (std::size_t i = 0; i < connections_.size(); ++i) fn(*connections_[i]);
}

void Listener::close(ConnectionId id) {
    if (Connection* c = find(id)) c->closeGracefully();
}

void Listener::drop(Connection& c, TransportHandler& handler, const std::string& reason) {
    handler.onDisconnect(c, reason);
    c.shutdownNow();
}

bool Listener::service(Connection& c, bool readable, bool writable, TransportHandler& handler,
                      int& delivered, std::string& error) {
    if (writable && !c.writeAvailable(error)) return false;

    if (readable) {
        const bool alive = c.readAvailable(error);
        while (c.nextFrame(frameScratch_)) {
            ByteReader reader(frameScratch_.data(), frameScratch_.size());
            handler.onMessage(c, reader);
            ++delivered;
            // A handler may have closed this connection out from under us.
            if (!c.open()) return false;
        }
        if (!alive) return false;
    }

    // Counted with what the transport is still holding: on the web backend
    // most of a slow peer's backlog lives there, and a cap that only saw this
    // object's half would never fire.
    if (c.pendingBytes() + c.transportBuffered() > maxPendingBytes) {
        error = "outbound backlog exceeded";
        return false;
    }
    if (c.closing() && !c.wantsWrite()) {
        error = "closed by server";
        return false;
    }
    return true;
}

#ifdef __EMSCRIPTEN__

int Listener::poll(TransportHandler& handler, int timeoutMillis) {
    if (listenFd_ < 0) return 0;
    // The timeout is deliberately ignored. Bytes reach a channel from the
    // JavaScript event loop, which only runs when this stack has returned to
    // it, so sleeping here would not wait for data -- it would prevent it.
    (void)timeoutMillis;

    acceptPending(handler);

    int delivered = 0;
    for (std::size_t i = 0; i < connections_.size(); ++i) {
        Connection& c = *connections_[i];
        if (!c.open()) continue;
        std::string error;
        // Both directions every pass: a channel has no readiness to report,
        // only a queue that is empty or is not, and both calls handle empty.
        if (!service(c, true, true, handler, delivered, error)) {
            drop(c, handler, error.empty() ? "disconnected" : error);
        }
    }

    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
                       [](const std::unique_ptr<Connection>& c) { return !c->open(); }),
        connections_.end());

    return delivered;
}

#else

int Listener::poll(TransportHandler& handler, int timeoutMillis) {
    if (listenFd_ < 0) return 0;

    std::vector<pollfd> fds;
    fds.reserve(connections_.size() + 1);
    fds.push_back(pollfd{listenFd_, POLLIN, 0});
    for (auto& c : connections_) {
        if (!c->open()) continue;
        short events = POLLIN;
        if (c->wantsWrite()) events |= POLLOUT;
        fds.push_back(pollfd{c->fd(), events, 0});
    }

    const int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMillis);
    if (ready < 0 && !wouldBlock()) return 0;

    if (ready > 0 && (fds[0].revents & POLLIN)) acceptPending(handler);

    int delivered = 0;

    // Walk connections by fd so the poll results line up even though
    // acceptPending() may have appended new entries since fds was built.
    std::size_t slot = 1;
    for (std::size_t i = 0; i < connections_.size() && slot < fds.size(); ++i) {
        Connection& c = *connections_[i];
        if (!c.open() || fds[slot].fd != c.fd()) continue;
        const short revents = fds[slot].revents;
        ++slot;
        if (revents == 0) continue;

        std::string error;
        if (!service(c, (revents & (POLLIN | POLLHUP | POLLERR)) != 0, (revents & POLLOUT) != 0,
                     handler, delivered, error)) {
            drop(c, handler, error.empty() ? "disconnected" : error);
        }
    }

    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
                       [](const std::unique_ptr<Connection>& c) { return !c->open(); }),
        connections_.end());

    return delivered;
}

#endif  // __EMSCRIPTEN__

void Listener::flush() {
    for (auto& c : connections_) {
        if (!c->open() || !c->wantsWrite()) continue;
        std::string error;
        if (!c->writeAvailable(error)) c->shutdownNow();
    }
}

// ---------------------------------------------------------------------------
// Dialer
// ---------------------------------------------------------------------------

Dialer::Dialer() = default;
Dialer::~Dialer() { disconnect(); }

#ifdef __EMSCRIPTEN__

bool Dialer::connect(const std::string& host, std::uint16_t port, std::string& errorOut) {
    disconnect();

    const int channel = web::connect(host, port);
    if (channel < 0) {
        errorOut = "no transport available for " + host + ":" + std::to_string(port);
        state_ = State::Failed;
        error_ = errorOut;
        return false;
    }
    // The peer string is provisional: which transport was chosen is not known
    // until the handshake resolves, and poll() replaces this with what the
    // channel says once it is open.
    connection_ = std::make_unique<Connection>(channel, 1, host + ":" + std::to_string(port));
    state_ = State::Connecting;
    announced_ = false;
    error_.clear();
    return true;
}

int Dialer::poll(TransportHandler& handler, int timeoutMillis) {
    if (!connection_ || !connection_->open()) return 0;
    // Ignored, as in Listener::poll: the handshake and every byte after it
    // arrive from the JavaScript event loop, which is not running while this
    // call is on the stack.
    (void)timeoutMillis;

    const web::State channel = web::state(connection_->fd());
    if (channel == web::State::Closed) {
        const std::string reason = web::error(connection_->fd());
        fail(reason.empty() ? "connection closed" : reason, &handler);
        return 0;
    }
    if (state_ == State::Connecting) {
        if (channel != web::State::Open) return 0;
        state_ = State::Connected;
        announced_ = true;
        handler.onConnect(*connection_);
    }

    std::string error;
    if (!connection_->writeAvailable(error)) {
        fail(error.empty() ? "write failed" : error, &handler);
        return 0;
    }

    int delivered = 0;
    const bool alive = connection_->readAvailable(error);
    while (connection_ && connection_->nextFrame(frameScratch_)) {
        ByteReader reader(frameScratch_.data(), frameScratch_.size());
        handler.onMessage(*connection_, reader);
        ++delivered;
    }
    if (!alive) {
        fail(error.empty() ? "disconnected" : error, &handler);
        return delivered;
    }
    return delivered;
}

#else

bool Dialer::connect(const std::string& host, std::uint16_t port, std::string& errorOut) {
    disconnect();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
    if (rc != 0 || !results) {
        errorOut = "cannot resolve " + host + ": " + ::gai_strerror(rc);
        state_ = State::Failed;
        error_ = errorOut;
        return false;
    }

    int fd = -1;
    for (addrinfo* it = results; it; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (!setNonBlocking(fd)) { ::close(fd); fd = -1; continue; }
        setNoDelay(fd);

        const int result = ::connect(fd, it->ai_addr, it->ai_addrlen);
        // EINPROGRESS is the expected answer for a non-blocking connect; the
        // handshake finishes when poll() reports the socket writable.
        if (result == 0 || errno == EINPROGRESS) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(results);

    if (fd < 0) {
        errorOut = "cannot connect to " + host + ":" + service + ": " + std::strerror(errno);
        state_ = State::Failed;
        error_ = errorOut;
        return false;
    }

    connection_ = std::make_unique<Connection>(fd, 1, host + ":" + service);
    state_ = State::Connecting;
    announced_ = false;
    error_.clear();
    return true;
}

int Dialer::poll(TransportHandler& handler, int timeoutMillis) {
    if (!connection_ || !connection_->open()) return 0;

    short events = POLLIN;
    if (state_ == State::Connecting || connection_->wantsWrite()) events |= POLLOUT;

    pollfd fd{connection_->fd(), events, 0};
    const int ready = ::poll(&fd, 1, timeoutMillis);
    if (ready <= 0) return 0;

    if (state_ == State::Connecting) {
        int soError = 0;
        socklen_t len = sizeof soError;
        ::getsockopt(connection_->fd(), SOL_SOCKET, SO_ERROR, &soError, &len);
        if (soError != 0) {
            fail(std::strerror(soError), &handler);
            return 0;
        }
        if (fd.revents & (POLLOUT | POLLIN)) {
            state_ = State::Connected;
            announced_ = true;
            handler.onConnect(*connection_);
        }
    }

    std::string error;
    if ((fd.revents & POLLOUT) && !connection_->writeAvailable(error)) {
        fail(error.empty() ? "write failed" : error, &handler);
        return 0;
    }

    int delivered = 0;
    if (fd.revents & (POLLIN | POLLHUP | POLLERR)) {
        const bool alive = connection_->readAvailable(error);
        while (connection_ && connection_->nextFrame(frameScratch_)) {
            ByteReader reader(frameScratch_.data(), frameScratch_.size());
            handler.onMessage(*connection_, reader);
            ++delivered;
        }
        if (!alive) {
            fail(error.empty() ? "disconnected" : error, &handler);
            return delivered;
        }
    }
    return delivered;
}

#endif  // __EMSCRIPTEN__

void Dialer::disconnect() {
    connection_.reset();
    state_ = State::Idle;
    announced_ = false;
}

void Dialer::send(const ByteWriter& message) {
    if (connection_) connection_->send(message);
}

void Dialer::fail(const std::string& reason, TransportHandler* handler) {
    if (handler && connection_ && announced_) handler->onDisconnect(*connection_, reason);
    connection_.reset();
    state_ = State::Failed;
    error_ = reason;
}

void Dialer::flush() {
    if (!connection_ || !connection_->open() || state_ != State::Connected) return;
    std::string error;
    if (!connection_->writeAvailable(error)) fail(error, nullptr);
}

} // namespace flix::net
