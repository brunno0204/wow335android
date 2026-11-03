#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>
namespace wow::android {
// Basic 3.3.5a Grunt wire subset, derived from original ClientLink methods.
// Decoded proof/result bytes are UNVERIFIED; no authentication state is implied.
struct AuthClientIdentity {
    uint32_t program = 0x00576f57, processor = 0x00783836, os = 0x0057696e; // OsGetOsId(): bytes "niW" -> server reads "Win"
    uint32_t locale = 0x656e5553;
    int32_t timezoneMinutes = 0;
    std::array<uint8_t, 4> ipv4{};
};
std::vector<uint8_t> encodeAuthHello(const std::string &account,
                                     const AuthClientIdentity &identity = {});
struct AuthPinChallenge {
    uint32_t seed = 0;
    std::array<uint8_t, 16> salt{};
};
struct AuthMatrixChallenge {
    uint8_t width = 0, height = 0, digits = 0, count = 0;
    uint64_t seed = 0;
};
struct AuthChallenge {
    uint8_t result = 0;
    std::array<uint8_t, 32> serverPublic{}, salt{};
    std::array<uint8_t, 16> versionChallenge{};
    std::vector<uint8_t> generator, prime;
    std::optional<AuthPinChallenge> pin;
    std::optional<AuthMatrixChallenge> matrix;
    std::optional<uint8_t> tokenRequired;
};
struct AuthClientProof {
    std::array<uint8_t, 32> publicKey{};
    std::array<uint8_t, 20> proof{}, versionProof{};
    struct Pin {
        std::array<uint8_t, 16> salt{};
        std::array<uint8_t, 20> hash{};
    };
    std::optional<Pin> pin;
    std::optional<std::array<uint8_t, 20>> matrix;
    std::optional<std::string> token;
};
// Caller supplies computed SRP/version/security proofs. No crypto is performed.
// CD-key count is zero; reconnect/patch/survey commands are unsupported.
std::vector<uint8_t> encodeAuthProof(const AuthClientProof &proof);
std::vector<uint8_t> encodeRealmListRequest();
struct AuthServerProof {
    uint8_t result = 0;
    std::array<uint8_t, 20> proof{};
    uint32_t accountFlags = 0, survey = 0;
    uint16_t loginFlags = 0;
    std::optional<uint16_t> unknownAccountExtra;
};
struct AuthRealmVersion {
    uint8_t major = 0, minor = 0, patch = 0;
    uint16_t build = 0;
};
struct AuthRealm {
    uint8_t type = 0, locked = 0, flags = 0, characters = 0, timezone = 0, id = 0;
    std::string name, address;
    float population = 0;
    std::optional<AuthRealmVersion> version;
};
struct AuthRealmList {
    uint32_t unknown = 0;
    std::vector<AuthRealm> realms;
    uint16_t trailer = 0;
};
using AuthMessage = std::variant<AuthChallenge, AuthServerProof, AuthRealmList>;
class AuthWireDecoder {
  public:
    static constexpr size_t capacity = 128 * 1024;
    void feed(const std::vector<uint8_t> &bytes);
    // Incomplete message consumes nothing. Malformed/unsupported input poisons
    // the decoder and clears bytes; reset is required. Outputs own all data.
    std::optional<AuthMessage> next();
    void finish(); // Call after draining messages at EOF; partial bytes are fatal.
    void reset() noexcept {
        bytes_.clear();
        offset_ = 0;
        failed_ = false;
    }
    size_t buffered() const noexcept { return bytes_.size() - offset_; }
    bool failed() const noexcept { return failed_; }

  private:
    std::vector<uint8_t> bytes_;
    size_t offset_ = 0;
    bool failed_ = false;
};
} // namespace wow::android
