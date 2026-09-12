#pragma once

// The shelf's remembered posture: which view, which orders, which way the
// titles run, and the book the cursor was on. Preferences rather than primary
// data — favorites must survive anything, this merely spares the reader from
// re-expressing choices after every sleep. This reader deep-sleeps between
// sessions and wakes through a full boot, so RAM statics alone forget the
// shelf several times a day; a lost or corrupt file costs nothing but
// defaults.
//
// The selection anchor is an identity, not a row number, so it survives a
// sort change, a filter, and even an index rebuild between sessions.
//
// The on-disk byte layout lives in LibraryStateCodec.h, split out so it can
// be host-tested.

#include "LibraryStateCodec.h"

namespace library {

// A missing file is defaults and success; a corrupt or implausible one is
// defaults too, logged. Same reject-don't-guess stance as everything else
// that reads this directory.
bool loadLibraryState(LibraryShelfState& out);
// Written through the same write-beside-then-rename install step the index
// and favorites use.
bool saveLibraryState(const LibraryShelfState& state);
// If the remembered cursor sits on `from`, follow the book to its new
// identity. No-op (false) when the selection is elsewhere or unset.
bool reanchorLibraryStateSelection(const FavoriteKey& from, const FavoriteKey& to);

const char* libraryStatePath();

// Freshness. Every ingestion path — nearby transfer, web upload, WebDAV, OPDS
// and Calibre downloads, USB serial — calls the first when a file lands on the
// card; the Library consumes the flag on its next entry and rebuilds,
// reconciled, so the newcomer tops Recently added and everyone else keeps
// their place. Only book files mark it: a font or image upload must not cost
// a rebuild. A file rather than RAM because ingesters and shelf do not always
// share a boot — web uploads arrive in a dedicated network boot mode.
void markShelfStaleIfBook(const char* path);
// True exactly once per marking: reading consumes the flag.
bool takeShelfStale();

}  // namespace library
