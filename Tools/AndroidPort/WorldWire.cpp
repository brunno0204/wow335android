#include "WorldWire.h"
#include "Engine/Source/BlizzardCore/Source/Packages/Util/SHA1.h"
#include "Engine/Source/Storm/H/storm.h"
#include "WoW/Source/WowServices/HMAC.h"
#include <algorithm>
#include <stdexcept>
namespace wow::android {
namespace {
void erase(void *data, size_t size) noexcept {
    auto *bytes = static_cast<volatile uint8_t *>(data);
    while (size--)
        *bytes++ = 0;
}
struct Wipe {
    void *data;
    size_t size;
    ~Wipe() { erase(data, size); }
};
// Same fixed seed halves/direction as original WowConnection::SetEncryptionKey.
constexpr uint8_t seeds[2][16] = {{0xcc, 0x98, 0xae, 0x04, 0xe8, 0x97, 0xea, 0xca, 0x12, 0xdd, 0xc0,
                                   0x93, 0x42, 0x91, 0x53, 0x57},
                                  {0xc2, 0xb3, 0x72, 0x3c, 0xc6, 0xae, 0xd9, 0xb5, 0x34, 0x3c, 0x53,
                                   0xee, 0x2f, 0x43, 0x67, 0xce}};
void prepare(SARC4Key &state, const uint8_t *seed, const std::array<uint8_t, 40> &key) {
    HMAC<SHA1> hmac{};
    SHA1::Digest digest{};
    std::array<uint8_t, 1024> discard{};
    Wipe cleanHmac{&hmac, sizeof(hmac)}, cleanDigest{&digest, sizeof(digest)},
        cleanDiscard{discard.data(), discard.size()};
    hmac.Init(seed, 16); // Fixed bounded key, not the generic long-key HMAC API.
    hmac.Process(key.data(), key.size());
    hmac.Finish(digest);
    SARC4PrepareKey(digest.m_data, sizeof(digest.m_data), &state);
    SARC4ProcessBuffer(discard.data(), discard.size(), &state, &state);
}
} // namespace
WorldWire::~WorldWire() { reset(); }
void WorldWire::reset() noexcept {
    erase(&send_, sizeof(send_));
    erase(&receive_, sizeof(receive_));
    bytes_.clear();
    offset_ = 0;
    encrypted_ = failed_ = false;
    boundary_ = true;
}
void WorldWire::poison() noexcept {
    reset();
    failed_ = true;
}
void WorldWire::enableEncryption(const std::array<uint8_t, 40> &key) {
    if (failed_ || encrypted_ || !boundary_)
        throw std::logic_error("Invalid world cipher transition");
    const auto direction = role_ == Role::Client ? 1 : 0;
    prepare(send_, seeds[direction], key);
    prepare(receive_, seeds[direction ^ 1], key);
    encrypted_ = true;
}
std::vector<uint8_t> WorldWire::encode(uint32_t opcode, const std::vector<uint8_t> &payload) {
    if (failed_)
        throw std::logic_error("World codec requires reset");
    const size_t width = role_ == Role::Client ? 4 : 2;
    if ((width == 2 && opcode > 0xffff) || payload.size() > maxPacket - width)
        throw std::length_error("World packet exceeds opcode/size limit");
    const auto size = payload.size() + width;
    const size_t prefix = size > 0x7fff ? 3 : 2;
    std::vector<uint8_t> result(prefix + size);
    if (prefix == 3) {
        result[0] = uint8_t(0x80 | (size >> 16));
        result[1] = uint8_t(size >> 8);
        result[2] = uint8_t(size);
    } else {
        result[0] = uint8_t(size >> 8);
        result[1] = uint8_t(size);
    }
    for (size_t i = 0; i < width; ++i)
        result[prefix + i] = uint8_t(opcode >> (i * 8));
    std::copy(payload.begin(), payload.end(), result.begin() + prefix + width);
    if (encrypted_)
        SARC4ProcessBuffer(result.data(), prefix + width, &send_, &send_);
    return result;
}
void WorldWire::feed(const std::vector<uint8_t> &bytes) {
    if (failed_)
        throw std::logic_error("World codec requires reset");
    if (bytes.size() > capacity - buffered())
        throw std::length_error("World receive capacity exceeded");
    // Reserve before compaction so an allocation failure preserves accepted input.
    if (bytes_.capacity() < buffered() + bytes.size())
        bytes_.reserve(buffered() + bytes.size());
    if (offset_) {
        bytes_.erase(bytes_.begin(), bytes_.begin() + offset_);
        offset_ = 0;
    }
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}
std::optional<WorldPacket> WorldWire::next() {
    if (failed_)
        throw std::logic_error("World codec requires reset");
    if (!buffered())
        return std::nullopt;
    const size_t width = role_ == Role::Client ? 2 : 4;
    SARC4Key trial = receive_;
    Wipe cleanTrial{&trial, sizeof(trial)};
    std::array<uint8_t, 7> header{};
    header[0] = bytes_[offset_];
    if (encrypted_)
        SARC4ProcessBuffer(header.data(), 1, &trial, &trial);
    const size_t prefix = header[0] & 0x80 ? 3 : 2;
    const size_t headerSize = prefix + width;
    if (buffered() < headerSize) {
        boundary_ = false;
        return std::nullopt;
    }
    std::copy_n(bytes_.data() + offset_ + 1, headerSize - 1, header.data() + 1);
    if (encrypted_)
        SARC4ProcessBuffer(header.data() + 1, headerSize - 1, &trial, &trial);
    size_t size = header[0] & 0x7f;
    for (size_t i = 1; i < prefix; ++i)
        size = (size << 8) | header[i];
    if (size < width || size > maxPacket || (prefix == 3 && size <= 0x7fff)) {
        poison();
        throw std::invalid_argument("Invalid world packet size");
    }
    if (buffered() < prefix + size) {
        boundary_ = false;
        return std::nullopt;
    }
    WorldPacket packet;
    for (size_t i = 0; i < width; ++i)
        packet.opcode |= uint32_t(header[prefix + i]) << (i * 8);
    packet.payload.assign(bytes_.begin() + offset_ + headerSize,
                          bytes_.begin() + offset_ + prefix + size);
    if (encrypted_)
        receive_ = trial;
    offset_ += prefix + size;
    boundary_ = true;
    return packet;
}
void WorldWire::finish() {
    if (failed_)
        throw std::logic_error("World codec requires reset");
    if (buffered()) {
        poison();
        throw std::invalid_argument("Truncated world packet at EOF");
    }
}
} // namespace wow::android
