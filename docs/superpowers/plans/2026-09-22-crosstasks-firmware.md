# CrossTasks — Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Tasks screen to the reader — list, detail with a readable note, add via the keyboard, a pomodoro started on a task, QR pairing, and a manual two-way sync with the CrossDrop VPS — all inside the ESP32-C3's budget.

**Architecture:** Four pure, host-tested units carry every piece of logic that can be tested without hardware — `TaskSyncReader` (the wire framing), `TaskOpQueue` (pending local writes), `TaskRecord`/ordering, and the id generator. Three activities sit on top as thin screens: `TaskListActivity`, `TaskDetailActivity`, `TaskSyncActivity`. `TaskStore` persists the index through the existing `PersistableStore`, while note bodies live as one file each on SD and are never held whole in RAM.

**Tech Stack:** C++20, PlatformIO, Arduino-ESP32, GoogleTest + CTest for host tests, FreeInkUI/HAL from `freeink-sdk`.

**Spec:** `docs/superpowers/specs/2026-09-22-crosstasks-design.md`

**Depends on:** `docs/superpowers/plans/2026-09-22-crosstasks-server.md` Task 4 — the response framing contract. Tasks 1–4 here can be written and tested against fixtures before the server exists; Task 9 needs it live.

## Global Constraints

Copied from `AGENTS.md` and the spec. Every task inherits these.

- Target `pio run -e default` — **one binary serves X3 and X4**. Never `#if` on board where a runtime capability check exists.
- **`pio run -e simulator` is BROKEN on this branch and is not a gate for any task.** Pre-existing and
  unrelated to CrossTasks: `src/SettingsList.h:1039` calls `HalGPIO::supportsMultiTouch()`, which `lib/hal`
  provides but the simulator ignores (`lib_ignore = hal`), taking its copy from the vendored
  `uxjulia/crossink-simulator` package where the method does not exist. Do not try to fix it, and do not
  report it as a failure of your task. The gates are `pio run -e default` and the host suite in `test/`.
- **There is therefore NO visual verification before the user's first flash.** Anything that only a running
  screen would catch must instead be read: extract pure logic so the host suite can pin it, and walk the
  error paths by hand. State in your report what you read and what remains unverified.
- **Never hardcode screen coordinates.** Derive every position from `renderer.getScreenWidth()`, `getScreenHeight()` and `renderer.getOrientedViewableTRBL()`.
- **All user-facing strings via `tr(STR_*)`.** New keys go in `lib/I18n/translations/english.yaml` and `french.yaml`, then `python3 scripts/gen_i18n.py`. Never hand-edit `lib/I18n/I18n*.h/cpp`. Logs stay hardcoded.
- **No exceptions, no bare `new`.** Use `makeUniqueNoThrow<T>()` / `makeUniqueNoThrow<T[]>()` from `lib/Memory/Memory.h`. `new` is not nothrow on ESP32 — bare `new` calls `abort()`.
- **Log before every failure return** (`LOG_ERR`), then `return false`.
- Keep stack frames under 256 bytes; anything larger must be justified in a comment.
- `std::string` is banned from the per-task hot paths; use `char[]`, `string_view` and `snprintf`. `string_view::data()` is **not** null-terminated — never hand it to a C API.
- Files are `FsFile`, never Arduino `File`, and are **always closed explicitly**. On hardware only one reader may hold a path open at a time.
- Buttons are `MappedInputManager::Button::*`. The action bar carries exactly four labels, and
  `mapLabels(back, confirm, previous, next)` (src/MappedInputManager.h:234) binds them to the physical
  **Back / Confirm / Left / Right** in that order — `btn1…btn4` are its OUTPUT, already reordered per device,
  never four free slots you may assign as you like. Up/Down are not in the bar at all and remain available
  for scrolling or paging. Any screen wanting a fifth action must move it elsewhere, not invent a slot.
- Run `clang-format -i` on every touched C++ file before committing.
- **Priority encoding `0 = high, 1 = normal, 2 = low`**, matching the server.
- Ids are `'d' + 8 lowercase hex` when created here. Regex on the wire: `^[wd][0-9a-f]{8}$`.
- Limits: title ≤ 200 bytes, note ≤ 4096 bytes, `MAX_TASKS = 120`.

---

### Task 1: `TaskRecord` and priority ordering

**Files:**
- Create: `src/tasks/TaskRecord.h`
- Create: `test/task_record/TaskRecordTest.cpp`, `test/task_record/CMakeLists.txt`
- Modify: `test/CMakeLists.txt` (one `add_subdirectory`)

**Interfaces:**
- Produces: `struct TaskRecord { char id[10]; char title[201]; uint8_t priority; bool done; uint16_t noteBytes; }`,
  `constexpr size_t MAX_TASKS = 120`, and
  `bool taskOrderBefore(const TaskRecord& a, const TaskRecord& b)`.

Fixed-size fields, not `std::string`: 120 records × 216 bytes ≈ 26 KB if held as
a flat array, which is why the list activity holds them and frees them in
`onExit()` rather than a singleton keeping them resident.

- [ ] **Step 1: Write the failing test**

Create `test/task_record/TaskRecordTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "TaskRecord.h"

namespace {
TaskRecord make(const char* id, const char* title, uint8_t priority, bool done = false) {
  TaskRecord r{};
  std::snprintf(r.id, sizeof(r.id), "%s", id);
  std::snprintf(r.title, sizeof(r.title), "%s", title);
  r.priority = priority;
  r.done = done;
  return r;
}
}  // namespace

TEST(TaskOrder, HighPriorityComesFirst) {
  EXPECT_TRUE(taskOrderBefore(make("w00000001", "haute", 0), make("w00000002", "basse", 2)));
  EXPECT_FALSE(taskOrderBefore(make("w00000002", "basse", 2), make("w00000001", "haute", 0)));
}

TEST(TaskOrder, DoneSinksBelowEveryOpenTask) {
  // Une tâche faite de priorité haute passe quand même après une ouverte de priorité basse.
  EXPECT_TRUE(taskOrderBefore(make("w2", "ouverte basse", 2), make("w1", "faite haute", 0, true)));
}

TEST(TaskOrder, EqualPriorityFallsBackToId) {
  EXPECT_TRUE(taskOrderBefore(make("w00000001", "a", 1), make("w00000002", "b", 1)));
}

TEST(TaskOrder, SortIsStableAndTotal) {
  std::vector<TaskRecord> v{make("w3", "c", 2), make("w1", "a", 0, true), make("w2", "b", 0), make("w4", "d", 1)};
  std::sort(v.begin(), v.end(), taskOrderBefore);
  EXPECT_STREQ(v[0].id, "w2");
  EXPECT_STREQ(v[1].id, "w4");
  EXPECT_STREQ(v[2].id, "w3");
  EXPECT_STREQ(v[3].id, "w1");
}

TEST(TaskRecord, TitleIsBoundedAndNullTerminated) {
  TaskRecord r{};
  const std::string longTitle(400, 'x');
  std::snprintf(r.title, sizeof(r.title), "%s", longTitle.c_str());
  EXPECT_EQ(std::strlen(r.title), 200u);
}
```

Create `test/task_record/CMakeLists.txt`:

```cmake
add_executable(TaskRecordTest TaskRecordTest.cpp)
target_include_directories(TaskRecordTest PRIVATE ${REPO_ROOT}/src/task)
target_link_libraries(TaskRecordTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TaskRecordTest)
```

Add `add_subdirectory(task_record)` to `test/CMakeLists.txt` beside the others.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake -S test -B build/test && cmake --build build/test --target TaskRecordTest
```

Expected: FAIL — `TaskRecord.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `src/tasks/TaskRecord.h`:

```cpp
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

// Largeurs fixes plutôt que std::string : 120 enregistrements tiennent dans un
// tableau plat que l'écran de liste alloue en onEnter() et rend en onExit(),
// sans une seule allocation par tâche ni fragmentation du tas du C3.
inline constexpr size_t MAX_TASKS = 120;
inline constexpr size_t TASK_ID_LEN = 9;        // 'd' + 8 hex
inline constexpr size_t TASK_TITLE_MAX = 200;   // octets, hors terminateur
inline constexpr size_t TASK_NOTE_MAX = 4096;

inline constexpr uint8_t TASK_PRIORITY_HIGH = 0;
inline constexpr uint8_t TASK_PRIORITY_NORMAL = 1;
inline constexpr uint8_t TASK_PRIORITY_LOW = 2;

struct TaskRecord {
  char id[TASK_ID_LEN + 1];
  char title[TASK_TITLE_MAX + 1];
  uint8_t priority;
  bool done;
  uint16_t noteBytes;
};

// Ordre d'affichage : les ouvertes d'abord, par priorité croissante (0 = haute),
// puis l'id pour que le tri soit total — deux tâches de même priorité ne doivent
// jamais changer de place d'un rendu à l'autre.
inline bool taskOrderBefore(const TaskRecord& a, const TaskRecord& b) {
  if (a.done != b.done) return !a.done;
  if (a.priority != b.priority) return a.priority < b.priority;
  return std::strcmp(a.id, b.id) < 0;
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build build/test --target TaskRecordTest && ctest --test-dir build/test -R TaskRecord --output-on-failure
```

Expected: 5 tests pass.

- [ ] **Step 5: Commit**

```bash
clang-format -i src/tasks/TaskRecord.h test/task_record/TaskRecordTest.cpp
git add src/task test/task_record test/CMakeLists.txt
git commit -m "feat(tasks): fixed-width task record and its total display order"
```

---

### Task 2: `TaskSyncReader` — the wire framing

