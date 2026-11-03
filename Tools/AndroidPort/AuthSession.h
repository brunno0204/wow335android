#pragma once
#include "RealmSession.h"
#include "ServerEndpoint.h"
#include "SrpAuthentication.h"
#include <functional>
namespace wow::android {
// Single worker/owner. DNS must finish before begin; pump never resolves names.
// This controller ends at an owned realm list, not a world/gameplay connection.
class AuthSession {
  public:
    enum class State {
        Idle,
        Connecting,
        Challenge,
        Proof,
        Realms,
        Ready,
        Failed,
        Cancelled,
        Consumed
    };
    enum class Error { None, Network, Timeout, Protocol, Rejected, Security, Checksum, Crypto };
    using VersionChecksum = std::function<std::array<uint8_t, 20>(const std::array<uint8_t, 16> &)>;
    AuthSession() = default;
    ~AuthSession();
    AuthSession(const AuthSession &) = delete;
    AuthSession &operator=(const AuthSession &) = delete;
    // Every begin clears the previous attempt, including on validation failure.
    // Password is retained only until the challenge is processed. Callback must
    // implement the executable checksum policy; no fabricated default is used.
    void begin(const ResolvedServerEndpoint &, const std::string &account,
               const std::string &password, VersionChecksum, uint32_t connectMs = 10000,
               uint32_t authMs = 10000, const AuthClientIdentity &identity = {});
    void pump(uint32_t waitMs = 0); // 0..100ms; one fixed total authentication deadline
    void cancel() noexcept;
    State state() const noexcept { return state_; }
    Error error() const noexcept { return error_; }
    int networkError() const noexcept { return networkError_; }
    std::optional<uint8_t> serverResult() const noexcept { return serverResult_; }
    const AuthRealmList &realms() const;
    const std::array<uint8_t, 40> &sessionKey() const;
    // Successful selection transfers the login channel to the owned ticket,
    // consumes Ready and erases this controller's key. Invalid selection
    // preserves Ready; no implicit first-realm selection or network IO occurs.
    RealmSession selectRealm(size_t index);

  private:
    void clear() noexcept;
    void clearCredentials() noexcept;
    void fail(Error, int networkError = 0) noexcept;
    void receive(const AuthMessage &);
    bool active() const noexcept;
    TcpEndpointConnector connection_;
    AuthWireDecoder decoder_;
    SrpAuthentication srp_;
    std::string account_, password_, worldAccount_;
    std::vector<uint8_t> hello_;
    VersionChecksum checksum_;
    AuthRealmList realms_;
    State state_ = State::Idle;
    Error error_ = Error::None;
    int networkError_ = 0;
    std::optional<uint8_t> serverResult_;
    uint32_t authMs_ = 0;
    std::chrono::steady_clock::time_point deadline_;
};
} // namespace wow::android
