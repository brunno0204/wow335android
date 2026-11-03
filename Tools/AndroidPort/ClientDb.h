#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace wow::android {
// Read-only WDBC table (DBFilesClient\*.dbc). Column order and counts follow the
// original record readers in WoW/Source/DB/Rec; strings are offsets into the block.
class DbcTable {
  public:
    DbcTable() = default;
    // fields/recordSize: expected layout from the matching *Rec.h; 0 skips the check.
    DbcTable(std::vector<uint8_t> bytes, uint32_t fields, uint32_t recordSize, const char *name);
    uint32_t rows() const noexcept { return rows_; }
    bool empty() const noexcept { return !rows_; }
    uint32_t u32(uint32_t row, uint32_t field) const;
    int32_t i32(uint32_t row, uint32_t field) const { return int32_t(u32(row, field)); }
    float f32(uint32_t row, uint32_t field) const;
    uint8_t byte(uint32_t row, uint32_t offset) const; // byte-packed tables (CharBaseInfo)
    std::string str(uint32_t row, uint32_t field) const;
    // Localized string: 16 locale offsets + flags; first non-empty locale wins.
    std::string loc(uint32_t row, uint32_t firstField) const;

  private:
    std::vector<uint8_t> bytes_;
    uint32_t rows_ = 0, fields_ = 0, recordSize_ = 0, stringSize_ = 0;
    const char *name_ = "";
};
} // namespace wow::android
