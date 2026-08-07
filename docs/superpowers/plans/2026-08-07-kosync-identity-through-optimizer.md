# KOSync Identity Through the Web Optimizer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An EPUB optimized by the web portal keeps its KOReader sync identity: the browser computes the ORIGINAL file's KOReader partial-MD5, embeds it in the optimized EPUB, and the firmware's smart sync uses it as a third identity — primary for uploads — so the original (e.g. on the user's phone running KOReader) and its optimized copy share one progress record on the kosync server.

**Architecture:** Three layers. (1) Browser: a pure-JS MD5 (RFC 1321) + a partial-MD5 that mirrors `KOReaderDocumentId::calculate()` byte-for-byte, run on the picked `File` before rewriting; result stored as `META-INF/crossink-sync.json` (STORE, ~70 bytes) in the output zip. (2) Firmware: new `lib/KOReaderSync/KOReaderEmbeddedId` reads that entry back via `ZipFile` and validates it with a strict pure parser (host-tested). (3) `KOReaderSyncActivity`: the embedded id becomes the upload identity and first probe when present; binary/filename become alternates; embedded matches map positions in `SourceDocument` space (the id refers to the pre-optimization original — two optimizer runs of the same original diverge in xpath, so `CurrentDocument` mapping would be wrong).

**Tech Stack:** Vanilla JS (web portal, no deps, JSZip already vendored), C++20 firmware (ESP-IDF/Arduino via PlatformIO), GoogleTest host suites under `test/`.

**Honest limit (accepted in design):** this fixes original↔optimized pairs through OUR firmware. Stock KOReader opening an optimized copy never reads our META-INF entry — that pairing stays broken, as designed.

## Global Constraints

- NEVER run two `pio` invocations at once, even one in a background task (`managed_components/` corruption; remedy: `rm -rf managed_components dependencies.lock .pio/build/<env>` + one clean retry).
- Do NOT ship sync changes untested: firmware tasks end at "builds green + host tests"; the feature is only DONE after the end-to-end device test (Task 6) against the user's kosync server with their phone present.
- Test firmware deliverables go in `firmware/CrossInkLibrary-<shortsha>.bin` (gitignored dir); ALWAYS build AFTER committing so the AppVersionGenerated stamp matches HEAD.
- `gh auth switch --user oreglio` before any release/push op on this repo; run the banned-token scan before any push (cheap; policy from the library scrub).
- `src/network/html/*.generated.h` are gitignored and rebuilt by `scripts/build_web.py` (PlatformIO pre-script). Edit only `web/` sources.
- Web portal UI is English-only (no `tr()` there); no new firmware user-facing strings in this plan → zero i18n keys.
- C3 RAM discipline: browser does the 12KB hashing; firmware reads ≤512 bytes from the zip, frees the `malloc`'d buffer from `readFileToMemory`, and closes the `ZipFile`.
- No exceptions, no bare `new`; `LOG_ERR` + return-empty on failure paths.
- Commit messages follow the repo style `<type>(<area>): <summary>`.
- Upstream sequencing: this is fork-first work on `feat/library-v1.1`'s successor workflow; nothing goes upstream while #2885-#2889 are in review.

## File Structure

- Modify: `web/pages/files.js` — sync-identity block (md5Hex, computeKoreaderPartialMd5, parseSyncIdentityId), embed logic in `convertEpubFile`, settings plumbing, warning softening.
- Modify: `web/pages/files.html` — "Preserve Sync Identity" advanced-setting row.
- Modify: `web/pages/files.css` — `.convert-warning.soft` variant.
- Create: `lib/KOReaderSync/KOReaderEmbeddedId.h` / `.cpp` — read + parse the embedded id.
- Create: `test/koreader_embedded_id/` — gtest suite (`CMakeLists.txt`, `KOReaderEmbeddedIdTest.cpp`, `stubs/Logging.h`, `stubs/ZipFile.h`).
- Modify: `test/CMakeLists.txt` — register the new suite.
- Modify: `src/activities/reader/KOReaderSyncActivity.h` / `.cpp` — probe + upload + coordinate-space wiring.
- Modify: `docs/file-formats.md` — document `META-INF/crossink-sync.json`.
- Modify: `CHANGELOG.md` — Added entry.
- Out of scope (explicitly): `NearbyBookPositionSyncActivity` (device↔device sync; both sides normally hold the same optimized file, binary id already matches; revisit only if a real mixed pairing shows up).

---

### Task 1: Browser-side partial-MD5 (node-verified)

**Files:**
- Modify: `web/pages/files.js` (insert the block right after `const DEFLATE_OPTS = ...`, currently line 2024)
- Verify with: scratchpad node harness + python reference (not committed)

**Interfaces:**
- Produces: `md5Hex(bytes: Uint8Array): string` (32 lowercase hex); `computeKoreaderPartialMd5(blob: Blob|File): Promise<string>`; `parseSyncIdentityId(text: string): string|null`; `const SYNC_IDENTITY_PATH = "META-INF/crossink-sync.json"`. Task 2 consumes all four.

- [ ] **Step 1: Write the failing verification harness first**

Create `<scratchpad>/verify_sync_identity.mjs` (scratchpad dir from the session env; NOT committed):