**Files:**
- Create: `src/tasks/TaskSyncReader.h`, `src/tasks/TaskSyncReader.cpp`
- Create: `test/task_sync_reader/TaskSyncReaderTest.cpp`, `test/task_sync_reader/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `TaskRecord` (Task 1), `StreamingJsonParser` (`lib/JsonParser/`).
- Produces:
  ```cpp
  struct TaskSyncCallbacks {
    void* ctx;
    void (*onHeader)(void* ctx, const char* cursor, bool more, bool reset);
    void (*onTask)(void* ctx, const TaskRecord& rec);
    void (*onDeleted)(void* ctx, const char* id);
    void (*onNoteChunk)(void* ctx, const char* id, const char* data, size_t len, bool last);
    // Une op refusee par le serveur, annoncee dans le tableau `rejected` de
    // l'en-tete : `reason` vaut "full", "unknown" ou "badid".
    void (*onRejected)(void* ctx, const char* id, const char* reason);
  };
  class TaskSyncReader {
   public:
    explicit TaskSyncReader(const TaskSyncCallbacks& cb);
    void feed(const char* data, size_t len);
    bool hasError() const;
    bool sawHeader() const;
  };
  ```

**The header carries a `rejected` array.** `{"rejected":[{"id":"d0000abc1","reason":"full"}]}`
is how the server refuses one op without failing the request, and the spec
requires the device to drop that op *and name the task* on the sync summary. The
reader must therefore emit `onRejected` per entry — which means its field sink
cannot stay flat: a sink that tracks only the last key seen would let the nested
`id` and `reason` keys masquerade as header fields. Track object depth, and read
`rejected` entries only at depth 2 inside that array. Add a fixture whose header
carries two rejections and assert both arrive.

**This is the most important task in the plan.** It is the one piece the device
and the server must agree on byte for byte, and the only one whose bugs are
invisible until a real sync corrupts a note. It is written host-first, against
fixtures, before any hardware code exists.

The reader is a line splitter with one twist: after a metadata line whose
`noteBytes > 0`, it switches to **raw byte counting** for exactly that many
bytes, emits them in chunks as they arrive, consumes the following `\n`, and
returns to line mode. Notes therefore never enter `StreamingJsonParser`'s
512-byte token buffer.

- [ ] **Step 1: Write the failing test**

Create `test/task_sync_reader/TaskSyncReaderTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "TaskSyncReader.h"

namespace {

struct Capture {
  std::string cursor;
  bool more = false;
  bool reset = false;
  bool header = false;
  std::vector<TaskRecord> tasks;
  std::vector<std::string> deleted;
  std::vector<std::pair<std::string, std::string>> notes;  // id -> corps accumulé
};

void onHeader(void* ctx, const char* cursor, bool more, bool reset) {
  auto* c = static_cast<Capture*>(ctx);
  c->cursor = cursor; c->more = more; c->reset = reset; c->header = true;
}
void onTask(void* ctx, const TaskRecord& rec) { static_cast<Capture*>(ctx)->tasks.push_back(rec); }
void onDeleted(void* ctx, const char* id) { static_cast<Capture*>(ctx)->deleted.emplace_back(id); }
void onNoteChunk(void* ctx, const char* id, const char* data, size_t len, bool) {
  auto* c = static_cast<Capture*>(ctx);
  if (c->notes.empty() || c->notes.back().first != id) c->notes.emplace_back(id, std::string());
  c->notes.back().second.append(data, len);
}

TaskSyncCallbacks callbacks(Capture& cap) { return {&cap, onHeader, onTask, onDeleted, onNoteChunk}; }

// Alimente le lecteur par tranches de `chunk` octets, pour prouver que le
// cadrage survit à un découpage réseau arbitraire.
void feedInChunks(TaskSyncReader& r, const std::string& body, size_t chunk) {
  for (size_t i = 0; i < body.size(); i += chunk) {
    r.feed(body.data() + i, std::min(chunk, body.size() - i));
  }
}

}  // namespace

TEST(TaskSyncReader, ReadsHeaderAndOneTaskWithoutNote) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MTg\",\"more\":false,\"reset\":false,\"count\":1}\n"
      "{\"id\":\"w17ab93c2\",\"title\":\"Rappeler le notaire\",\"priority\":0,\"done\":false,\"noteBytes\":0}\n";
  r.feed(body.data(), body.size());

  EXPECT_TRUE(cap.header);
  EXPECT_EQ(cap.cursor, "MTg");
  EXPECT_FALSE(cap.more);
  ASSERT_EQ(cap.tasks.size(), 1u);
  EXPECT_STREQ(cap.tasks[0].id, "w17ab93c2");
  EXPECT_STREQ(cap.tasks[0].title, "Rappeler le notaire");
  EXPECT_EQ(cap.tasks[0].priority, 0);
  EXPECT_FALSE(cap.tasks[0].done);
  EXPECT_EQ(cap.tasks[0].noteBytes, 0);
  EXPECT_TRUE(cap.notes.empty());
  EXPECT_FALSE(r.hasError());
}

TEST(TaskSyncReader, ReadsARawNoteBlockAfterItsMetadataLine) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string note = "Curseur + file d'ops.\nDeuxieme ligne.";
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MQ\",\"more\":false,\"reset\":false,\"count\":1}\n"
      "{\"id\":\"w17ab93c2\",\"title\":\"Spec\",\"priority\":1,\"done\":false,\"noteBytes\":" +
      std::to_string(note.size()) + "}\n" + note + "\n";
  r.feed(body.data(), body.size());

  ASSERT_EQ(cap.notes.size(), 1u);
  EXPECT_EQ(cap.notes[0].first, "w17ab93c2");
  // Le \n interne à la note ne doit PAS être pris pour une fin de ligne.
  EXPECT_EQ(cap.notes[0].second, note);
  EXPECT_FALSE(r.hasError());
}

TEST(TaskSyncReader, SurvivesANoteSplitAcrossNetworkChunks) {
  const std::string note(3000, 'n');
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MQ\",\"more\":false,\"reset\":false,\"count\":1}\n"
      "{\"id\":\"d0000abc1\",\"title\":\"Longue\",\"priority\":2,\"done\":false,\"noteBytes\":" +
      std::to_string(note.size()) + "}\n" + note + "\n";

  for (const size_t chunk : {1u, 7u, 64u, 512u, 1500u}) {
    Capture cap;
    TaskSyncReader r(callbacks(cap));
    feedInChunks(r, body, chunk);
    ASSERT_EQ(cap.notes.size(), 1u) << "chunk=" << chunk;
    EXPECT_EQ(cap.notes[0].second, note) << "chunk=" << chunk;
    EXPECT_FALSE(r.hasError()) << "chunk=" << chunk;
  }
}

TEST(TaskSyncReader, ReadsSeveralTasksAndTombstonesInOneBody) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"Mg\",\"more\":true,\"reset\":false,\"count\":3}\n"
      "{\"id\":\"w00000001\",\"title\":\"A\",\"priority\":0,\"done\":false,\"noteBytes\":2}\n"
      "ab\n"
      "{\"id\":\"w00000002\",\"deleted\":true}\n"
      "{\"id\":\"d0000abc1\",\"title\":\"C\",\"priority\":1,\"done\":true,\"noteBytes\":0}\n";
  feedInChunks(r, body, 13);

  EXPECT_TRUE(cap.more);
  ASSERT_EQ(cap.tasks.size(), 2u);
  EXPECT_STREQ(cap.tasks[1].id, "d0000abc1");
  EXPECT_TRUE(cap.tasks[1].done);
  ASSERT_EQ(cap.deleted.size(), 1u);
  EXPECT_EQ(cap.deleted[0], "w00000002");
  ASSERT_EQ(cap.notes.size(), 1u);
  EXPECT_EQ(cap.notes[0].second, "ab");
}

TEST(TaskSyncReader, ReportsTheResetFlag) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body = "{\"schema\":1,\"cursor\":\"MQ\",\"more\":false,\"reset\":true,\"count\":0}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(cap.reset);
}

TEST(TaskSyncReader, RejectsAMalformedIdWithoutEmittingATask) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MQ\",\"more\":false,\"reset\":false,\"count\":1}\n"
      "{\"id\":\"../etc/passwd\",\"title\":\"X\",\"priority\":1,\"done\":false,\"noteBytes\":0}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(cap.tasks.empty());
}

TEST(TaskSyncReader, RejectsANoteLongerThanTheCap) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MQ\",\"more\":false,\"reset\":false,\"count\":1}\n"
      "{\"id\":\"w00000001\",\"title\":\"X\",\"priority\":1,\"done\":false,\"noteBytes\":99999}\n";
  r.feed(body.data(), body.size());
  EXPECT_TRUE(r.hasError());
}

TEST(TaskSyncReader, TruncatesAnOverlongTitleInsteadOfOverflowing) {
  Capture cap;
  TaskSyncReader r(callbacks(cap));
  const std::string title(400, 't');
  const std::string body =
      "{\"schema\":1,\"cursor\":\"MQ\",\"more\":false,\"reset\":false,\"count\":1}\n"
      "{\"id\":\"w00000001\",\"title\":\"" + title + "\",\"priority\":1,\"done\":false,\"noteBytes\":0}\n";
  r.feed(body.data(), body.size());
  ASSERT_EQ(cap.tasks.size(), 1u);
  EXPECT_LE(std::strlen(cap.tasks[0].title), TASK_TITLE_MAX);
}
```

Create `test/task_sync_reader/CMakeLists.txt`:

```cmake
add_executable(TaskSyncReaderTest
  TaskSyncReaderTest.cpp
  ${REPO_ROOT}/src/tasks/TaskSyncReader.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)
