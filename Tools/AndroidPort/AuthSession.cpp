#include "AuthSession.h"
#include <algorithm>
#include <stdexcept>
namespace wow::android {
namespace {
void wipe(std::string &s) noexcept {
    auto *p = reinterpret_cast<volatile char *>(s.data());
    for (size_t i = 0; i < s.size(); ++i)
        p[i] = 0;
    s.clear();
}
} // namespace
AuthSession::~AuthSession() { clear(); }
void AuthSession::clearCredentials() noexcept {
    wipe(account_);
    wipe(password_);
    checksum_ = nullptr;
}
void AuthSession::clear() noexcept {
    connection_.close();
    decoder_.reset();
    srp_.reset();
    clearCredentials();
    wipe(worldAccount_);
    hello_.clear();
    realms_.realms.clear();
    realms_.unknown = 0;
    realms_.trailer = 0;
    serverResult_.reset();
    error_ = Error::None;
    networkError_ = 0;
    state_ = State::Idle;
}
void AuthSession::cancel() noexcept {
    clear();
    state_ = State::Cancelled;
}
bool AuthSession::active() const noexcept {
    return state_ == State::Connecting || state_ == State::Challenge || state_ == State::Proof ||
           state_ == State::Realms;
}
void AuthSession::fail(Error error, int networkError) noexcept {
    const auto result = serverResult_;
    clear();
    serverResult_ = result;
    error_ = error;
    networkError_ = networkError;
    state_ = State::Failed;
}
void AuthSession::begin(const ResolvedServerEndpoint &endpoint, const std::string &account,
                        const std::string &password, VersionChecksum checksum, uint32_t connectMs,
                        uint32_t authMs, const AuthClientIdentity &identity) {
    clear();
    try {
        if (!checksum || authMs < 1 || authMs > 60000 || password.empty() ||
            password.size() > 255 || account.empty() || account.size() > 255 || account[0] == '#')
            throw std::invalid_argument("Invalid auth session configuration");
        for (unsigned char c : account)
            if (c < 33 || c > 126 || c == ':')
                throw std::invalid_argument("Invalid ASCII auth account");
        for (unsigned char c : password)
            if (c < 32 || c > 126)
                throw std::invalid_argument("Invalid ASCII auth password");
        hello_ = encodeAuthHello(account, identity);
        account_ = account;
        password_ = password;
        worldAccount_ = account;
        for (char &c : worldAccount_)
            if (c >= 'a' && c <= 'z')
                c = char(c - 'a' + 'A');
        checksum_ = std::move(checksum);
        authMs_ = authMs;
        connection_.connect(endpoint, connectMs);
        state_ = State::Connecting;
    } catch (...) {
        fail(Error::Protocol);
        throw;
    }
}
void AuthSession::receive(const AuthMessage &message) {
    // A response is only valid after its request has left our send queue.
    if (connection_.pendingSend()) {
        fail(Error::Protocol);
        return;
    }
    if (state_ == State::Challenge && std::holds_alternative<AuthChallenge>(message)) {
        const auto &challenge = std::get<AuthChallenge>(message);
        if (challenge.result) {
            serverResult_ = challenge.result;
            fail(Error::Rejected);
            return;
        }
        if (challenge.pin || challenge.matrix || challenge.tokenRequired) {
            fail(Error::Security);
            return;
        }
        std::array<uint8_t, 20> checksum{};
        try {
            checksum = checksum_(challenge.versionChallenge);
        } catch (...) {
            fail(Error::Checksum);
            return;
        }
        try {
            srp_.begin(account_, password_, challenge, checksum);
            clearCredentials();
            if (connection_.state() != NativeTcp::State::Connected) {
                fail(Error::Network, connection_.error());
                return;
            }
            connection_.queue(encodeAuthProof(srp_.clientProof()));
            state_ = State::Proof;
        } catch (...) {
            fail(Error::Crypto);
        }
    } else if (state_ == State::Proof && std::holds_alternative<AuthServerProof>(message)) {
        const auto &proof = std::get<AuthServerProof>(message);
        if (proof.result) {
            serverResult_ = proof.result;
            fail(Error::Rejected);
        } else if (!srp_.verify(proof)) {
            fail(Error::Crypto);
        } else {
            if (connection_.state() != NativeTcp::State::Connected) {
                fail(Error::Network, connection_.error());
                return;
            }
            connection_.queue(encodeRealmListRequest());
            state_ = State::Realms;
        }
    } else if (state_ == State::Realms && std::holds_alternative<AuthRealmList>(message)) {
        realms_ = std::get<AuthRealmList>(message);
        state_ = State::Ready;
    } else {
        fail(Error::Protocol);
    }
}
void AuthSession::pump(uint32_t waitMs) {
    if (waitMs > 100)
        throw std::invalid_argument("Auth pump wait exceeds 100ms");
    if (!active())
        return;
    try {
        const auto now = std::chrono::steady_clock::now();
        if (state_ != State::Connecting) {
            if (now >= deadline_) {
                fail(Error::Timeout);
                return;
            }
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now);
            waitMs = std::min(waitMs, uint32_t(left.count()));
        }
        connection_.pump(waitMs);
        if (state_ == State::Connecting && connection_.state() == NativeTcp::State::Connected) {
            deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(authMs_);
            connection_.queue(hello_);
            hello_.clear();
            state_ = State::Challenge;
        } else if (state_ != State::Connecting && std::chrono::steady_clock::now() >= deadline_) {
            fail(Error::Timeout);
            return;
        }
        const auto bytes = connection_.takeReceived();
        if (!bytes.empty())
            decoder_.feed(bytes);
        while (active() || state_ == State::Ready) {
            const auto message = decoder_.next();
            if (!message)
                break;
            receive(*message);
        }
        if ((active() || state_ == State::Ready) && state_ != State::Connecting &&
            std::chrono::steady_clock::now() >= deadline_) {
            fail(Error::Timeout);
            return;
        }
        if (state_ == State::Ready) {
            // Reject a complete OR partial unsolicited tail before publishing readiness.
            decoder_.finish();
            // The original client keeps the login connection through world handoff.
            // Keep it owned here until explicit selection transfers it to the ticket.
            decoder_.reset();
            return;
        }
        if (!active())
            return;
        if (connection_.state() == NativeTcp::State::PeerClosed) {
            decoder_.finish();
            fail(Error::Network);
        } else if (connection_.state() == NativeTcp::State::Failed ||
                   connection_.state() == NativeTcp::State::Closed) {
            fail(Error::Network, connection_.error());
        }
    } catch (...) {
        fail(Error::Protocol);
    }
}
const AuthRealmList &AuthSession::realms() const {
    if (state_ != State::Ready)
        throw std::logic_error("Authenticated realm list is unavailable");
    return realms_;
}
const std::array<uint8_t, 40> &AuthSession::sessionKey() const {
    if (state_ != State::Ready)
        throw std::logic_error("Authenticated realm session is unavailable");
    return srp_.sessionKey();
}
RealmSession AuthSession::selectRealm(size_t index) {
    if (state_ != State::Ready)
        throw std::logic_error("Realm selection requires a verified realm list");
    auto selected = prepareRealmSelection(realms_, index);
    RealmSession ticket(std::move(selected), worldAccount_, srp_.sessionKey());
    ticket.authConnection_ = std::move(connection_);
    clear();
    state_ = State::Consumed;
    return ticket;
}
} // namespace wow::android
