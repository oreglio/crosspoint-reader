#pragma once
#include <string>
#include <string_view>

/**
 * Reads the sync identity embedded in optimized EPUBs.
 *
 * The web optimizer rewrites every entry of an EPUB, which changes the
 * binary partial-MD5 KOReader uses as the document id. To keep progress
 * sync paired with the original file, the optimizer computes the ORIGINAL
 * file's partial MD5 in the browser and stores it inside the optimized copy
 * as META-INF/crossink-sync.json. This class reads it back.
 *
 * Payload (see docs/file-formats.md):
 *   {"version":1,"koreaderPartialMd5":"<32 lowercase hex>"}
 */
class KOReaderEmbeddedId {
 public:
  static constexpr const char* SYNC_ID_PATH = "META-INF/crossink-sync.json";

  // Read the embedded id from an EPUB. Empty string when absent or invalid.
  static std::string read(const std::string& epubPath);

  // Extract the id from the JSON payload; empty string when the version is
  // unknown or the id is malformed. Pure — host-tested.
  static std::string parse(std::string_view json);
};
