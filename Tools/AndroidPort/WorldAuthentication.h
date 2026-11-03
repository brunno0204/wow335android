#pragma once
#include "Engine/Source/BlizzardCore/Source/Packages/Util/SHA1.h"
#include "RealmSession.h"
#include "WorldWire.h"
namespace wow::android {
struct WorldChallenge {
    uint32_t difficulty = 0;
    std::array<uint8_t, 4> serverSeed{};
    std::array<uint8_t, 32> workSeed{};
};
WorldChallenge decodeWorldChallenge(const WorldPacket &);
struct WorldAuthStatus {
    uint8_t result = 0, billingFlags = 0, expansion = 0;
    uint32_t billingRemaining = 0, billingRested = 0, queuePosition = 0;
    bool hasBilling = false, freeMigration = false;
};
WorldAuthStatus decodeWorldAuthStatus(const WorldPacket &);
// Original account/work-seed/nonce SHA1 rule, performed in bounded worker steps.
// Local policy: difficulty<=20 and at most2^20 attempts; no unbounded brute force.
class WorldProofOfWork {
  public:
    ~WorldProofOfWork();
    WorldProofOfWork() = default;
    WorldProofOfWork(const WorldProofOfWork &) = delete;
    WorldProofOfWork &operator=(const WorldProofOfWork &) = delete;
    void begin(std::string_view account, const WorldChallenge &);
    std::optional<uint64_t> step(uint32_t budget); // 1..4096 attempts
    void reset() noexcept;
    uint32_t attempts() const noexcept { return attempts_; }

  private:
    SHA1_CONTEXT base_{};
    uint32_t difficulty_ = 0, attempts_ = 0;
    uint64_t nonce_ = 0;
    bool active_ = false, ready_ = false;
};
// Fresh OS client seed; original account/zero/client/server/session-key digest.
// Standard Grunt login fields and an explicitly empty compressed addon list.
// Build comes from the verified ticket's bounded world profile (12340 or experimental12341).
WorldPacket encodeWorldAuthentication(const RealmSession &, const WorldChallenge &, uint64_t nonce);
} // namespace wow::android