```js
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";

const src = readFileSync(new URL(process.argv[2], `file://${process.cwd()}/`), "utf8");
const start = src.indexOf("// --- sync-identity (node-testable block) ---");
const end = src.indexOf("// --- end sync-identity ---");
if (start < 0 || end < 0) { console.error("sentinels not found"); process.exit(1); }
const block = src.slice(start, end);
const api = new Function(`${block}; return { md5Hex, computeKoreaderPartialMd5, parseSyncIdentityId, SYNC_IDENTITY_PATH };`)();

// 1. RFC 1321 vectors + padding-edge lengths
const vectors = [
  ["", "d41d8cd98f00b204e9800998ecf8427e"],
  ["abc", "900150983cd24fb0d6963f7d28e17f72"],
  ["The quick brown fox jumps over the lazy dog", "9e107d9d372bb6826bd81d3542a419d6"],
];
for (const [msg, expected] of vectors) {
  const got = api.md5Hex(new TextEncoder().encode(msg));
  if (got !== expected) { console.error(`md5("${msg}") = ${got}, want ${expected}`); process.exit(1); }
}
for (const len of [55, 56, 63, 64, 65, 128, 12288]) {
  const buf = new Uint8Array(len).fill(0xaa);
  const want = createHash("md5").update(buf).digest("hex");
  const got = api.md5Hex(buf);
  if (got !== want) { console.error(`md5(len ${len}) = ${got}, want ${want}`); process.exit(1); }
}

// 2. Partial-MD5 against the firmware algorithm (python reference below must agree)
const epubPath = process.argv[3];
const bytes = readFileSync(epubPath);
const blob = new Blob([bytes]);
console.log("partial:", await api.computeKoreaderPartialMd5(blob));

// 3. Round-trip through the JS validator
const id = await api.computeKoreaderPartialMd5(blob);
const json = JSON.stringify({ version: 1, koreaderPartialMd5: id });
if (api.parseSyncIdentityId(json) !== id) { console.error("parseSyncIdentityId round-trip failed"); process.exit(1); }
console.log("ALL OK");
```

Create `<scratchpad>/partial_md5_ref.py` (the firmware loop, transliterated):

```python
import hashlib, os, sys
path = sys.argv[1]
size = os.path.getsize(path)
md5 = hashlib.md5()
with open(path, "rb") as f:
    for i in range(-1, 11):                    # KOReaderDocumentId: i = -1..10
        off = 0 if i < 0 else 1024 << (2 * i)  # i<0 -> 0, else 1024 << 2i
        if off >= size:
            continue
        f.seek(off)
        md5.update(f.read(min(1024, size - off)))
print("partial:", md5.hexdigest())
```

- [ ] **Step 2: Run harness to verify it fails**

Run: `node <scratchpad>/verify_sync_identity.mjs web/pages/files.js test/epubs/<largest .epub in that dir>`
Expected: FAIL with "sentinels not found" (block not written yet).

- [ ] **Step 3: Add the sync-identity block to files.js**

Insert immediately after the `DEFLATE_OPTS` const (files.js:2024), between sentinel comments (the node harness slices on them — keep them verbatim):

```js
// --- sync-identity (node-testable block) ---
// KOReader identifies a document by a "partial MD5": MD5 over up to twelve
// 1KB chunks read at offsets 0, then 1024 << (2*i) for i = 0..10. The web
// optimizer rewrites every zip entry, which changes that id — so we compute
// the ORIGINAL file's id here, before rewriting, and embed it in the output
// (META-INF/crossink-sync.json). The firmware reads it back as the book's
// sync identity. Mirrors KOReaderDocumentId::calculate() byte-for-byte.
const SYNC_IDENTITY_PATH = "META-INF/crossink-sync.json";

/** One-shot RFC 1321 MD5 over a Uint8Array; returns 32 lowercase hex chars. */
function md5Hex(bytes) {
  const S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
             5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
             4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
             6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
  const K = new Uint32Array(64);
  for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296);

  const msgLen = bytes.length;
  const padded = new Uint8Array((((msgLen + 8) >> 6) + 1) << 6);
  padded.set(bytes);
  padded[msgLen] = 0x80;
  const dv = new DataView(padded.buffer);
  dv.setUint32(padded.length - 8, (msgLen * 8) >>> 0, true);
  dv.setUint32(padded.length - 4, Math.floor(msgLen / 536870912), true);

  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let off = 0; off < padded.length; off += 64) {
    for (let j = 0; j < 16; j++) M[j] = dv.getUint32(off + j * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
      else { F = C ^ (B | ~D); g = (7 * i) % 16; }
      const tmp = D;
      D = C;
      C = B;
      const sum = (A + F + K[i] + M[g]) >>> 0;
      B = (B + ((sum << S[i]) | (sum >>> (32 - S[i])))) >>> 0;
      A = tmp;
    }
    a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
  }
  const digest = new Uint8Array(16);
  const outDv = new DataView(digest.buffer);
  outDv.setUint32(0, a0, true); outDv.setUint32(4, b0, true);
  outDv.setUint32(8, c0, true); outDv.setUint32(12, d0, true);
  let hex = "";
  for (let i = 0; i < 16; i++) hex += digest[i].toString(16).padStart(2, "0");
  return hex;
}

