#pragma once

#include <cstdint>
#include <string>

#include "Epub.h"

namespace serialization {

inline void writeString(HalFile&, const std::string&) {}
inline void readString(HalFile&, std::string& out) { out.clear(); }
inline bool tryReadString(HalFile&, std::string&) { return false; }

template <typename T>
inline void writePod(HalFile&, const T&) {}

template <typename T>
inline bool tryReadPod(HalFile&, T&) {
  return false;
}

}  // namespace serialization