target_include_directories(TaskSyncReaderTest PRIVATE
  ${REPO_ROOT}/src/task
  ${REPO_ROOT}/lib/JsonParser
)
target_link_libraries(TaskSyncReaderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TaskSyncReaderTest)
```

Add `add_subdirectory(task_sync_reader)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake -S test -B build/test && cmake --build build/test --target TaskSyncReaderTest
```

Expected: FAIL — `TaskSyncReader.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `src/tasks/TaskSyncReader.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

#include "TaskRecord.h"

struct TaskSyncCallbacks {
  void* ctx;
  void (*onHeader)(void* ctx, const char* cursor, bool more, bool reset);
  void (*onTask)(void* ctx, const TaskRecord& rec);
  void (*onDeleted)(void* ctx, const char* id);
  // Emis par morceaux au fil de l'arrivee reseau ; `last` marque le dernier.
  void (*onNoteChunk)(void* ctx, const char* id, const char* data, size_t len, bool last);
};

// Lit le corps cadre de POST /api/v1/tasks/sync.
//
// Le corps est du NDJSON avec une exception : quand une ligne de metadonnees
// porte noteBytes > 0, exactement ce nombre d'octets BRUTS suit, puis un \n non
// compte. Le lecteur bascule donc entre deux modes — lignes et comptage — ce
// qui permet a une note de contenir des \n et lui evite de passer par le tampon
// de jeton de 512 octets de StreamingJsonParser.
//
// Aucune allocation : la ligne courante tient dans un tampon membre, les octets
// de note sont relayes sans etre stockes.
class TaskSyncReader {
 public:
  // Une ligne de metadonnees vaut au pire ~450 octets (titre echappe + champs).
  static constexpr size_t LINE_BUF_SIZE = 640;

  explicit TaskSyncReader(const TaskSyncCallbacks& callbacks) : cb_(callbacks) {}

  void feed(const char* data, size_t len);
  bool hasError() const { return error_; }
  bool sawHeader() const { return sawHeader_; }

 private:
  enum class Mode : uint8_t { Line, NoteBytes };

  void consumeLine();
  void parseHeaderLine();
  void parseTaskLine();

  TaskSyncCallbacks cb_;
  Mode mode_ = Mode::Line;
  char line_[LINE_BUF_SIZE];
  size_t lineLen_ = 0;
  bool overflow_ = false;
  bool error_ = false;
  bool sawHeader_ = false;

  // Etat de la note en cours de relais.
  char noteId_[TASK_ID_LEN + 1] = {};
  size_t noteRemaining_ = 0;
  bool noteSwallowNewline_ = false;
};
```

- [ ] **Step 4: Write the implementation**

Create `src/tasks/TaskSyncReader.cpp`:

```cpp
#include "TaskSyncReader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "StreamingJsonParser.h"

// Logging.h n'existe que dans la compilation firmware ; le test hote compile le
// meme .cpp sans Arduino, donc le log se reduit a rien la-bas.
#ifdef ARDUINO
#include <Logging.h>
#define TASK_LOG_ERR(fmt, ...) LOG_ERR("TASK", fmt, ##__VA_ARGS__)
#else
#define TASK_LOG_ERR(fmt, ...) ((void)0)
#endif

namespace {

bool isValidTaskId(const char* id) {
  if (id[0] != 'w' && id[0] != 'd') return false;
  for (size_t i = 1; i <= 8; i++) {
    const char c = id[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return id[9] == '\0';
}

// Collecteur de champs plats : une ligne de metadonnees n'a qu'un niveau, donc
// retenir la derniere cle vue suffit — pas de pile, pas d'allocation.
struct FieldSink {
  char key[24] = {};
  char id[TASK_ID_LEN + 1] = {};
  char title[TASK_TITLE_MAX + 1] = {};
  char cursor[48] = {};
  long priority = TASK_PRIORITY_NORMAL;
  long noteBytes = 0;
  bool done = false;
  bool deleted = false;
  bool more = false;
  bool reset = false;
  bool titleTruncated = false;
};

void sinkKey(void* ctx, const char* k, size_t len) {
  auto* f = static_cast<FieldSink*>(ctx);
  const size_t n = len < sizeof(f->key) - 1 ? len : sizeof(f->key) - 1;
  std::memcpy(f->key, k, n);
  f->key[n] = '\0';
}

void sinkString(void* ctx, const char* v, size_t len) {
  auto* f = static_cast<FieldSink*>(ctx);
  if (std::strcmp(f->key, "id") == 0) {
    const size_t n = len < TASK_ID_LEN ? len : TASK_ID_LEN;
    std::memcpy(f->id, v, n);
    f->id[n] = '\0';
  } else if (std::strcmp(f->key, "title") == 0) {
    // Un titre trop long est tronque, jamais deborde : la ligne vient du reseau.
    const size_t n = len < TASK_TITLE_MAX ? len : TASK_TITLE_MAX;
    if (len > TASK_TITLE_MAX) f->titleTruncated = true;
    std::memcpy(f->title, v, n);
    f->title[n] = '\0';
  } else if (std::strcmp(f->key, "cursor") == 0) {
    const size_t n = len < sizeof(f->cursor) - 1 ? len : sizeof(f->cursor) - 1;
    std::memcpy(f->cursor, v, n);
    f->cursor[n] = '\0';
  }
}

void sinkNumber(void* ctx, const char* v, size_t len) {
  auto* f = static_cast<FieldSink*>(ctx);
  char buf[16];
  const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  std::memcpy(buf, v, n);
  buf[n] = '\0';
  if (std::strcmp(f->key, "priority") == 0) f->priority = std::strtol(buf, nullptr, 10);
  else if (std::strcmp(f->key, "noteBytes") == 0) f->noteBytes = std::strtol(buf, nullptr, 10);
}

void sinkBool(void* ctx, bool value) {
  auto* f = static_cast<FieldSink*>(ctx);
  if (std::strcmp(f->key, "done") == 0) f->done = value;
  else if (std::strcmp(f->key, "deleted") == 0) f->deleted = value;
  else if (std::strcmp(f->key, "more") == 0) f->more = value;
  else if (std::strcmp(f->key, "reset") == 0) f->reset = value;
}

void sinkNoop(void*) {}
void sinkNoopNull(void* ctx) { (void)ctx; }

JsonCallbacks fieldCallbacks(FieldSink& sink) {
  return JsonCallbacks{&sink, sinkKey, sinkString, sinkNumber, sinkBool, sinkNoopNull,
                       sinkNoop, sinkNoop, sinkNoop, sinkNoop};
}

bool parseFlatObject(const char* line, size_t len, FieldSink& sink) {
  StreamingJsonParser parser(fieldCallbacks(sink));
  parser.feed(line, len);
  return !parser.hasError();
}

}  // namespace

void TaskSyncReader::feed(const char* data, size_t len) {
  size_t i = 0;
  while (i < len && !error_) {
    if (mode_ == Mode::NoteBytes) {
      if (noteSwallowNewline_) {
        // Le \n qui clot le bloc de note n'est pas compte dans noteBytes.
        if (data[i] == '\n') {
          noteSwallowNewline_ = false;
          mode_ = Mode::Line;
          lineLen_ = 0;
          overflow_ = false;
        }
        i++;
        continue;
      }
      const size_t take = (len - i) < noteRemaining_ ? (len - i) : noteRemaining_;
      noteRemaining_ -= take;
      if (cb_.onNoteChunk != nullptr && take > 0) {
        cb_.onNoteChunk(cb_.ctx, noteId_, data + i, take, noteRemaining_ == 0);
      }
      i += take;
      if (noteRemaining_ == 0) noteSwallowNewline_ = true;
      continue;
    }

    const char c = data[i++];
    if (c == '\n') {
      consumeLine();
      continue;
    }
    if (lineLen_ < LINE_BUF_SIZE - 1) {
      line_[lineLen_++] = c;
    } else {
      overflow_ = true;  // ligne aberrante : on la jettera a la fin de ligne
    }
  }
}

void TaskSyncReader::consumeLine() {
  line_[lineLen_] = '\0';
  const size_t len = lineLen_;
  const bool dropped = overflow_;
  lineLen_ = 0;
  overflow_ = false;

  if (len == 0) return;
  if (dropped) {
    LOG_LINE_DROPPED();
    return;
  }
  if (!sawHeader_) {
    parseHeaderLine();
    return;
  }
  parseTaskLine();
}

void TaskSyncReader::parseHeaderLine() {
  FieldSink sink;
  if (!parseFlatObject(line_, std::strlen(line_), sink)) {
    error_ = true;
    return;
  }
  sawHeader_ = true;
  if (cb_.onHeader != nullptr) cb_.onHeader(cb_.ctx, sink.cursor, sink.more, sink.reset);
}

void TaskSyncReader::parseTaskLine() {
  FieldSink sink;
  if (!parseFlatObject(line_, std::strlen(line_), sink)) {
    // Une ligne illisible est sautee : le curseur n'avancera pas au-dela, donc
    // la prochaine sync la redemandera.
    return;
  }
  if (!isValidTaskId(sink.id)) return;

  if (sink.deleted) {
    if (cb_.onDeleted != nullptr) cb_.onDeleted(cb_.ctx, sink.id);
    return;
  }
  if (sink.noteBytes < 0 || static_cast<size_t>(sink.noteBytes) > TASK_NOTE_MAX) {
    error_ = true;
    return;
  }

  TaskRecord rec{};
  std::snprintf(rec.id, sizeof(rec.id), "%s", sink.id);
  std::snprintf(rec.title, sizeof(rec.title), "%s", sink.title);
  rec.priority = (sink.priority == 0 || sink.priority == 2) ? static_cast<uint8_t>(sink.priority)
                                                            : TASK_PRIORITY_NORMAL;
  rec.done = sink.done;
  rec.noteBytes = static_cast<uint16_t>(sink.noteBytes);
  if (cb_.onTask != nullptr) cb_.onTask(cb_.ctx, rec);

  if (rec.noteBytes > 0) {
    std::snprintf(noteId_, sizeof(noteId_), "%s", rec.id);
    noteRemaining_ = rec.noteBytes;
    noteSwallowNewline_ = false;
    mode_ = Mode::NoteBytes;
  }
}
```

