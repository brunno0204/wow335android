#include "WorldObjects.h"
#include "ThirdParty/StormLib/src/zlib/zlib.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
namespace wow::android {
namespace {
constexpr uint32_t SMSG_UPDATE_OBJECT = 0x0a9, SMSG_DESTROY_OBJECT = 0x0aa,
                   SMSG_COMPRESSED_UPDATE_OBJECT = 0x1f6, SMSG_MONSTER_MOVE = 0x0dd,
                   CMSG_NAME_QUERY = 0x050, SMSG_NAME_QUERY_RESPONSE = 0x051,
                   CMSG_ITEM_QUERY_SINGLE = 0x056, SMSG_ITEM_QUERY_SINGLE_RESPONSE = 0x058,
                   CMSG_CREATURE_QUERY = 0x060, SMSG_CREATURE_QUERY_RESPONSE = 0x061;
void need(const std::vector<uint8_t> &p, size_t o, size_t n) {
    if (o > p.size() || p.size() - o < n)
        throw std::invalid_argument("Truncated object update");
}
uint8_t u8(const std::vector<uint8_t> &p, size_t &o) {
    need(p, o, 1);
    return p[o++];
}
uint16_t u16(const std::vector<uint8_t> &p, size_t &o) {
    need(p, o, 2);
    const uint16_t v = uint16_t(p[o] | p[o + 1] << 8);
    o += 2;
    return v;
}
uint32_t u32(const std::vector<uint8_t> &p, size_t &o) {
    need(p, o, 4);
    const uint32_t v = uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 |
                       uint32_t(p[o + 3]) << 24;
    o += 4;
    return v;
}
uint64_t u64(const std::vector<uint8_t> &p, size_t &o) {
    const uint64_t low = u32(p, o);
    return low | uint64_t(u32(p, o)) << 32;
}
float f32(const std::vector<uint8_t> &p, size_t &o) {
    const uint32_t bits = u32(p, o);
    float v;
    std::memcpy(&v, &bits, 4);
    return std::isfinite(v) ? v : 0.f;
}
uint64_t packed(const std::vector<uint8_t> &p, size_t &o) {
    const uint8_t mask = u8(p, o);
    uint64_t guid = 0;
    for (unsigned i = 0; i < 8; ++i)
        if (mask & (1u << i))
            guid |= uint64_t(u8(p, o)) << (8 * i);
    return guid;
}
std::string cstring(const std::vector<uint8_t> &p, size_t &o) {
    const size_t start = o;
    while (o < p.size() && p[o])
        ++o;
    need(p, o, 1);
    std::string s(p.begin() + long(start), p.begin() + long(o));
    ++o;
    return s;
}
void put32(std::vector<uint8_t> &p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p.push_back(uint8_t(v >> (8 * i)));
}
void put64(std::vector<uint8_t> &p, uint64_t v) {
    put32(p, uint32_t(v));
    put32(p, uint32_t(v >> 32));
}
float asFloat(uint32_t bits) {
    float v;
    std::memcpy(&v, &bits, 4);
    return std::isfinite(v) ? v : 0.f;
}
bool isMoveOpcode(uint32_t op) {
    return (op >= 0x0b5 && op <= 0x0c3) || op == 0x0c9 || op == 0x0ca || op == 0x0cb ||
           op == 0x0da || op == 0x0db || op == 0x0ee;
}
} // namespace
bool WorldObjects::handle(const WorldPacket &packet, uint64_t nowMs, std::vector<WorldPacket> &out) {
    const auto &p = packet.payload;
    size_t o = 0;
    try {
        switch (packet.opcode) {
        case SMSG_UPDATE_OBJECT:
            update(p, nowMs);
            break;
        case SMSG_COMPRESSED_UPDATE_OBJECT: {
            const uint32_t size = u32(p, o);
            if (!size || size > 8 * 1024 * 1024)
                throw std::invalid_argument("Invalid compressed update size");
            std::vector<uint8_t> inflated(size);
            z_stream stream{};
            stream.next_in = const_cast<Bytef *>(p.data() + 4);
            stream.avail_in = uInt(p.size() - 4);
            stream.next_out = inflated.data();
            stream.avail_out = uInt(size);
            if (inflateInit(&stream) != Z_OK)
                throw std::invalid_argument("Compressed update inflate init failed");
            const int result = inflate(&stream, Z_FINISH);
            const auto produced = stream.total_out;
            inflateEnd(&stream);
            if (result != Z_STREAM_END && result != Z_OK)
                throw std::invalid_argument("Compressed update inflate failed");
            inflated.resize(produced);
            update(inflated, nowMs);
            break;
        }
        case SMSG_DESTROY_OBJECT: {
            const uint64_t guid = u64(p, o);
            if (const auto found = objects_.find(guid); found != objects_.end() &&
                (found->second.type == ObjectType::Item || found->second.type == ObjectType::Container))
                itemsChanged_ = true;
            changed_ |= objects_.erase(guid) != 0;
            break;
        }
        case SMSG_MONSTER_MOVE:
            monsterMove(p, nowMs);
            break;
        case 0x183: { // SMSG_QUESTGIVER_STATUS: guid, status
            const uint64_t guid = u64(p, o);
            const uint8_t status = p.at(o);
            if (auto found = objects_.find(guid); found != objects_.end() && found->second.questStatus != status) {
                found->second.questStatus = status;
                changed_ = true;
            }
            break;
        }
        case 0x418: { // SMSG_QUESTGIVER_STATUS_MULTIPLE: count, {guid, status}
            const uint32_t count = u32(p, o);
            for (uint32_t i = 0; i < count && o + 9 <= p.size(); ++i) {
                const uint64_t guid = u64(p, o);
                const uint8_t status = p[o++];
                if (auto found = objects_.find(guid); found != objects_.end() && found->second.questStatus != status) {
                    found->second.questStatus = status;
                    changed_ = true;
                }
            }
            break;
        }
        case SMSG_NAME_QUERY_RESPONSE: {
            const uint64_t guid = packed(p, o);
            if (u8(p, o) == 0) {
                playerNames_[guid] = cstring(p, o);
                changed_ = true;
            }
            break;
        }
        case SMSG_CREATURE_QUERY_RESPONSE: {
            const uint32_t entry = u32(p, o);
            if (entry & 0x80000000u)
                break;
            CreatureInfo info;
            info.name = cstring(p, o);
            for (int i = 0; i < 3; ++i)
                cstring(p, o);
            info.subName = cstring(p, o);
            creatures_[entry] = std::move(info);
            changed_ = true;
            break;
        }
        case SMSG_ITEM_QUERY_SINGLE_RESPONSE: {
            const uint32_t entry = u32(p, o);
            if (entry & 0x80000000u) {
                items_[entry & 0x7fffffffu] = {};
                break;
            }
            u32(p, o); // class
            u32(p, o); // subclass
            u32(p, o); // sound override
            for (int i = 0; i < 4; ++i)
                cstring(p, o);
            ItemInfo info;
            info.display = u32(p, o);
            u32(p, o); // quality
            u32(p, o); // flags
            u32(p, o); // flags2
            u32(p, o); // buy price
            u32(p, o); // sell price
            info.inventoryType = u32(p, o);
            items_[entry] = info;
            changed_ = true;
            break;
        }
        default:
            if (isMoveOpcode(packet.opcode)) {
                const uint64_t guid = packed(p, o);
                if (guid != self_)
                    moved(guid, p, o, nowMs);
                return objects_.count(guid) != 0;
            }
            return false;
        }
    } catch (const std::exception &) {
        // A malformed or unsupported update is dropped; the world session continues.
    }
    flushQueries(out);
    return true;
}
void WorldObjects::flushQueries(std::vector<WorldPacket> &out) {
    for (uint64_t guid : nameQueue_) {
        WorldPacket q{CMSG_NAME_QUERY, {}};
        put64(q.payload, guid);
        out.push_back(std::move(q));
    }
    nameQueue_.clear();
    for (const auto &[guid, object] : objects_)
        if (object.type == ObjectType::Unit)
            if (const uint32_t entry = object.get(field::OBJECT_ENTRY);
                entry && askedCreatures_.insert(entry).second)
                creatureQueue_.push_back(entry), (void)guid;
    for (uint32_t entry : creatureQueue_) {
        // The original also sends the GUID of an instance; the entry is what servers use.
        uint64_t guid = 0;
        for (const auto &[g, object] : objects_)
            if (object.get(field::OBJECT_ENTRY) == entry && object.type == ObjectType::Unit) {
                guid = g;
                break;
            }
        WorldPacket q{CMSG_CREATURE_QUERY, {}};
        put32(q.payload, entry);
        put64(q.payload, guid);
        out.push_back(std::move(q));
    }
    creatureQueue_.clear();
    for (uint32_t entry : itemQueue_) {
        WorldPacket q{CMSG_ITEM_QUERY_SINGLE, {}};
        put32(q.payload, entry);
        out.push_back(std::move(q));
    }
    itemQueue_.clear();
}
void WorldObjects::update(const std::vector<uint8_t> &p, uint64_t nowMs) {
    size_t o = 0;
    const uint32_t blocks = u32(p, o);
    for (uint32_t b = 0; b < blocks && b < 4096; ++b) {
        const uint8_t kind = u8(p, o);
        switch (kind) {
        case 0: { // values
            const uint64_t guid = packed(p, o);
            auto &object = objects_[guid];
            readValues(object, p, o);
            request(object, guid);
            noteChange(object, guid);
            break;
        }
        case 1: { // movement
            const uint64_t guid = packed(p, o);
            readMovement(objects_[guid], p, o, nowMs);
            break;
        }
        case 2:
        case 3: { // create
            const uint64_t guid = packed(p, o);
            const uint8_t type = u8(p, o);
            Object object;
            object.type = ObjectType(std::min<uint8_t>(type, 7));
            readMovement(object, p, o, nowMs);
            readValues(object, p, o);
            request(object, guid);
            noteChange(object, guid);
            objects_[guid] = std::move(object);
            break;
        }
        case 4:
        case 5: { // out of range / near objects
            const uint32_t count = u32(p, o);
            for (uint32_t i = 0; i < count && i < 65536; ++i) {
                const uint64_t guid = packed(p, o);
                if (kind == 4) {
                    if (const auto found = objects_.find(guid); found != objects_.end() &&
                        (found->second.type == ObjectType::Item || found->second.type == ObjectType::Container))
                        itemsChanged_ = true;
                    objects_.erase(guid);
                }
            }
            break;
        }
        default:
            throw std::invalid_argument("Unknown update block");
        }
        changed_ = true;
    }
}
void WorldObjects::readMovement(Object &object, const std::vector<uint8_t> &p, size_t &o,
                                uint64_t nowMs) {
    const uint16_t flags = u16(p, o);
    if (flags & 0x20) { // living
        const uint32_t moveFlags = u32(p, o);
        const uint16_t moveFlags2 = u16(p, o);
        u32(p, o); // time
        object.x = f32(p, o);
        object.y = f32(p, o);
        object.z = f32(p, o);
        object.facing = f32(p, o);
        object.timeMs = nowMs;
        object.moveFlags = moveFlags;
        object.walking = moveFlags & 0x100;
        if (moveFlags & 0x200) { // on transport
            packed(p, o);
            for (int i = 0; i < 4; ++i)
                f32(p, o);
            u32(p, o);
            u8(p, o);
            if (moveFlags2 & 0x400)
                u32(p, o);
        }
        if ((moveFlags & (0x200000 | 0x2000000)) || (moveFlags2 & 0x20))
            f32(p, o); // pitch
        u32(p, o);     // fall time
        if (moveFlags & 0x1000)
            for (int i = 0; i < 4; ++i)
                f32(p, o);
        if (moveFlags & 0x4000000)
            f32(p, o);
        float speeds[9];
        for (float &s : speeds)
            s = f32(p, o);
        object.walkSpeed = speeds[0] > 0 && speeds[0] < 100 ? speeds[0] : 2.5f;
        object.runSpeed = speeds[1] > 0 && speeds[1] < 100 ? speeds[1] : 7.f;
        object.path.clear();
        if (moveFlags & 0x8000000) { // spline enabled (MoveSpline::WriteCreate)
            const uint32_t splineFlags = u32(p, o);
            if (splineFlags & 0x20000)
                f32(p, o);
            else if (splineFlags & 0x10000)
                u64(p, o);
            else if (splineFlags & 0x8000)
                for (int i = 0; i < 3; ++i)
                    f32(p, o);
            const int32_t passed = int32_t(u32(p, o));
            const int32_t duration = int32_t(u32(p, o));
            u32(p, o); // id
            f32(p, o);
            f32(p, o);
            f32(p, o);
            u32(p, o);
            const uint32_t nodes = u32(p, o);
            if (nodes > 1000)
                throw std::invalid_argument("Spline node count");
            std::vector<std::array<float, 3>> path(nodes);
            for (auto &node : path)
                for (float &c : node)
                    c = f32(p, o);
            u8(p, o); // mode
            for (int i = 0; i < 3; ++i)
                f32(p, o);
            if (nodes >= 2 && duration > 0 && passed < duration && !(splineFlags & 0x80000)) {
                object.path = std::move(path);
                object.pathDurationMs = uint32_t(duration);
                object.pathStartMs = nowMs - uint64_t(std::max(passed, 0));
                object.walking = splineFlags & 0x1000;
            }
        }
    } else if (flags & 0x100) { // position (transport-relative capable)
        packed(p, o);
        object.x = f32(p, o);
        object.y = f32(p, o);
        object.z = f32(p, o);
        for (int i = 0; i < 3; ++i)
            f32(p, o);
        object.facing = f32(p, o);
        f32(p, o);
        object.timeMs = nowMs;
    } else if (flags & 0x40) { // stationary position
        object.x = f32(p, o);
        object.y = f32(p, o);
        object.z = f32(p, o);
        object.facing = f32(p, o);
        object.timeMs = nowMs;
    }
    if (flags & 0x8)
        u32(p, o);
    if (flags & 0x10)
        u32(p, o);
    if (flags & 0x4)
        packed(p, o);
    if (flags & 0x2)
        u32(p, o);
    if (flags & 0x80) {
        u32(p, o);
        f32(p, o);
    }
    if (flags & 0x200)
        u64(p, o);
}
void WorldObjects::readValues(Object &object, const std::vector<uint8_t> &p, size_t &o) {
    const uint8_t count = u8(p, o);
    std::vector<uint32_t> mask(count);
    for (auto &m : mask)
        m = u32(p, o);
    for (uint32_t block = 0; block < count; ++block)
        for (uint32_t bit = 0; bit < 32; ++bit)
            if (mask[block] & (1u << bit)) {
                const uint32_t index = block * 32 + bit;
                const uint32_t value = u32(p, o);
                if (index < 0x600) {
                    if (object.fields.size() <= index)
                        object.fields.resize(index + 1, 0);
                    object.fields[index] = value;
                }
            }
}
void WorldObjects::noteChange(const Object &object, uint64_t guid) {
    if (guid == self_)
        selfChanged_ = true;
    if (object.type == ObjectType::Item || object.type == ObjectType::Container)
        itemsChanged_ = true;
}
void WorldObjects::playerFields(PlayerFieldsView &view, bool items) const {
    view.fields.clear();
    if (const auto found = objects_.find(self_); found != objects_.end())
        view.fields = found->second.fields;
    if (!items)
        return;
    view.items.clear();
    for (const auto &[guid, object] : objects_)
        if (object.type == ObjectType::Item || object.type == ObjectType::Container)
            view.items[guid] = object.fields;
}
void WorldObjects::request(const Object &object, uint64_t guid) {
    if (object.type == ObjectType::Player && !playerNames_.count(guid) && askedNames_.insert(guid).second)
        nameQueue_.push_back(guid);
    auto item = [&](uint32_t entry) {
        if (entry && !items_.count(entry) && askedItems_.insert(entry).second)
            itemQueue_.push_back(entry);
    };
    if (object.type == ObjectType::Player)
        for (uint32_t slot = 0; slot < 19; ++slot)
            item(object.get(uint16_t(field::PLAYER_VISIBLE_ITEM_1_ENTRYID + slot * 2)));
    if (object.type == ObjectType::Unit)
        for (uint32_t slot = 0; slot < 3; ++slot)
            item(object.get(uint16_t(field::UNIT_VIRTUAL_ITEM_SLOT_ID + slot)));
    if (object.type == ObjectType::Item || object.type == ObjectType::Container)
        item(object.get(field::OBJECT_ENTRY));
}
void WorldObjects::moved(uint64_t guid, const std::vector<uint8_t> &p, size_t &o, uint64_t nowMs) {
    auto found = objects_.find(guid);
    if (found == objects_.end())
        return;
    auto &object = found->second;
    const uint32_t moveFlags = u32(p, o);
    u16(p, o);
    u32(p, o);
    object.x = f32(p, o);
    object.y = f32(p, o);
    object.z = f32(p, o);
    object.facing = f32(p, o);
    object.moveFlags = moveFlags;
    object.walking = moveFlags & 0x100;
    object.timeMs = nowMs;
    object.path.clear();
    changed_ = true;
}
void WorldObjects::monsterMove(const std::vector<uint8_t> &p, uint64_t nowMs) {
    size_t o = 0;
    const uint64_t guid = packed(p, o);
    auto found = objects_.find(guid);
    if (found == objects_.end())
        return;
    auto &object = found->second;
    u8(p, o);
    std::array<float, 3> start{f32(p, o), f32(p, o), f32(p, o)};
    u32(p, o); // spline id
    const uint8_t type = u8(p, o);
    object.x = start[0];
    object.y = start[1];
    object.z = start[2];
    object.timeMs = nowMs;
    object.path.clear();
    object.moveFlags = 0;
    changed_ = true;
    if (type == 1) // stop
        return;
    if (type == 2)
        for (int i = 0; i < 3; ++i)
            f32(p, o);
    else if (type == 3)
        u64(p, o);
    else if (type == 4)
        object.facing = f32(p, o);
    const uint32_t flags = u32(p, o);
    if (flags & 0x200000) { // animation
        u8(p, o);
        u32(p, o);
    }
    const uint32_t duration = u32(p, o);
    if (flags & 0x800) { // parabolic
        f32(p, o);
        u32(p, o);
    }
    const uint32_t count = u32(p, o);
    if (!count || count > 1000)
        return;
    std::vector<std::array<float, 3>> path{start};
    if (flags & (0x40000 | 0x2000)) { // catmull-rom / flying: all points
        for (uint32_t i = 0; i < count; ++i)
            path.push_back({f32(p, o), f32(p, o), f32(p, o)});
    } else {
        const std::array<float, 3> last{f32(p, o), f32(p, o), f32(p, o)};
        const float mx = (start[0] + last[0]) * .5f, my = (start[1] + last[1]) * .5f,
                    mz = (start[2] + last[2]) * .5f;
        for (uint32_t i = 1; i < count; ++i) {
            const uint32_t packedXyz = u32(p, o);
            auto component = [&](int shift, int bits) {
                int32_t v = int32_t(packedXyz >> shift) & ((1 << bits) - 1);
                if (v & (1 << (bits - 1)))
                    v -= 1 << bits;
                return float(v) * .25f;
            };
            path.push_back({mx - component(0, 11), my - component(11, 11), mz - component(22, 10)});
        }
        path.push_back(last);
    }
    if (path.size() >= 2 && duration) {
        object.path = std::move(path);
        object.pathStartMs = nowMs;
        object.pathDurationMs = duration;
        object.walking = flags & 0x1000;
        const auto &a = object.path[object.path.size() - 2], &b = object.path.back();
        if (type != 4 && (std::abs(b[0] - a[0]) > 1e-3f || std::abs(b[1] - a[1]) > 1e-3f))
            object.facing = std::atan2(b[1] - a[1], b[0] - a[0]);
    }
}
std::array<uint32_t, 19> WorldObjects::equipment(uint64_t guid, const Object &object) {
    std::array<uint32_t, 19> displays{};
    for (uint32_t slot = 0; slot < 19; ++slot) {
        const uint32_t entry = object.get(uint16_t(field::PLAYER_VISIBLE_ITEM_1_ENTRYID + slot * 2));
        if (const auto found = items_.find(entry); entry && found != items_.end())
            displays[slot] = found->second.display;
        else if (guid == self_ && entry)
            displays[slot] = selfEquipment_[slot];
    }
    if (guid == self_ && object.fields.size() <= field::PLAYER_VISIBLE_ITEM_1_ENTRYID)
        return selfEquipment_;
    return displays;
}
std::vector<UnitView> WorldObjects::snapshot(uint64_t nowMs) {
    changed_ = false;
    std::vector<UnitView> views;
    views.reserve(objects_.size());
    for (const auto &[guid, object] : objects_) {
        if (object.type != ObjectType::Unit && object.type != ObjectType::Player &&
            object.type != ObjectType::GameObject)
            continue;
        if (guid == self_)
            continue;
        UnitView v;
        v.guid = guid;
        v.type = object.type;
        v.entry = object.get(field::OBJECT_ENTRY);
        v.scale = asFloat(object.get(field::OBJECT_SCALE_X));
        if (!(v.scale > 0.01f && v.scale < 50))
            v.scale = 1;
        v.x = object.x;
        v.y = object.y;
        v.z = object.z;
        v.facing = object.facing;
        v.timeMs = object.timeMs;
        v.path = object.path;
        v.pathStartMs = object.pathStartMs;
        v.pathDurationMs = object.pathDurationMs;
        v.walking = object.walking;
        if (!v.path.empty() && nowMs >= v.pathStartMs + v.pathDurationMs) {
            v.x = v.path.back()[0];
            v.y = v.path.back()[1];
            v.z = v.path.back()[2];
            v.path.clear();
        }
        if (object.type == ObjectType::GameObject) {
            v.displayId = object.get(field::GAMEOBJECT_DISPLAYID);
            if (!v.displayId)
                continue;
            v.modelKey = "g:" + std::to_string(v.displayId);
            v.questStatus = object.questStatus;
            views.push_back(std::move(v));
            continue;
        }
        v.displayId = object.get(field::UNIT_DISPLAYID);
        v.mountDisplay = object.get(field::UNIT_MOUNTDISPLAYID);
        v.sheath = uint8_t(object.get(field::UNIT_BYTES_2) & 0xff);
        v.questStatus = object.questStatus;
        v.factionTemplate = object.get(field::UNIT_FACTIONTEMPLATE);
        v.health = object.get(field::UNIT_HEALTH);
        v.maxHealth = object.get(field::UNIT_MAXHEALTH);
        const uint32_t bytes0 = object.get(field::UNIT_BYTES_0);
        v.race = uint8_t(bytes0);
        v.cls = uint8_t(bytes0 >> 8);
        v.sex = uint8_t(bytes0 >> 16);
        v.powerType = uint8_t(bytes0 >> 24);
        v.power = object.get(uint16_t(field::UNIT_POWER1 + std::min<uint8_t>(v.powerType, 6)));
        v.maxPower = object.get(uint16_t(field::UNIT_MAXPOWER1 + std::min<uint8_t>(v.powerType, 6)));
        v.level = object.get(field::UNIT_LEVEL);
        v.flags = object.get(field::UNIT_FLAGS);
        v.npcFlags = object.get(field::UNIT_NPC_FLAGS);
        v.dead = v.maxHealth && !v.health;
        // Movement flags: forward 1, backward 2, strafe 4/8.
        const uint32_t mf = object.moveFlags;
        if (v.path.empty() && (mf & 0xf) && guid != self_) {
            double dx = 0, dy = 0;
            if (mf & 1) dx += 1;
            if (mf & 2) dx -= 1;
            if (mf & 4) dy += 1;
            if (mf & 8) dy -= 1;
            const double length = std::hypot(dx, dy);
            if (length > 0) {
                const double speed = (mf & 2) && !(mf & 1) ? 4.5 : (v.walking ? object.walkSpeed : object.runSpeed);
                const double c = std::cos(object.facing), s = std::sin(object.facing);
                v.vx = float((c * dx - s * dy) / length * speed);
                v.vy = float((s * dx + c * dy) / length * speed);
                v.moving = true;
            }
        }
        if (!v.path.empty())
            v.moving = true;
        if (object.type == ObjectType::Player) {
            if (const auto found = playerNames_.find(guid); found != playerNames_.end())
                v.name = found->second;
            const uint32_t bytes = object.get(field::PLAYER_BYTES), bytes2 = object.get(field::PLAYER_BYTES_2);
            const uint32_t bytes3 = object.get(field::PLAYER_BYTES_3);
            if (object.fields.size() > field::PLAYER_BYTES_3)
                v.sex = uint8_t(bytes3);
            std::ostringstream key;
            key << "p:" << int(v.race) << ':' << int(v.sex) << ':' << (bytes & 0xff) << ':'
                << ((bytes >> 8) & 0xff) << ':' << ((bytes >> 16) & 0xff) << ':' << ((bytes >> 24) & 0xff)
                << ':' << (bytes2 & 0xff) << ':' << int(v.cls);
            for (uint32_t display : equipment(guid, object))
                key << ':' << display;
            for (uint32_t slot = 15; slot < 18; ++slot) {
                const uint32_t entry = object.get(uint16_t(field::PLAYER_VISIBLE_ITEM_1_ENTRYID + slot * 2));
                const auto found = items_.find(entry);
                key << ':' << (found != items_.end() ? found->second.inventoryType : 0);
            }
            v.modelKey = key.str();
        } else {
            if (const auto found = creatures_.find(v.entry); found != creatures_.end()) {
                v.name = found->second.name;
                v.subName = found->second.subName;
            }
            std::ostringstream key;
            key << "d:" << v.displayId;
            for (uint32_t slot = 0; slot < 3; ++slot) {
                const uint32_t entry = object.get(uint16_t(field::UNIT_VIRTUAL_ITEM_SLOT_ID + slot));
                const auto found = items_.find(entry);
                key << ':' << (found != items_.end() ? found->second.display : 0) << ','
                    << (found != items_.end() ? found->second.inventoryType : 0);
            }
            v.modelKey = key.str();
        }
        if (!v.displayId)
            continue;
        views.push_back(std::move(v));
    }
    return views;
}
HudState WorldObjects::hud() const {
    HudState h;
    const auto found = objects_.find(self_);
    if (found == objects_.end())
        return h;
    const auto &object = found->second;
    h.health = object.get(field::UNIT_HEALTH);
    h.maxHealth = object.get(field::UNIT_MAXHEALTH);
    h.powerType = uint8_t(object.get(field::UNIT_BYTES_0) >> 24);
    h.power = object.get(uint16_t(field::UNIT_POWER1 + std::min<uint8_t>(h.powerType, 6)));
    h.maxPower = object.get(uint16_t(field::UNIT_MAXPOWER1 + std::min<uint8_t>(h.powerType, 6)));
    h.level = object.get(field::UNIT_LEVEL);
    h.mountDisplay = object.get(field::UNIT_MOUNTDISPLAYID);
    h.unitFlags = object.get(field::UNIT_FLAGS);
    h.sheath = uint8_t(object.get(field::UNIT_BYTES_2) & 0xff);
    h.factionTemplate = object.get(field::UNIT_FACTIONTEMPLATE);
    return h;
}
bool WorldObjects::describe(uint64_t guid, Interaction &out) const {
    const auto found = objects_.find(guid);
    if (found == objects_.end())
        return false;
    const auto &o = found->second;
    out.type = o.type;
    out.entry = o.get(field::OBJECT_ENTRY);
    out.x = o.x;
    out.y = o.y;
    out.z = o.z;
    if (o.type == ObjectType::Unit || o.type == ObjectType::Player) {
        out.npcFlags = o.get(field::UNIT_NPC_FLAGS);
        out.dynamicFlags = o.get(field::UNIT_DYNAMIC_FLAGS);
        out.unitFlags = o.get(field::UNIT_FLAGS);
        out.dead = o.get(field::UNIT_MAXHEALTH) && !o.get(field::UNIT_HEALTH);
    }
    return true;
}
std::string WorldObjects::nameOf(uint64_t guid) const {
    if (const auto found = playerNames_.find(guid); found != playerNames_.end())
        return found->second;
    if (const auto found = objects_.find(guid); found != objects_.end())
        if (const auto c = creatures_.find(found->second.get(field::OBJECT_ENTRY)); c != creatures_.end())
            return c->second.name;
    return {};
}
} // namespace wow::android
