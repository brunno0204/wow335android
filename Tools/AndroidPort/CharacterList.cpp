#include "CharacterList.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace wow::android {
namespace {
class Cursor {
  public:
    explicit Cursor(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}
    uint64_t number(size_t count) {
        if (count > remaining())
            throw std::invalid_argument("Truncated character record");
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i)
            value |= uint64_t(bytes_[offset_++]) << (i * 8);
        return value;
    }
    uint8_t byte() { return uint8_t(number(1)); }
    uint32_t word() { return uint32_t(number(4)); }
    float real() {
        const uint32_t bits = word();
        float value;
        static_assert(sizeof(value) == sizeof(bits));
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value))
            throw std::invalid_argument("Nonfinite character position");
        return value;
    }
    std::string name() {
        const auto first = offset_;
        while (remaining() && bytes_[offset_]) {
            if (offset_ - first >= 47)
                throw std::invalid_argument("Character name exceeds47 bytes");
            ++offset_;
        }
        if (!remaining() || offset_ == first)
            throw std::invalid_argument("Missing character name");
        std::string value(bytes_.begin() + first, bytes_.begin() + offset_);
        ++offset_;
        return value;
    }
    size_t remaining() const { return bytes_.size() - offset_; }

  private:
    const std::vector<uint8_t> &bytes_;
    size_t offset_ = 0;
};
} // namespace
CharacterList decodeCharacterList(const WorldPacket &packet) {
    if (packet.opcode != 0x3b)
        throw std::invalid_argument("Unexpected character-list opcode");
    Cursor cursor(packet.payload);
    const auto count = cursor.byte();
    if (count > 10)
        throw std::invalid_argument("Character count exceeds original limit");
    CharacterList result;
    result.characters.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        CharacterSummary character;
        character.guid = cursor.number(8);
        if (!character.guid ||
            std::any_of(result.characters.begin(), result.characters.end(),
                        [&](const auto &entry) { return entry.guid == character.guid; }))
            throw std::invalid_argument("Zero or duplicate character GUID");
        character.name = cursor.name();
        character.race = cursor.byte();
        character.classId = cursor.byte();
        character.sex = cursor.byte();
        character.skin = cursor.byte();
        character.face = cursor.byte();
        character.hairStyle = cursor.byte();
        character.hairColor = cursor.byte();
        character.facialHair = cursor.byte();
        character.level = cursor.byte();
        character.zone = cursor.word();
        character.map = cursor.word();
        for (auto &component : character.position)
            component = cursor.real();
        character.guild = cursor.word();
        character.flags = cursor.word();
        character.customizeFlags = cursor.word();
        const auto firstLogin = cursor.byte();
        character.firstLogin = firstLogin != 0;
        character.petDisplay = cursor.word();
        character.petLevel = cursor.word();
        character.petFamily = cursor.word();
        for (auto &slot : character.equipment) {
            slot.display = cursor.word();
            slot.inventoryType = cursor.byte();
            slot.visual = cursor.word();
        }
        result.characters.push_back(std::move(character));
    }
    if (cursor.remaining()) {
        if (cursor.remaining() < 40)
            return result; // unknown short tail: ignored like the original reader
        result.raceRestrictions.emplace();
        for (auto &restriction : *result.raceRestrictions)
            restriction = cursor.word();
    }
    return result;
}
} // namespace wow::android