/** KOReader partial-MD5 of a Blob/File. Offsets past EOF are skipped, like the firmware. */
async function computeKoreaderPartialMd5(blob) {
  const CHUNK = 1024;
  const parts = [];
  let total = 0;
  for (let i = -1; i <= 10; i++) {
    const offset = i < 0 ? 0 : CHUNK << (2 * i);
    if (offset >= blob.size) continue;
    const end = Math.min(offset + CHUNK, blob.size);
    const part = new Uint8Array(await blob.slice(offset, end).arrayBuffer());
    parts.push(part);
    total += part.length;
  }
  const all = new Uint8Array(total);
  let cursor = 0;
  for (const part of parts) { all.set(part, cursor); cursor += part.length; }
  return md5Hex(all);
}

/** Extract the 32-hex id from a crossink-sync.json payload; null if malformed. */
function parseSyncIdentityId(text) {
  if (!/"version"\s*:\s*1\s*[,}]/.test(text)) return null;
  const m = /"koreaderPartialMd5"\s*:\s*"([0-9a-f]{32})"/.exec(text);
  return m ? m[1] : null;
}
// --- end sync-identity ---
```

- [ ] **Step 4: Run harness + python reference, verify agreement**

Run (pick the largest fixture so several offsets land in-range):
`ls -S test/epubs/*.epub | head -1`
`python3 <scratchpad>/partial_md5_ref.py test/epubs/<largest>.epub`
`node <scratchpad>/verify_sync_identity.mjs web/pages/files.js test/epubs/<largest>.epub`
Expected: node prints `ALL OK` and both `partial:` lines are IDENTICAL. Also run both against a second, small fixture (fewer offsets in range) and compare.

- [ ] **Step 5: Syntax gate + commit**

Run: `node --check web/pages/files.js` (expected: no output) then:

```bash
git add web/pages/files.js
git commit -m "feat(web): the optimizer learns the original's KOReader identity"
```

### Task 2: Embed the identity + "Preserve Sync Identity" UI

**Files:**
- Modify: `web/pages/files.js` — `convertEpubFile` (currently starts :3933; write-out region :4282-4337), settings plumbing (:1452-1528), `toggleConvertOptions` (:374)
- Modify: `web/pages/files.html` — new row after the Split Long Sections row (:88-99); warning div (:260-263) stays, its text becomes JS-driven
- Modify: `web/pages/files.css` — after `.convert-warning` (:332-343)

**Interfaces:**
- Consumes: `SYNC_IDENTITY_PATH`, `computeKoreaderPartialMd5`, `parseSyncIdentityId` from Task 1.
- Produces: checkbox id `preserveSyncIdentityToggle` (default checked); settings key `preserveSyncIdentity`; `updateConvertWarning()`. Task 3+ consume the emitted `META-INF/crossink-sync.json` payload `{"version":1,"koreaderPartialMd5":"<32 hex>"}`.

- [ ] **Step 1: Add the HTML row**

In `web/pages/files.html`, immediately after the Split Long Sections `advanced-setting-row` (closes at :99), insert:

```html
            <div class="advanced-setting-row">
              <div class="setting-label">
                <div class="setting-title">Preserve Sync Identity</div>
                <div class="setting-desc">Embed the original's KOReader hash so progress sync still pairs</div>
              </div>
              <div class="setting-controls">
                <label class="toggle-switch">
                  <input
                    type="checkbox"
                    id="preserveSyncIdentityToggle"
                    checked
                    onchange="onPreserveSyncIdentityChange()"
                  />
                  <span class="toggle-slider"></span>
                </label>
              </div>
            </div>
```

- [ ] **Step 2: Wire settings plumbing + warning softening in files.js**

In `DEFAULT_UPLOAD_SETTINGS` (:1453) add `preserveSyncIdentity: true,` after `splitLongSections: true,`.
In `getCurrentUploadSettings()` (:1466) add `preserveSyncIdentity: !!document.getElementById("preserveSyncIdentityToggle")?.checked,` after the `splitLongSections` line.
In `applyUploadSettings()` (:1483) add after the `splitLongSectionsToggle` line:

```js
    document.getElementById("preserveSyncIdentityToggle").checked = !!merged.preserveSyncIdentity;
```

Add near `toggleConvertOptions` (:374):

```js
function onPreserveSyncIdentityChange() {
  updateConvertWarning();
  updateUploadSettingsPersistence();
}

function updateConvertWarning() {
  const warning = document.getElementById("convertWarning");
  if (!warning) return;
  const preserve = !!document.getElementById("preserveSyncIdentityToggle")?.checked;
  warning.classList.toggle("soft", preserve);
  warning.innerHTML = preserve
    ? "ℹ️ Optimizing keeps this book's sync identity: the original's KOReader hash travels inside the optimized EPUB, so progress sync still pairs with the original file."
    : "⚠️ Converting will modify files and can break hash‑based sync. ⚠️<br />Please back up or disable sync before proceeding.";
}
```

In `toggleConvertOptions()` add `updateConvertWarning();` directly after the `document.getElementById("convertWarning").style.display = ...` line (:377).

In `web/pages/files.css`, after the `.convert-warning` rule (:332-343), add (match the existing rule's property style when reading it — this softens color only):

```css
.convert-warning.soft {
  color: #2471a3;
  border-color: #2471a3;
  background: rgba(36, 113, 163, 0.08);
}
```

(If `.convert-warning` uses different property names — e.g. no `border-color` — mirror the ones it actually sets.)

- [ ] **Step 3: Embed in convertEpubFile**

In `convertEpubFile` (:3933), right after `const zip = await JSZip.loadAsync(file);` (:3947), insert:

```js
  // Sync identity: computed from the picked file's BYTES before any rewrite.
  // Renames (metadata/collision) wrap the same bytes in a new File, so this
  // is still the original's identity. If the source already carries one
  // (re-optimizing an optimized book), preserve it — recomputing here would
  // capture the optimized bytes, not the true original's.
  const preserveSyncIdentity = !!document.getElementById("preserveSyncIdentityToggle")?.checked;
  let syncIdentityJson = null;
  if (preserveSyncIdentity) {
    try {
      const existing = zip.files[SYNC_IDENTITY_PATH];
      if (existing && !existing.dir) {
        const existingText = await existing.async("string");
        if (parseSyncIdentityId(existingText)) {
          syncIdentityJson = existingText;
          log("Sync identity: preserved from source EPUB", "", "INFO");
        }
      }
      if (!syncIdentityJson) {
        const syncId = await computeKoreaderPartialMd5(file);
        syncIdentityJson = JSON.stringify({ version: 1, koreaderPartialMd5: syncId });
        log(`Sync identity: ${syncId}`, "", "INFO");
      }
    } catch (err) {
      console.error("Sync identity error:", err);
      log("Sync identity could not be computed; continuing without it", "warning", "INFO");
      syncIdentityJson = null;
    }
  }
```

In the copy-remaining loop, after the x-locations skip `if (low === X_LOCATION_MANIFEST_PATH.toLowerCase()) continue;` (:4312), add:

```js
    if (syncIdentityJson && low === SYNC_IDENTITY_PATH.toLowerCase()) continue;
```

(When the toggle is OFF, an existing entry passes through the generic copy exactly as today.)

After the copy-remaining loop closes (:4337) and before `if (progressCallback) progressCallback(100);` (:4339), add:

```js
  if (syncIdentityJson) {
    out.file(SYNC_IDENTITY_PATH, syncIdentityJson, { compression: "STORE", createFolders: false });
  }
```

- [ ] **Step 4: Verify**

Run: `node --check web/pages/files.js` (no output) then `python3 scripts/build_web.py` (expected: regenerates `src/network/html/*.generated.h` without error).
Then re-run the Task 1 node harness (sentinels untouched → still `ALL OK`).
Manual spot-check available now or at device time: open the Files page, tick Optimize → warning is the soft ℹ️ blue variant; untick Preserve Sync Identity → orange warning returns.

- [ ] **Step 5: Commit**

```bash
git add web/pages/files.js web/pages/files.html web/pages/files.css
git commit -m "feat(web): optimized EPUBs carry their original's sync identity"
```

### Task 3: Firmware reader — KOReaderEmbeddedId (+ host tests)

**Files:**
- Create: `lib/KOReaderSync/KOReaderEmbeddedId.h`, `lib/KOReaderSync/KOReaderEmbeddedId.cpp`
- Create: `test/koreader_embedded_id/CMakeLists.txt`, `test/koreader_embedded_id/KOReaderEmbeddedIdTest.cpp`, `test/koreader_embedded_id/stubs/Logging.h`, `test/koreader_embedded_id/stubs/ZipFile.h`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(koreader_embedded_id)` alongside the existing suites, :41-55)

**Interfaces:**
- Produces: `KOReaderEmbeddedId::read(const std::string& epubPath) -> std::string` (32-hex id or empty) and `KOReaderEmbeddedId::parse(std::string_view json) -> std::string`; constant `KOReaderEmbeddedId::SYNC_ID_PATH == "META-INF/crossink-sync.json"`. Task 4 consumes `read`.
- Consumes: `lib/ZipFile` (`open/close`, `getInflatedFileSize(const char*, size_t*)`, `readFileToMemory(const char*, size_t*, bool)` — returns a `malloc`'d buffer the CALLER frees; `ZipFile(const std::string&)` stores a reference, argument must outlive it).

- [ ] **Step 1: Write the failing tests**

`test/koreader_embedded_id/stubs/Logging.h`:

```cpp
#pragma once
// Host-test stub: logging is a no-op.
#define LOG_INF(tag, fmt, ...) ((void)0)
#define LOG_DBG(tag, fmt, ...) ((void)0)
#define LOG_ERR(tag, fmt, ...) ((void)0)
```

`test/koreader_embedded_id/stubs/ZipFile.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

// Host-test stub: serves one canned entry and mirrors lib/ZipFile's ownership
// contract (readFileToMemory returns a malloc'd buffer the caller frees).
class ZipFile {
 public:
  static inline std::string entryName;
  static inline std::string entryContent;
  static inline bool openable = true;

  explicit ZipFile(const std::string&) {}
  bool open() { return openable; }
  bool close() { return true; }
  bool getInflatedFileSize(const char* name, size_t* size) {
    if (!openable || entryName != name) return false;
    *size = entryContent.size();
    return true;
  }
  uint8_t* readFileToMemory(const char* name, size_t* size = nullptr, bool trailingNullByte = false) {
    if (!openable || entryName != name) return nullptr;
    const size_t n = entryContent.size();
    uint8_t* buf = static_cast<uint8_t*>(malloc(n + (trailingNullByte ? 1 : 0)));
    if (!buf) return nullptr;
    memcpy(buf, entryContent.data(), n);
    if (trailingNullByte) buf[n] = 0;
    if (size) *size = n;
    return buf;
  }
};
```

`test/koreader_embedded_id/KOReaderEmbeddedIdTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "KOReaderEmbeddedId.h"
#include "ZipFile.h"

namespace {
constexpr const char* VALID_ID = "0123456789abcdef0123456789abcdef";
constexpr const char* VALID_JSON = "{\"version\":1,\"koreaderPartialMd5\":\"0123456789abcdef0123456789abcdef\"}";
}  // namespace

TEST(EmbeddedIdParse, AcceptsCanonicalPayload) { EXPECT_EQ(KOReaderEmbeddedId::parse(VALID_JSON), VALID_ID); }

TEST(EmbeddedIdParse, AcceptsWhitespaceVariants) {
  EXPECT_EQ(KOReaderEmbeddedId::parse(
                "{ \"version\" : 1 ,\n  \"koreaderPartialMd5\" : \"0123456789abcdef0123456789abcdef\" }"),
            VALID_ID);
}

TEST(EmbeddedIdParse, RejectsMissingVersion) {
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"koreaderPartialMd5\":\"0123456789abcdef0123456789abcdef\"}"), "");
}

TEST(EmbeddedIdParse, RejectsUnknownVersion) {
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":2,\"koreaderPartialMd5\":\"0123456789abcdef0123456789abcdef\"}"),
            "");
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":12,\"koreaderPartialMd5\":\"0123456789abcdef0123456789abcdef\"}"),
            "");
}

TEST(EmbeddedIdParse, RejectsMalformedIds) {
  // Uppercase hex, short, long, non-string, empty input.
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":1,\"koreaderPartialMd5\":\"0123456789ABCDEF0123456789ABCDEF\"}"),
            "");
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":1,\"koreaderPartialMd5\":\"0123\"}"), "");
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":1,\"koreaderPartialMd5\":\"0123456789abcdef0123456789abcdef0\"}"),
            "");
  EXPECT_EQ(KOReaderEmbeddedId::parse("{\"version\":1,\"koreaderPartialMd5\":42}"), "");
  EXPECT_EQ(KOReaderEmbeddedId::parse(""), "");
}

TEST(EmbeddedIdRead, ReadsCannedEntry) {
  ZipFile::openable = true;
  ZipFile::entryName = KOReaderEmbeddedId::SYNC_ID_PATH;
  ZipFile::entryContent = VALID_JSON;
  EXPECT_EQ(KOReaderEmbeddedId::read("/books/x.epub"), VALID_ID);
}

TEST(EmbeddedIdRead, EmptyWhenEntryMissing) {
  ZipFile::openable = true;
  ZipFile::entryName = "META-INF/container.xml";
  ZipFile::entryContent = VALID_JSON;
  EXPECT_EQ(KOReaderEmbeddedId::read("/books/x.epub"), "");
}

TEST(EmbeddedIdRead, EmptyWhenOversized) {
  ZipFile::openable = true;
  ZipFile::entryName = KOReaderEmbeddedId::SYNC_ID_PATH;
  ZipFile::entryContent = std::string(4096, 'x');
  EXPECT_EQ(KOReaderEmbeddedId::read("/books/x.epub"), "");
}

TEST(EmbeddedIdRead, EmptyWhenZipUnopenable) {
  ZipFile::openable = false;
  ZipFile::entryName = KOReaderEmbeddedId::SYNC_ID_PATH;
  ZipFile::entryContent = VALID_JSON;
  EXPECT_EQ(KOReaderEmbeddedId::read("/books/x.epub"), "");
}
```

`test/koreader_embedded_id/CMakeLists.txt` (stubs dir FIRST on the include path, ahead of the lib — the `reader_progress_save_debouncer` precedent):

```cmake
add_executable(KOReaderEmbeddedIdTest
  KOReaderEmbeddedIdTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderEmbeddedId.cpp
)

target_include_directories(KOReaderEmbeddedIdTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(KOReaderEmbeddedIdTest PRIVATE crosspoint_test_common GTest::gtest_main)

gtest_discover_tests(KOReaderEmbeddedIdTest)
```

Register in `test/CMakeLists.txt` with the other `add_subdirectory` lines: `add_subdirectory(koreader_embedded_id)`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake -S test -B test/build && cmake --build test/build -j 2>&1 | tail -5`
Expected: FAIL — `KOReaderEmbeddedId.h: No such file or directory`.

- [ ] **Step 3: Implement**

`lib/KOReaderSync/KOReaderEmbeddedId.h`:

```cpp
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
```

`lib/KOReaderSync/KOReaderEmbeddedId.cpp`:

```cpp
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
  if (zip.getInflatedFileSize(SYNC_ID_PATH, &inflatedSize) && inflatedSize > 0 &&
      inflatedSize <= MAX_INFLATED_BYTES) {
    size_t size = 0;
    uint8_t* data = zip.readFileToMemory(SYNC_ID_PATH, &size, /*trailingNullByte=*/false);
    if (data) {
      id = parse(std::string_view(reinterpret_cast<const char*>(data), size));
      free(data);  // readFileToMemory hands over a malloc'd buffer
      if (id.empty()) LOG_ERR("KOSync", "Embedded sync id present but malformed; ignoring");
    } else {
      LOG_ERR("KOSync", "Embedded sync id could not be read");
    }
  }
  zip.close();
  return id;
}
```

Implementation notes for the engineer: (a) verify in `lib/ZipFile/ZipFile.cpp` whether `getInflatedFileSize` needs a prior `open()` — `readFileToMemory` self-opens via `ScopedOpenClose` (ZipFile.cpp:645), but this code opens explicitly anyway, which is safe in both cases; keep the explicit open/close pair. (b) `ZipFile` stores a `const std::string&` (ZipFile.h:103) — `epubPath` outlives `zip` here, fine.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake -S test -B test/build && cmake --build test/build -j && ctest --test-dir test/build --output-on-failure -R EmbeddedId`
Expected: 9/9 PASS. Then run the FULL suite (`ctest --test-dir test/build --output-on-failure`) — expected: all pre-existing suites still green (69 + 9).

