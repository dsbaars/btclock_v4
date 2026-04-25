// Walk the three partition CSVs, validate offsets/sizes/ordering.
//
// Rules we enforce (from ESP-IDF partition_table docs + experience):
//   - App partitions: offset must be aligned to 0x10000 (MMU page).
//   - Data partitions: offset must be aligned to 0x1000 (flash sector).
//   - Partitions appear in strictly increasing order of offset.
//   - No partition extends past the declared flash size.
//   - No two partitions overlap.
//   - The two OTA app slots (ota_0 / ota_1) are the same size.

#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef BTCLOCK_PROJECT_ROOT
#error "BTCLOCK_PROJECT_ROOT must be defined (see test_host/CMakeLists.txt)"
#endif

namespace {

struct Row {
  std::string name;
  std::string type;
  std::string sub_type;
  uint32_t offset = 0;
  uint32_t size = 0;
};

uint32_t ParseHex(std::string s) {
  // Trim whitespace. Accepts "0x...", bare hex, or decimal with no 0x.
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  s.erase(0, i);
  return static_cast<uint32_t>(std::stoul(s, nullptr, 0));
}

std::string Trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return s.substr(i);
}

std::vector<Row> LoadCsv(const std::string& path) {
  std::ifstream f(path);
  REQUIRE_MESSAGE(f.good(), "cannot open " << path);
  std::vector<Row> rows;
  std::string line;
  while (std::getline(f, line)) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    std::vector<std::string> fields;
    std::string cell;
    std::stringstream ss(trimmed);
    while (std::getline(ss, cell, ',')) fields.push_back(Trim(cell));
    // 5 fields required, Flags (6th) optional.
    REQUIRE(fields.size() >= 5);
    Row r;
    r.name = fields[0];
    r.type = fields[1];
    r.sub_type = fields[2];
    r.offset = ParseHex(fields[3]);
    r.size = ParseHex(fields[4]);
    rows.push_back(r);
  }
  return rows;
}

void CheckCsv(const std::string& path, uint32_t flash_size) {
  const auto rows = LoadCsv(path);
  CAPTURE(path);
  REQUIRE(!rows.empty());

  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& r = rows[i];
    CAPTURE(r.name);
    CAPTURE(r.offset);
    CAPTURE(r.size);

    // Alignment: app=64KB, everything else=4KB.
    const uint32_t align = (r.type == "app") ? 0x10000u : 0x1000u;
    CHECK_MESSAGE((r.offset % align) == 0,
                  r.name << " offset not aligned to 0x" << std::hex << align);

    const uint64_t end = static_cast<uint64_t>(r.offset) + r.size;
    CHECK_MESSAGE(end <= flash_size,
                  r.name << " extends past flash end (flash=0x" << std::hex
                         << flash_size << ")");

    if (i > 0) {
      const auto& prev = rows[i - 1];
      CHECK_MESSAGE(prev.offset < r.offset,
                    r.name << " is not strictly after " << prev.name);
      const uint64_t prev_end =
          static_cast<uint64_t>(prev.offset) + prev.size;
      CHECK_MESSAGE(prev_end <= r.offset,
                    prev.name << " overlaps " << r.name);
    }
  }

  // OTA app slots must match in size so the OTA flow can copy between them.
  const Row* app0 = nullptr;
  const Row* app1 = nullptr;
  for (const auto& r : rows) {
    if (r.sub_type == "ota_0") app0 = &r;
    else if (r.sub_type == "ota_1") app1 = &r;
  }
  // Parens around the expression stop doctest from trying to decompose `&&`.
  REQUIRE_MESSAGE((app0 && app1), "both OTA slots required in " << path);
  CHECK_MESSAGE(app0->size == app1->size,
                "ota_0 and ota_1 must be the same size");
}

}  // namespace

TEST_CASE("partitions_4mb.csv: offsets, sizes, OTA symmetry") {
  CheckCsv(BTCLOCK_PROJECT_ROOT "/partitions_4mb.csv", 0x400000);
}

TEST_CASE("partitions_8mb.csv: offsets, sizes, OTA symmetry") {
  CheckCsv(BTCLOCK_PROJECT_ROOT "/partitions_8mb.csv", 0x800000);
}

TEST_CASE("partitions_16mb.csv: offsets, sizes, OTA symmetry") {
  CheckCsv(BTCLOCK_PROJECT_ROOT "/partitions_16mb.csv", 0x1000000);
}
