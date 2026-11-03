#pragma once
#include "AuthWire.h"
#include "ServerEndpoint.h"
#include <string_view>
namespace wow::android {
struct RealmSelection {
    AuthRealm realm;
    ServerEndpoint endpoint;
};
// Explicit list INDEX selection; realm indicators are preserved, not a local veto.
// Validates optional version metadata consistency and explicit host:port or
// [IPv6]:port. Only the explicit, bounded 3.3.5/12341 experimental world profile
// changes the world build; Grunt auth remains12340. Other tuples stay metadata.
RealmSelection prepareRealmSelection(const AuthRealmList &, size_t index);
uint16_t worldClientBuildForRealm(const AuthRealm &) noexcept;
class AuthSession;
// Owned handoff from a verified AuthSession only. A TCP connection to the
// selected endpoint still needs the world authentication/crypto handshake.
class RealmSession {
  public:
    RealmSession() = default;
    ~RealmSession();
    RealmSession(const RealmSession &) = delete;
    RealmSession &operator=(const RealmSession &) = delete;
    RealmSession(RealmSession &&) noexcept;
    RealmSession &operator=(RealmSession &&) noexcept;
    void reset() noexcept;
    bool valid() const noexcept { return valid_; }
    const AuthRealm &realm() const;
    const ServerEndpoint &endpoint() const;
    uint16_t worldClientBuild() const;
    std::string_view account() const;
    const std::array<uint8_t, 40> &sessionKey() const;
    // Blocking system DNS on the owning worker; no timeout/cancellation promise.
    // Failure preserves the ticket; no socket is opened and no key is sent here.
    ResolvedServerEndpoint resolveEndpoint() const;

  private:
    friend class AuthSession;
    RealmSession(RealmSelection, const std::string &, const std::array<uint8_t, 40> &);
    void requireValid() const;
    // Owns the already-authenticated login channel across world handoff.
    TcpEndpointConnector authConnection_;
    RealmSelection selected_;
    std::array<char, 256> account_{};
    size_t accountSize_ = 0;
    std::array<uint8_t, 40> key_{};
    bool valid_ = false;
};
} // namespace wow::android
