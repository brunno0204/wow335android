#include "RealmSession.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace wow::android {
namespace {
void wipe(void *data, size_t size) noexcept {
    auto *bytes = static_cast<volatile uint8_t *>(data);
    while (size--)
        *bytes++ = 0;
}
} // namespace
// Experimental WoW-Mania world profile, based on the observed3.3.5/12341
// advertisement and the server's published client guide. Do not echo arbitrary
// advertised builds or change the already working Grunt12340 checksum identity.
uint16_t worldClientBuildForRealm(const AuthRealm &realm) noexcept {
    if ((realm.flags & 4) && realm.version) {
        const auto &v = *realm.version;
        if (v.major == 3 && v.minor == 3 && v.patch == 5 && v.build == 12341) return 12341;
    }
    return 12340;
}
RealmSelection prepareRealmSelection(const AuthRealmList &list, size_t index) {
    if (index >= list.realms.size())
        throw std::out_of_range("Realm index outside authenticated list");
    const auto &realm = list.realms[index];
    // Match CGlueMgr::ChangeRealm -> ClientServices::SelectRealm ->
    // SetSelectedRealmInfo(force=1): this is an explicit list-index choice,
    // not the automatic realm suggestion policy. Preserve list indicators;
    // only the world authentication response authorizes entry.

    if (realm.name.empty() || realm.name.size() > 255 ||
        realm.name.find('\0') != std::string::npos || !std::isfinite(realm.population))
        throw std::invalid_argument("Invalid realm metadata");
    if (bool(realm.flags & 4) != realm.version.has_value())
        throw std::invalid_argument("Inconsistent realm version metadata");
    // An advertised tuple is list metadata, not the wire identity of this client.
    // The original explicit SelectRealm(force=1) path does not veto it here.
    // WorldAuthentication uses only a known world profile; server auth is mandatory.
    const auto &address = realm.address;
    if (address.empty() || address.size() > 255)
        throw std::invalid_argument("Invalid realm address length");
    // The login default3724 must never silently become a world-server port.
    if (address.front() == '[') {
        const auto bracket = address.find(']');
        if (bracket == std::string::npos || bracket + 2 >= address.size() ||
            address[bracket + 1] != ':')
            throw std::invalid_argument("Realm IPv6 address requires an explicit port");
    } else {
        const auto colon = address.find(':');
        if (colon == std::string::npos || address.find(':', colon + 1) != std::string::npos)
            throw std::invalid_argument("Realm address requires an explicit port");
    }
    return {realm, parseServerEndpoint(address)};
}
RealmSession::RealmSession(RealmSelection selected, const std::string &account,
                           const std::array<uint8_t, 40> &key)
    : selected_(std::move(selected)) {
    if (account.empty() || account.size() > 255)
        throw std::invalid_argument("Invalid authenticated account length");
    std::copy(account.begin(), account.end(), account_.begin());
    accountSize_ = account.size();
    key_ = key;
    valid_ = true;
}
RealmSession::~RealmSession() { reset(); }
RealmSession::RealmSession(RealmSession &&other) noexcept { *this = std::move(other); }
RealmSession &RealmSession::operator=(RealmSession &&other) noexcept {
    if (this != &other) {
        reset();
        authConnection_ = std::move(other.authConnection_);
        selected_ = std::move(other.selected_);
        account_ = other.account_;
        accountSize_ = other.accountSize_;
        key_ = other.key_;
        valid_ = other.valid_;
        other.reset();
    }
    return *this;
}
void RealmSession::reset() noexcept {
    authConnection_.close();
    wipe(key_.data(), key_.size());
    wipe(account_.data(), account_.size());
    accountSize_ = 0;
    selected_ = {};
    valid_ = false;
}
void RealmSession::requireValid() const {
    if (!valid_)
        throw std::logic_error("Realm session handoff is unavailable");
}
const AuthRealm &RealmSession::realm() const {
    requireValid();
    return selected_.realm;
}
const ServerEndpoint &RealmSession::endpoint() const {
    requireValid();
    return selected_.endpoint;
}
uint16_t RealmSession::worldClientBuild() const {
    requireValid();
    return worldClientBuildForRealm(selected_.realm);
}
std::string_view RealmSession::account() const {
    requireValid();
    return {account_.data(), accountSize_};
}
const std::array<uint8_t, 40> &RealmSession::sessionKey() const {
    requireValid();
    return key_;
}
ResolvedServerEndpoint RealmSession::resolveEndpoint() const {
    requireValid();
    return resolveServerEndpoint(selected_.endpoint);
}
} // namespace wow::android