- [ ] **Step 5: Format + commit**

Run: `clang-format -i lib/KOReaderSync/KOReaderEmbeddedId.h lib/KOReaderSync/KOReaderEmbeddedId.cpp test/koreader_embedded_id/KOReaderEmbeddedIdTest.cpp test/koreader_embedded_id/stubs/ZipFile.h test/koreader_embedded_id/stubs/Logging.h`

```bash
git add lib/KOReaderSync/KOReaderEmbeddedId.h lib/KOReaderSync/KOReaderEmbeddedId.cpp test/koreader_embedded_id test/CMakeLists.txt
git commit -m "feat(sync): the firmware reads the identity an optimized book carries"
```

### Task 4: Smart-sync wiring — embedded id as third identity, primary for uploads

**Files:**
- Modify: `src/activities/reader/KOReaderSyncActivity.h` (members, :82-97 region)
- Modify: `src/activities/reader/KOReaderSyncActivity.cpp` (includes :1-38; `ensureLocalProgressLoaded` :147-149; `performSync` :238-296, :341-346, :448-453; `NO_REMOTE_PROGRESS` handler :841-852)

**Interfaces:**
- Consumes: `KOReaderEmbeddedId::read` (Task 3); existing `calculateDocumentHashForMethod` / `alternateMatchMethod` / `matchMethodName` (file-local, :66-77); `KOReaderSyncClient::getProgress(hash, out)`.
- Produces: no new external interface — behavior only. New members: `std::string embeddedHash;` and `bool remoteMatchedEmbedded = false;`.

