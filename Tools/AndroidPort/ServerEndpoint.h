#pragma once
#include "NativeTcp.h"
#include <string>
#include <vector>
namespace wow::android {
struct ServerEndpoint {
    std::string host;
    uint16_t port = 3724;
};
struct ResolvedServerEndpoint {
    std::vector<std::string> addresses;
    uint16_t port = 3724;
};
// host[:port], IPv4[:port], bare IPv6 or [IPv6][:port]. ASCII DNS labels only.
// No URL, credentials, realmlist.wtf statements, whitespace or scope identifiers.
ServerEndpoint parseServerEndpoint(const std::string &text);
// Blocking system resolver: asset/network worker ONLY. No DNS timeout/cancellation
// guarantee. Owns <=16 deduplicated numeric candidates; no socket is opened here.
ResolvedServerEndpoint resolveServerEndpoint(const ServerEndpoint &endpoint);
// Pure candidate validation; useful for owned resolver handoff and deterministic tests.
ResolvedServerEndpoint prepareResolvedServerEndpoint(const std::vector<std::string> &addresses,
                                                     uint16_t port);
class TcpEndpointConnector {
  public:
    TcpEndpointConnector() = default;
    TcpEndpointConnector(TcpEndpointConnector &&) noexcept = default;
    TcpEndpointConnector &operator=(TcpEndpointConnector &&) noexcept = default;
    TcpEndpointConnector(const TcpEndpointConnector &) = delete;
    TcpEndpointConnector &operator=(const TcpEndpointConnector &) = delete;
    // One total connect budget1..60000ms shared by all numeric candidates.
    // Validation precedes replacing an active connection. No resolver/frame IO.
    void connect(const ResolvedServerEndpoint &endpoint, uint32_t timeoutMs);
    void pump(uint32_t waitMs);
    void close() noexcept;
    NativeTcp::State state() const noexcept;
    int error() const noexcept;
    void queue(const std::vector<uint8_t> &bytes) { stream_.queue(bytes); }
    std::vector<uint8_t> takeReceived() noexcept { return stream_.takeReceived(); }
    size_t available() const noexcept { return stream_.available(); }
    size_t pendingSend() const noexcept { return stream_.pendingSend(); }

  private:
    void startNext();
    void expire() noexcept;
    NativeTcp stream_;
    ResolvedServerEndpoint endpoint_;
    size_t next_ = 0;
    bool established_ = false, expired_ = false;
    std::chrono::steady_clock::time_point deadline_;
};
} // namespace wow::android
