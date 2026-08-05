#pragma once

// Text normalisation for the Library index: the fold used for search and sort
// keys, the filename parse that produces the pre-enrichment row, and the author
// key that merges spellings of one person.
//
// Pure functions over UTF-8, no hardware and no allocation beyond the returned
// strings, so the whole unit is host-testable (test/library_text).
//
// Design notes that are easy to get wrong and were measured on a real card
// (docs/superpowers/specs/2026-08-05-addendum-a-findability.md, A2.1-A2.8):
//
//   * The fold DECOMPOSES. `utf8ComposeNfc()` goes the other way, so a fold
//     built on it passes on a card holding only decomposed text and then mangles
//     the first precomposed file to arrive from Windows or Calibre. Both forms
//     must produce the same output, and the host tests assert exactly that.
//   * Some letters have no canonical decomposition at all — U+00F8 (ø) is a
//     distinct letter, not o-with-stroke — so a decompose-only fold turns
//     "Nesbø" into "Nesb". Those need an explicit map.
//   * The author key sorts its tokens, so name order stops mattering. There is
//     deliberately no First/Last heuristic anywhere: "Qiu Xiaolong" and
//     "Lee Min Jin" defeat every such rule.

#include <cstdint>
#include <string>
#include <string_view>

namespace library {

// Longest author key written into an index record. Sized so a key fits the
// record's fixed field; measured to collide 0 times over a 69-book library.
inline constexpr size_t AUTHOR_KEY_MAX_BYTES = 12;

// Casefold and strip diacritics for matching and sorting.
//
// Maps a handful of letters that have no canonical decomposition, decomposes the
// rest and drops combining marks, lowercases ASCII alphanumerics, and turns
// everything else into a single space. Space runs collapse and the result is
// trimmed. Apostrophes survive as ASCII '\'' so "Anna's Archive" keeps its shape.
//
// `stripArticle` additionally removes one leading article ("the ", "le ", "la ",
// ...) — correct for sort keys and search text, wrong for anything displayed.
std::string fold(std::string_view text, bool stripArticle = false);

// Title and author as they can be recovered from a filename alone. This is what
// the row shows before metadata enrichment has run, so it is never allowed to be
// empty-handed: `title` falls back to the whole stem.
struct ParsedName {
  std::string title;
  std::string author;  // empty when the filename carries no usable author
};

// Split a filename stem (basename minus extension) on " -- ".
//
// When at least two segments are present, segment 0 is the title and segment 1
// the candidate author; segments 2 and beyond are discarded unread. That is safe
// because no title in the measured corpus contains " -- " and segment counts are
// never exactly 2. Segment 0 is never classified, so the rule cannot mangle a
// title.
//
// Segment 1 passes through one guard (`looksLikeMetadata`) because exporters
// that omit the author leave a publisher, a year or a hash in that position.
ParsedName parseFilename(std::string_view stem);

// True when `raw` is a metadata field rather than a person: a bare hash, an
// ISBN, a year, a "publisher, 2019" tail, or an archive credit.
//
// Takes RAW text, not folded: the fold turns ',' ';' and '#' into spaces, and
// the "publisher, year" shape is defined by exactly those separators. Exposed
// for testing. Only ever applied to a candidate author, never to a title.
bool looksLikeMetadata(std::string_view raw);

// Tidy a person's name for DISPLAY, without reordering it.
//
// Drops bracketed spans ("Karine Giebel [Giebel, Karine]"), everything after a
// multi-author separator, and the trailing underscores and punctuation that
// exporters leave behind ("Qiu Xiaolong_", "Michael S_ Heiser"). An underscore
// between letters becomes a full stop, since that is what it replaced in a name
// a filesystem refused to hold.
//
// It deliberately does NOT swap "Last, First" into "First Last": "Qiu Xiaolong"
// and "Lee Min Jin" defeat every such rule, and guessing wrong is worse than
// leaving the author's own spelling alone. Harmonising the several spellings of
// one person is done by picking the most common one that actually occurs, which
// needs the whole library and so belongs to the index build.
std::string cleanPersonName(std::string_view author);

// Order-insensitive identity for one person, at most AUTHOR_KEY_MAX_BYTES.
//
// Drops bracketed spans and everything after ';' (multi-author separator), folds,
// drops single-character tokens (initials), sorts the remaining tokens and joins
// them. "Qiu, Xiaolong", "Xiaolong, Qiu" and "Qiu Xiaolong [Xiaolong, Qiu]" all
// collapse to one key. Truncation is on a whole-token boundary where possible so
// a cut key stays a prefix of the untruncated one.
std::string authorKey(std::string_view author);

// Whether the filename's title segment should win over the OPF `dc:title`.
//
// True only when dc:title is a word-boundary prefix of the filename title AND
// the filename adds at least two further words of three or more letters AND its
// last word is not a stop word. The last condition is what blocks an exporter's
// mid-phrase truncation ("...a new science of") from replacing a complete title.
bool preferFilenameTitle(std::string_view dcTitle, std::string_view fnTitle);

}  // namespace library
