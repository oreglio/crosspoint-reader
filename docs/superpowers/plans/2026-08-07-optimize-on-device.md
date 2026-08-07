# Optimize on Device Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Optimize books already on the SD card from the web portal (toolbar batch + per-row ⚡), replacing each original via a firmware-side atomic-ish swap that keeps reading progress, the favorite star and the remembered selection — plus the File Manager mobile card layout ("variante 3") and the WS shelf-freshness fix.

**Architecture:** Browser queue (download → probe markers → convert with forced sync identity → upload beside as `<name>.optimizing` → `POST /replace`). New firmware endpoint `/replace` performs the swap: preserve-user-state cache clear, delete+rename, favorites/state re-anchor `{nameHash,oldSize}→{nameHash,newSize}`, stale mark. Mobile: ≤600 px media query turns table rows into tap-to-select cards (ring outline via `tr:has(:checked)`), serif clamped titles, ⬇ download action.

**Tech Stack:** Vanilla JS/CSS (web portal, JSZip vendored), C++20 firmware, GoogleTest host suites.

**Spec:** `docs/superpowers/specs/2026-08-07-optimize-on-device-design.md` (approved). Mockup: https://claude.ai/code/artifact/689915c0-e57d-49e0-826e-c906bb923e73 (variante 3).

## Global Constraints

