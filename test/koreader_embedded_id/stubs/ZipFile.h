#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

// Host-test stub: serves one canned entry and mirrors lib/ZipFile's ownership
// contract (readFileToMemory returns a malloc'd buffer the caller frees).
class ZipFile {
 public:
  static inline std::string entryName;
  static inline std::string entryContent;
  static inline bool openable = true;

  explicit ZipFile(const std::string&) {}
  bool open() { return openable; }
  bool close() { return true; }
  bool getInflatedFileSize(const char* name, size_t* size) {
    if (!openable || entryName != name) return false;
    *size = entryContent.size();
    return true;
  }
  uint8_t* readFileToMemory(const char* name, size_t* size = nullptr, bool trailingNullByte = false) {
    if (!openable || entryName != name) return nullptr;
    const size_t n = entryContent.size();
    uint8_t* buf = static_cast<uint8_t*>(malloc(n + (trailingNullByte ? 1 : 0)));
    if (!buf) return nullptr;
    memcpy(buf, entryContent.data(), n);
    if (trailingNullByte) buf[n] = 0;
    if (size) *size = n;
    return buf;
  }
};
