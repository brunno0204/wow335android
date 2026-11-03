#include "NativeTcp.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
namespace wow::android {
NativeTcp::~NativeTcp() { close(); }
NativeTcp::NativeTcp(NativeTcp &&other) noexcept { *this = std::move(other); }
NativeTcp &NativeTcp::operator=(NativeTcp &&other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::exchange(other.socket_, -1);
        error_ = std::exchange(other.error_, 0);
        state_ = std::exchange(other.state_, State::Closed);
        deadline_ = other.deadline_;
        outgoing_ = std::move(other.outgoing_);
        incoming_ = std::move(other.incoming_);
        sent_ = std::exchange(other.sent_, 0);
        other.outgoing_.clear();
        other.incoming_.clear();
    }
    return *this;
}
void NativeTcp::finish(State state, int error) noexcept {
    if (socket_ >= 0)
        ::close(socket_);
    socket_ = -1;
    state_ = state;
    error_ = error;
    outgoing_.clear();
    sent_ = 0;
}
void NativeTcp::close() noexcept {
    finish(State::Closed);
    incoming_.clear();
}
void NativeTcp::connect(const std::string &address, uint16_t port, uint32_t timeoutMs) {
    if (!port || !timeoutMs || timeoutMs > 60000 || address.empty() || address.size() > 45 ||
        address.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid TCP endpoint/deadline");
    sockaddr_storage storage{};
    sockaddr_in v4{};
    sockaddr_in6 v6{};
    socklen_t length = 0;
    int family = 0;
    if (inet_pton(AF_INET, address.c_str(), &v4.sin_addr) == 1) {
        family = AF_INET;
        v4.sin_family = family;
        v4.sin_port = htons(port);
        length = sizeof(v4);
        std::memcpy(&storage, &v4, length);
    } else if (inet_pton(AF_INET6, address.c_str(), &v6.sin6_addr) == 1) {
        family = AF_INET6;
        v6.sin6_family = family;
        v6.sin6_port = htons(port);
        length = sizeof(v6);
        std::memcpy(&storage, &v6, length);
    } else
        throw std::invalid_argument("TCP currently requires a numeric IP address");
    close();
    socket_ = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (socket_ < 0) {
        finish(State::Failed, errno);
        return;
    }
    const int enabled = 1;
    if (setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
        finish(State::Failed, errno);
        return;
    }
    deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    if (::connect(socket_, reinterpret_cast<const sockaddr *>(&storage), length) == 0)
        state_ = State::Connected;
    else if (errno == EINPROGRESS || errno == EINTR)
        state_ = State::Connecting;
    else
        finish(State::Failed, errno);
}
void NativeTcp::queue(const std::vector<uint8_t> &bytes) {
    if (state_ != State::Connected)
        throw std::logic_error("TCP is not connected");
    if (bytes.size() > capacity - pendingSend())
        throw std::length_error("TCP send buffer full");
    // Compact only after accepted budget validation; failure leaves queued bytes intact.
    if (sent_) {
        outgoing_.erase(outgoing_.begin(), outgoing_.begin() + sent_);
        sent_ = 0;
    }
    outgoing_.insert(outgoing_.end(), bytes.begin(), bytes.end());
}
std::vector<uint8_t> NativeTcp::takeReceived() noexcept {
    std::vector<uint8_t> result;
    result.swap(incoming_);
    return result;
}
void NativeTcp::pump(uint32_t waitMs) {
    if (waitMs > 100)
        throw std::invalid_argument("TCP pump wait exceeds100ms");
    if (state_ != State::Connecting && state_ != State::Connected)
        return;
    int timeout = int(waitMs);
    if (state_ == State::Connecting) {
        const auto remaining = deadline_ - std::chrono::steady_clock::now();
        if (remaining <= decltype(remaining)::zero()) {
            finish(State::Failed, ETIMEDOUT);
            return;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        timeout = std::min(timeout, int(ms + 1));
    }
    pollfd descriptor{socket_, 0, 0};
    if (state_ == State::Connecting || pendingSend())
        descriptor.events |= POLLOUT;
    if (state_ == State::Connected && incoming_.size() < capacity)
        descriptor.events |= POLLIN;
    const int ready = ::poll(&descriptor, 1, timeout);
    if (ready < 0) {
        if (errno != EINTR)
            finish(State::Failed, errno);
        return;
    }
    if (state_ == State::Connecting) {
        if (!ready) {
            if (std::chrono::steady_clock::now() >= deadline_)
                finish(State::Failed, ETIMEDOUT);
            return;
        }
        int code = 0;
        socklen_t size = sizeof(code);
        if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, &code, &size) != 0)
            code = errno;
        if (code) {
            finish(State::Failed, code);
            return;
        }
        state_ = State::Connected;
    }
    if (descriptor.revents & POLLNVAL) {
        finish(State::Failed, EBADF);
        return;
    }
    if (descriptor.revents & POLLERR) {
        int code = 0;
        socklen_t size = sizeof(code);
        if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, &code, &size) != 0)
            code = errno;
        if (code) {
            finish(State::Failed, code);
            return;
        }
    }
    if ((descriptor.revents & POLLOUT) && pendingSend()) {
        size_t budget = 256 * 1024;
        while (pendingSend() && budget) {
            const auto count = ::send(socket_, outgoing_.data() + sent_,
                                      std::min(pendingSend(), budget), MSG_NOSIGNAL);
            if (count > 0) {
                sent_ += size_t(count);
                budget -= size_t(count);
            } else {
                if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    finish(State::Failed, errno);
                break;
            }
        }
        if (!pendingSend()) {
            outgoing_.clear();
            sent_ = 0;
        }
    }
    if (socket_ >= 0 && (descriptor.revents & (POLLIN | POLLHUP))) {
        size_t budget = 256 * 1024;
        while (budget && incoming_.size() < capacity) {
            uint8_t bytes[16384];
            const auto amount = std::min({sizeof(bytes), capacity - incoming_.size(), budget});
            // Allocate before consuming the stream so allocation failure cannot lose bytes.
            incoming_.reserve(incoming_.size() + amount);
            const auto count = ::recv(socket_, bytes, amount, 0);
            if (count > 0) {
                incoming_.insert(incoming_.end(), bytes, bytes + count);
                budget -= size_t(count);
            } else {
                if (!count)
                    finish(State::PeerClosed);
                else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    finish(State::Failed, errno);
                break;
            }
        }
    }
}
} // namespace wow::android