- NEVER two `pio` invocations at once (remedy for managed_components corruption: `rm -rf managed_components dependencies.lock .pio/build/<env>` + one retry).
- One WS upload at a time server-side — the browser queue is strictly sequential.
- Staging suffix contract: `.optimizing` (survives `sanitizeFilename` verbatim; keeps every book mechanism dormant — verified in exploration).
- The original is never destroyed before a complete optimized copy sits beside it. The only no-second-copy window is delete+rename inside `/replace`.
- OoD conversions ALWAYS embed the sync identity (the on-card book is the original), regardless of the upload modal toggle.
- Already-optimized books (zip has `META-INF/crossink-sync.json` or `META-INF/x-locations.json`) are skipped and counted.
- Desktop table unchanged; card layout only inside the ≤600 px media query (the file's existing mobile breakpoint).
- Portal is English-only; no firmware UI strings → zero i18n keys.
- Task 1's sync-identity node harness must stay green after every files.js change: `node <scratchpad>/verify_sync_identity.mjs web/pages/files.js test/epubs/test_jpeg_images.epub` → `ALL OK` (sentinels untouched).
- `src/network/html/*.generated.h` are gitignored build artifacts; regen check is `python3 scripts/build_web.py`.
- Commit style `<type>(<area>): <summary>`; release v1.5.16 only after the device matrix (Task 8) — nothing ships untested.

## File Structure

- Modify: `src/network/CrossPointWebServer.cpp` (WS stale fix ×2 sites; `/replace` handler + route), `src/network/CrossPointWebServer.h` (declaration).
- Modify: `lib/LibraryIndex/LibraryFavorites.h/.cpp` (pure `reanchorFavoriteKey`), `lib/LibraryIndex/LibraryFavoritesFile.h/.cpp` (`reanchor`), `lib/LibraryIndex/LibraryState.h/.cpp` (`reanchorLibraryStateSelection`).
- Modify: `test/library_favorites/LibraryFavoritesTest.cpp` (reanchor tests).
- Modify: `web/pages/files.js` (row class hooks, row-tap select, dl button, optimize queue), `web/pages/files.css` (card layout, optimize-mode modal), `web/pages/files.html` (toolbar button).
- Modify: `CHANGELOG.md`.

---

### Task 1: WS uploads wake the shelf

**Files:**
- Modify: `src/network/CrossPointWebServer.cpp` (two WS completion sites inside `onWebSocketEvent`)

**Interfaces:**
- Consumes: `library::markShelfStaleIfBook(const char*)` (already included and used at :953).
- Produces: behavior only — WS-uploaded books mark `/.crosspoint/library.stale`.

- [ ] **Step 1: Locate the two completion sites**

In `onWebSocketEvent`: (a) the zero-byte branch — `LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());` followed by `clearBookCachePreservingUserState(filePath.c_str());`; (b) the normal-completion branch in `case WStype_BIN` — `clearBookCachePreservingUserState(filePath.c_str());` right before `wsServer->sendTXT(num, "DONE");`.

- [ ] **Step 2: Add the stale mark at both sites**

Immediately after each of the two `clearBookCachePreservingUserState(filePath.c_str());` lines, add:

```cpp
        library::markShelfStaleIfBook(filePath.c_str());
```

(Indentation to match each site. No include change needed — the HTTP path at :953 already uses it.)

- [ ] **Step 3: Compile gate**

Run: `pio run -e simulator` (foreground, alone). Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/network/CrossPointWebServer.cpp
git commit -m "fix(web): books that arrive by WebSocket now wake the shelf"
```

### Task 2: Re-anchor APIs (favorites + shelf state) — TDD

**Files:**
- Modify: `lib/LibraryIndex/LibraryFavorites.h` and `.cpp` (pure function, host-tested)
- Modify: `lib/LibraryIndex/LibraryFavoritesFile.h` and `.cpp` (thin wrapper + save)
- Modify: `lib/LibraryIndex/LibraryState.h` and `.cpp` (selection re-anchor)
- Test: `test/library_favorites/LibraryFavoritesTest.cpp`

**Interfaces:**
- Produces: `bool library::reanchorFavoriteKey(std::vector<FavoriteKey>& keys, const FavoriteKey& from, const FavoriteKey& to)` (pure; keys stays sorted); `bool LibraryFavoritesFile::reanchor(const FavoriteKey& from, const FavoriteKey& to)` (write-through); `bool library::reanchorLibraryStateSelection(const FavoriteKey& from, const FavoriteKey& to)`. Task 3 consumes the latter two.
- Consumes: existing `FavoriteKey` (operator== / operator< exist — used by `toggle`), `loadLibraryState`/`saveLibraryState`.

- [ ] **Step 1: Write the failing tests**

Append to `test/library_favorites/LibraryFavoritesTest.cpp`:

```cpp
TEST(FavoritesReanchor, MovesAPresentKeyToItsNewSize) {
  std::vector<library::FavoriteKey> keys = {{100, 10}, {200, 20}, {300, 30}};
  EXPECT_TRUE(library::reanchorFavoriteKey(keys, {200, 20}, {200, 25}));
  EXPECT_TRUE(std::binary_search(keys.begin(), keys.end(), library::FavoriteKey{200, 25}));
  EXPECT_FALSE(std::binary_search(keys.begin(), keys.end(), library::FavoriteKey{200, 20}));
  EXPECT_EQ(keys.size(), 3u);
  EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
}

TEST(FavoritesReanchor, AbsentFromIsANoOp) {
  std::vector<library::FavoriteKey> keys = {{100, 10}};
  EXPECT_FALSE(library::reanchorFavoriteKey(keys, {200, 20}, {200, 25}));
  EXPECT_EQ(keys.size(), 1u);
}

TEST(FavoritesReanchor, ToAlreadyPresentDeduplicates) {
  std::vector<library::FavoriteKey> keys = {{200, 20}, {200, 25}};
  EXPECT_TRUE(library::reanchorFavoriteKey(keys, {200, 20}, {200, 25}));
  EXPECT_EQ(keys.size(), 1u);
  EXPECT_TRUE(std::binary_search(keys.begin(), keys.end(), library::FavoriteKey{200, 25}));
}

TEST(FavoritesReanchor, IdenticalFromAndToIsANoOp) {
  std::vector<library::FavoriteKey> keys = {{200, 20}};
  EXPECT_FALSE(library::reanchorFavoriteKey(keys, {200, 20}, {200, 20}));
  EXPECT_EQ(keys.size(), 1u);
}
```

(If `FavoriteKey` lacks `operator<` visible to `std::is_sorted`/`binary_search` in the test TU, mirror how the existing tests in this file compare keys.)

- [ ] **Step 2: Run to verify failure**

Run: `cmake -S test -B test/build && cmake --build test/build --target LibraryFavoritesTest 2>&1 | tail -5`
Expected: FAIL — `reanchorFavoriteKey` not declared.

- [ ] **Step 3: Implement**

`lib/LibraryIndex/LibraryFavorites.h`, next to the other free functions:

```cpp
// Re-anchors a key to a new identity (same book, new file size after an
// in-place replacement). Keeps `keys` sorted; dedups if `to` already exists.
// Returns true when the vector changed.
bool reanchorFavoriteKey(std::vector<FavoriteKey>& keys, const FavoriteKey& from, const FavoriteKey& to);
```

`lib/LibraryIndex/LibraryFavorites.cpp`:

```cpp
bool reanchorFavoriteKey(std::vector<FavoriteKey>& keys, const FavoriteKey& from, const FavoriteKey& to) {
  if (from == to) return false;
  const auto it = std::lower_bound(keys.begin(), keys.end(), from);
  if (it == keys.end() || !(*it == from)) return false;
  keys.erase(it);
  const auto dst = std::lower_bound(keys.begin(), keys.end(), to);
  if (dst == keys.end() || !(*dst == to)) keys.insert(dst, to);
  return true;
}
```

`lib/LibraryIndex/LibraryFavoritesFile.h`, after `toggle`:

```cpp
  // In-place file replacement changed the size half of a key: move the star
  // to the new identity. Count is unchanged, so the cap cannot refuse it.
  bool reanchor(const FavoriteKey& from, const FavoriteKey& to);
```

`lib/LibraryIndex/LibraryFavoritesFile.cpp`, after `toggle`:

```cpp
bool LibraryFavoritesFile::reanchor(const FavoriteKey& from, const FavoriteKey& to) {
  if (!reanchorFavoriteKey(keys, from, to)) return false;
  if (!save()) LOG_ERR("LIBFAV", "favorite re-anchored in RAM but not saved");
  return true;
}
```

`lib/LibraryIndex/LibraryState.h`, after `saveLibraryState`:

```cpp
// If the remembered cursor sits on `from`, follow the book to its new
// identity. No-op (false) when the selection is elsewhere or unset.
bool reanchorLibraryStateSelection(const FavoriteKey& from, const FavoriteKey& to);
```

`lib/LibraryIndex/LibraryState.cpp`, after `saveLibraryState`:

```cpp
bool reanchorLibraryStateSelection(const FavoriteKey& from, const FavoriteKey& to) {
  LibraryShelfState state;
  loadLibraryState(state);
  if (!(state.selected == from)) return false;
  state.selected = to;
  return saveLibraryState(state);
}
```

(HAL-backed load/save keeps this out of the host suite; the logic above it is one comparison. Device matrix covers it.)

- [ ] **Step 4: Run to verify pass + no regression**

Run: `cmake --build test/build --target LibraryFavoritesTest && ctest --test-dir test/build -R Favorites --output-on-failure`
Expected: all pass (existing suite + 4 new).

- [ ] **Step 5: Format + commit**

Run: `clang-format -i lib/LibraryIndex/LibraryFavorites.h lib/LibraryIndex/LibraryFavorites.cpp lib/LibraryIndex/LibraryFavoritesFile.h lib/LibraryIndex/LibraryFavoritesFile.cpp lib/LibraryIndex/LibraryState.h lib/LibraryIndex/LibraryState.cpp test/library_favorites/LibraryFavoritesTest.cpp`

```bash
git add lib/LibraryIndex test/library_favorites/LibraryFavoritesTest.cpp
git commit -m "feat(library): a replaced book keeps its star and its cursor"
```

### Task 3: `POST /replace` — the swap endpoint

**Files:**
- Modify: `src/network/CrossPointWebServer.h` (declare `void handleReplace() const;` next to `handleDelete`)
- Modify: `src/network/CrossPointWebServer.cpp` (route + handler)

**Interfaces:**
- Consumes: `LibraryFavoritesFile::reanchor`, `library::reanchorLibraryStateSelection`, `library::favoriteNameHash(const char*, size_t)` (Task 2); existing `normalizeWebPath`, `isProtectedPath`, `clearBookCachePreservingUserState`, `library::markShelfStaleIfBook`.
- Produces: `POST /replace` form args `path` (original) + `staging` (`.optimizing` beside it) → 200 `{"ok":true}` or distinct 4xx/5xx text errors. Task 5 consumes it.

- [ ] **Step 1: Route + includes**

In the route table after the `/delete` registration add:

```cpp
  // Swap a book for its optimized staging copy (Optimize on device)
  server->on("/replace", HTTP_POST, [this] { handleReplace(); });
```

Add includes next to the existing library include(s) at the top of `CrossPointWebServer.cpp` if not already present: `#include "LibraryFavoritesFile.h"` (LibraryState.h is already reachable — `markShelfStaleIfBook` compiles at :953; verify and add what is missing).

- [ ] **Step 2: Handler (place after `handleDelete`)**

```cpp
void CrossPointWebServer::handleReplace() const {
  if (!server->hasArg("path") || !server->hasArg("staging")) {
    server->send(400, "text/plain", "Missing path or staging");
    return;
  }

  const String targetPath = normalizeWebPath(server->arg("path"));
  const String stagingPath = normalizeWebPath(server->arg("staging"));
  if (targetPath.isEmpty() || targetPath == "/" || stagingPath.isEmpty() || stagingPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (isProtectedPath(targetPath) || isProtectedPath(stagingPath)) {
    server->send(403, "text/plain", "Protected path");
    return;
  }
  if (!stagingPath.endsWith(".optimizing")) {
    server->send(400, "text/plain", "Staging must end with .optimizing");
    return;
  }
  const String targetParent = targetPath.substring(0, targetPath.lastIndexOf('/'));
  const String stagingParent = stagingPath.substring(0, stagingPath.lastIndexOf('/'));
  if (targetParent != stagingParent) {
    server->send(400, "text/plain", "Staging must sit beside the target");
    return;
  }

  uint32_t oldSize = 0;
  {
    HalFile target = Storage.open(targetPath.c_str());
    if (!target) {
      server->send(404, "text/plain", "Target not found");
      return;
    }
    if (target.isDirectory()) {
      target.close();
      server->send(400, "text/plain", "Target is a directory");
      return;
    }
    oldSize = static_cast<uint32_t>(target.fileSize());
    target.close();
  }

  uint32_t newSize = 0;
  {
    HalFile staging = Storage.open(stagingPath.c_str());
    if (!staging) {
      server->send(404, "text/plain", "Staging not found");
      return;
    }
    if (staging.isDirectory()) {
      staging.close();
      server->send(400, "text/plain", "Staging is a directory");
      return;
    }
    newSize = static_cast<uint32_t>(staging.fileSize());
    staging.close();
    if (newSize == 0) {
      server->send(400, "text/plain", "Staging is empty");
      return;
    }
  }

  // Derived cache dropped, reading position and stats kept — same path, same
  // cache dir, so the preserved files greet the optimized copy.
  clearBookCachePreservingUserState(targetPath.c_str());

  // The only moment without two complete copies: delete + rename, entirely
  // inside the firmware, Wi-Fi-independent.
  if (!Storage.remove(targetPath.c_str())) {
    LOG_ERR("WEB", "Replace: cannot remove %s", targetPath.c_str());
    server->send(500, "text/plain", "Cannot remove original");
    return;
  }
  {
    HalFile staging = Storage.open(stagingPath.c_str());
    const bool renamed = staging && staging.rename(targetPath.c_str());
    if (staging) staging.close();
    if (!renamed) {
      // The book still exists — in the staging file. Say so instead of guessing.
      LOG_ERR("WEB", "Replace: original removed but rename failed; book lives at %s", stagingPath.c_str());
      server->send(500, "text/plain", "Rename failed - book preserved in staging file");
      return;
    }
  }

  // Same name, new size: the star and the remembered cursor follow.
  const String base = targetPath.substring(targetPath.lastIndexOf('/') + 1);
  const library::FavoriteKey from{library::favoriteNameHash(base.c_str(), base.length()), oldSize};
  const library::FavoriteKey to{from.nameHash, newSize};
  library::LibraryFavoritesFile favorites;
  if (favorites.load()) favorites.reanchor(from, to);
  library::reanchorLibraryStateSelection(from, to);

  library::markShelfStaleIfBook(targetPath.c_str());
  LOG_DBG("WEB", "Replaced %s (%u -> %u bytes)", targetPath.c_str(), oldSize, newSize);
  server->send(200, "application/json", "{\"ok\":true}");
}
```

(Adjust only to match real signatures found while editing — e.g. `HalFile::fileSize()` vs `fileSize64()`, `FavoriteKey` aggregate init — and note any adjustment in the report.)

- [ ] **Step 3: Compile gate**

Run: `pio run -e simulator`. Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/network/CrossPointWebServer.h src/network/CrossPointWebServer.cpp
git commit -m "feat(web): /replace swaps a book for its optimized self"
```

### Task 4: Mobile card layout — variante 3

**Files:**
- Modify: `web/pages/files.js` (row/cell class hooks in `hydrate()` :179-226, `.dl-btn` action, row-tap handler)
- Modify: `web/pages/files.css` (replace the table rules inside the existing `@media (max-width: 600px)` block :1256+)

**Interfaces:**
- Consumes: existing `.select-item` checkboxes, `downloadUrl()`, `handleFileActionClick`.
- Produces: cell classes `c-sel`, `c-name`, `c-type`, `c-size` and row classes `file-row`/`folder-row` (Task 5's ⚡ button lands inside the same `.action-icon-group`); selection styling via `tr:has(.select-item:checked)`.

- [ ] **Step 1: Class hooks in hydrate()**

In the folder branch (:186-192): `<tr class="folder-row">` already exists; add classes to its cells: checkbox td → `<td class="c-sel">`, name td → `<td class="c-name">`, badge td → `<td class="c-type">`, `-` td → `<td class="c-size">` (actions td keeps `actions-col`).

In the file branch (:199-218): change `<tr class="${file.isEpub ? "epub-file" : ""}">` to `<tr class="file-row ${file.isEpub ? "epub-file" : ""}">`; add the same four cell classes; and append a download button to the action group, FIRST in the group:

```js
        fileTableContent += `<button class="dl-btn file-action-btn" data-action="download" data-name="${escapeHtml(file.name)}" data-path="${encodeURIComponent(filePath)}" title="Download">⬇</button>`;
```

In `handleFileActionClick`, add a `download` case that does `window.open(downloadUrl(decodeURIComponent(btn.dataset.path)), "_blank")` (mirror how the existing cases read `data-path`).

- [ ] **Step 2: Row-tap select (mobile only)**

After the `fileTable.querySelectorAll(".file-action-btn")...` wiring (:224-226), add:

```js
    // Mobile cards: the whole row is the checkbox (variante 3). Desktop keeps
    // the real checkboxes; guard on the same width the CSS card layout uses.
    fileTable.querySelectorAll("tr.file-row").forEach((row) => {
      row.addEventListener("click", (e) => {
        if (window.innerWidth > 600) return;
        if (e.target.closest(".action-icon-group, input, a")) return;
        const box = row.querySelector(".select-item");
        if (!box) return;
        box.checked = !box.checked;
        box.dispatchEvent(new Event("change", { bubbles: true }));
      });
    });
```

(Folder rows keep their link-tap = navigate; folders are selected via their visible checkbox — see CSS. Do NOT attach the row-tap handler to `folder-row`.)

- [ ] **Step 3: Card CSS**

Inside the existing `@media (max-width: 600px)` block in `files.css` (:1256+), REPLACE the `.file-table` squeeze rules (padding/font-size shrinks for th/td, `.file-icon`, `.epub-badge`, `.action-icon-group` tweaks — read the block first and remove only the table-related rules, keep unrelated ones) with the card layout:

```css
      /* ——— cards replace the table (variante 3: whole-row select, ring) ——— */
      .file-table, .file-table tbody { display: block; }
      .file-table tr:first-child { display: none; } /* header row */
      .file-table tr {
        display: grid;
        grid-template-columns: 1fr auto;
        gap: 2px 8px;
        background: #fff;
        border: 1px solid #e3e2d8;
        border-radius: 14px;
        padding: 10px 12px;
        margin: 0 0 10px;
      }
      .file-table tr:hover { background: #fff; }
      .file-table td, .file-table th { display: block; border: 0; padding: 0; }
      .c-name { grid-column: 1 / -1; min-width: 0; }
      .c-name .file-link, .c-name .folder-link {
        font-family: "Iowan Old Style", Palatino, "Palatino Linotype", Georgia, serif;
        font-size: 15.5px; line-height: 1.35; font-weight: 600;
        display: -webkit-box; -webkit-line-clamp: 3; -webkit-box-orient: vertical;
        overflow: hidden; overflow-wrap: anywhere; text-decoration: none;
      }
      .c-type, .c-size { display: inline-block; }
      .c-type { grid-row: 2; grid-column: 1; }
      .c-size { grid-row: 2; grid-column: 1; margin-left: 64px; color: #78806f; font-size: 12px; font-variant-numeric: tabular-nums; align-self: center; }
      .actions-col { grid-row: 2; grid-column: 2; justify-self: end; }
      .action-icon-group .file-action-btn { width: 38px; height: 38px; font-size: 15px; }
      /* files: the row IS the checkbox; folders keep a visible one */
      tr.file-row .c-sel { display: none; }
      tr.folder-row { grid-template-columns: auto 1fr auto; }
      tr.folder-row .c-sel { grid-row: 1; align-self: center; }
      tr.folder-row .c-name { grid-column: 2; }
      tr.folder-row .c-type, tr.folder-row .c-size { display: none; }
      tr.folder-row .actions-col { grid-row: 1; grid-column: 3; }
      /* selection: sage wash + ring + ✓ (variante 3) */
      .file-table tr:has(.select-item:checked) {
        background: #eef2ea;
        border-color: #5c7a57;
        box-shadow: 0 0 0 1.5px #5c7a57;
      }
      tr.file-row:has(.select-item:checked) .c-name .file-link::before {
        content: "✓ "; color: #40573d; font-weight: 800;
      }
      /* row-tap selects: the name must not hijack the tap; ⬇ replaces it */
      tr.file-row .file-link { pointer-events: none; color: inherit; }
```

Outside the media query (desktop default), add near `.action-icon-group` (:1092):

```css
.dl-btn { display: none; } /* download is the name link on desktop */
@media (max-width: 600px) { .dl-btn { display: inline-flex; } }
```

(Colors mirror the approved mockup: sage `#5c7a57`/`#40573d`, wash `#eef2ea`, hairline `#e3e2d8`. If files.css defines CSS variables for its palette, use those instead and note it.)

- [ ] **Step 4: Verify**

Run: `node --check web/pages/files.js` (silent), `python3 scripts/build_web.py` (no error), Task 1 harness (`ALL OK`).

- [ ] **Step 5: Commit**

```bash
git add web/pages/files.js web/pages/files.css
git commit -m "feat(web): the file manager becomes cards on mobile"
```

### Task 5: The optimize engine — one book, per-row ⚡

**Files:**
- Modify: `web/pages/files.js` (queue engine, `convertEpubFile` opts param, `uploadFileWebSocket` dest param, per-row ⚡, optimize-mode modal presentation)
- Modify: `web/pages/files.css` (`.optimize-mode` hides upload chrome)

**Interfaces:**
- Consumes: `downloadUrl`, `SYNC_IDENTITY_PATH`, `X_LOCATION_MANIFEST_PATH`, `convertEpubFile`, `uploadFileWebSocket`, `POST /replace` (Task 3), `/delete` idiom, modal elements (`uploadModal`, `progress-fill`, `progress-text`, log).
- Produces: `async optimizeBookOnDevice(filePath, fileName, onPhase) -> {skipped}|{replaced,oldSize,newSize}`; `runOptimizeQueue(items)` (Task 6 feeds it multi-select); per-row `data-action="optimize"` button.

- [ ] **Step 1: convertEpubFile learns a forced identity**

Change the signature `async function convertEpubFile(file, progressCallback)` to `async function convertEpubFile(file, progressCallback, opts = {})`, and in the sync-identity block change the toggle read to:

```js
  const preserveSyncIdentity = opts.forceSyncIdentity === true || !!document.getElementById("preserveSyncIdentityToggle")?.checked;
```

(The Task-1 sentinel block is NOT touched — this line lives in the Task-2 embed block, outside the sentinels. Verify with the harness afterwards.)

- [ ] **Step 2: uploadFileWebSocket learns a destination**

Change `function uploadFileWebSocket(file, onProgress, onComplete, onError)` to accept a fifth param `destPath = currentPath` and use it in the START frame instead of the global: `ws.send(\`START:${file.name}:${file.size}:${destPath}\`)`. All existing callers keep their behavior (default).

- [ ] **Step 3: The engine**

Add after the sync-identity related constants area:

```js
const OPTIMIZING_SUFFIX = ".optimizing";

async function deleteDevicePath(path) {
  try {
    await fetch("/delete", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: "path=" + encodeURIComponent(path),
    });
  } catch (e) {
    console.warn("cleanup delete failed:", path, e);
  }
}

/**
 * Optimize one on-card book in place. Phases via onPhase(label, pct 0-100).
 * Returns {skipped:true} for already-optimized books. Throws on failure —
 * the original is untouched; staging is cleaned up here on a failed replace.
 */
async function optimizeBookOnDevice(filePath, fileName, onPhase) {
  onPhase("Downloading", 2);
  const resp = await fetch(downloadUrl(filePath));
  if (!resp.ok) throw new Error("Download failed: " + resp.status);
  const blob = await resp.blob();

  onPhase("Checking", 20);
  const zip = await JSZip.loadAsync(blob);
  const lower = Object.keys(zip.files).map((p) => p.toLowerCase());
  if (lower.includes(SYNC_IDENTITY_PATH.toLowerCase()) || lower.includes(X_LOCATION_MANIFEST_PATH.toLowerCase())) {
    return { skipped: true };
  }

  const original = new File([blob], fileName, { type: "application/epub+zip" });
  const converted = await convertEpubFile(original, (p) => onPhase("Optimizing", 20 + p * 0.45), {
    forceSyncIdentity: true,
  });

  const stagingPath = filePath + OPTIMIZING_SUFFIX;
  const stagingName = fileName + OPTIMIZING_SUFFIX;
  const bookDir = filePath.slice(0, filePath.lastIndexOf("/")) || "/";
  await deleteDevicePath(stagingPath); // orphan from a previous failed run

  onPhase("Uploading", 68);
  const stagingFile = new File([converted], stagingName, { type: "application/octet-stream" });
  await uploadFileWebSocket(
    stagingFile,
    (loaded, total) => onPhase("Uploading", 68 + Math.round((loaded / total) * 27)),
    null,
    null,
    bookDir,
  );

  onPhase("Replacing", 96);
  const form = new FormData();
  form.append("path", filePath);
  form.append("staging", stagingPath);
  const rep = await fetch("/replace", { method: "POST", body: form });
  if (!rep.ok) {
    await deleteDevicePath(stagingPath);
    throw new Error("Replace failed: " + (await rep.text()));
  }
  onPhase("Done", 100);
  return { replaced: true, oldSize: blob.size, newSize: converted.size };
}

/** Sequential queue over [{path, name}]; drives the upload modal in optimize mode. */
async function runOptimizeQueue(items) {
  if (!items.length) return;
  if (items.length > 1 && !confirm(`Optimize ${items.length} books? Each original is replaced in place (reading progress and favorites are kept).`)) {
    return;
  }
  const progressFill = document.getElementById("progress-fill");
  const progressText = document.getElementById("progress-text");
  document.getElementById("uploadModal").classList.add("open", "optimize-mode");
  document.getElementById("progress-container").style.display = "block";
  clearLog();
  showLog();

  let done = 0, skipped = 0, failed = 0;
  for (let i = 0; i < items.length; i++) {
    const { path, name } = items[i];
    const label = `(${i + 1}/${items.length}) ${name}`;
    try {
      const result = await optimizeBookOnDevice(path, name, (phase, pct) => {
        progressFill.style.width = pct + "%";
        progressText.textContent = `${phase} ${label}`;
      });
      if (result.skipped) {
        skipped++;
        log(`Already optimized — skipped: ${name}`, "", "INFO");
      } else {
        done++;
        log(`Optimized: ${name} (${formatBytes(result.oldSize)} → ${formatBytes(result.newSize)})`, "success", "DONE");
      }
    } catch (err) {
      failed++;
      console.error("Optimize failed:", name, err);
      logError(`Failed (original untouched): ${name} — ${err.message}`);
    }
  }
  progressFill.style.width = "100%";
  progressText.textContent = `Optimize complete: ${done} optimized · ${skipped} already optimized · ${failed} failed`;
  log(`Summary: ${done} optimized · ${skipped} already optimized · ${failed} failed`, "", "DONE");
  await hydrate(); // refresh the listing without a full reload
}
```

(`formatBytes` and `clearLog`/`showLog`/`log`/`logError` already exist — verify names while editing and use the file's actual helpers; `formatFileSize` is the listing's formatter if `formatBytes` differs.)

- [ ] **Step 4: Per-row ⚡ + modal presentation**

In `hydrate()`'s file branch, add before the move button, EPUB files only:

```js
        if (file.isEpub) {
          fileTableContent += `<button class="optimize-btn file-action-btn" data-action="optimize" data-name="${escapeHtml(file.name)}" data-path="${encodeURIComponent(filePath)}" title="Optimize on device">⚡</button>`;
        }
```

In `handleFileActionClick`, add the case:

```js
  if (action === "optimize") {
    runOptimizeQueue([{ path: decodeURIComponent(btn.dataset.path), name: btn.dataset.name }]);
    return;
  }
```

(match the function's actual variable names for `action`/`btn`).

In `files.css`, near the modal styles:

```css
.optimize-mode .drop-zone,
.optimize-mode .convert-options,
.optimize-mode #uploadBtn,
.optimize-mode #startConversionBtn { display: none !important; }
```

And ensure `closeUploadModal()` also removes `optimize-mode` from the modal's classList (one line where it removes/cleans up).

- [ ] **Step 5: Verify + commit**

Run: `node --check web/pages/files.js`, `python3 scripts/build_web.py`, Task 1 harness (`ALL OK`).

```bash
git add web/pages/files.js web/pages/files.css
git commit -m "feat(web): one tap optimizes a book where it lives"
```

### Task 6: Batch — toolbar button, selection, folder recursion

**Files:**
- Modify: `web/pages/files.html` (toolbar button — read the toolbar at :7-10 first and mirror the Delete Selected button's exact classes/markup)
- Modify: `web/pages/files.js` (selection → items, recursion)

**Interfaces:**
- Consumes: `getSelectedItems()` (`[{name, path, isFolder}]`, paths URI-encoded — decode like the delete flow does), `runOptimizeQueue` (Task 5), `/api/files`.
- Produces: toolbar `⚡ Optimize Selected` button → `openOptimizeSelected()`.

- [ ] **Step 1: Toolbar button**

In `web/pages/files.html`, after the Delete Selected button, same classes:

```html
      <button ... onclick="openOptimizeSelected()">⚡ Optimize Selected</button>
```

(`...` = the exact class/attribute set the Delete Selected button uses — copy it.)

- [ ] **Step 2: Selection expansion + kickoff**

Add to `files.js`:

```js
const OPTIMIZE_WALK_MAX_DEPTH = 8;

async function collectEpubsUnder(path, depth, out) {
  if (depth > OPTIMIZE_WALK_MAX_DEPTH) return;
  const resp = await fetch("/api/files?path=" + encodeURIComponent(path) + "&_=" + Date.now());
  if (!resp.ok) throw new Error("Cannot list " + path);
  const entries = await resp.json();
  for (const entry of entries) {
    const childPath = (path.endsWith("/") ? path : path + "/") + entry.name;
    if (entry.isDirectory) {
      await collectEpubsUnder(childPath, depth + 1, out);
    } else if (entry.isEpub || /\.epub$/i.test(entry.name)) {
      out.push({ path: childPath, name: entry.name });
    }
  }
}

async function openOptimizeSelected() {
  const selected = getSelectedItems();
  if (!selected.length) {
    alert("Select books or folders first.");
    return;
  }
  const items = [];
  try {
    for (const sel of selected) {
      const path = decodeURIComponent(sel.path);
      if (sel.isFolder) {
        await collectEpubsUnder(path, 0, items);
      } else if (/\.epub$/i.test(sel.name)) {
        items.push({ path, name: sel.name });
      }
    }
  } catch (err) {
    alert("Could not scan the selection: " + err.message);
    return;
  }
  if (!items.length) {
    alert("No EPUB files in the selection.");
    return;
  }
  runOptimizeQueue(items);
}
```

(Match `getSelectedItems()`'s real field names/encoding by reading it — the delete flow at :1190-1245 is the reference consumer.)

- [ ] **Step 3: Verify + commit**

Run: `node --check web/pages/files.js`, `python3 scripts/build_web.py`, Task 1 harness.

```bash
git add web/pages/files.html web/pages/files.js
git commit -m "feat(web): the toolbar optimizes a whole selection, folders included"
```

### Task 7: CHANGELOG

**Files:**
- Modify: `CHANGELOG.md` (`[Unreleased]`)

- [ ] **Step 1: Entries**

Under `### Added`:

```markdown
- Optimize on device: the File Manager can now optimize books that are already on the SD card — select books or folders (or use the per-book ⚡ action) and each one is downloaded, optimized in the browser, and swapped in place. Reading progress, favorites and the library cursor survive the swap; already-optimized books are detected and skipped; the original is never deleted before its optimized copy is safely on the card.
```

Under `### Changed`:

```markdown
- The File Manager becomes cards on phone-sized screens: tap a card to select it, serif titles that wrap instead of crushing the table, and a download button on each card.
```

Under `### Fixed`:

```markdown
- Books uploaded through the fast (WebSocket) upload path now appear in the Library without a manual index rebuild.
```

- [ ] **Step 2: Commit**

```bash
git add CHANGELOG.md
git commit -m "docs: the changelog learns about on-device optimization"
```

### Task 8: Full builds, release gate, device matrix

**Files:** none (verification + release).

- [ ] **Step 1: Full verification, strictly sequential**

1. `cmake --build test/build -j --target LibraryFavoritesTest KOReaderEmbeddedIdTest && ctest --test-dir test/build -R "Favorites|EmbeddedId" --output-on-failure`
2. `node --check web/pages/files.js` + Task 1 harness
3. `pio run -e simulator` → `pio run -e default` → `pio run -e sticky` → `pio run -e x4-pro` (one at a time)
4. `pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high`

- [ ] **Step 2: Test build AFTER the last commit**

```bash
git rev-parse --short HEAD
pio run -e default
cp .pio/build/default/firmware.bin firmware/CrossInkLibrary-<shortsha>.bin
```

- [ ] **Step 3: Device matrix (user, X4/X3 + browser)**

| # | Scenario | Expected |
|---|----------|----------|
| 1 | Mobile: File Manager on the phone | Cards, not a squeezed table; serif clamped titles; ⬇ on each card |
| 2 | Mobile: tap a book card | Ring outline + sage wash + ✓; toolbar counts follow; tap again deselects |
| 3 | Mobile: tap a folder card's name | Navigates (does NOT select); folder checkbox still selects it |
| 4 | Per-row ⚡ on a favorited, in-progress book | After swap: star intact, reading position intact, book resurfaces atop Added, KOSync still paired (same embedded id in serial log) |
| 5 | ⚡ on an already-optimized book | Skipped, counted "already optimized" |
| 6 | Toolbar batch with a folder selected | Recurses, optimizes EPUBs only, sequential, summary correct |
| 7 | Kill Wi-Fi mid-upload of staging | Original intact; orphan `.optimizing` visible; re-running the book cleans it up first |
| 8 | Upload any book via portal (WS path) | Appears on the shelf next entry WITHOUT manual rebuild (Task 1 fix) |
| 9 | Desktop browser | Table unchanged; ⚡ present in actions; no ⬇ button |

Also still pending from the sync chantier: matrix rows 5/7/8 (control book, re-optimize preserved hash — row 7 doubles with this feature's skip detection, method flip).

- [ ] **Step 4: Release v1.5.16 (only on green matrix + user go)**

Recipe per `docs/releases-and-ota.md`: `CROSSINK_RELEASE_VERSION=1.5.16 pio run -e default`, `gh auth switch --user oreglio`, `gh release create v1.5.16 --repo oreglio/CrossInkLibrary --title ... --notes ... .pio/build/default/firmware-x3-x4.bin`, switch back to aurelien-edusign.

---

## Self-Review (done at plan time)

- Spec coverage: WS fix (T1), reanchor hook (T2), /replace with write-beside swap (T3), variante-3 mobile cards incl. ⬇ + folder checkbox nuance (T4), engine with skip detection + forced identity + orphan cleanup (T5), toolbar batch + recursion (T6), CHANGELOG (T7), verification + matrix + release gate (T8). Spec's out-of-scope list untouched.
- Placeholder scan: the two "mirror the existing markup/idiom" steps (toolbar button classes, handleFileActionClick variable names) name their exact reference lines — deliberate anchors, not gaps.
- Type consistency: `optimizeBookOnDevice`/`runOptimizeQueue`/`openOptimizeSelected` names match across T5/T6; `reanchorFavoriteKey`/`reanchor`/`reanchorLibraryStateSelection` match T2→T3; staging suffix `.optimizing` constant used in T3 validation and T5 engine; `destPath` param default keeps existing WS callers intact.
- Known risk accepted: `tr:has(:checked)` needs Safari ≥15.4 / Chrome ≥105 — fine for the user's devices; degraded (no ring) elsewhere, selection still works via toolbar counts.
