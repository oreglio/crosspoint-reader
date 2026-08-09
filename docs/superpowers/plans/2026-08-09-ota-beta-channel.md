# OTA Beta Channel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a device check the beta channel instead of stable, chosen per check, and install an older release once explicitly confirmed.

**Architecture:** Three layers, each independently testable. `ReleaseJsonParser` learns to skip one wrapping array so `/releases` can be read at all. `OtaUpdater` gains a channel argument and an explicit downgrade intent. `OtaUpdateActivity` gains a channel-selection state and a second confirmation wording.

**Tech Stack:** C++20, PlatformIO, ESP-IDF/Arduino-ESP32, GoogleTest for host tests.

## Global Constraints

- Build **both** environments: `pio run -e default` (X3/X4) and `pio run -e simulator`. `src/network/OtaUpdater.cpp:1-8` is a `#ifdef SIMULATOR` stub block with parallel signatures — any signature change must be mirrored there or the simulator build breaks.
- All user-facing strings via `tr(STR_*)`. New keys go in `lib/I18n/translations/english.yaml` and `french.yaml`, then `python3 scripts/gen_i18n.py`.
- `clang-format -i` on every touched C++ file before committing.
- The OTA asset name contract is unchanged: `firmware-x3-x4.bin` (`OtaUpdater.cpp:37`).
- Do not publish any release from this plan.

---

### Task 1: Teach ReleaseJsonParser to skip one wrapping array

**Files:**
- Modify: `lib/JsonParser/ReleaseJsonParser.h`
- Modify: `lib/JsonParser/ReleaseJsonParser.cpp`
- Modify: `test/release_json_parser/ReleaseJsonParserTest.cpp`

**Interfaces:**
- Produces: `void ReleaseJsonParser::setExpectArray(bool expectArray)`. Default false, which leaves every existing behaviour untouched.

- [ ] **Step 1: Write the failing tests**

`ReleaseJsonParserTest.cpp` does **not** use GoogleTest despite the CMake linking it:
it is a hand-rolled harness of `void testXxx()` functions with `ASSERT_TRUE`/`ASSERT_EQ`/
`ASSERT_STREQ` macros, `PASS()`, and its own `main()` listing every test. Write in that
style and register the new functions in `main()`, or they never run.

Consequence for verification: `ctest -R ReleaseJsonParser` reports **0 tests** and passes
vacuously. Always run the binary directly.

Append before `main()`:

```cpp
void testWrappingArraySingleRelease() {
  printf("testWrappingArraySingleRelease...\n");

  const char* json = R"([{
      "tag_name": "v1.5.26-rc1",
      "prerelease": true,
      "assets": [{"name": "firmware.bin", "browser_download_url": "https://fw-rc", "size": 1234}]
    }])";

  ReleaseJsonParser p;
  p.setExpectArray(true);
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_STREQ(p.getTagName(), "v1.5.26-rc1");
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getFirmwareUrl(), "https://fw-rc");
  ASSERT_EQ(p.getFirmwareSize(), 1234u);

  printf("  passed\n");
  PASS();
}

void testWrappingArrayTakesFirstRelease() {
  printf("testWrappingArrayTakesFirstRelease...\n");

  const char* json = R"([
      {"tag_name": "v1.5.27", "assets": [{"name": "firmware.bin", "browser_download_url": "https://new", "size": 10}]},
      {"tag_name": "v1.5.26", "assets": [{"name": "firmware.bin", "browser_download_url": "https://old", "size": 20}]}
    ])";

  ReleaseJsonParser p;
  p.setExpectArray(true);
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_STREQ(p.getTagName(), "v1.5.27");
  ASSERT_STREQ(p.getFirmwareUrl(), "https://new");
  ASSERT_EQ(p.getFirmwareSize(), 10u);

  printf("  passed\n");
  PASS();
}

void testWrappingArrayEmpty() {
  printf("testWrappingArrayEmpty...\n");

  ReleaseJsonParser p;
  p.setExpectArray(true);
  const char* json = "[]";
  p.feed(json, strlen(json));

  ASSERT_TRUE(!p.foundTag());
  ASSERT_TRUE(!p.foundFirmware());

  printf("  passed\n");
  PASS();
}

void testWrappingArrayIgnoredWhenFlagOff() {
  printf("testWrappingArrayIgnoredWhenFlagOff...\n");

  const char* json = R"([{
      "tag_name": "v1.5.26-rc1",
      "assets": [{"name": "firmware.bin", "browser_download_url": "https://fw-rc", "size": 1234}]
    }])";

  ReleaseJsonParser p;  // flag left off: behaviour must be exactly as before
  p.feed(json, strlen(json));

  ASSERT_TRUE(!p.foundTag());

  printf("  passed\n");
  PASS();
}
```