**The rules being encoded** (from the agreed design + one derived consequence):
1. Embedded id present → it is the UPLOAD identity and the FIRST probe. Uploads never migrate to alternate identities (existing doctrine, :449-450 comment) — they stay on the embedded id so the shared record heals.
2. Smart mode probes the remaining identities as alternates: the configured method's hash, then its alternate (max 3 network probes total; duplicates and empties skipped). Accept rule unchanged from today (:288-294): alternate wins iff `OK && (primary NOT_FOUND || further progress)`.
3. Coordinate spaces: an embedded match refers to the PRE-OPTIMIZATION original, and two optimizer runs of one original diverge in xpath — so embedded maps like FILENAME (`SourceDocument`), for both remote-apply (:341) and local-upload (:147) directions. Only a BINARY match of the current bytes earns `CurrentDocument`.

- [ ] **Step 1: Header members**

In `KOReaderSyncActivity.h`, after `DocumentMatchMethod remoteMatchMethod;` (:83), add:

```cpp
  // Sync identity embedded by the web optimizer (original file's partial
  // MD5); empty when the book doesn't carry one. When present it is the
  // upload identity and the first probe, and it maps like a FILENAME match
  // (SourceDocument): it names the pre-optimization original, whose layout
  // this optimized copy no longer shares.
  std::string embeddedHash;
  bool remoteMatchedEmbedded = false;
```

