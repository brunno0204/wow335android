#pragma once
#include "CharacterLogin.h"
#include "WorldWire.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace wow::android {
// Original 3.3.5a opcodes used by the in-world presence (WoW/Source/Net/Types.h).
namespace opcode {
constexpr uint32_t SMSG_NEW_WORLD = 0x03e, SMSG_TRANSFER_PENDING = 0x03f,
                   SMSG_LOGOUT_COMPLETE = 0x04d, MSG_MOVE_START_FORWARD = 0x0b5, MSG_MOVE_START_BACKWARD = 0x0b6,
                   MSG_MOVE_SET_RUN_MODE = 0x0c2, MSG_MOVE_SET_WALK_MODE = 0x0c3,
                   MSG_MOVE_START_SWIM = 0x0cb, MSG_MOVE_STOP_SWIM = 0x0cc,
                   MSG_MOVE_STOP = 0x0b7, MSG_MOVE_JUMP = 0x0bb, MSG_MOVE_TELEPORT_ACK = 0x0c7,
                   MSG_MOVE_FALL_LAND = 0x0c9, MSG_MOVE_SET_FACING = 0x0da,
                   MSG_MOVE_WORLDPORT_ACK = 0x0dc, SMSG_FORCE_RUN_SPEED_CHANGE = 0x0e2,
                   CMSG_FORCE_RUN_SPEED_CHANGE_ACK = 0x0e3, MSG_MOVE_HEARTBEAT = 0x0ee,
                   CMSG_PING = 0x1dc, SMSG_PONG = 0x1dd, SMSG_TIME_SYNC_REQ = 0x390,
                   CMSG_TIME_SYNC_RESP = 0x391;
}
constexpr uint32_t MOVEFLAG_FORWARD = 0x1, MOVEFLAG_BACKWARD = 0x2, MOVEFLAG_WALKING = 0x100,
                   MOVEFLAG_SWIMMING = 0x200000,
                   MOVEFLAG_FALLING = 0x1000;
// Original CMovementStatus wire subset (CMovementStatus.cpp operator<<): no transport,
// pitch or spline elevation. Jump fields are written only with MOVEFLAG_FALLING.
struct MovementStatus {
    uint32_t flags = 0;
    uint16_t extraFlags = 0;
    uint32_t time = 0;
    float x = 0, y = 0, z = 0, facing = 0;
    uint32_t fallTime = 0;
    float zSpeed = 0, sinAngle = 0, cosAngle = 0, xySpeed = 0;
};
void putPackedGuid(std::vector<uint8_t> &out, uint64_t guid);
uint64_t readPackedGuid(const std::vector<uint8_t> &in, size_t &offset);
void putMovementStatus(std::vector<uint8_t> &out, const MovementStatus &status);
// Reads flags..facing, skipping the optional tail is not needed by callers.
MovementStatus readMovementHead(const std::vector<uint8_t> &in, size_t &offset);
WorldPacket encodeMovement(uint32_t opcode, uint64_t guid, const MovementStatus &status);

// Local mover produced by the frame owner (development walker, not CMovement_C).
struct MoverSample {
    bool valid = false;
    double x = 0, y = 0, z = 0;
    double directionX = 0, directionY = 0; // horizontal unit command, 0 when idle
    double horizontalSpeed = 0, verticalSpeed = 0;
    bool grounded = true;
    bool backward = false, walking = false, swimming = false; // MOVEFLAG_BACKWARD / WALKING / SWIMMING
    double facing = 0;                      // body facing (radians); backpedal faces ahead
    bool hasFacing = false;
};
struct TerrainPlacement {
    uint32_t serial = 0, map = 0;
    float x = 0, y = 0, z = 0, facing = 0;
    bool stream = false; // local tile-streaming request (keeps the player where it is)
};
// Network-worker-owned in-world protocol: time sync, ping, server relocation and
// movement reporting for the active player only. No object updates, chat or combat.
class WorldPresence {
  public:
    void begin(uint64_t guid, const WorldEntry &entry, uint32_t nowMs);
    void reset() noexcept { *this = WorldPresence{}; }
    bool active() const noexcept { return active_; }
    bool loggedOut() const noexcept { return loggedOut_; }
    // Returns replies. Unknown opcodes are counted and ignored.
    std::vector<WorldPacket> handle(const WorldPacket &packet, uint32_t nowMs);
    // Caller drops samples from a stale placement (before a teleport/new world).
    std::vector<WorldPacket> tick(const MoverSample &sample, uint32_t nowMs);
    // Latest server-required placement; serial increases on login/teleport/new world.
    const TerrainPlacement &placement() const noexcept { return placement_; }
    const MovementStatus &lastSent() const noexcept { return sent_; }
    uint32_t latency() const noexcept { return latency_; }
    float runSpeed() const noexcept { return runSpeed_; }
    uint32_t ignoredPackets() const noexcept { return ignored_; }
    uint32_t movementPackets() const noexcept { return moves_; }
    const std::string &notice() const noexcept { return notice_; }

  private:
    void relocate(uint32_t map, float x, float y, float z, float facing);
    MovementStatus status(const MoverSample &sample, uint32_t nowMs) const;
    bool active_ = false, loggedOut_ = false;
    uint64_t guid_ = 0;
    TerrainPlacement placement_;
    uint32_t requiredSerial_ = 0;
    MovementStatus sent_;
    bool haveSent_ = false, wasMoving_ = false, wasGrounded_ = true, wasWalking_ = false, wasBackward_ = false, wasSwimming_ = false;
    uint32_t lastSendMs_ = 0, fallStartMs_ = 0;
    uint32_t pingSerial_ = 0, lastPingMs_ = 0, pingSentMs_ = 0, latency_ = 0;
    bool pingOutstanding_ = false;
    float runSpeed_ = 7;
    uint32_t ignored_ = 0, moves_ = 0;
    std::string notice_;
};
} // namespace wow::android
