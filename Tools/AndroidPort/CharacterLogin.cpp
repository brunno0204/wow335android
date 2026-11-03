#include "CharacterLogin.h"
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace wow::android {
CharacterLoginRequest prepareCharacterLogin(const CharacterList &list, size_t index) {
    if (index >= list.characters.size())
        throw std::out_of_range("Character index outside current list");
    const auto &c = list.characters[index];
    if (!c.guid || c.name.empty() || c.name.size() > 47 || c.name.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid selected character identity");
    if ((c.flags & (0x4u | 0x1000000u | 0x4000u)) ||
        (c.customizeFlags & (0x1u | 0x10000u | 0x100000u)))
        throw std::invalid_argument(
            "Character requires an unsupported unlock/name/customization flow");
    CharacterLoginRequest request{c, {0x3d, {}}};
    request.packet.payload.resize(8);
    for (unsigned i = 0; i < 8; ++i)
        request.packet.payload[i] = uint8_t(c.guid >> (i * 8));
    return request;
}
WorldEntry decodeWorldEntry(const WorldPacket &packet) {
    if (packet.opcode != 0x236 || packet.payload.size() != 20)
        throw std::invalid_argument("Invalid world-entry confirmation");
    auto word = [&](size_t i) {
        return uint32_t(packet.payload[i]) | (uint32_t(packet.payload[i + 1]) << 8) |
               (uint32_t(packet.payload[i + 2]) << 16) | (uint32_t(packet.payload[i + 3]) << 24);
    };
    auto real = [&](size_t i) {
        const auto bits = word(i);
        float value;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value))
            throw std::invalid_argument("Nonfinite world-entry transform");
        return value;
    };
    return {word(0), {real(4), real(8), real(12)}, real(16)};
}
uint8_t decodeCharacterLoginFailure(const WorldPacket &packet) {
    if (packet.opcode != 0x41 || packet.payload.size() != 1)
        throw std::invalid_argument("Invalid character-login rejection");
    return packet.payload[0];
}
} // namespace wow::android