- [ ] **Step 2: performSync — identity setup**

In `KOReaderSyncActivity.cpp` add `#include "KOReaderEmbeddedId.h"` next to the existing `KOReaderDocumentId.h` include. Replace :239-251 (`const DocumentMatchMethod primaryMethod ... const std::string primaryHash = documentHash;`) with:

```cpp
  const DocumentMatchMethod primaryMethod = primaryMatchMethod;
  remoteMatchMethod = primaryMethod;
  remoteMatchedEmbedded = false;

  embeddedHash = KOReaderEmbeddedId::read(epubPath);
  const std::string primaryHash = calculateDocumentHashForMethod(epubPath, primaryMethod);
  documentHash = embeddedHash.empty() ? primaryHash : embeddedHash;
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }
  remoteMatchedEmbedded = !embeddedHash.empty();
  const std::string uploadHash = documentHash;
```

- [ ] **Step 3: performSync — probe loop**

Replace the primary-probe LOG line's identity label (:274-276): use `embeddedHash.empty() ? matchMethodName(primaryMethod) : "embedded"` instead of `matchMethodName(primaryMethod)`.

Replace the smart-mode alternate block (:278-296) with:

```cpp
  if (smartSyncEnabled()) {
    // Probe the remaining identities. With an embedded id the candidates are
    // the configured method and its alternate; without one, just the
    // alternate (today's behavior). Accept rule is unchanged: an alternate
    // only replaces the accepted record when it is OK and either the current
    // result is NOT_FOUND or it is further along.
    struct ProbeCandidate {
      std::string hash;
      DocumentMatchMethod method;
    };
    ProbeCandidate candidates[2];
    size_t candidateCount = 0;
    if (!embeddedHash.empty()) {
      candidates[candidateCount++] = {primaryHash, primaryMethod};
    }
    const DocumentMatchMethod altMethod = alternateMatchMethod(primaryMethod);
    candidates[candidateCount++] = {calculateDocumentHashForMethod(epubPath, altMethod), altMethod};

    for (size_t ci = 0; ci < candidateCount; ci++) {
      const ProbeCandidate& candidate = candidates[ci];
      if (candidate.hash.empty() || candidate.hash == uploadHash) continue;
      if (ci > 0 && candidate.hash == candidates[0].hash) continue;

      KOReaderProgress altProgress;
      const auto altResult = KOReaderSyncClient::getProgress(candidate.hash, altProgress);
      LOG_DBG("KOSync", "Alternate remote (%s): result=%d http=%d doc=%s remote=%.6f xpath=%s",
              matchMethodName(candidate.method), altResult, KOReaderSyncClient::lastHttpCode, candidate.hash.c_str(),
              altProgress.percentage, altProgress.progress.c_str());

      if (altResult == KOReaderSyncClient::OK &&
          (result == KOReaderSyncClient::NOT_FOUND || altProgress.percentage > remoteProgress.percentage)) {
        documentHash = candidate.hash;
        remoteProgress = std::move(altProgress);
        remoteMatchMethod = candidate.method;
        remoteMatchedEmbedded = false;
        result = KOReaderSyncClient::OK;
      }
    }
  }
```

