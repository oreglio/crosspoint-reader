#pragma once
#include <Epub.h>

#include <memory>
#include <optional>
#include <string>

#include "CrossPointPosition.h"
#include "KOReaderSyncClient.h"

/**
 * KOReader position representation.
 */
struct KOReaderPosition {
  std::string xpath;                 // XPath-like progress string
  float percentage;                  // Progress percentage (0.0 to 1.0)
  bool valid = true;                 // False when an exact requested coordinate mapping is unavailable
  bool needsReoptimization = false;  // Source-document mapping is missing or unusable
};

enum class PositionCoordinateSpace : uint8_t {
  CurrentDocument,
  SourceDocument,
};

/**
 * Maps between CrossPoint and KOReader position formats.
 *
 * CrossPoint tracks position as (spineIndex, pageNumber).
 * KOReader uses XPath-like strings + percentage.
 *
 * Outgoing positions retain the chapter's HTML ancestry. If structural
 * resolution fails, the position is invalid and must not be uploaded.
 */
class ProgressMapper {
 public:
  /**
   * Convert CrossPoint position to KOReader format.
   *
   * @param epub The EPUB book
   * @param pos CrossPoint position
   * @return KOReader position
   */
  static KOReaderPosition toKOReader(
      const std::shared_ptr<Epub>& epub, const CrossPointPosition& pos,
      PositionCoordinateSpace coordinateSpace = PositionCoordinateSpace::CurrentDocument);

  /**
   * Convert KOReader position to CrossPoint format.
   *
   * Note: The returned pageNumber may be approximate since different
   * rendering settings produce different page counts.
   *
   * @param epub The EPUB book
   * @param koPos KOReader position
   * @param currentSpineIndex Index of the currently open spine item (for density estimation)
   * @param totalPagesInCurrentSpine Total pages in the current spine item (for density estimation)
   * @return CrossPoint position
   */
  static CrossPointPosition toCrossPoint(
      const std::shared_ptr<Epub>& epub, const KOReaderPosition& koPos, int currentSpineIndex = -1,
      int totalPagesInCurrentSpine = 0,
      PositionCoordinateSpace coordinateSpace = PositionCoordinateSpace::CurrentDocument);

  /**
   * Convert a rich CrossPoint position (downloaded from a crosspoint-sync
   * server) directly to a CrossPoint position, without XPath approximation.
   * When the local layout matches the uploader's (same spine page count) the
   * page transfers losslessly; otherwise the paragraph LUT or the intra-spine
   * page fraction is used.
   *
   * @return The position, or std::nullopt when the rich position cannot be
   *         applied (spine out of range, no section cache) and the caller
   *         should fall back to toCrossPoint().
   */
  static std::optional<CrossPointPosition> fromRichPosition(const std::shared_ptr<Epub>& epub,
                                                            const KOReaderRichPosition& rich, GfxRenderer& renderer,
                                                            const char* sectionCacheSuffix);
};
