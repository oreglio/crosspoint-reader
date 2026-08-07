#include "KOReaderEmbeddedId.h"

#include <Logging.h>
#include <ZipFile.h>

#include <cstdlib>

namespace {
// The payload is ~70 bytes; anything larger is not ours and is not inflated.
constexpr size_t MAX_INFLATED_BYTES = 512;

bool skipSpaces(const std::string_view json, size_t& pos) {
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) pos++;
  return pos < json.size();
}
}  // namespace

std::string KOReaderEmbeddedId::parse(const std::string_view json) {
  static constexpr std::string_view VERSION_KEY = "\"version\"";
  size_t pos = json.find(VERSION_KEY);
  if (pos == std::string_view::npos) return "";
  pos += VERSION_KEY.size();
  if (!skipSpaces(json, pos) || json[pos] != ':') return "";
  pos++;
  if (!skipSpaces(json, pos) || json[pos] != '1') return "";
  pos++;
  // "1" followed by another digit is a future format (10, 12, ...): drop it.
  if (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') return "";

  static constexpr std::string_view ID_KEY = "\"koreaderPartialMd5\"";
  pos = json.find(ID_KEY);
  if (pos == std::string_view::npos) return "";
  pos += ID_KEY.size();
  if (!skipSpaces(json, pos) || json[pos] != ':') return "";
  pos++;
  if (!skipSpaces(json, pos) || json[pos] != '"') return "";
  pos++;
  if (pos + 33 > json.size() || json[pos + 32] != '"') return "";
  for (size_t i = 0; i < 32; i++) {
    const char c = json[pos + i];
    const bool hexDigit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hexDigit) return "";
  }
  return std::string(json.substr(pos, 32));
}

std::string KOReaderEmbeddedId::read(const std::string& epubPath) {
  ZipFile zip(epubPath);
  if (!zip.open()) {
    // Not an error worth logging: sync also runs on non-zip formats.
    return "";
  }

  std::string id;
  size_t inflatedSize = 0;
  if (zip.getInflatedFileSize(SYNC_ID_PATH, &inflatedSize)) {
    if (inflatedSize > 0 && inflatedSize <= MAX_INFLATED_BYTES) {
      size_t size = 0;
      uint8_t* data = zip.readFileToMemory(SYNC_ID_PATH, &size, /*trailingNullByte=*/false);
      if (data) {
        id = parse(std::string_view(reinterpret_cast<const char*>(data), size));
        free(data);  // readFileToMemory hands over a malloc'd buffer
        if (id.empty()) LOG_ERR("KOSync", "Embedded sync id present but malformed; ignoring");
      } else {
        LOG_ERR("KOSync", "Embedded sync id could not be read");
      }
    } else {
      LOG_ERR("KOSync", "Embedded sync id has implausible size %zu; ignoring", inflatedSize);
    }
  }
  zip.close();
  return id;
}
