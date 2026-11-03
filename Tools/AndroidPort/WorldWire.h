#pragma once
#include "Engine/Source/Storm/H/Types.h"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>
namespace wow::android {
struct WorldPacket {
    uint32_t opcode = 0;
    std::vector<uint8_t> payload;
};
// Worker-owned Wrath framing/header cipher only; no opcode dispatch,
// authentication, compression/addon interpretation or gameplay state implied.
class WorldWire {
  public:
    enum class Role { Client, Server };
    static constexpr size_t maxPacket = 1024 * 1024; // size includes opcode
    static constexpr size_t capacity = 2 * maxPacket;
    explicit WorldWire(Role role = Role::Client) : role_(role) {}
    ~WorldWire();
    WorldWire(const WorldWire &) = delete;
    WorldWire &operator=(const WorldWire &) = delete;
    void reset() noexcept;
    // One transition at a packet boundary; already buffered raw next-packet
    // bytes are permitted. Reject after an incomplete next() or if already on.
    // Caller must first send its unencrypted world auth packet; this API cannot
    // infer authentication state. Standard seeds only; redirected keys absent.
    void enableEncryption(const std::array<uint8_t, 40> &sessionKey);
    std::vector<uint8_t> encode(uint32_t opcode, const std::vector<uint8_t> &payload);
    void feed(const std::vector<uint8_t> &bytes);
    // Incomplete input never consumes bytes or advances receive cipher state.
    // Malformed input/partial EOF poisons the codec and erases cipher state.
    std::optional<WorldPacket> next();
    void finish();
    size_t buffered() const noexcept { return bytes_.size() - offset_; }
    bool encrypted() const noexcept { return encrypted_; }
    bool failed() const noexcept { return failed_; }

  private:
    void poison() noexcept;
    Role role_;
    SARC4Key send_{}, receive_{};
    std::vector<uint8_t> bytes_;
    size_t offset_ = 0;
    bool encrypted_ = false, failed_ = false, boundary_ = true;
};
} // namespace wow::android
