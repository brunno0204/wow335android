#include "SrpAuthentication.h"
#include "Engine/Source/Os/OsSecureRandom.h"
#include "WoW/Source/WowServices/SRP6_Random.h"
#include <algorithm>
#include <stdexcept>
namespace wow::android {
namespace {
void erase(void *data, size_t size) noexcept {
    auto *p = static_cast<volatile uint8_t *>(data);
    while (size--)
        *p++ = 0;
}
struct Wipe {
    void *data;
    size_t size;
    ~Wipe() { erase(data, size); }
};
void hash(std::array<uint8_t, 20> &out,
          std::initializer_list<std::pair<const uint8_t *, size_t>> parts) {
    SHA1_CONTEXT context{};
    Wipe clean{&context, sizeof(context)};
    SHA1_Init(&context);
    for (const auto &p : parts)
        SHA1_Update(&context, p.first, uint32_t(p.second));
    SHA1_Final(out.data(), &context);
}
bool equal(const std::array<uint8_t, 20> &a, const std::array<uint8_t, 20> &b) noexcept {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff = uint8_t(diff | (a[i] ^ b[i]));
    return diff == 0;
}
} // namespace
const std::array<uint8_t, 32> &SrpAuthentication::prime() noexcept {
    static const std::array<uint8_t, 32> value{0xb7, 0x9b, 0x3e, 0x2a, 0x87, 0x82, 0x3c, 0xab,
                                               0x8f, 0x5e, 0xbf, 0xbf, 0x8e, 0xb1, 0x01, 0x08,
                                               0x53, 0x50, 0x06, 0x29, 0x8b, 0x5b, 0xad, 0xbd,
                                               0x5b, 0x53, 0xe1, 0x89, 0x5e, 0x64, 0x4b, 0x89};
    return value;
}
SrpAuthentication::~SrpAuthentication() { reset(); }
void SrpAuthentication::clearProof() noexcept {
    erase(&client_, sizeof(client_));
    erase(expected_.data(), expected_.size());
    erase(proof_.publicKey.data(), proof_.publicKey.size());
    erase(proof_.proof.data(), proof_.proof.size());
    erase(proof_.versionProof.data(), proof_.versionProof.size());
    proof_.pin.reset();
    proof_.matrix.reset();
    proof_.token.reset();
}
void SrpAuthentication::reset() noexcept {
    clearProof();
    erase(key_.data(), key_.size());
    state_ = State::Empty;
}
void SrpAuthentication::begin(const std::string &account, const std::string &password,
                              const AuthChallenge &challenge,
                              const std::array<uint8_t, 20> &checksum) {
    beginImpl(account, password, challenge, checksum, nullptr);
}
void SrpAuthentication::beginImpl(const std::string &account, const std::string &password,
                                  const AuthChallenge &challenge,
                                  const std::array<uint8_t, 20> &checksum,
                                  const std::array<uint8_t, 16> *testSeed) {
    const auto savedChecksum = checksum;
    reset();
    state_ = State::Failed;
    if (challenge.result || challenge.generator != std::vector<uint8_t>{7} ||
        challenge.prime != std::vector<uint8_t>(prime().begin(), prime().end()) || challenge.pin ||
        challenge.matrix || challenge.tokenRequired)
        throw std::invalid_argument("Unsupported SRP group/result/security options");
    // Match original B>0 and B<N checks before invoking big integer arithmetic.
    if (std::all_of(challenge.serverPublic.begin(), challenge.serverPublic.end(),
                    [](uint8_t x) { return !x; }) ||
        !std::lexicographical_compare(challenge.serverPublic.rbegin(),
                                      challenge.serverPublic.rend(), prime().rbegin(),
                                      prime().rend()))
        throw std::invalid_argument("Invalid SRP server public value");
    if (account.empty() || account.size() > 255 || password.empty() || password.size() > 255)
        throw std::invalid_argument("Invalid ASCII SRP credential length");
    std::string name = account, secret = password;
    Wipe wipeName{name.data(), name.size()}, wipeSecret{secret.data(), secret.size()};
    for (char &c : name) {
        if (c < 33 || c > 126 || c == ':')
            throw std::invalid_argument("Unsupported SRP account bytes");
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    }
    for (char &c : secret) {
        if (c < 32 || c > 126)
            throw std::invalid_argument("Unsupported SRP password bytes");
        if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
    }
    const auto decoration = name.find('#');
    if (decoration != std::string::npos)
        name[decoration] = '\0';
    if (name[0] == '\0')
        throw std::invalid_argument("Empty undecorated SRP account");
    try {
        std::array<uint8_t, 16> seed{};
        Wipe wipeSeed{seed.data(), seed.size()};
        if (testSeed)
            seed = *testSeed;
        else
            OsSecureRandom(seed.data(), seed.size());
        SRP6_Random random(seed.data(), seed.size());
        Wipe wipeRandom{&random, sizeof(random)};
        if (client_.BeginAuthentication(name.c_str(), secret.c_str()) ||
            client_.CalculateProof(prime().data(), prime().size(), challenge.generator.data(),
                                   challenge.generator.size(), challenge.salt.data(),
                                   challenge.salt.size(), challenge.serverPublic.data(),
                                   challenge.serverPublic.size(), random))
            throw std::runtime_error("Original SRP proof calculation failed");
        std::copy_n(client_.m_publicKey, 32, proof_.publicKey.begin());
        std::copy_n(client_.m_clientProof, 20, proof_.proof.begin());
        hash(proof_.versionProof, {{proof_.publicKey.data(), 32}, {savedChecksum.data(), 20}});
        hash(
            expected_,
            {{proof_.publicKey.data(), 32}, {proof_.proof.data(), 20}, {client_.m_sessionKey, 40}});
        state_ = State::ProofReady;
    } catch (...) {
        clearProof();
        state_ = State::Failed;
        throw;
    }
}
const AuthClientProof &SrpAuthentication::clientProof() const {
    if (state_ != State::ProofReady)
        throw std::logic_error("SRP client proof is unavailable");
    return proof_;
}
bool SrpAuthentication::verify(const AuthServerProof &server) {
    if (state_ != State::ProofReady)
        throw std::logic_error("Unexpected SRP verification state");
    const bool accepted = server.result == 0 && equal(expected_, server.proof) &&
                          client_.VerifyServerProof(server.proof.data(), server.proof.size()) == 0;
    if (accepted)
        std::copy_n(client_.m_sessionKey, 40, key_.begin());
    else
        erase(key_.data(), key_.size());
    clearProof();
    state_ = accepted ? State::Verified : State::Failed;
    return accepted;
}
const std::array<uint8_t, 40> &SrpAuthentication::sessionKey() const {
    if (state_ != State::Verified)
        throw std::logic_error("SRP server proof has not been verified");
    return key_;
}
} // namespace wow::android
