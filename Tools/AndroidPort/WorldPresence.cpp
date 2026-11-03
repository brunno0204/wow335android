#include "WorldPresence.h"
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace wow::android {
namespace {
constexpr double pi = 3.14159265358979323846;
void put32(std::vector<uint8_t> &p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p.push_back(uint8_t(v >> (8 * i)));
}
void put16(std::vector<uint8_t> &p, uint16_t v) {
    p.push_back(uint8_t(v));
    p.push_back(uint8_t(v >> 8));
}
void putFloat(std::vector<uint8_t> &p, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put32(p, bits);
}
void need(const std::vector<uint8_t> &p, size_t o, size_t n) {
    if (o > p.size() || p.size() - o < n)
        throw std::invalid_argument("Truncated in-world packet");
}
uint32_t get32(const std::vector<uint8_t> &p, size_t &o) {
    need(p, o, 4);
    const uint32_t v = uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 |
                       uint32_t(p[o + 3]) << 24;
    o += 4;
    return v;
}
uint16_t get16(const std::vector<uint8_t> &p, size_t &o) {
    need(p, o, 2);
    const uint16_t v = uint16_t(p[o] | p[o + 1] << 8);
    o += 2;
    return v;
}
float getFloat(const std::vector<uint8_t> &p, size_t &o) {
    const uint32_t bits = get32(p, o);
    float v;
    std::memcpy(&v, &bits, 4);
    if (!std::isfinite(v))
        throw std::invalid_argument("Nonfinite in-world coordinate");
    return v;
}
float normalizeFacing(double facing) {
    facing = std::fmod(facing, 2 * pi);
    if (facing < 0)
        facing += 2 * pi;
    return float(facing);
}
} // namespace
void putPackedGuid(std::vector<uint8_t> &out, uint64_t guid) {
    const size_t mask = out.size();
    out.push_back(0);
    for (unsigned i = 0; i < 8; ++i)
        if (const uint8_t byte = uint8_t(guid >> (8 * i))) {
            out[mask] |= uint8_t(1u << i);
            out.push_back(byte);
        }
}
uint64_t readPackedGuid(const std::vector<uint8_t> &in, size_t &offset) {
    need(in, offset, 1);
    const uint8_t mask = in[offset++];
    uint64_t guid = 0;
    for (unsigned i = 0; i < 8; ++i)
        if (mask & (1u << i)) {
            need(in, offset, 1);
            guid |= uint64_t(in[offset++]) << (8 * i);
        }
    return guid;
}
void putMovementStatus(std::vector<uint8_t> &p, const MovementStatus &s) {
    put32(p, s.flags);
    put16(p, s.extraFlags);
    put32(p, s.time);
    putFloat(p, s.x);
    putFloat(p, s.y);
    putFloat(p, s.z);
    putFloat(p, s.facing);
    if (s.flags & (MOVEFLAG_SWIMMING | 0x2000000u)) // swimming / flying: pitch
        putFloat(p, 0);
    put32(p, s.fallTime);
    if (s.flags & MOVEFLAG_FALLING) {
        putFloat(p, s.zSpeed);
        putFloat(p, s.sinAngle);
        putFloat(p, s.cosAngle);
        putFloat(p, s.xySpeed);
    }
}
MovementStatus readMovementHead(const std::vector<uint8_t> &in, size_t &o) {
    MovementStatus s;
    s.flags = get32(in, o);
    s.extraFlags = get16(in, o);
    s.time = get32(in, o);
    s.x = getFloat(in, o);
    s.y = getFloat(in, o);
    s.z = getFloat(in, o);
    s.facing = getFloat(in, o);
    return s;
}
WorldPacket encodeMovement(uint32_t opcode, uint64_t guid, const MovementStatus &status) {
    WorldPacket packet{opcode, {}};
    putPackedGuid(packet.payload, guid);
    putMovementStatus(packet.payload, status);
    return packet;
}
void WorldPresence::begin(uint64_t guid, const WorldEntry &entry, uint32_t nowMs) {
    if (!guid)
        throw std::invalid_argument("In-world presence requires the player GUID");
    reset();
    guid_ = guid;
    active_ = true;
    lastPingMs_ = nowMs;
    relocate(entry.map, entry.position[0], entry.position[1], entry.position[2],
             entry.orientation);
    notice_ = "Entrou no mundo";
}
void WorldPresence::relocate(uint32_t map, float x, float y, float z, float facing) {
    placement_ = {++requiredSerial_, map, x, y, z, normalizeFacing(facing)};
    haveSent_ = wasMoving_ = false;
    wasGrounded_ = true;
    sent_ = {};
    sent_.x = x;
    sent_.y = y;
    sent_.z = z;
    sent_.facing = placement_.facing;
}
std::vector<WorldPacket> WorldPresence::handle(const WorldPacket &packet, uint32_t nowMs) {
    std::vector<WorldPacket> out;
    if (!active_)
        return out;
    const auto &p = packet.payload;
    size_t o = 0;
    switch (packet.opcode) {
    case opcode::SMSG_TIME_SYNC_REQ: {
        const uint32_t counter = get32(p, o);
        WorldPacket reply{opcode::CMSG_TIME_SYNC_RESP, {}};
        put32(reply.payload, counter);
        put32(reply.payload, nowMs);
        out.push_back(std::move(reply));
        break;
    }
    case opcode::SMSG_PONG:
        if (pingOutstanding_ && get32(p, o) == pingSerial_) {
            latency_ = nowMs - pingSentMs_;
            pingOutstanding_ = false;
        }
        break;
    case opcode::SMSG_TRANSFER_PENDING:
        notice_ = "Transferindo de mapa…";
        break;
    case opcode::SMSG_NEW_WORLD: {
        const uint32_t map = get32(p, o);
        const float x = getFloat(p, o), y = getFloat(p, o), z = getFloat(p, o),
                    facing = getFloat(p, o);
        relocate(map, x, y, z, facing);
        out.push_back({opcode::MSG_MOVE_WORLDPORT_ACK, {}});
        notice_ = "Novo mapa " + std::to_string(map);
        break;
    }
    case opcode::MSG_MOVE_TELEPORT_ACK: {
        if (readPackedGuid(p, o) != guid_)
            break;
        const uint32_t counter = get32(p, o);
        const auto head = readMovementHead(p, o);
        relocate(placement_.map, head.x, head.y, head.z, head.facing);
        WorldPacket reply{opcode::MSG_MOVE_TELEPORT_ACK, {}};
        putPackedGuid(reply.payload, guid_);
        put32(reply.payload, counter);
        put32(reply.payload, nowMs);
        out.push_back(std::move(reply));
        notice_ = "Teleportado";
        break;
    }
    case opcode::SMSG_FORCE_RUN_SPEED_CHANGE: {
        if (readPackedGuid(p, o) != guid_)
            break;
        const uint32_t counter = get32(p, o);
        need(p, o, 1);
        ++o; // original extra byte for run speed only
        const float speed = getFloat(p, o);
        if (speed <= 0 || speed > 1000)
            throw std::invalid_argument("Invalid forced run speed");
        runSpeed_ = speed;
        WorldPacket reply{opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK, {}};
        putPackedGuid(reply.payload, guid_);
        put32(reply.payload, counter);
        auto current = sent_;
        current.time = nowMs;
        current.flags &= ~MOVEFLAG_FALLING;
        putMovementStatus(reply.payload, current);
        putFloat(reply.payload, speed);
        out.push_back(std::move(reply));
        break;
    }
    case opcode::SMSG_LOGOUT_COMPLETE:
        loggedOut_ = true;
        active_ = false;
        notice_ = "Sessão encerrada pelo servidor";
        break;
    default:
        ++ignored_;
    }
    return out;
}
MovementStatus WorldPresence::status(const MoverSample &s, uint32_t nowMs) const {
    MovementStatus m;
    const bool moving = s.directionX != 0 || s.directionY != 0;
    m.flags = (moving ? (s.backward ? MOVEFLAG_BACKWARD : MOVEFLAG_FORWARD) : 0) | (s.grounded ? 0 : MOVEFLAG_FALLING) |
              (s.walking ? MOVEFLAG_WALKING : 0) | (s.swimming ? MOVEFLAG_SWIMMING : 0);
    m.time = nowMs;
    m.x = float(s.x);
    m.y = float(s.y);
    m.z = float(s.z);
    m.facing = s.hasFacing ? normalizeFacing(s.facing)
               : moving ? normalizeFacing(std::atan2(s.directionY, s.directionX)) : sent_.facing;
    if (!s.grounded) {
        m.fallTime = nowMs - fallStartMs_;
        // Original jump sign convention: negative z speed while rising.
        m.zSpeed = float(-s.verticalSpeed);
        m.cosAngle = float(moving ? s.directionX : 0);
        m.sinAngle = float(moving ? s.directionY : 0);
        m.xySpeed = float(moving ? s.horizontalSpeed : 0);
    }
    return m;
}
std::vector<WorldPacket> WorldPresence::tick(const MoverSample &sample, uint32_t nowMs) {
    std::vector<WorldPacket> out;
    if (!active_)
        return out;
    if (!pingOutstanding_ && nowMs - lastPingMs_ >= 30000) {
        WorldPacket ping{opcode::CMSG_PING, {}};
        put32(ping.payload, ++pingSerial_);
        put32(ping.payload, latency_);
        out.push_back(std::move(ping));
        lastPingMs_ = pingSentMs_ = nowMs;
        pingOutstanding_ = true;
    } else if (pingOutstanding_ && nowMs - pingSentMs_ >= 60000) {
        pingOutstanding_ = false; // allow a new ping; the server owns disconnect policy
    }
    if (!sample.valid)
        return out;
    const bool moving = sample.directionX != 0 || sample.directionY != 0;
    const bool grounded = sample.grounded;
    if (wasGrounded_ && !grounded)
        fallStartMs_ = nowMs;
    auto next = status(sample, nowMs);
    std::optional<uint32_t> op;
    if (wasGrounded_ && !grounded && sample.verticalSpeed > 0)
        op = opcode::MSG_MOVE_JUMP;
    else if (!wasGrounded_ && grounded) {
        op = opcode::MSG_MOVE_FALL_LAND;
        next.fallTime = nowMs - fallStartMs_;
    } else if (sample.swimming != wasSwimming_ && haveSent_) {
        op = sample.swimming ? opcode::MSG_MOVE_START_SWIM : opcode::MSG_MOVE_STOP_SWIM;
        wasSwimming_ = sample.swimming;
    } else if (sample.walking != wasWalking_ && haveSent_)
        op = sample.walking ? opcode::MSG_MOVE_SET_WALK_MODE : opcode::MSG_MOVE_SET_RUN_MODE;
    else if (grounded && moving && (!wasMoving_ || sample.backward != wasBackward_))
        op = sample.backward ? opcode::MSG_MOVE_START_BACKWARD : opcode::MSG_MOVE_START_FORWARD;
    else if (grounded && !moving && wasMoving_)
        op = opcode::MSG_MOVE_STOP;
    else if (moving && haveSent_ && std::abs(std::remainder(next.facing - sent_.facing, 2 * pi)) > .1 &&
             nowMs - lastSendMs_ >= 100)
        op = opcode::MSG_MOVE_SET_FACING;
    else if ((moving || !grounded) && nowMs - lastSendMs_ >= 500)
        op = opcode::MSG_MOVE_HEARTBEAT;
    const bool modeChange = op && (*op == opcode::MSG_MOVE_SET_WALK_MODE || *op == opcode::MSG_MOVE_SET_RUN_MODE ||
                                   *op == opcode::MSG_MOVE_START_SWIM || *op == opcode::MSG_MOVE_STOP_SWIM);
    if (!modeChange) // a start/stop in the same frame goes out on the next tick
        wasMoving_ = moving;
    wasGrounded_ = grounded;
    if (op && (*op == opcode::MSG_MOVE_SET_WALK_MODE || *op == opcode::MSG_MOVE_SET_RUN_MODE))
        wasWalking_ = sample.walking;
    if (op && (*op == opcode::MSG_MOVE_START_BACKWARD || *op == opcode::MSG_MOVE_START_FORWARD))
        wasBackward_ = sample.backward;
    if (!moving)
        wasBackward_ = false;
    if (!op)
        return out;
    out.push_back(encodeMovement(*op, guid_, next));
    sent_ = next;
    haveSent_ = true;
    lastSendMs_ = nowMs;
    ++moves_;
    return out;
}
} // namespace wow::android