(Note: `remoteMatchedEmbedded` stays `true` only while the accepted record is the embedded-id primary probe. When no embedded id exists it was already `false` from Step 2.)

- [ ] **Step 4: performSync — coordinate spaces and upload identity**

Replace :341-343 with:

```cpp
  const PositionCoordinateSpace remoteCoordinateSpace =
      (remoteMatchedEmbedded || remoteMatchMethod == DocumentMatchMethod::FILENAME)
          ? PositionCoordinateSpace::SourceDocument
          : PositionCoordinateSpace::CurrentDocument;
```

and extend the comment just below it (:345-346) with one line: `// Embedded ids name the pre-optimization original, so they map like filename matches.`

In `ensureLocalProgressLoaded`, replace :147-149 with:

```cpp
  const PositionCoordinateSpace coordinateSpace =
      (!embeddedHash.empty() || primaryMatchMethod == DocumentMatchMethod::FILENAME)
          ? PositionCoordinateSpace::SourceDocument
          : PositionCoordinateSpace::CurrentDocument;
```

(Safe ordering: the KOSync flow always runs through the network-boot constructor with `localProgressDeferred = true`, and `ensureLocalProgressLoaded` is only invoked from `performSync` at :300 — after Step 2 set `embeddedHash`.)

In the smart upload branch, replace :448-453 with:

```cpp
    if (delta > 0) {
      // Alternate hashes are only probes for newer remote state. Uploads stay
      // on the upload identity — the embedded id when present, else the
      // user's configured matching method — so its primary record heals.
      documentHash = uploadHash;
      performUpload();
      return;
    }
```

- [ ] **Step 5: NO_REMOTE_PROGRESS manual path**

Replace the defensive recompute at :844-849 with:

```cpp
      if (documentHash.empty()) {
        embeddedHash = KOReaderEmbeddedId::read(epubPath);
        documentHash =
            embeddedHash.empty() ? calculateDocumentHashForMethod(epubPath, primaryMatchMethod) : embeddedHash;
      }
```

- [ ] **Step 6: Build everything (ONE pio at a time), run host tests**

Run in sequence, never overlapping:
1. `cmake --build test/build -j && ctest --test-dir test/build --output-on-failure` — all green.
2. `pio run -e simulator` — SUCCESS.
3. `pio run -e default` — SUCCESS (C3 image; watch RAM/flash deltas — expected: ~1-2 KB flash, no static RAM).
4. `pio run -e sticky` — SUCCESS.
5. `pio run -e x4-pro` — SUCCESS.
6. `pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high` — clean.

- [ ] **Step 7: Format + commit**

Run: `clang-format -i src/activities/reader/KOReaderSyncActivity.h src/activities/reader/KOReaderSyncActivity.cpp`

```bash
git add src/activities/reader/KOReaderSyncActivity.h src/activities/reader/KOReaderSyncActivity.cpp
git commit -m "feat(sync): smart sync trusts the identity a book carries with it"
```

### Task 5: Documentation

