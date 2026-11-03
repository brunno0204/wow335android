#pragma once
#include "AuthWire.h"
#include "WoW/Source/WowServices/SRP6_Client.h"
namespace wow::android {
struct SrpTestAccess;
// Worker-owned basic Wrath group only. No network/controller/login UI here.
class SrpAuthentication {
  public:
    enum class State { Empty, ProofReady, Verified, Failed };
    SrpAuthentication() = default;
    ~SrpAuthentication();
    SrpAuthentication(const SrpAuthentication &) = delete;
    SrpAuthentication &operator=(const SrpAuthentication &) = delete;
    void reset() noexcept;
    // Starts a NEW attempt, clearing old state even on failure. ASCII credentials,
    // original decorated-account policy, fixed N/g, no PIN/matrix/token yet.
    // versionChecksum must be computed by caller; this derives H(A|checksum).
    void begin(const std::string &account, const std::string &password,
               const AuthChallenge &challenge, const std::array<uint8_t, 20> &versionChecksum);
    const AuthClientProof &clientProof() const;
    // One-shot server proof. Failed wire result/proof wipes session material;
    // unexpected call order throws. Key access is forbidden until verification.
    bool verify(const AuthServerProof &server);
    const std::array<uint8_t, 40> &sessionKey() const;
    State state() const noexcept { return state_; }
    static const std::array<uint8_t, 32> &prime() noexcept;

  private:
    friend struct SrpTestAccess; // Deterministic synthetic fixtures; no public seed option.
    void beginImpl(const std::string &, const std::string &, const AuthChallenge &,
                   const std::array<uint8_t, 20> &, const std::array<uint8_t, 16> *);
    void clearProof() noexcept;
    SRP6_Client client_{};
    AuthClientProof proof_;
    std::array<uint8_t, 20> expected_{};
    std::array<uint8_t, 40> key_{};
    State state_ = State::Empty;
};
} // namespace wow::android
