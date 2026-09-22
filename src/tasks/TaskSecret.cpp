#include "TaskSecret.h"

#include <cstring>

void taskSecretEncode(const uint8_t bytes[TASK_SECRET_BYTES], char out[TASK_SECRET_LEN + 1]) {
  // Accumulateur de bits : on y pousse un octet des qu'il reste moins de 5
  // bits, et on en retire 5 par caractere. 16 octets = 128 bits = 25
  // caracteres pleins + 3 bits, completes a droite par deux zeros.
  uint32_t buffer = 0;
  int bits = 0;
  size_t in = 0;
  for (size_t i = 0; i < TASK_SECRET_LEN; ++i) {
    if (bits < 5) {
      if (in < TASK_SECRET_BYTES) {
        buffer = (buffer << 8) | bytes[in++];
        bits += 8;
      } else {
        buffer <<= 5 - bits;
        bits = 5;
      }
    }
    bits -= 5;
    out[i] = TASK_SECRET_ALPHABET[(buffer >> bits) & 0x1F];
  }
  out[TASK_SECRET_LEN] = '\0';
}

bool taskSecretIsCanonical(const char* s) {
  if (s == nullptr) return false;
  for (size_t i = 0; i < TASK_SECRET_LEN; ++i) {
    // strchr trouverait aussi le terminateur de l'alphabet : il faut exclure
    // '\0' explicitement, sinon une chaine courte passerait.
    if (s[i] == '\0' || std::strchr(TASK_SECRET_ALPHABET, s[i]) == nullptr) return false;
  }
  return s[TASK_SECRET_LEN] == '\0';
}

bool taskSecretGroup(const char* canonical, char out[TASK_SECRET_GROUPED_LEN + 1]) {
  out[0] = '\0';
  if (!taskSecretIsCanonical(canonical)) return false;
  size_t o = 0;
  for (size_t i = 0; i < TASK_SECRET_LEN; ++i) {
    if (i > 0 && i % 4 == 0) out[o++] = ' ';
    out[o++] = canonical[i];
  }
  out[o] = '\0';
  return true;
}
