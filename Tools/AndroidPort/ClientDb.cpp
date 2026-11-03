#include "ClientDb.h"
#include <cstring>
#include <stdexcept>
namespace wow::android {
namespace {
uint32_t read32(const std::vector<uint8_t> &b, size_t o) {
    return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 |
           uint32_t(b[o + 3]) << 24;
}
} // namespace
DbcTable::DbcTable(std::vector<uint8_t> bytes, uint32_t fields, uint32_t recordSize,
                   const char *name)
    : bytes_(std::move(bytes)), name_(name ? name : "") {
    auto fail = [&](const char *why) {
        throw std::invalid_argument(std::string(name_) + ": " + why);
    };
    if (bytes_.size() < 20 || read32(bytes_, 0) != 0x43424457)
        fail("invalid WDBC header");
    rows_ = read32(bytes_, 4);
    fields_ = read32(bytes_, 8);
    recordSize_ = read32(bytes_, 12);
    stringSize_ = read32(bytes_, 16);
    if ((fields && fields_ != fields) || (recordSize && recordSize_ != recordSize))
        fail("unexpected column layout");
    if (rows_ > 1000000 || recordSize_ > 65536 ||
        uint64_t(rows_) * recordSize_ + stringSize_ + 20 != bytes_.size())
        fail("size mismatch");
    if (fields_ * 4 > recordSize_ && recordSize_ != fields_) // byte-packed tables allowed
        fail("field count exceeds record");
}
uint32_t DbcTable::u32(uint32_t row, uint32_t field) const {
    if (row >= rows_ || (field + 1) * 4 > recordSize_)
        throw std::out_of_range(std::string(name_) + ": field out of range");
    return read32(bytes_, 20 + size_t(row) * recordSize_ + field * 4);
}
float DbcTable::f32(uint32_t row, uint32_t field) const {
    const uint32_t bits = u32(row, field);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
uint8_t DbcTable::byte(uint32_t row, uint32_t offset) const {
    if (row >= rows_ || offset >= recordSize_)
        throw std::out_of_range(std::string(name_) + ": byte out of range");
    return bytes_[20 + size_t(row) * recordSize_ + offset];
}
std::string DbcTable::str(uint32_t row, uint32_t field) const {
    const uint32_t offset = u32(row, field);
    if (offset >= stringSize_)
        return {};
    const size_t begin = 20 + size_t(rows_) * recordSize_ + offset;
    size_t end = begin;
    while (end < bytes_.size() && bytes_[end])
        ++end;
    return std::string(bytes_.begin() + long(begin), bytes_.begin() + long(end));
}
std::string DbcTable::loc(uint32_t row, uint32_t firstField) const {
    for (uint32_t i = 0; i < 16; ++i) {
        auto value = str(row, firstField + i);
        if (!value.empty())
            return value;
    }
    return {};
}
} // namespace wow::android
