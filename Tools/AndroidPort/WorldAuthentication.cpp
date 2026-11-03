#include "WorldAuthentication.h"
#include "Engine/Source/Os/OsSecureRandom.h"
#include <algorithm>
#include <stdexcept>
namespace wow::android {
namespace {
void erase(void *data, size_t size) noexcept {
    auto *bytes = static_cast<volatile uint8_t *>(data);
    while (size--)
        *bytes++ = 0;
}
struct Wipe {
    void *data;
    size_t size;
    ~Wipe() { erase(data, size); }
};
uint32_t read32(const std::vector<uint8_t> &p, size_t i) {
    return uint32_t(p[i]) | (uint32_t(p[i + 1]) << 8) | (uint32_t(p[i + 2]) << 16) |
           (uint32_t(p[i + 3]) << 24);
}
void put(std::vector<uint8_t> &p, uint64_t n, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i)
        p.push_back(uint8_t(n >> (i * 8)));
}
void accountCheck(std::string_view account) {
    if (account.empty() || account.size() > 255)
        throw std::invalid_argument("Invalid world account length");
    for (unsigned char c : account)
        if (c < 33 || c > 126 || c == ':' || (c >= 'a' && c <= 'z'))
            throw std::invalid_argument("World account must be normalized printable ASCII");
}
} // namespace
WorldChallenge decodeWorldChallenge(const WorldPacket &packet) {
    if (packet.opcode != 0x1ec || packet.payload.size() != 40)
        throw std::invalid_argument("Invalid world challenge packet");
    WorldChallenge challenge;
    challenge.difficulty = read32(packet.payload, 0);
    std::copy_n(packet.payload.begin() + 4, 4, challenge.serverSeed.begin());
    std::copy_n(packet.payload.begin() + 8, 32, challenge.workSeed.begin());
    return challenge;
}
WorldAuthStatus decodeWorldAuthStatus(const WorldPacket &packet) {
    // RealmConnection::HandleAuthResponse: billing block only when >= 10 bytes remain
    // (excluding the 5-byte queue tail of result 27); trailing bytes are ignored, so
    // AzerothCore/TrinityCore's long AUTH_OK form (with queue fields) is accepted.
    if (packet.opcode != 0x1ee || packet.payload.empty())
        throw std::invalid_argument("Invalid world auth result packet");
    WorldAuthStatus status;
    const auto &p = packet.payload;
    status.result = p[0];
    if (status.result == 12 || status.result == 27) {
        const size_t queueBytes = status.result == 27 ? 5 : 0;
        if (p.size() < 1 + queueBytes)
            throw std::invalid_argument("Invalid world auth result length");
        status.hasBilling = p.size() - queueBytes - 1 >= 10;
        if (status.hasBilling) {
            status.billingRemaining = read32(p, 1);
            status.billingFlags = p[5];
            status.billingRested = read32(p, 6);
            status.expansion = p[10];
        }
        if (queueBytes) {
            const size_t at = status.hasBilling ? 11 : 1;
            status.queuePosition = read32(p, at);
            status.freeMigration = p[at + 4] != 0;
        }
    }
    return status;
}
WorldProofOfWork::~WorldProofOfWork() { reset(); }
void WorldProofOfWork::reset() noexcept {
    erase(&base_, sizeof(base_));
    difficulty_ = attempts_ = 0;
    nonce_ = 0;
    active_ = ready_ = false;
}
void WorldProofOfWork::begin(std::string_view account, const WorldChallenge &challenge) {
    reset();
    accountCheck(account);
    if (challenge.difficulty > 20)
        throw std::invalid_argument("Unsupported world work difficulty");
    difficulty_ = challenge.difficulty;
    SHA1_Init(&base_);
    SHA1_Update(&base_, reinterpret_cast<const uint8_t *>(account.data()),
                uint32_t(account.size()));
    SHA1_Update(&base_, challenge.workSeed.data(), challenge.workSeed.size());
    active_ = true;
}
std::optional<uint64_t> WorldProofOfWork::step(uint32_t budget) {
    if (!active_)
        throw std::logic_error("World work solver is inactive");
    if (!budget || budget > 4096)
        throw std::invalid_argument("Invalid world work step budget");
    if (ready_)
        return nonce_;
    while (budget-- && attempts_ < (1u << 20)) {
        auto context = base_;
        std::array<uint8_t, 8> nonce{};
        std::array<uint8_t, 20> digest{};
        Wipe cleanContext{&context, sizeof(context)}, cleanDigest{digest.data(), digest.size()};
        for (unsigned i = 0; i < 8; ++i)
            nonce[i] = uint8_t(nonce_ >> (i * 8));
        SHA1_Update(&context, nonce.data(), nonce.size());
        SHA1_Final(digest.data(), &context);
        ++attempts_;
        unsigned bits = 0;
        for (uint8_t value : digest) {
            if (!value) {
                bits += 8;
                continue;
            }
            while (!(value & 1)) {
                ++bits;
                value >>= 1;
            }
            break;
        }
        if (bits >= difficulty_) {
            ready_ = true;
            return nonce_;
        }
        ++nonce_;
    }
    if (attempts_ == (1u << 20))
        throw std::runtime_error("World proof work budget exhausted");
    return std::nullopt;
}
WorldPacket encodeWorldAuthentication(const RealmSession &session, const WorldChallenge &challenge,
                                      uint64_t nonce) {
    const auto account = session.account();
    accountCheck(account);
    std::array<uint8_t, 4> seed{}, zero{};
    std::array<uint8_t, 20> digest{};
    SHA1_CONTEXT context{};
    Wipe cleanContext{&context, sizeof(context)}, cleanDigest{digest.data(), digest.size()},
        cleanSeed{seed.data(), seed.size()};
    OsSecureRandom(seed.data(), seed.size());
    SHA1_Init(&context);
    SHA1_Update(&context, reinterpret_cast<const uint8_t *>(account.data()),
                uint32_t(account.size()));
    SHA1_Update(&context, zero.data(), zero.size());
    SHA1_Update(&context, seed.data(), seed.size());
    SHA1_Update(&context, challenge.serverSeed.data(), challenge.serverSeed.size());
    const auto &key = session.sessionKey();
    SHA1_Update(&context, key.data(), key.size());
    SHA1_Final(digest.data(), &context);
    WorldPacket packet{0x1ed, {}};
    auto &p = packet.payload;
    p.reserve(account.size() + 80);
    put(p, session.worldClientBuild(), 4);
    put(p, 0, 4);
    p.insert(p.end(), account.begin(), account.end());
    p.push_back(0);
    put(p, 0, 4);
    p.insert(p.end(), seed.begin(), seed.end());
    put(p, 0, 4);
    put(p, 0, 4);
    put(p, session.realm().id, 4);
    put(p, nonce, 8);
    p.insert(p.end(), digest.begin(), digest.end());
    // zlib stream: one stored DEFLATE block containing addonCount0/timestamp0.
    // Adler32 of eight zero bytes is0x00080001; independently decompressed in tests.
    constexpr uint8_t emptyAddons[]{0x78, 0x01, 0x01, 0x08, 0x00, 0xf7, 0xff, 0,    0,   0,
                                    0,    0,    0,    0,    0,    0x00, 0x08, 0x00, 0x01};
    put(p, 8, 4);
    p.insert(p.end(), std::begin(emptyAddons), std::end(emptyAddons));
    return packet;
}
} // namespace wow::android