Replace the `LOG_LINE_DROPPED();` placeholder with a real log. At the top of the
file add `#include <Logging.h>` **only when building for the device**; for the
host test, define a no-op. Use this exact shim just below the includes:

```cpp
#ifdef ARDUINO
#include <Logging.h>
#define TASK_LOG_ERR(fmt, ...) LOG_ERR("TASK", fmt, ##__VA_ARGS__)
#else
#define TASK_LOG_ERR(fmt, ...) ((void)0)
#endif
```

and write `TASK_LOG_ERR("sync: overlong line dropped");` in place of `LOG_LINE_DROPPED();`.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build/test --target TaskSyncReaderTest && ctest --test-dir build/test -R TaskSyncReader --output-on-failure
```

Expected: 8 tests pass, including every chunk size in the split-note test.

- [ ] **Step 6: Commit**

```bash
clang-format -i src/tasks/TaskSyncReader.h src/tasks/TaskSyncReader.cpp test/task_sync_reader/TaskSyncReaderTest.cpp
git add src/task test/task_sync_reader test/CMakeLists.txt
git commit -m "feat(tasks): streaming reader for the sync framing, host-tested"
```

---

### Task 3: `TaskOpQueue` — pending local writes

**Files:**
- Create: `src/tasks/TaskOpQueue.h`, `src/tasks/TaskOpQueue.cpp`
- Create: `test/task_op_queue/TaskOpQueueTest.cpp`, `test/task_op_queue/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `TaskRecord` (Task 1).
- Produces:
  ```cpp
  enum class TaskOpKind : uint8_t { Add, Done, Prio, Title };
  struct TaskOp { TaskOpKind kind; char id[10]; char title[201]; uint8_t priority; bool done; };
  size_t taskOpToLine(const TaskOp& op, char* out, size_t outSize);   // octets ecrits, 0 si trop court
  bool taskOpFromLine(const char* line, size_t len, TaskOp& out);
  size_t taskOpsToRequestBody(const TaskOp* ops, size_t count, const char* cursor, char* out, size_t outSize);
  ```

The queue is stored as one op per line so appending costs one `write` with no
read-modify-write of the whole file — the C3 must never rewrite a file to record
a tick.

- [ ] **Step 1: Write the failing test**

Create `test/task_op_queue/TaskOpQueueTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "TaskOpQueue.h"

namespace {
TaskOp addOp(const char* id, const char* title, uint8_t priority) {
  TaskOp op{};
  op.kind = TaskOpKind::Add;
  std::snprintf(op.id, sizeof(op.id), "%s", id);
  std::snprintf(op.title, sizeof(op.title), "%s", title);
  op.priority = priority;
  return op;
}
}  // namespace

TEST(TaskOpQueue, AddRoundTrips) {
  char line[512];
  const TaskOp op = addOp("d0000abc1", "Racheter du cafe", 0);
  const size_t n = taskOpToLine(op, line, sizeof(line));
  ASSERT_GT(n, 0u);

  TaskOp back{};
  ASSERT_TRUE(taskOpFromLine(line, n, back));
  EXPECT_EQ(back.kind, TaskOpKind::Add);
  EXPECT_STREQ(back.id, "d0000abc1");
  EXPECT_STREQ(back.title, "Racheter du cafe");
  EXPECT_EQ(back.priority, 0);
}

TEST(TaskOpQueue, DoneAndPrioRoundTrip) {
  char line[512];
  TaskOp done{};
  done.kind = TaskOpKind::Done;
  std::snprintf(done.id, sizeof(done.id), "w17ab93c2");
  done.done = true;
  const size_t n = taskOpToLine(done, line, sizeof(line));
  TaskOp back{};
  ASSERT_TRUE(taskOpFromLine(line, n, back));
  EXPECT_EQ(back.kind, TaskOpKind::Done);
  EXPECT_TRUE(back.done);
}

TEST(TaskOpQueue, EscapesQuotesAndBackslashesInTitles) {
  char line[512];
  const TaskOp op = addOp("d0000abc1", "Lire \"Dune\" et C:\\temp", 1);
  const size_t n = taskOpToLine(op, line, sizeof(line));
  ASSERT_GT(n, 0u);
  TaskOp back{};
  ASSERT_TRUE(taskOpFromLine(line, n, back));
  EXPECT_STREQ(back.title, "Lire \"Dune\" et C:\\temp");
}

TEST(TaskOpQueue, RefusesABufferTooSmall) {
  char tiny[8];
  EXPECT_EQ(taskOpToLine(addOp("d0000abc1", "x", 1), tiny, sizeof(tiny)), 0u);
}

TEST(TaskOpQueue, RejectsAGarbageLine) {
  TaskOp back{};
  const char* junk = "pas du json";
  EXPECT_FALSE(taskOpFromLine(junk, std::strlen(junk), back));
}

TEST(TaskOpQueue, BuildsARequestBodyWithCursorAndOps) {
  const TaskOp ops[] = {addOp("d0000abc1", "Cafe", 0)};
  char body[1024];
  const size_t n = taskOpsToRequestBody(ops, 1, "MTg=", body, sizeof(body));
  ASSERT_GT(n, 0u);
  const std::string s(body, n);
  EXPECT_NE(s.find("\"cursor\":\"MTg\""), std::string::npos);
  EXPECT_NE(s.find("\"op\":\"add\""), std::string::npos);
  EXPECT_NE(s.find("\"id\":\"d0000abc1\""), std::string::npos);
  EXPECT_EQ(s.front(), '{');
  EXPECT_EQ(s.back(), '}');
}

TEST(TaskOpQueue, BuildsAnEmptyOpsBodyWhenNothingIsPending) {
  char body[256];
  const size_t n = taskOpsToRequestBody(nullptr, 0, "", body, sizeof(body));
  const std::string s(body, n);
  EXPECT_NE(s.find("\"ops\":[]"), std::string::npos);
}

TEST(TaskOpQueue, StopsBeforeOverflowingTheRequestBuffer) {
  TaskOp many[60];
  for (auto& op : many) op = addOp("d0000abc1", "titre assez long pour peser", 1);
  char body[256];
  EXPECT_EQ(taskOpsToRequestBody(many, 60, "", body, sizeof(body)), 0u);
}
```

Create `test/task_op_queue/CMakeLists.txt`:

```cmake
add_executable(TaskOpQueueTest
  TaskOpQueueTest.cpp
  ${REPO_ROOT}/src/tasks/TaskOpQueue.cpp
)
target_include_directories(TaskOpQueueTest PRIVATE ${REPO_ROOT}/src/task ${REPO_ROOT}/lib/JsonParser)
target_link_libraries(TaskOpQueueTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TaskOpQueueTest)
```

Add `add_subdirectory(task_op_queue)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake -S test -B build/test && cmake --build build/test --target TaskOpQueueTest
```

Expected: FAIL — `TaskOpQueue.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `src/tasks/TaskOpQueue.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

#include "TaskRecord.h"

enum class TaskOpKind : uint8_t { Add, Done, Prio, Title };

struct TaskOp {
  TaskOpKind kind;
  char id[TASK_ID_LEN + 1];
  char title[TASK_TITLE_MAX + 1];
  uint8_t priority;
  bool done;
};

// Limite de lot : le serveur refuse au-dela de 50 ops, et le corps doit tenir
// dans la limite de 16 Ko de Fastify.
inline constexpr size_t TASK_MAX_OPS_PER_SYNC = 50;
inline constexpr size_t TASK_REQUEST_BUF_SIZE = 12288;

// Serialise une op en une ligne JSON (sans \n final). Rend 0 si `out` est trop
// petit — l'appelant ne doit alors rien ecrire.
size_t taskOpToLine(const TaskOp& op, char* out, size_t outSize);

bool taskOpFromLine(const char* line, size_t len, TaskOp& out);

// Assemble {"schema":1,"cursor":"…","ops":[…]}. Rend 0 si tout ne tient pas.
size_t taskOpsToRequestBody(const TaskOp* ops, size_t count, const char* cursor, char* out, size_t outSize);
```

Create `src/tasks/TaskOpQueue.cpp`:

```cpp
#include "TaskOpQueue.h"

#include <cstdio>
#include <cstring>

#include "StreamingJsonParser.h"

