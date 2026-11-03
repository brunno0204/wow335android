#pragma once
#include "WorldObjects.h"
#include "WorldWire.h"
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>
namespace wow::android {
// Network thread: server packets the original interface reacts to (auras, casts, cooldowns,
// combat log, loot, NPC dialogs, vendors, trainers, death, friends, guild, who, zone), turned
// into string events for the UI's Lua game layer (ui/5x_*.lua), which keeps the state.
class ServerEvents {
  public:
    using Event = std::vector<std::string>;
    using NameOf = std::function<std::string(uint64_t)>;
    void reset(uint64_t self) { *this = ServerEvents{}; self_ = self; }
    // true when the packet was one of ours; queries/replies go to out.
    bool handle(const WorldPacket &packet, const NameOf &nameOf, std::vector<WorldPacket> &out);
    std::vector<Event> take() { return std::move(events_); }
    std::vector<UnitAction> takeActions() { return std::move(actions_); }
    // Corpse distance for the ghost (CORPSE_IN_RANGE / CORPSE_OUT_OF_RANGE).
    void corpseDistance(bool ghost, double distance);
    // Game time from SMSG_LOGIN_SETTIMESPEED (half-minutes since midnight) and its serial.
    uint32_t gameHalfMinutes() const noexcept { return gameHalfMinutes_; }
    uint32_t gameTimeSerial() const noexcept { return gameTimeSerial_; }

  private:
    void emit(Event event) {
        if (events_.size() < 4096)
            events_.push_back(std::move(event));
    }
    uint64_t self_ = 0;
    std::vector<Event> events_;
    std::vector<UnitAction> actions_;
    void act(uint64_t guid, uint16_t anim, uint32_t ms = 0, bool loop = false) {
        if (guid && actions_.size() < 512)
            actions_.push_back({guid, anim, ms, loop});
    }
    bool corpseInRange_ = false;
    uint32_t gameHalfMinutes_ = 0, gameTimeSerial_ = 0;
};
std::string guidHex(uint64_t guid);
} // namespace wow::android
