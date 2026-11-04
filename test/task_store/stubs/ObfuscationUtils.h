#pragma once
#include <string>
namespace obfuscation {
enum class DecodeStatus { INVALID, VALIDATED, LEGACY };
inline std::string obfuscateToBase64(const std::string& value) { return value; }
inline std::string deobfuscateFromBase64(const char* value, DecodeStatus* status) {
  *status = DecodeStatus::VALIDATED;
  return value;
}
}  // namespace obfuscation