namespace {

const char* kindName(const TaskOpKind kind) {
  switch (kind) {
    case TaskOpKind::Add: return "add";
    case TaskOpKind::Done: return "done";
    case TaskOpKind::Prio: return "prio";
    case TaskOpKind::Title: return "title";
  }
  return "add";
}

bool kindFromName(const char* name, TaskOpKind& out) {
  if (std::strcmp(name, "add") == 0) { out = TaskOpKind::Add; return true; }
  if (std::strcmp(name, "done") == 0) { out = TaskOpKind::Done; return true; }
  if (std::strcmp(name, "prio") == 0) { out = TaskOpKind::Prio; return true; }
  if (std::strcmp(name, "title") == 0) { out = TaskOpKind::Title; return true; }
  return false;
}

// Echappe pour du JSON : guillemets, antislashs et caracteres de controle.
// Rend false si la sortie ne tient pas, sans rien ecrire d'utilisable.
bool appendEscaped(const char* src, char* out, size_t outSize, size_t& pos) {
  for (const char* p = src; *p != '\0'; p++) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      if (pos + 2 >= outSize) return false;
      out[pos++] = '\\';
      out[pos++] = static_cast<char>(c);
    } else if (c < 0x20) {
      if (pos + 6 >= outSize) return false;
      pos += static_cast<size_t>(std::snprintf(out + pos, outSize - pos, "\\u%04x", c));
    } else {
      if (pos + 1 >= outSize) return false;
      out[pos++] = static_cast<char>(c);
    }
  }
  return true;
}

struct OpSink {
  char key[16] = {};
  char op[12] = {};
  char id[TASK_ID_LEN + 1] = {};
  char title[TASK_TITLE_MAX + 1] = {};
  long priority = TASK_PRIORITY_NORMAL;
  bool done = false;
  bool sawOp = false;
};

void opKey(void* ctx, const char* k, size_t len) {
  auto* s = static_cast<OpSink*>(ctx);
  const size_t n = len < sizeof(s->key) - 1 ? len : sizeof(s->key) - 1;
  std::memcpy(s->key, k, n);
  s->key[n] = '\0';
}
void opString(void* ctx, const char* v, size_t len) {
  auto* s = static_cast<OpSink*>(ctx);
  if (std::strcmp(s->key, "op") == 0) {
    const size_t n = len < sizeof(s->op) - 1 ? len : sizeof(s->op) - 1;
    std::memcpy(s->op, v, n); s->op[n] = '\0'; s->sawOp = true;
  } else if (std::strcmp(s->key, "id") == 0) {
    const size_t n = len < TASK_ID_LEN ? len : TASK_ID_LEN;
    std::memcpy(s->id, v, n); s->id[n] = '\0';
  } else if (std::strcmp(s->key, "title") == 0) {
    const size_t n = len < TASK_TITLE_MAX ? len : TASK_TITLE_MAX;
    std::memcpy(s->title, v, n); s->title[n] = '\0';
  }
}
void opNumber(void* ctx, const char* v, size_t len) {
  auto* s = static_cast<OpSink*>(ctx);
  char buf[16];
  const size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  std::memcpy(buf, v, n); buf[n] = '\0';
  if (std::strcmp(s->key, "priority") == 0) s->priority = std::strtol(buf, nullptr, 10);
}
void opBool(void* ctx, bool value) {
  auto* s = static_cast<OpSink*>(ctx);
  if (std::strcmp(s->key, "done") == 0) s->done = value;
}
void opNoop(void*) {}
void opNoopNull(void* ctx) { (void)ctx; }

}  // namespace

size_t taskOpToLine(const TaskOp& op, char* out, size_t outSize) {
  size_t pos = 0;
  const int head = std::snprintf(out, outSize, "{\"op\":\"%s\",\"id\":\"%s\"", kindName(op.kind), op.id);
  if (head < 0 || static_cast<size_t>(head) >= outSize) return 0;
  pos = static_cast<size_t>(head);

  if (op.kind == TaskOpKind::Add || op.kind == TaskOpKind::Title) {
    const int lead = std::snprintf(out + pos, outSize - pos, ",\"title\":\"");
    if (lead < 0 || pos + static_cast<size_t>(lead) >= outSize) return 0;
    pos += static_cast<size_t>(lead);
    if (!appendEscaped(op.title, out, outSize, pos)) return 0;
    if (pos + 1 >= outSize) return 0;
    out[pos++] = '"';
  }
  if (op.kind == TaskOpKind::Add || op.kind == TaskOpKind::Prio) {
    const int p = std::snprintf(out + pos, outSize - pos, ",\"priority\":%u", op.priority);
    if (p < 0 || pos + static_cast<size_t>(p) >= outSize) return 0;
    pos += static_cast<size_t>(p);
  }
  if (op.kind == TaskOpKind::Done) {
    const int d = std::snprintf(out + pos, outSize - pos, ",\"done\":%s", op.done ? "true" : "false");
    if (d < 0 || pos + static_cast<size_t>(d) >= outSize) return 0;
    pos += static_cast<size_t>(d);
  }
  if (pos + 2 > outSize) return 0;
  out[pos++] = '}';
  out[pos] = '\0';
  return pos;
}

bool taskOpFromLine(const char* line, const size_t len, TaskOp& out) {
  OpSink sink;
  const JsonCallbacks cb{&sink, opKey, opString, opNumber, opBool, opNoopNull, opNoop, opNoop, opNoop, opNoop};
  StreamingJsonParser parser(cb);
  parser.feed(line, len);
  if (parser.hasError() || !sink.sawOp) return false;

  TaskOpKind kind{};
  if (!kindFromName(sink.op, kind)) return false;
  out = TaskOp{};
  out.kind = kind;
  std::snprintf(out.id, sizeof(out.id), "%s", sink.id);
  std::snprintf(out.title, sizeof(out.title), "%s", sink.title);
  out.priority = (sink.priority == 0 || sink.priority == 2) ? static_cast<uint8_t>(sink.priority)
                                                            : TASK_PRIORITY_NORMAL;
  out.done = sink.done;
  return true;
}

size_t taskOpsToRequestBody(const TaskOp* ops, const size_t count, const char* cursor, char* out,
                            const size_t outSize) {
  const int head = std::snprintf(out, outSize, "{\"schema\":1,\"cursor\":\"%s\",\"ops\":[", cursor);
  if (head < 0 || static_cast<size_t>(head) >= outSize) return 0;
  size_t pos = static_cast<size_t>(head);

  const size_t n = count < TASK_MAX_OPS_PER_SYNC ? count : TASK_MAX_OPS_PER_SYNC;
  for (size_t i = 0; i < n; i++) {
    if (i > 0) {
      if (pos + 1 >= outSize) return 0;
      out[pos++] = ',';
    }
    const size_t written = taskOpToLine(ops[i], out + pos, outSize - pos);
    if (written == 0) return 0;
    pos += written;
  }
  if (pos + 3 > outSize) return 0;
  out[pos++] = ']';
  out[pos++] = '}';
  out[pos] = '\0';
  return pos;
}
```

Add `#include <cstdlib>` to the implementation for `strtol`.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build/test --target TaskOpQueueTest && ctest --test-dir build/test -R TaskOpQueue --output-on-failure
```

Expected: 8 tests pass.

- [ ] **Step 5: Commit**

```bash
clang-format -i src/tasks/TaskOpQueue.h src/tasks/TaskOpQueue.cpp test/task_op_queue/TaskOpQueueTest.cpp
git add src/task test/task_op_queue test/CMakeLists.txt
git commit -m "feat(tasks): pending-op queue serialization, host-tested"
```

---

### Task 4: `TaskStore` — the SD index, notes and secret

**Files:**
- Create: `src/TaskStore.h`, `src/TaskStore.cpp`
- Modify: `src/main.cpp` (one `loadFromFile()` call at boot, beside the other stores)

**Interfaces:**
- Consumes: `TaskRecord` (Task 1), `TaskOp` (Task 3), `PersistableStore`, `ObfuscationUtils`, `HalStorage`.
- Produces: singleton `TaskStore` (via `PersistableStore<TaskStore>`) with
  `bool load(std::vector<TaskRecord>& out)`, `void replaceAll(const std::vector<TaskRecord>&)`,
  `void upsert(const TaskRecord&)`, `void remove(const char* id)`,
  `bool appendOp(const TaskOp&)`, `size_t readOps(TaskOp* out, size_t max)`, `void clearOps()`,
  `bool readCursor(char* out, size_t size)`, `void writeCursor(const char* cursor)`,
  `static bool isSafeCursor(const char* c)`,
  `bool notePath(const char* id, char* out, size_t size)`, `void clearAllNotes()`,
  `bool hasSecret()`, `bool readSecret(char* out, size_t size)`, `void writeSecret(const char* secret)`,
  `void clearSecret()`, `std::string newDeviceId()`.

**Paths, fixed by the spec:** `/.crosspoint/tasks/index.json`,
`/.crosspoint/tasks/n/<id>.txt`, `/.crosspoint/tasks/ops.ndjson`,
`/.crosspoint/tasks/cursor.txt`.

- [ ] **Step 1: Write the header**

Create `src/TaskStore.h`:

```cpp
#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

#include "task/TaskOpQueue.h"
#include "task/TaskRecord.h"

/**
 * Index des taches sur la carte SD, plus la file d'ops en attente, le curseur
 * de sync et le secret d'appairage.
 *
 * Les notes NE SONT PAS dans cet index : chacune vit dans son propre fichier
 * sous /.crosspoint/tasks/n/, ecrit pendant la sync et lu a la demande par
 * l'ecran de detail. C'est ce qui garde l'empreinte RAM independante du volume
 * de texte, comme le fait deja /Articles pour les .md.
 *
 * Le secret est obfusque avec l'adresse MAC materielle puis encode en base64,
 * exactement comme OpdsServerStore traite les mots de passe.
 */
class TaskStore : public PersistableStore<TaskStore> {
 private:
  std::vector<TaskRecord> records;
  std::string secretObfuscated;
  bool loaded_ = false;

