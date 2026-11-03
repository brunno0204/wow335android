#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
namespace wow::android::m2 {
inline void require(bool condition, const char *message) {
    if (!condition)
        throw std::invalid_argument(message);
}
inline uint32_t u32(const std::vector<uint8_t> &b, size_t o) {
    require(o <= b.size() && b.size() - o >= 4, "Truncated M2/SKIN field");
    return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24;
}
inline uint16_t u16(const std::vector<uint8_t> &b, size_t o) {
    require(o <= b.size() && b.size() - o >= 2, "Truncated M2 field");
    return b[o] | uint16_t(b[o + 1]) << 8;
}
template <class T>
std::vector<T> records(const std::vector<uint8_t> &b, uint32_t count, uint32_t offset, uint32_t limit,
                       size_t header) {
    static_assert(std::is_trivially_copyable_v<T>);
#if defined(__BYTE_ORDER__)
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "M2 records require little endian");
#endif
    require(count <= limit, "M2/SKIN array exceeds budget");
    if (!count)
        return {};
    require(offset >= header && offset <= b.size() && count <= (b.size() - offset) / sizeof(T),
            "M2/SKIN array exceeds file");
    std::vector<T> out(count);
    std::memcpy(out.data(), b.data() + offset, size_t(count) * sizeof(T));
    return out;
}
template <class T>
std::vector<T> array(const std::vector<uint8_t> &b, size_t descriptor, uint32_t limit, size_t header) {
    return records<T>(b, u32(b, descriptor), u32(b, descriptor + 4), limit, header);
}
inline size_t modelHeader(const std::vector<uint8_t> &b) {
    require(b.size() >= 0x130 && b.size() <= 128 * 1024 * 1024, "Invalid M2 file size");
    require(u32(b, 0) == 0x3032444d && u32(b, 4) == 264, "Unsupported M2 magic/version");
    size_t size = (u32(b, 0x10) & 8) ? 0x138 : 0x130;
    require(b.size() >= size, "Truncated optional M2 header");
    return size;
}
std::vector<uint8_t> readFile(const std::string &path, uint32_t limit = 128 * 1024 * 1024);
} // namespace wow::android::m2