and add the four calls to `main()`, next to `testArraysAtTopLevel();`:

```cpp
  testWrappingArraySingleRelease();
  testWrappingArrayTakesFirstRelease();
  testWrappingArrayEmpty();
  testWrappingArrayIgnoredWhenFlagOff();
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake -S test -B test/build -DCMAKE_BUILD_TYPE=Release
cmake --build test/build --target ReleaseJsonParserTest
```

Expected: FAIL to compile — `no member named 'setExpectArray'`.

- [ ] **Step 3: Add the flag to the header**

In `lib/JsonParser/ReleaseJsonParser.h`, add to the public section after `setAssetMatcher`:

```cpp
  // /releases returns a list where /releases/latest returns one object. With this
  // set, the first top-level '[' is stepped over without changing depth, so the
  // release inside it still sits at depth 1 and every existing rule applies.
  void setExpectArray(bool expectArray);
```

and to the private data members:

```cpp
  bool expectArray = false;
  bool arrayEntered = false;
  bool completed = false;
```

- [ ] **Step 4: Implement it**

In `lib/JsonParser/ReleaseJsonParser.cpp`, add the setter next to `setAssetMatcher`:

```cpp
void ReleaseJsonParser::setExpectArray(const bool expect) {
  expectArray = expect;
  arrayEntered = false;
  completed = false;
}
```

In `reset()`, alongside `depth = 0;`, add:

```cpp
  arrayEntered = false;
  completed = false;
```

In `sOnArrayStart`, replace the `Position::TOP_LEVEL` case body with:

```cpp
    case Position::TOP_LEVEL:
      if (self->expectArray && !self->arrayEntered && self->depth == 0) {
        // Step over the wrapping array without counting it, so the release
        // object inside still lands at depth 1.
        self->arrayEntered = true;
      } else if (self->lastKey == LastKey::ASSETS && self->depth == 1) {
        self->position = Position::IN_ASSETS_ARRAY;
      } else {
        self->depth++;
      }
      self->lastKey = LastKey::NONE;
      break;
```

In `sOnObjectEnd`, replace the `Position::TOP_LEVEL` case body with:

```cpp
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      // A list may carry more releases after this one; the first is the newest,
      // so stop taking values once it has closed.
      if (self->arrayEntered && self->depth == 0 && self->tagFound) self->completed = true;
      break;
```

`tagFound` is the member `foundTag()` returns (`ReleaseJsonParser.cpp:54`).

Then make the three value handlers ignore input once complete. At the very top of the
body of `sOnKey`, `sOnString` and `sOnNumber`, after the `self` cast:

```cpp
  if (self->completed) return;
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build test/build --target ReleaseJsonParserTest && ./test/build/release_json_parser/ReleaseJsonParserTest
```

Expected: `=== Results: N passed, 0 failed ===`, N being four more than before. Run the
binary, not `ctest` — this suite is invisible to it.

- [ ] **Step 6: Commit**

```bash
clang-format -i lib/JsonParser/ReleaseJsonParser.cpp lib/JsonParser/ReleaseJsonParser.h test/release_json_parser/ReleaseJsonParserTest.cpp
git add lib/JsonParser/ReleaseJsonParser.h lib/JsonParser/ReleaseJsonParser.cpp test/release_json_parser/ReleaseJsonParserTest.cpp
git commit -m "feat: let the release parser read a one-element release list"
```

---

### Task 2: A channel argument and an explicit downgrade intent on OtaUpdater

