#pragma once
#include "CharacterList.h"
namespace wow::android {
struct CharacterLoginRequest {
    CharacterSummary character;
    WorldPacket packet;
};
struct WorldEntry {
    uint32_t map = 0;
    std::array<float, 3> position{};
    float orientation = 0;
};
// Explicit current-list index; preserves the full GUID and takes an owned copy.
// Reject known transfer/billing/rename/customization gates. No DBC/expansion/
// declension/UI eligibility validation is provided by this portable policy.
CharacterLoginRequest prepareCharacterLogin(const CharacterList &, size_t index);
// Exact original SMSG_LOGIN_VERIFY_WORLD(0x236): map, XYZ and orientation.
// This validates a protocol confirmation only; no map/assets/player are loaded.
WorldEntry decodeWorldEntry(const WorldPacket &);
uint8_t decodeCharacterLoginFailure(const WorldPacket &);
} // namespace wow::android
