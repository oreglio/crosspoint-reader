#pragma once

#include <cstdint>

#include "../../persistable_store_atomic/stubs/Arduino.h"

// TaskStore::reserveFullCapacity() lit le plus grand bloc libre du tas.
struct EspClass {
  uint32_t getMaxAllocHeap() const { return 1u << 20; }
};
inline EspClass ESP;
