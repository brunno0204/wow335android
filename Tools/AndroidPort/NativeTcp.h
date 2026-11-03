#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
namespace wow::android {
// Single worker/owner, nonblocking raw stream. No DNS, WoW framing or authentication.
class NativeTcp {
  public:
    enum class State { Closed, Connecting, Connected, PeerClosed, Failed };
    static constexpr size_t capacity = 1024 * 1024;
    NativeTcp() = default;
    ~NativeTcp();
    NativeTcp(const NativeTcp &) = delete;
    NativeTcp &operator=(const NativeTcp &) = delete;
    NativeTcp(NativeTcp &&other) noexcept;
    NativeTcp &operator=(NativeTcp &&other) noexcept;
    // Numeric IPv4/IPv6 only, port1..65535, deadline1..60000ms. Invalid input
    // preserves an active connection. Valid reconnect closes the previous one.
    void connect(const std::string &address, uint16_t port, uint32_t timeoutMs);
    void close() noexcept;
    void queue(const std::vector<uint8_t> &bytes);
    // Bounded wait0..100ms and <=256KiB IO per direction. Full receive buffer
    // applies TCP backpressure until takeReceived, including before observing EOF.
    void pump(uint32_t waitMs);
    std::vector<uint8_t> takeReceived() noexcept;
    State state() const noexcept { return state_; }
    int error() const noexcept { return error_; }
    size_t pendingSend() const noexcept { return outgoing_.size() - sent_; }
    size_t available() const noexcept { return incoming_.size(); }

  private:
    void finish(State state, int error = 0) noexcept;
    int socket_ = -1, error_ = 0;
    State state_ = State::Closed;
    std::chrono::steady_clock::time_point deadline_;
    std::vector<uint8_t> outgoing_, incoming_;
    size_t sent_ = 0;
};
} // namespace wow::android