**Files:**
- Modify: `src/network/OtaUpdater.h`
- Modify: `src/network/OtaUpdater.cpp` (both the simulator stub block and the real one)

**Interfaces:**
- Consumes: `ReleaseJsonParser::setExpectArray` (Task 1).
- Produces: `enum class OtaChannel : uint8_t { Stable, Beta }`, `OtaUpdaterError checkForUpdate(OtaChannel channel = OtaChannel::Stable)`, `bool isDifferentVersion() const`, and `installUpdate(ProgressCallback, void*, std::atomic<bool>*, bool allowOlder = false)`.

- [ ] **Step 1: Declare the channel and the two new entry points**

In `src/network/OtaUpdater.h`, above the class:

```cpp
enum class OtaChannel : uint8_t { Stable, Beta };
```

Change the two declarations to:

```cpp
  OtaUpdaterError checkForUpdate(OtaChannel channel = OtaChannel::Stable);
  // An asset was found and its tag differs from ours — in either direction.
  bool isDifferentVersion() const;
  OtaUpdaterError installUpdate(ProgressCallback onProgress, void* ctx, std::atomic<bool>* cancelRequested,
                                bool allowOlder = false);
```

The header needs `#include <cstdint>` if it does not already have it.

- [ ] **Step 2: Update the simulator stubs in the same edit**

`src/network/OtaUpdater.cpp:1-8` must keep compiling. Replace the two stub lines with:

```cpp
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate(OtaChannel) { return NO_UPDATE; }
bool OtaUpdater::isDifferentVersion() const { return false; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*, std::atomic<bool>*, bool) {
  return NO_UPDATE;
}
```

- [ ] **Step 3: Pick the URL from the channel**

In the real `checkForUpdate`, replace the fixed `latestReleaseUrl` constant use. Next to the existing `#define CROSSINK_OTA_RELEASE_URL`, add:

```cpp
#ifndef CROSSINK_OTA_RELEASES_URL
// The list route, unlike /releases/latest, includes prereleases. per_page=1 keeps
// the body to one release so the parser never has to choose between them.
#define CROSSINK_OTA_RELEASES_URL "https://api.github.com/repos/oreglio/CrossInkLibrary/releases?per_page=1"
#endif

constexpr char betaReleasesUrl[] = CROSSINK_OTA_RELEASES_URL;
```

Change the signature and the client config:

```cpp
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate(const OtaChannel channel) {
```

```cpp
  const bool beta = channel == OtaChannel::Beta;
  ReleaseJsonParser releaseParser(isMatchingFirmwareAssetName);
  releaseParser.setExpectArray(beta);

  esp_http_client_config_t client_config = {
      .url = beta ? betaReleasesUrl : latestReleaseUrl,
```

and log which one was used, right after the existing "Checking for update" line:

```cpp
  LOG_DBG("OTA", "Channel: %s", beta ? "beta" : "stable");
```

- [ ] **Step 4: Add isDifferentVersion and gate the install**

After `isUpdateNewer()`:

```cpp
bool OtaUpdater::isDifferentVersion() const {
  return updateAvailable && !latestVersion.empty() && latestVersion != CROSSINK_VERSION;
}
```

Change the signature and guard of `installUpdate`:

```cpp
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx,
                                                      std::atomic<bool>* cancelRequested, const bool allowOlder) {
```

```cpp
  // The guard stays; allowOlder is the caller stating that the user was shown the
  // direction of the change and accepted it. Anti-rollback is disabled in
  // sdkconfig, so nothing below this line will refuse an older image.
  if (!allowOlder && !isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }
```

- [ ] **Step 5: Build both environments**

```bash
clang-format -i src/network/OtaUpdater.cpp src/network/OtaUpdater.h
pio run -e default
pio run -e simulator
```

Expected: both SUCCESS. The simulator build is the one that catches a forgotten stub.

- [ ] **Step 6: Commit**

```bash
git add src/network/OtaUpdater.h src/network/OtaUpdater.cpp
git commit -m "feat: give the OTA updater a channel and an explicit downgrade intent"
```

---

### Task 3: Channel selection and the older-version prompt