**Files:**
- Modify: `docs/file-formats.md` (new section; follow the CLX1 section's structure)
- Modify: `CHANGELOG.md` (`[Unreleased]` → `Added`)

- [ ] **Step 1: docs/file-formats.md**

Add a section (place it near the other META-INF/EPUB-adjacent formats; adapt heading level to the file's convention):

```markdown
## Sync identity manifest (`META-INF/crossink-sync.json`)

Embedded by the web optimizer inside optimized EPUBs; read by
`KOReaderEmbeddedId` (lib/KOReaderSync). Carries the KOReader partial-MD5 of
the ORIGINAL file so progress sync keeps pairing the optimized copy with its
original across devices.

```json
{"version":1,"koreaderPartialMd5":"<32 lowercase hex>"}
```

- `version` — format version; readers MUST ignore the file when it is not `1`.
- `koreaderPartialMd5` — MD5 over 1KB chunks of the original file at offsets
  `0` and `1024 << (2*i)` for `i = 0..10`; offsets past EOF skipped
  (KOReader's document-id algorithm, mirrored by `KOReaderDocumentId`).
- Stored uncompressed (STORE); firmware rejects payloads over 512 bytes.
- Re-optimizing an already-optimized EPUB preserves the existing manifest
  verbatim (the original's identity chain survives repeated optimization).
- Renaming or re-downloading the ORIGINAL changes nothing here (identity is
  content-based); books optimized before this feature simply lack the entry.
```

- [ ] **Step 2: CHANGELOG.md**

Under `[Unreleased]` / `### Added` (create the subsection if absent):

```markdown
- Optimized EPUBs now keep their KOReader sync identity: the web optimizer embeds the original file's document hash, and Progress Sync uses it to pair the optimized copy with the original (e.g. the same book in KOReader on your phone). A "Preserve Sync Identity" toggle (default on) lives in the optimizer's Advanced Mode.
```

- [ ] **Step 3: Commit**

```bash
git add docs/file-formats.md CHANGELOG.md
git commit -m "docs: the sync identity manifest joins the file formats"
```

### Task 6: End-to-end device test (gates shipping — nothing ships if this fails)

**Files:** none (protocol). Requires: the user, their X3, their phone (KOReader + the ORIGINAL epub), their kosync server.

- [ ] **Step 1: Build the test firmware AFTER the last commit**

```bash
git rev-parse --short HEAD
pio run -e default
cp .pio/build/default/firmware.bin firmware/CrossInkLibrary-<shortsha>.bin
```

(Or, if the user prefers OTA: `gh auth switch --user oreglio`, then the release recipe from `docs/releases-and-ota.md` — `CROSSINK_RELEASE_VERSION=1.5.15 pio run -e default`, asset named `firmware-x3-x4.bin`, tag numerically above the device's current version, not a prerelease. Run the banned-token scan before pushing anything.)

- [ ] **Step 2: Test matrix (user drives, phone in hand)**

Prep: pick one EPUB whose ORIGINAL lives in phone-KOReader and is registered on the kosync server. Via the web portal, optimize+upload that original to the X3 with Preserve Sync Identity ON. Keep a second copy optimized with the toggle OFF as control.

| # | Scenario | Expected |
|---|----------|----------|
| 1 | Web UI: tick Optimize | Soft blue ℹ️ warning (not orange) |
| 2 | Web UI: untick Preserve Sync Identity | Orange warning returns; re-tick for the rest |
| 3 | Phone reads ahead → X3 "Sync Progress" on the optimized copy | X3 jumps to the phone's position (serial log: `Embedded sync id:`-less? no — `Primary remote (embedded)` line, result OK) |
| 4 | X3 reads ahead → sync → phone KOReader syncs | Phone lands on the X3's position; kosync record key = the embedded hash (server-side check if easy) |
| 5 | Control book (toggle OFF) | Behaves exactly as before this feature (no embedded probe in serial log) |
| 6 | Book with NO identity (pre-existing optimized book) | Sync unchanged; log shows filename/binary probes only |
| 7 | Re-optimize the already-optimized copy, re-upload, sync | Log shows the SAME embedded hash (preserved, not recomputed) |
| 8 | Match-method setting flipped (FILENAME↔BINARY) on a book WITH identity | Upload still goes to the embedded hash; alternates probed in smart mode |

Serial log checkpoints: `Primary remote (embedded): ...` then `Alternate remote (binary/filename): ...` lines; `Smart decision: doc=<embedded hash>`.

- [ ] **Step 3: On green: mark the feature DONE in memory; on red: systematic-debugging, fix, re-run the matrix**

---

## Self-Review (done at plan time)

- Spec coverage: browser hash (T1), embed + UI + default-on + warning softening (T2), firmware read (T3), third identity + primary-for-upload + smart probe (T4), docs/changelog (T5), the user's "nothing untested ships on sync" gate (T6). The memory's "12×1KB File.slice reads" and "META-INF/crossink-sync.json" decisions are honored; the OPF-meta alternative from the memory was dropped in favor of the META-INF file (single writer, no OPF-parser coupling).
- Placeholder scan: none — every step carries runnable code/commands.
- Type consistency: `SYNC_IDENTITY_PATH` (JS) vs `KOReaderEmbeddedId::SYNC_ID_PATH` (C++) both equal `"META-INF/crossink-sync.json"`; JSON keys `version`/`koreaderPartialMd5` identical in T1/T2/T3/T5; `uploadHash` is a `performSync` local (mirrors today's `primaryHash` lifetime); `embeddedHash`/`remoteMatchedEmbedded` declared in T4-Step 1 before use in Steps 2-5.
