#pragma once
#include <string>

// Which document ids one KOSync visit reads and writes. Pure, host-tested.
//
// Readest and KOReader name a book by the partial MD5 of the file THEY hold
// (Readest has no other method: its "filename" option was greyed out, then
// removed). A book optimized for this reader carries two ids: the original's,
// embedded by the optimizer (KOReaderEmbeddedId), and this card's copy's.
// A phone may hold either file, so a visit that writes under one id only
// leaves the other reader blind to this device's progress.

// The id to write besides `chosen`: the other of {embedded original, this
// copy}. Empty when the book has a single identity (no embedded id, an
// unreadable copy, or an embedded id equal to the copy's), or when `chosen` is
// neither of the two (a filename match).
inline std::string koreaderCompanionUploadHash(const std::string& chosen, const std::string& embedded,
                                               const std::string& copy) {
  if (embedded.empty() || copy.empty() || embedded == copy) return "";
  if (chosen == embedded) return copy;
  if (chosen == copy) return embedded;
  return "";
}

// Whether to look beyond the primary id. Smart sync always does; a book
// carrying an embedded id does in every mode, since the other reader's record
// may sit under either of its two ids.
inline bool koreaderProbeAlternateIdentities(const bool smartSync, const bool hasEmbeddedId) {
  return smartSync || hasEmbeddedId;
}
