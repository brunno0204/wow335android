#include "AuthWire.h"
#include "GameVersion.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace wow::android {
namespace {
struct Incomplete {};
[[noreturn]] void malformed() {
    throw std::invalid_argument("Malformed or unsupported Grunt auth message");
}
struct Reader {
    const std::vector<uint8_t> &bytes;
    size_t pos = 0, end = 0;
    bool complete = false;
    void need(size_t n) const {
        if (n > end - pos) {
            if (complete)
                malformed();
            throw Incomplete{};
        }
    }
    uint8_t u8() {
        need(1);
        return bytes[pos++];
    }
    uint16_t u16() {
        auto a = u8();
        return uint16_t(a | uint16_t(u8()) << 8);
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i)
            v |= uint32_t(u8()) << (8 * i);
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        for (unsigned i = 0; i < 8; ++i)
            v |= uint64_t(u8()) << (8 * i);
        return v;
    }
    template <size_t N> void array(std::array<uint8_t, N> &out) {
        need(N);
        std::copy_n(bytes.begin() + pos, N, out.begin());
        pos += N;
    }
    std::vector<uint8_t> field(size_t n) {
        need(n);
        std::vector<uint8_t> out(bytes.begin() + pos, bytes.begin() + pos + n);
        pos += n;
        return out;
    }
    std::string text() {
        std::string out;
        for (size_t i = 0; i <= 255; ++i) {
            const auto c = u8();
            if (!c)
                return out;
            if (i == 255)
                malformed();
            out.push_back(char(c));
        }
        malformed();
    }
};
void le(std::vector<uint8_t> &out, uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        out.push_back(uint8_t(value >> (8 * i)));
}
template <size_t N> void append(std::vector<uint8_t> &out, const std::array<uint8_t, N> &value) {
    out.insert(out.end(), value.begin(), value.end());
}
AuthChallenge challenge(Reader &r) {
    if (r.u8() != 0)
        malformed();
    AuthChallenge value;
    value.result = r.u8();
    if (value.result)
        return value;
    r.array(value.serverPublic);
    const auto g = r.u8();
    if (!g || g > 32)
        malformed();
    value.generator = r.field(g);
    const auto n = r.u8();
    if (!n || n > 32)
        malformed();
    value.prime = r.field(n);
    r.array(value.salt);
    r.array(value.versionChallenge);
    const auto security = r.u8();
    if (security & ~7)
        malformed();
    if (security & 1) {
        AuthPinChallenge p;
        p.seed = r.u32();
        r.array(p.salt);
        value.pin = p;
    }
    if (security & 2) {
        AuthMatrixChallenge m;
        m.width = r.u8();
        m.height = r.u8();
        m.digits = r.u8();
        m.count = r.u8();
        m.seed = r.u64();
        if (!m.count)
            malformed();
        value.matrix = m;
    }
    if (security & 4)
        value.tokenRequired = r.u8();
    return value;
}
AuthServerProof serverProof(Reader &r) {
    AuthServerProof value;
    value.result = r.u8();
    if (!value.result) {
        r.array(value.proof);
        value.accountFlags = r.u32();
        value.survey = r.u32();
        value.loginFlags = r.u16();
    } else if (value.result == 4)
        value.unknownAccountExtra = r.u16();
    return value;
}
AuthRealmList realmList(Reader &outer) {
    const auto length = outer.u16();
    if (length < 8)
        malformed();
    outer.need(length);
    Reader r{outer.bytes, outer.pos, outer.pos + length, true};
    AuthRealmList value;
    value.unknown = r.u32();
    const auto count = r.u16();
    if (count > 1024)
        malformed();
    value.realms.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        AuthRealm realm;
        realm.type = r.u8();
        realm.locked = r.u8();
        realm.flags = r.u8();
        realm.name = r.text();
        realm.address = r.text();
        const auto bits = r.u32();
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        std::memcpy(&realm.population, &bits, sizeof(bits));
        if (!std::isfinite(realm.population))
            malformed();
        realm.characters = r.u8();
        realm.timezone = r.u8();
        realm.id = r.u8();
        if (realm.flags & 4) {
            AuthRealmVersion v;
            v.major = r.u8();
            v.minor = r.u8();
            v.patch = r.u8();
            v.build = r.u16();
            realm.version = v;
        }
        value.realms.push_back(std::move(realm));
    }
    value.trailer = r.u16();
    if (r.pos != r.end)
        malformed();
    outer.pos = r.pos;
    return value;
}
} // namespace
std::vector<uint8_t> encodeAuthHello(const std::string &account, const AuthClientIdentity &id) {
    if (account.empty() || account.size() > 255)
        throw std::invalid_argument("Invalid auth account length");
    std::string upper = account;
    for (char &c : upper) {
        if (c < 33 || c > 126 || c == ':')
            throw std::invalid_argument("Auth account requires printable ASCII without colon");
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    }
    std::vector<uint8_t> out{0, 8};
    le(out, 30 + upper.size(), 2);
    le(out, id.program, 4);
    out.push_back(GAME_VERSION_MAJOR);
    out.push_back(GAME_VERSION_MINOR);
    out.push_back(GAME_VERSION_PATCH);
    le(out, GAME_BUILD, 2);
    le(out, id.processor, 4);
    le(out, id.os, 4);
    le(out, id.locale, 4);
    le(out, uint32_t(id.timezoneMinutes), 4);
    append(out, id.ipv4);
    out.push_back(uint8_t(upper.size()));
    out.insert(out.end(), upper.begin(), upper.end());
    return out;
}
std::vector<uint8_t> encodeAuthProof(const AuthClientProof &proof) {
    if (proof.token && (proof.token->empty() || proof.token->size() > 16))
        throw std::invalid_argument("Invalid auth token length");
    if (proof.token)
        for (unsigned char c : *proof.token)
            if (c < 33 || c > 126)
                throw std::invalid_argument("Invalid auth token bytes");
    std::vector<uint8_t> out{1};
    append(out, proof.publicKey);
    append(out, proof.proof);
    append(out, proof.versionProof);
    out.push_back(0);
    out.push_back(uint8_t((proof.pin ? 1 : 0) | (proof.matrix ? 2 : 0) | (proof.token ? 4 : 0)));
    if (proof.pin) {
        append(out, proof.pin->salt);
        append(out, proof.pin->hash);
    }
    if (proof.matrix)
        append(out, *proof.matrix);
    if (proof.token) {
        out.push_back(uint8_t(proof.token->size()));
        out.insert(out.end(), proof.token->begin(), proof.token->end());
    }
    return out;
}
std::vector<uint8_t> encodeRealmListRequest() { return {0x10, 0, 0, 0, 0}; }
void AuthWireDecoder::feed(const std::vector<uint8_t> &bytes) {
    if (failed_)
        throw std::logic_error("Auth wire decoder requires reset");
    if (bytes.size() > capacity - buffered())
        throw std::length_error("Auth receive buffer full");
    if (offset_) {
        bytes_.erase(bytes_.begin(), bytes_.begin() + offset_);
        offset_ = 0;
    }
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}
void AuthWireDecoder::finish() {
    if (failed_)
        throw std::logic_error("Auth wire decoder requires reset");
    if (buffered()) {
        failed_ = true;
        bytes_.clear();
        offset_ = 0;
        throw std::invalid_argument("Truncated Grunt auth stream");
    }
}
std::optional<AuthMessage> AuthWireDecoder::next() {
    if (failed_)
        throw std::logic_error("Auth wire decoder requires reset");
    if (!buffered())
        return std::nullopt;
    Reader r{bytes_, offset_, bytes_.size(), false};
    try {
        AuthMessage message;
        switch (r.u8()) {
        case 0:
            message = challenge(r);
            break;
        case 1:
            message = serverProof(r);
            break;
        case 0x10:
            message = realmList(r);
            break;
        default:
            malformed();
        }
        offset_ = r.pos;
        if (offset_ == bytes_.size()) {
            bytes_.clear();
            offset_ = 0;
        }
        return message;
    } catch (const Incomplete &) {
        return std::nullopt;
    } catch (const std::invalid_argument &) {
        failed_ = true;
        bytes_.clear();
        offset_ = 0;
        throw;
    }
}
} // namespace wow::android