**Files:**
- Modify: `src/activities/settings/OtaUpdateActivity.h`
- Modify: `src/activities/settings/OtaUpdateActivity.cpp`
- Modify: `lib/I18n/translations/english.yaml`, `lib/I18n/translations/french.yaml`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: `OtaChannel`, `checkForUpdate(OtaChannel)`, `isDifferentVersion()`, `installUpdate(..., bool allowOlder)` (Task 2).

- [ ] **Step 1: Add the strings**

Append to `lib/I18n/translations/english.yaml`:

```yaml
STR_OTA_CHANNEL_TITLE: "Update channel"
STR_OTA_CHANNEL_STABLE: "Stable"
STR_OTA_CHANNEL_BETA: "Beta"
STR_OTA_OLDER_AVAILABLE: "Older version available"
STR_OTA_OLDER_CONFIRM: "%s is older than the version installed. Install it anyway?"
```

Append to `lib/I18n/translations/french.yaml`:

```yaml
STR_OTA_CHANNEL_TITLE: "Canal de mise à jour"
STR_OTA_CHANNEL_STABLE: "Stable"
STR_OTA_CHANNEL_BETA: "Bêta"
STR_OTA_OLDER_AVAILABLE: "Version antérieure disponible"
STR_OTA_OLDER_CONFIRM: "%s est antérieure à la version installée. L'installer quand même ?"
```

Then:

```bash
python3 scripts/gen_i18n.py
```

- [ ] **Step 2: Add the state and the two fields**

In `src/activities/settings/OtaUpdateActivity.h`, add `CHANNEL_SELECTION` as the **first**
enum value so it precedes `WIFI_SELECTION`:

```cpp
  enum State {
    CHANNEL_SELECTION,
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    UPDATE_IN_PROGRESS,
    NO_UPDATE,
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };
```

Change the initial state and add the two fields:

```cpp
  State state = CHANNEL_SELECTION;
  OtaChannel channel = OtaChannel::Stable;
  // True when the release found is older than the one running: the confirmation
  // names the direction and installUpdate is told the user accepted it.
  bool installingOlder = false;
```

Add the private method:

```cpp
  void askChannel();
```

Include `"../util/OptionSelectionActivity.h"` in the `.cpp`.

- [ ] **Step 3: Ask for the channel on entry**

`onEnter()` currently either short-circuits to `onWifiSelectionComplete(true)` when a
connection already exists, or pushes `WifiSelectionActivity`. Both paths must happen only
*after* the channel is known, so move that body into a new private `startWifiFlow()` and
leave `onEnter()` as:

```cpp
void OtaUpdateActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);
  askChannel();
}
```

```cpp
void OtaUpdateActivity::startWifiFlow() {
  if (hasActiveWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  // Turn on WiFi immediately
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}
```

Declare `void startWifiFlow();` beside `askChannel();` in the header. Then:

```cpp
void OtaUpdateActivity::askChannel() {
  std::vector<std::string> channels{tr(STR_OTA_CHANNEL_STABLE), tr(STR_OTA_CHANNEL_BETA)};
  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "OtaChannel", StrId::STR_OTA_CHANNEL_TITLE,
                                                std::move(channels), 0),
      [this](const ActivityResult& result) {
        mappedInput.suppressNextConfirmRelease();
        const auto* choice = std::get_if<OptionSelectionResult>(&result.data);
        if (result.isCancelled || !choice) {
          finish();
          return;
        }
        channel = choice->index == 1 ? OtaChannel::Beta : OtaChannel::Stable;
        {
          RenderLock lock(*this);
          state = WIFI_SELECTION;
        }
        startWifiFlow();
      });
}
```

The `.cpp` needs `#include <string>` and `#include <vector>` if absent.

- [ ] **Step 4: Pass the channel to the check, and branch three ways on the result**

Find the `updater.checkForUpdate()` call and make it `updater.checkForUpdate(channel)`.

Replace the block at `OtaUpdateActivity.cpp:74-81`:

```cpp
  if (!updater.isUpdateNewer()) {
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    requestUpdate(true);
    return;
  }
```

with:

```cpp
  if (!updater.isUpdateNewer() && !updater.isDifferentVersion()) {
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    requestUpdate(true);
    return;
  }

  {
    RenderLock lock(*this);
    // Same release either way; only the wording and the install intent differ.
    installingOlder = !updater.isUpdateNewer();
    state = WAITING_CONFIRMATION;
  }
  requestUpdate(true);
  return;
```

and delete the original `state = WAITING_CONFIRMATION;` block that followed it, so the
function has exactly one path to that state.

- [ ] **Step 5: Word the confirmation from installingOlder**

At `OtaUpdateActivity.cpp:150`, in the `WAITING_CONFIRMATION` render branch, choose the
heading and body from `installingOlder`:

```cpp
    if (installingOlder) {
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_OTA_OLDER_AVAILABLE), true, EpdFontFamily::BOLD);
      char body[128];
      snprintf(body, sizeof(body), I18N.get(StrId::STR_OTA_OLDER_CONFIRM), updater.getLatestVersion().c_str());
      renderer.drawCenteredText(UI_10_FONT_ID, top + renderer.getLineHeight(UI_10_FONT_ID) + 8, body, true);
    } else {
      // ... the existing newer-version wording, unchanged
    }
```

Keep the existing button hints as they are: the confirm/back pair is the same in both cases.

- [ ] **Step 6: Pass the intent to the install**

`installUpdate` is declared with three defaulted parameters
(`OtaUpdater.h:39-40`) and the call site at `OtaUpdateActivity.cpp:216-223` passes only
two. The new flag is the fourth, so the third has to be spelled out. Change the closing
lines of that call from:

```cpp
      },
      this);
```

to:

```cpp
      },
      this, nullptr, installingOlder);
```

- [ ] **Step 7: Build both environments**

```bash
clang-format -i src/activities/settings/OtaUpdateActivity.cpp src/activities/settings/OtaUpdateActivity.h
pio run -e default
pio run -e simulator
./test/build/release_json_parser/ReleaseJsonParserTest
```

Expected: all three SUCCESS.

- [ ] **Step 8: Measure the flash budget**

```bash
ls -l .pio/build/default/firmware.bin
```

`app0` is 6,553,600 bytes and the countdown work left it at 6,344,832. Report the new size
and the percentage. If it exceeds the partition, stop and raise `partitions.csv`.

- [ ] **Step 9: Verify on hardware — both devices**

```bash
pio run -e default -t upload
```

On **X3** and again on **X4**:
1. Settings → Update: a two-row chooser appears, `Stable` / `Bêta`, before any Wi-Fi screen.
2. Back on that chooser leaves the update screen entirely.
3. `Stable` on an up-to-date device: `No update`, exactly as before this change.
4. `Bêta` reaches the network and reports a version — with no prerelease published, it
   reports the same release the stable channel does. Serial shows `OTA: Channel: beta`.
5. To exercise the downgrade, publish nothing: instead confirm the wording by checking a
   device running a version *newer* than the latest release — the screen must read
   "antérieure" and installing must succeed and boot.

- [ ] **Step 10: Update the changelog and commit**

Add under `## [Unreleased]` → `### Added` in `CHANGELOG.md`:

```markdown
- An update channel choice on the update screen: `Stable` behaves as before, `Beta` also offers pre-release builds so a device can try one before everybody gets it. The choice is made each time you check rather than stored in Settings. The update screen will now also offer a release that is *older* than the one installed, naming the direction plainly and asking before it does anything — which is how a device gets itself off a bad build without a computer.
```

```bash
git add src/activities/settings/OtaUpdateActivity.h src/activities/settings/OtaUpdateActivity.cpp \
        lib/I18n/translations/english.yaml lib/I18n/translations/french.yaml CHANGELOG.md
git commit -m "feat: choose an update channel per check, and allow a confirmed downgrade"
```

---

## Verification summary

| What | How |
|---|---|
| Array parsing | `./test/build/release_json_parser/ReleaseJsonParserTest` (ctest sees 0 tests here) |
| C3 firmware builds | `pio run -e default` |
| Stubs stayed in sync | `pio run -e simulator` |
| Channel chooser, both promptings | Flash and look, on **both** X3 and X4 |
| Flash budget | Task 3 Step 8, against 6,553,600 bytes |

No release is published by this plan.