  TaskStore() = default;
  friend class PersistableStore<TaskStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/tasks/index.json"; }
  static const char* opsPath() { return "/.crosspoint/tasks/ops.ndjson"; }
  static const char* cursorPath() { return "/.crosspoint/tasks/cursor.txt"; }
  static const char* notesDir() { return "/.crosspoint/tasks/n"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::vector<TaskRecord>& all() const { return records; }
  void replaceAll(std::vector<TaskRecord> next);
  void upsert(const TaskRecord& rec);
  void remove(const char* id);
  const TaskRecord* find(const char* id) const;

  bool appendOp(const TaskOp& op);
  size_t readOps(TaskOp* out, size_t max) const;
  void clearOps();

  bool readCursor(char* out, size_t size) const;
  void writeCursor(const char* cursor);

  static bool notePath(const char* id, char* out, size_t size);
  void clearAllNotes() const;

  bool hasSecret() const { return !secretObfuscated.empty(); }
  bool readSecret(char* out, size_t size) const;
  void writeSecret(const char* secret);
  void clearSecret();

  // 'd' + 8 hex tires du generateur materiel : pas de table de correspondance
  // cote serveur, et rejouer un ajout apres coupure est sans effet.
  static std::string newDeviceId();
};
```

- [ ] **Step 2: Write the implementation**

Create `src/TaskStore.cpp` following `src/OpdsServerStore.cpp` exactly for the
`toJson` / `fromJson` shape and the obfuscation calls. The JSON shape is:

```json
{"schema":1,"secret":"<base64 obfusque>","tasks":[{"id":"w17ab93c2","t":"Titre","p":0,"d":false,"n":124}]}
```

Key points the implementation must honour, each for a stated reason:

- `fromJson` **clamps** `records` to `MAX_TASKS` and skips any entry whose id
  fails the `^[wd][0-9a-f]{8}$` check — the file can be hand-edited on a PC.
- `notePath` builds `/.crosspoint/tasks/n/<id>.txt` **only after** validating the
  id with the same check, so no remote value ever reaches a path.
- `appendOp` opens the ops file with `FILE_WRITE` in append mode, writes one
  line plus `\n`, and closes immediately — a tick must cost one small write, not
  a rewrite of the queue.
- `readOps` reads line by line into the caller's array, stopping at `max`.
- **`readCursor` validates before returning.** The cursor file sits on an SD card
  the user can edit on a PC, and its contents go straight into a request body, so
  it is remote-grade input. Accept only the base64url alphabet
  (`A-Z a-z 0-9 - _`), non-empty, at most 32 characters — the same check
  `RaindropSyncActivity.cpp:46-56` already applies. Anything else is treated as
  **absent**, which costs one full snapshot and cannot corrupt anything. Size the
  caller's buffer at `TASK_CURSOR_BUF = 40`: the server's measured worst case is
  22 characters and the spec's limit is 32.
- **Never parse the cursor.** It is opaque by contract: store the bytes, hand them
  back. The server's encoding has already changed once — it now carries a
  snapshot-mode marker — and a device that tried to read a sequence number out of
  it would break on the next such change.
- Every `FsFile` is closed on every path, including the error paths.
- `newDeviceId` uses `esp_random()` under `#ifndef SIMULATOR` and `rand()` in
  the simulator, formatted with `snprintf(buf, sizeof buf, "d%08x", value)`.

In `src/main.cpp`, beside the existing store loads, add:

```cpp
  TaskStore::getInstance().loadFromFile();
```

- [ ] **Step 3: Verify it builds for both targets**

```bash
pio run -e default
```

Expected: both succeed. `pio run -e default` must not grow the binary by more
than ~8 KB at this point; note the reported flash figure for comparison later.

- [ ] **Step 4: Commit**

```bash
clang-format -i src/TaskStore.h src/TaskStore.cpp
git add src/TaskStore.h src/TaskStore.cpp src/main.cpp
git commit -m "feat(tasks): SD store for the task index, op queue, cursor and secret"
```

---

### Task 5: `TaskListActivity` — the main screen

**Files:**
- Create: `src/activities/tasks/TaskListActivity.h`, `.cpp`
- Modify: `src/activities/ActivityManager.h`, `.cpp` (one navigation factory)
- Modify: `src/activities/home/HomeActivity.cpp` (one menu entry)

**Interfaces:**
- Consumes: `TaskStore` (Task 4), `TaskRecord` / `taskOrderBefore` (Task 1), `UITheme`, `MappedInputManager`.
- Produces: `class TaskListActivity : public UiListActivity` and
  `ActivityManager::navigateToTaskList()`.

**Derive from `src/activities/UiListActivity`, do not rebuild list mechanics.**
`.claude/CONTEXT.md:58-59` requires list screens to share the FreeInkUI list
configuration rather than compute their own, and roughly forty activities already
derive from this base. It owns the viewport sync, the selection (`fui::ListNav`),
button navigation with page jumps on hold, swipe scrolling that moves the viewport
without the selection, touch routing and the chrome/app/footer render skeleton.
The subclass supplies `listCount()`, `buildScreen()` and `activateIndex()`.
A bespoke row-fitting or scroll calculation duplicates `syncListViewport` and will
drift from the forty screens that share the real one.

What IS worth extracting and host-testing is the CrossTasks-specific filter, which
is not geometry: which rows are shown given the toggle state and the set ticked
during this visit. Its edge cases — ticking the last open task, unticking one back,
the toggle being on when the last completed task is unticked — need no screen.

**Design rules, from the approved mockups — these are requirements, not taste:**

- One uniform body size for every title. **Priority is carried by weight only**, in
  **TWO tiers, not three**: `ListItem::emphasis = true` (bold) when
  `priority == TASK_PRIORITY_HIGH`, plain otherwise. No badge, no pip, no rule between
  rows. `EpdFontFamily::Style` (lib/EpdFont/EpdFontFamily.h:8-20) has only
  REGULAR / BOLD / ITALIC / BOLD_ITALIC — there is no Light, so the mockups' three-weight
  scale is not implementable. Do not substitute ITALIC for the low tier: on a 1-bit panel
  italic attracts the eye rather than quieting it, inverting the intent, and the sort
  already puts low-priority tasks at the bottom. Mark the exception, not the rule.
- Circular bullets. Selected row is drawn inverted (`fillRect` then white text).
- Completed tasks are **hidden by default**; `btn3` shows `☑ n` and toggles them
  into a section at the bottom of the same screen.
- A task ticked during this visit **stays in place, dimmed** (`fui::StateDisabled`), until
  `onExit()` — an accidental tick must be undoable without opening the toggle. Dimmed and
  not struck through: `fui::list()` reports only aggregate `effectiveTop`/`drawnRows`, never
  a per-row Y, so overlaying a strike would mean re-deriving the row geometry that
  `UiListActivity` exists to own.
- Action bar: `MappedInputManager::mapLabels(back, confirm, previous, next)`
  (src/MappedInputManager.h:234) binds its four arguments to the physical
  **Back / Confirm / Left / Right** — it is not a free four-slot bar, and Up/Down are not in
  it at all. Four actions is all there is: **Back = quitter, Confirm = cocher,
  Left = ☑ n (bascule terminées), Right = ▸ détail**. Sync is NOT on this screen; Task 10
  puts it in Settings as `SettingAction::TaskSync`.

- [ ] **Step 1: Write the header**

```cpp
#pragma once
#include <vector>

#include "activities/Activity.h"
#include "task/TaskRecord.h"

// Ecran principal des taches. Charge l'index en onEnter(), le rend en onExit() :
// rien ne reste resident quand on lit un livre.
class TaskListActivity : public Activity {
 public:
  TaskListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TaskList", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void toggleSelected();
  void openDetail();
  void startSync();
  void rebuildOrder();
  int visibleCount() const;

  std::vector<TaskRecord> records;
  // Ids coches pendant cette visite : ils restent en place, barres, pour qu'un
  // decochage apres erreur ne demande pas d'ouvrir la bascule.
  std::vector<std::string> tickedHere;
  int selected = 0;
  int scrollTop = 0;
  bool showDone = false;
  bool dirty = true;
};
```

- [ ] **Step 2: Implement rendering against the theme**

The `.cpp` must:

- take the safe area from `renderer.getOrientedViewableTRBL()` and derive every
  y from `UITheme::getInstance().getMetrics()`; **no literal 480 or 800**;
- draw the header as `tr(STR_TASK_TITLE)` plus the open count, and the sync
  freshness on the right;
- iterate visible rows, choosing the font style from `priority`;
- draw the bullet with `drawCircle` / `fillCircle` from `GfxRenderer`;
- call `mappedInput.setLabels({tr(STR_TASK_TICK), tr(STR_TASK_DETAIL), doneLabel, tr(STR_TASK_SYNC)})`.

`loop()` routes `Button::Up` / `Down` to move the selection, `Confirm` to
`toggleSelected()`, `Right` to `openDetail()`, and `Back` to finish.

- [ ] **Step 3: Verify on the device build and the host suite**

The simulator does not build on this branch (see Global Constraints) — there is no
verification by eye here.

```bash
pio run -e default
cd test/build && cmake .. && cmake --build . -j8 && ctest
```

Expect SUCCESS and 2 pre-existing `SectionPersistenceTest` failures, nothing more. The
ordering and row-building rules are pinned by the host tests in `test/task_list_model/`
instead; anything you cannot pin there, name in your report as unverified.

- [ ] **Step 4: Commit**

```bash
clang-format -i src/activities/tasks/TaskListActivity.h src/activities/tasks/TaskListActivity.cpp
git add src/activities/tasks src/activities/ActivityManager.* src/activities/home/HomeActivity.cpp
git commit -m "feat(tasks): Tasks list screen with weight-carried priority"
```

---

### Task 6: `TaskDetailActivity` — note, pomodoro, edit

**Files:**
- Create: `src/activities/tasks/TaskDetailActivity.h`, `.cpp`
- Modify: `src/activities/util/PomodoroActivity.h`, `.cpp` (one constructor parameter)

**Interfaces:**
- Consumes: `TaskStore`, `PomodoroActivity`, `KeyboardEntryActivity`, `OptionSelectionActivity`.
- Produces: `class TaskDetailActivity : public Activity`, constructed with the task id;
  `PomodoroActivity(renderer, mappedInput, const char* contextTitle = nullptr)`.

**Design rules, from the approved mockups:**

- Title in LexendDeca 16, **`EpdFontFamily::BOLD` when `priority == TASK_PRIORITY_HIGH`,
  `REGULAR` otherwise**. **No priority label** — the weight already says it.
  TWO tiers, not three: `EpdFontFamily::Style` (lib/EpdFont/EpdFontFamily.h:8-20) offers
  only REGULAR / BOLD / ITALIC / BOLD_ITALIC — there is no Light, so the mockups'
  three-weight scale is not implementable. This matches `ListItem::emphasis` on the list
  screen, which also marks only the exception. Do not substitute ITALIC for the low tier:
  on a 1-bit panel italic attracts the eye rather than quieting it, which inverts the
  intent, and the sort already puts low-priority tasks at the bottom.
- Note in **LexendDeca 10 `REGULAR`** inside a bounded region, paginated. Read from
  `/.crosspoint/tasks/n/<id>.txt` **in chunks**; never load 4 KB into a `std::string`.
- A framed Pomodoro block anchored above the action bar, showing the duration
  that will start and the position in the series.
- Header shows `n / total`.
- Action bar: `MappedInputManager::mapLabels(back, confirm, previous, next)`
  (src/MappedInputManager.h:234) binds its four arguments to the physical
  **Back / Confirm / Left / Right**, in that order — it is not a free four-slot bar, and
  Up/Down are not in it at all. So: **Back = ◂ retour, Confirm = OK/cocher,
  Left = ≡ modifier, Right = ▸ pomodoro**, and **Up/Down page the note**, which is why
  the note's pagination must not claim Left/Right. Call it exactly as
  `TaskListActivity::render()` does.

- [ ] **Step 1: Add the pomodoro context parameter**

In `PomodoroActivity.h`, change the constructor to take an optional title and
store it as a `std::string contextTitle`. In `render()`, when it is non-empty,
draw it above the ring in LexendDeca 12 Regular. Nothing else about the pomodoro
changes — no counter, no elapsed time, nothing synced.

- [ ] **Step 2: Implement the detail screen**

`onEnter()` reads the record from `TaskStore::find(id)` and opens the note file;
`onExit()` closes it. Paginate by measuring with the font's `advanceY` (26 px for
LexendDeca 10) against the bounded region height, and keep only the current
page's bytes in a `char[]` buffer sized from that region — not the whole note.

`≡ modifier` pushes `KeyboardEntryActivity` for the title, then
`OptionSelectionActivity` for the priority, and on return writes a `Title` /
`Prio` op through `TaskStore::appendOp`.

- [ ] **Step 3: Verify — on the device build, NOT the simulator**

`pio run -e simulator` is **broken on this branch**, pre-existing and unrelated to
CrossTasks: `src/SettingsList.h:1039` calls `HalGPIO::supportsMultiTouch()`, which
`lib/hal` provides but the simulator ignores (`lib_ignore = hal`), taking its copy from
the vendored `uxjulia/crossink-simulator` package where the method does not exist. Do not
try to fix it and do not report it as a failure of this task.

```bash
pio run -e default
cd test/build && cmake .. && cmake --build . -j8 && ctest
```

Expect SUCCESS and 2 pre-existing `SectionPersistenceTest` failures, nothing more.

Since there is no visual verification available, the file-handle discipline has to be read
rather than run. On hardware SdFat permits **one open handle per path at a time** (AGENTS.md,
"Hardware Constraints"), while the simulator's POSIX backend allows several — so a leaked
handle passes every simulator run and fails only on the device, as an open that returns
false for no visible reason. Before committing, walk every `return` in the note-reading
path, **including the error returns**, and confirm the note file is closed on each one.
State in the report that you did this and name the paths you checked.

- [ ] **Step 4: Commit**

```bash
clang-format -i src/activities/tasks/TaskDetailActivity.* src/activities/util/PomodoroActivity.*
git add src/activities/tasks src/activities/util/PomodoroActivity.*
git commit -m "feat(tasks): task detail with a paginated note and an anchored pomodoro"
```

---

### Task 7: Adding a task on the device

**Files:**
- Modify: `src/activities/tasks/TaskListActivity.cpp`
- Modify: `src/SettingsList.h`, `src/activities/settings/SettingsActivity.h` (one `SettingAction`)

**Interfaces:**
- Consumes: `KeyboardEntryActivity`, `OptionSelectionActivity`, `TaskStore::newDeviceId`, `TaskStore::appendOp`.

- [ ] **Step 1: Chain the two existing screens**

From the list, a long press on `Confirm` (or the Home menu's *New task*) pushes
`KeyboardEntryActivity` with `tr(STR_TASK_NEW)` and `maxLength = TASK_TITLE_MAX`.
On a non-cancelled result, push `OptionSelectionActivity` with the three
priorities. On its result:

```cpp
  TaskRecord rec{};
  const std::string id = TaskStore::newDeviceId();
  std::snprintf(rec.id, sizeof(rec.id), "%s", id.c_str());
  std::snprintf(rec.title, sizeof(rec.title), "%s", title.c_str());
  rec.priority = static_cast<uint8_t>(chosenPriority);
  rec.done = false;
  rec.noteBytes = 0;

  TaskOp op{};
  op.kind = TaskOpKind::Add;
  std::memcpy(op.id, rec.id, sizeof(op.id));
  std::memcpy(op.title, rec.title, sizeof(op.title));
  op.priority = rec.priority;

  if (!TaskStore::getInstance().appendOp(op)) {
    LOG_ERR("TASK", "could not queue the add op; task not created");
    return;
  }
  TaskStore::getInstance().upsert(rec);   // visible tout de suite, avant toute sync
  TaskStore::getInstance().saveToFile();
```

The order matters: the op is queued **first**, so a power loss between the two
writes loses the local display but not the user's intent.

- [ ] **Step 2: Verify on the device build**

The simulator does not build on this branch (see Global Constraints), so this cannot be
checked by eye or against `fs_/`. Run `pio run -e default` and the host suite, then READ
the added path and confirm in your report: exactly one `add` op is appended per accepted
entry, a cancelled `KeyboardEntryActivity` or `OptionSelectionActivity` result appends
nothing at all, and the op is queued before the local record is written — so a power loss
between the two loses the display, not the user's intent.

- [ ] **Step 3: Commit**

```bash
clang-format -i src/activities/tasks/TaskListActivity.cpp
git add src/activities/tasks src/SettingsList.h src/activities/settings/SettingsActivity.h
git commit -m "feat(tasks): create a task on the device from the existing keyboard"
```

---

### Task 8: Pairing screen

**Files:**
- Create: `src/activities/tasks/TaskPairActivity.h`, `.cpp`
- Modify: `src/SettingsList.h` (one action row)

**Interfaces:**
- Consumes: `QrDisplayActivity` (`src/activities/reader/QrDisplayActivity.h`), `TaskStore`.

- [ ] **Step 1: Generate and show the secret**

`onEnter()` draws 16 bytes from `esp_random()`, encodes them as 26 Crockford
base32 characters (alphabet `0123456789ABCDEFGHJKMNPQRSTVWXYZ`, no I, L, O or U),
writes them through `TaskStore::writeSecret`, and renders the QR plus the code
grouped in fours below it.

`≡ régénérer` draws a new secret and invalidates the old one — the user must
re-pair on the web.

- [ ] **Step 2: Verify on hardware**

Flash, open Settings → Tasks → Pair, and scan with a phone. The scanned string
must be exactly the 26 characters shown, with no prefix or URL wrapper.

- [ ] **Step 3: Commit**

```bash
clang-format -i src/activities/tasks/TaskPairActivity.*
git add src/activities/tasks src/SettingsList.h
git commit -m "feat(tasks): pair by showing a device-generated secret as a QR"
```

---

### Task 9: `TaskSyncActivity` — the network boot target

**Files:**
- Create: `src/activities/tasks/TaskSyncActivity.h`, `.cpp`
- Modify: `src/SilentRestart.h` (add `TASK_SYNC = 9` and its `static_assert` term)
- Modify: `src/main.cpp` (route the boot target)

**Interfaces:**
- Consumes: `TaskSyncReader` (Task 2), `TaskOpQueue` (Task 3), `TaskStore` (Task 4),
  `SecureHttpClient`, `WifiSelectionActivity`, `IsrgRootX1`, `silentRestart`.

**This activity is the twin of `RaindropSyncActivity`.** Read that file first and
mirror it; every hazard it solves is present here too.

- [ ] **Step 1: Add the boot target**

In `src/SilentRestart.h`, add `TASK_SYNC = 9,` to the enum, a `case` to
`isNetworkBootTargetValue`, and a term to the `static_assert` chain. Omitting the
last two is the classic way to make the target silently unreachable.

**You must also load the task store in the boot branch.** `src/main.cpp` no longer loads it
unconditionally — that cost ~26 KB of internal RAM from boot on every device, including for
users who never open Tasks, and `TaskListActivity` calls `TASK_STORE.ensureLoaded()` itself.
Follow the `KOREADER_STORE` precedent a few lines below in the same function: load it only
inside the branch that matches your boot target, e.g.

```cpp
  } else if (snapshotTarget == static_cast<uint32_t>(NetworkBootTarget::TASK_SYNC)) {
    TASK_STORE.loadFromFile();
  }
```

and re-add `#include "TaskStore.h"` to `main.cpp`, which was removed with the load. Without
this the sync boots with an empty index and an empty secret, and will read as unpaired.

- [ ] **Step 2: Implement the sync**

The sequence, in order, each step with its reason:

1. `startActivityForResult(WifiSelectionActivity)`.
2. `syncSystemClockFromRtc()` — copy the helper from
   `RaindropSyncActivity.cpp:58-83`. Without it wolfSSL rejects the certificate
   with `ASN_BEFORE_DATE_E`, because the minimal network boot never sets the
   system clock.
3. Heap gate: abort with a readable message if
   `ESP.getFreeHeap() < 35000 || ESP.getMaxAllocHeap() < 20000`. Failing here is
   far better than failing inside TLS.
4. Read the cursor and up to `TASK_MAX_OPS_PER_SYNC` ops, build the body with
   `taskOpsToRequestBody` into a `TASK_REQUEST_BUF_SIZE` buffer allocated with
   `makeUniqueNoThrow<char[]>` — 12 KB must not go on the stack.
5. `SecureHttpClient http; http.setCACert(ISRG_ROOT_X1_PEM);` then
   `POST` to `<url>/api/v1/tasks/sync` with `Authorization: Bearer <secret>` and
   `Content-Type: application/json`.
6. Feed the response through `TaskSyncReader` using the client's data callback,
   so nothing larger than one chunk is ever buffered. In `onNoteChunk`, append
   straight to the note file and close it when `last` is true.
7. On `reset`, clear the index and `clearAllNotes()` **before** applying rows.
8. On 2xx: write the new cursor, `clearOps()`, save the index. On `more`, loop
   from step 4 with an empty ops array.
9. On 401: keep the secret, show `tr(STR_TASK_PAIRING_REQUIRED)`.
9b. **On ANY other failure — 5xx, a timeout, a TLS handshake error, a truncated
    response, or `TaskSyncReader::hasError()` — change nothing that is persisted.**
    Do NOT advance the cursor and do NOT `clearOps()`; leave both exactly as they
    were and report the failure. The protocol is built so a retry is free: ops are
    idempotent and the cursor is the server's, so the only way to lose a user's tick
    is to clear the queue for a response that was never fully accepted. Note this is
    the opposite of the 2xx path in step 8 — write that ordering out explicitly
    rather than relying on an early `return`, and say in your report which failure
    modes you traced.
10. Show the summary: received / sent / total, and **each rejected op named with
    its reason** — a task the server refused because the list was full must be
    reported by title, not silently dropped, since the device has already removed
    it from its own queue. Then
    `silentRestart()` in `onExit()` — the Wi-Fi session leaves the heap
    fragmented, exactly as for Raindrop.

`preventAutoSleep()` returns true while syncing and on the summary screen.

- [ ] **Step 3: Verify on hardware**

With CrossDrop running, from the device: Settings → Tasks → Sync. Then, in order:

```
1. add a task on the web, sync, confirm it appears with the right weight
2. tick it on the device, sync, confirm the web shows it done
3. add a task on the device with Wi-Fi off, sync, confirm a d… id on the web
4. pull the power mid-response, re-sync, confirm no duplicate and no lost tick
5. note ESP.getFreeHeap() and getMaxAllocHeap() shown on the sync screen,
   on an X3 with an SD-card font loaded
```

Step 4 is the one that matters: it is the whole reason ids are
device-namespaced and adds are idempotent.

- [ ] **Step 4: Commit**

```bash
clang-format -i src/activities/tasks/TaskSyncActivity.* src/SilentRestart.h
git add src/activities/tasks src/SilentRestart.h src/main.cpp
git commit -m "feat(tasks): manual two-way sync from a minimal network boot"
```

---

### Task 10: Settings, i18n, changelog, docs

**Files:**
- Modify: `src/CrossPointSettings.h` (two fields), `src/SettingsList.h`
- Modify: `lib/I18n/translations/english.yaml`, `lib/I18n/translations/french.yaml`
- Modify: `CHANGELOG.md`
- Create: `docs/crosstasks.md`

- [ ] **Step 1: Add the settings**

In `CrossPointSettings.h`, beside the Raindrop block:

```cpp
  // Liste de taches (serveur CrossDrop). Le secret n'est PAS ici : il vit
  // obfusque dans TaskStore, comme les mots de passe OPDS.
  uint8_t taskEnabled = 0;
  char taskServerUrl[96] = "";
```

Register them in `SettingsList.h` with a toggle and a string row, plus an
`Action(StrId::STR_TASK_SYNC, SettingAction::TaskSync)` and
`Action(StrId::STR_TASK_PAIR, SettingAction::TaskPair)`, both gated on
`taskEnabled` — mirroring how the Raindrop rows are gated.

- [ ] **Step 2: Add the strings**

Add to **both** `english.yaml` and `french.yaml` (never edit the generated files).

**Each file holds ONE language**, as a flat `KEY: "value"` map — there is no
`{ en: …, fr: … }` form, and writing one would corrupt both files. Open the tails of
`lib/I18n/translations/english.yaml` and `french.yaml` and follow the shape you find there.

Several of these keys were already added by Tasks 5-9 as each screen needed them. **Add
only the ones actually missing**, and before regenerating, verify with
`python3 -c "import yaml,sys; [yaml.safe_load(open(f)) for f in sys.argv[1:]]" lib/I18n/translations/english.yaml lib/I18n/translations/french.yaml`
that both files still parse and that **every `STR_TASK_*` key present in one is present in
the other** — a key in only one language is the failure mode this step exists to prevent.

The full set the feature needs, `english.yaml` on the left, `french.yaml` on the right:

| key | en | fr |
| --- | --- | --- |
| `STR_TASK_TITLE` | `Tasks` | `Tâches` |
| `STR_TASK_SYNC` | `Sync tasks` | `Synchroniser les tâches` |
| `STR_TASK_PAIR` | `Pair with server` | `Appairer au serveur` |
| `STR_TASK_NEW` | `New task` | `Nouvelle tâche` |
| `STR_TASK_TICK` | `tick` | `cocher` |
| `STR_TASK_DETAIL` | `detail` | `détail` |
| `STR_TASK_DONE_COUNT` | `done` | `faites` |
| `STR_TASK_EMPTY` | `No tasks yet` | `Aucune tâche pour l'instant` |
| `STR_TASK_PRIORITY_HIGH` | `High` | `Haute` |
| `STR_TASK_PRIORITY_NORMAL` | `Normal` | `Normale` |
| `STR_TASK_PRIORITY_LOW` | `Low` | `Basse` |
| `STR_TASK_POMODORO` | `Pomodoro` | `Pomodoro` |
| `STR_TASK_PAIRING_REQUIRED` | `Pairing required` | `Appairage requis` |
| `STR_TASK_LIST_FULL` | `List full` | `Liste pleine` |
| `STR_TASK_UP_TO_DATE` | `Up to date` | `À jour` |
| `STR_TASK_RECEIVED` | `Received` | `Reçues` |
| `STR_TASK_SENT` | `Sent` | `Envoyées` |

Then regenerate and verify:

```bash
python3 scripts/gen_i18n.py && git diff --stat lib/I18n/
```

Expected: `I18nKeys.h`, `I18nStrings.h` and `I18nStrings.cpp` change; nothing
else does.

- [ ] **Step 3: Write the changelog entry**

Under the current `## [Unreleased]` → `### Added` in `CHANGELOG.md`:

```markdown
- Tasks: a to-do list synced with a personal CrossDrop server. Add and complete
  tasks on the device, write longer notes from the web interface, and start a
  pomodoro on a task. Sync happens only when you ask for it, and pairing is done
  by scanning a QR code the device generates.
```

- [ ] **Step 4: Write the protocol doc**

Create `docs/crosstasks.md` documenting the request and response framing, the
pairing flow, the SD layout, and the limits — enough that the endpoint can be
reimplemented without reading the firmware. Link it from `docs/index.md`.

- [ ] **Step 5: Full verification**

```bash
python3 scripts/gen_i18n.py
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure
pio run -e default && pio run -e sticky && pio run -e x4-pro
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
git status --short
```

Expected: all green, and `git status --short` shows no unexpected generated
files staged (`.pio/`, `compile_commands.json`, `platformio.local.ini` must not
appear).

- [ ] **Step 6: Commit**

```bash
git add src/CrossPointSettings.h src/SettingsList.h lib/I18n CHANGELOG.md docs/
git commit -m "feat(tasks): settings, translations, changelog and protocol doc"
```

---

## Done when

- `ctest` is green: `TaskRecordTest`, `TaskSyncReaderTest`, `TaskOpQueueTest`.
- `pio run` succeeds for `simulator`, `default`, `sticky` and `x4-pro`.
- `pio check -e default` reports no low/medium/high defects.
- `scripts/run_simulator_smoke_test.py` passes.
- On hardware, the five-step verification in Task 9 passes, **including the
  interrupted sync producing no duplicate**.
- Free heap on an X3 with an SD-card font stays within the documented baseline
  (~85–90 KB free / ~49 KB max alloc) outside the Tasks screen — the feature must
  cost nothing while reading.
