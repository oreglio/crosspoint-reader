# CrossTasks — Design

Date: 2026-09-22 · Status: validated with the user (this session) · Target: fork-local feature, firmware + CrossDrop companion

A task list edited from a web UI on the user's VPS and synced, both ways, to a
Tasks screen on the reader. Sync is manual only. Pairing is done by scanning a
QR the device generates. Mockups live in `.superpowers/brainstorm/` (local only,
gitignored); the decisions they settled are all recorded below.

## Scope note — read this first

`SCOPE.md` lists **"Interactive Apps: No Notepads… This is a reader, not a PDA"**
and **"No typed out notes"** as explicitly out of scope. This feature is
therefore **fork-local and will never be proposed upstream**. It must not
degrade the reading path: no background task, no always-resident allocation, no
new boot cost. Every budget below is written against that constraint.

## Why

The user already runs CrossDrop on a VPS (Fastify + SQLite + Caddy) to push
Raindrop articles to the reader. A task list is the same shape of problem —
small records, authored comfortably on a real keyboard, consumed on e-ink — and
CrossDrop already contains the two hard parts: a monotonic `seq` diff cursor
(`src/db.ts`) and a device→server write queue (`archive_queue`).

## Decisions (all settled with the user)

1. **Target hardware: ESP32-C3 (X3/X4) first.** Everything shared must fit the
   C3. No PSRAM assumption, no touch assumption.
2. **Sync is manual only**, on a button press. Never background, never on wake.
3. **Task = title + priority (3 levels) + done + long note.** No due date, no
   project, no subtasks. Dropping due dates removes the X4 RTC-drift problem
   entirely.
4. **The device never edits notes.** It creates tasks (title + priority),
   toggles done, and renames. Notes are authored on the web and *read* on e-ink.
5. **Pomodoro shows the task title and nothing else.** No counters, no elapsed
   time, nothing synced back.
6. **Protocol: cursor + op queue in one request**, NDJSON response. (Axis 1, B.)
7. **Pairing: device-generated 128-bit secret shown as a QR.** Per-request HMAC
   signing is designed but deferred. (Axis 2, B; C deferred.)
8. **Web stack: server-rendered HTML + Alpine.js, no build step.** (Axis 3, A.)
9. **UI rules, identical on both sides:** priority is carried by *font weight*,
   never by a badge; completed tasks are collapsed behind a counter.

## Architecture

Three units with one interface between them.

| Unit | Lives in | Owns |
| --- | --- | --- |
| `task` module | `crossdrop/src/task*.ts` | SQLite table, op application, cursor diff, pairing secrets |
| Web UI | `crossdrop/web/` | Two templates + one stylesheet + vendored Alpine |
| Device | `CrossInkLibrary/src/activities/tasks/` + `src/TaskStore.*` | Screens, SD persistence, pending-op queue, sync client |

The device and the server share exactly one contract: `POST /api/v1/tasks/sync`.
Everything else on either side can change without touching the other.

### Why one endpoint and not a REST resource

On the C3, TLS costs ~35 KB free / 20 KB contiguous heap
(`RaindropSyncActivity.cpp:33-34`), and the first Raindrop design died of
repeated-handshake OOM. One request per sync means one handshake. A REST
resource would mean one handshake per task.

## Data model

### Server (new table; migration is additive, articles untouched)

```sql
CREATE TABLE IF NOT EXISTS tasks (
  id         TEXT PRIMARY KEY,           -- 'w'+8hex (web-created) or 'd'+8hex (device-created)
  title      TEXT NOT NULL,
  note       TEXT NOT NULL DEFAULT '',
  priority   INTEGER NOT NULL DEFAULT 1, -- 0 high, 1 normal, 2 low (ascending = sort order)
  done       INTEGER NOT NULL DEFAULT 0,
  deleted    INTEGER NOT NULL DEFAULT 0, -- tombstone, so the device learns about deletions
  updated_at TEXT NOT NULL,
  seq        INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_tasks_seq ON tasks(seq);
```

`seq` comes from its own `kv` key `task_seq`, **not** the article counter, so the
two features stay independent. It is bumped on write **only when what the device
can see changes** — the exact rule `upsertArticle` already applies (`db.ts:49-56`),
so a no-op save does not re-serve the task.

Tombstones are purged after 30 days, and the purge horizon is recorded in `kv`
as `task_tombstone_floor`. A device whose cursor predates that floor would
silently miss deletions, so the server answers it with `"reset": true` in the
header and a full snapshot; the device clears its index and note directory
before applying. This is the one case where the device throws away local state,
and it is safe because unsent ops are replayed in the *same* request, before the
snapshot is computed.

### Device

Two tiers, because notes must never be held in RAM as a block:

- `/.crosspoint/tasks/index.json` — `id`, `title`, `priority`, `done`, `noteBytes`.
  Loaded in `onEnter()`, freed in `onExit()`. **`MAX_TASKS = 120`** active tasks
  ≈ 8.4 KB resident.
  The cap is enforced **server-side only**: an `add` is refused with 409 once 120
  active tasks exist, whether it comes from the web or the device. A personal
  list with 120 open items is already pathological, and enforcing it at the one
  place that can count authoritatively removes every overflow-eviction path from
  the device — which would otherwise need to decide what to forget, and to
  remember that it forgot.
- `/.crosspoint/tasks/n/<id>.txt` — one note per file, written during sync, read
  and paginated on demand by the detail screen. Never fully resident. This is
  the same shape as `/Articles/*.md`.
- `/.crosspoint/tasks/ops.ndjson` — pending local operations, appended on each
  local change, replayed at the next sync, truncated only after a 2xx.
  Mirrors `raindrop-done.txt` (`RaindropSyncActivity.cpp:31`).
- `/.crosspoint/tasks/cursor.txt` — opaque base64url cursor, validated with the
  same alphabet check as `isSafeCursor` (`RaindropSyncActivity.cpp:46-56`).

`TaskStore` derives from `PersistableStore<TaskStore>` like `OpdsServerStore`,
so JSON machinery stays in `PersistableStore.cpp` and the store stays
flash-neutral.

## Protocol

### Request — `POST /api/v1/tasks/sync`

`Content-Type: application/json`, within the existing 16 KB `bodyLimit`.

```json
{
  "schema": 1,
  "cursor": "MTI=",
  "ops": [
    {"op": "add",   "id": "d3f9a1c04", "title": "Racheter du café", "priority": 1},
    {"op": "done",  "id": "w17ab93c2", "done": true},
    {"op": "prio",  "id": "d3f9a1c04", "priority": 0},
    {"op": "title", "id": "d3f9a1c04", "title": "Racheter du café en grains"}
  ]
}
```

There is no `note` op: the device never authors notes (decision 4).

**Device-generated ids carry their own namespace.** `'d'` + 8 hex from
`esp_random()`. The server rejects a `'w'`-prefixed id in an `add`. This
removes the temp-id↔real-id mapping table entirely, and makes a replay after a
dropped connection a no-op by primary key — which matters, because a sync that
fails after the server committed but before the device truncated its queue *will*
replay.

### Response — `application/x-ndjson`

One JSON object per line. First line is the header. **A note is not a JSON
string**: when `noteBytes > 0`, exactly that many raw bytes follow the metadata
line, terminated by a newline that is not counted in `noteBytes`.

```
{"schema":1,"cursor":"MTg=","more":false,"reset":false,"count":3}
{"id":"w17ab93c2","title":"Rappeler le notaire","priority":0,"done":false,"noteBytes":124}
Curseur + file d'ops dans une seule requête… (124 raw bytes, then \n)
{"id":"d3f9a1c04","title":"Racheter du café en grains","priority":0,"done":false,"noteBytes":0}
{"id":"w0091fe22","deleted":true}
```

Two reasons, and the second is the binding one:

- The device reads **one line at a time**, so peak RAM is one task's metadata
  regardless of list size. A single JSON array would have to be buffered whole.
- The firmware's existing `StreamingJsonParser` (`lib/JsonParser/`) holds one
  token in a **512-byte** buffer. A 4 KB note as a JSON string would not fit, and
  widening that buffer would cost every other JSON consumer in the firmware.
  Framing the note as raw bytes keeps it out of the parser entirely: the device
  streams it straight to SD in chunks, and the server never escapes it.

Only `title` remains an unbounded JSON string, capped at 200 bytes raw — at most
400 once escaped, comfortably inside the 512-byte token buffer.

### Server algorithm

1. Authenticate (below). Validate the body with Ajv.
2. Apply `ops` **in array order**, each bumping `task_seq` only on a real change.
3. `SELECT … WHERE seq > cursor ORDER BY seq LIMIT 200`.
   An **absent or empty cursor means a full snapshot**: every non-deleted task,
   paged the same way. That is the first-sync path and the `reset` path, and it
   is the same code — there is no separate bootstrap endpoint.
4. Stream the header, then the rows. `cursor` = last row's `seq`, or the request
   cursor when nothing changed. `more: true` when the limit was hit; the device
   immediately re-requests with the new cursor. Ops are sent only with the
   **first** page of a multi-page sync, since they have already been applied.

**Ops are applied before the diff is computed.** That single ordering choice is
what makes the whole design clock-free: the device's own writes come back to it
confirmed, conflicts resolve to "whatever the server holds after applying my
ops", and nothing anywhere compares timestamps.

The confirmation arrives within the same **sync**, not necessarily the same
*page*: an op takes the highest `seq`, so a device more than one page behind
receives its own write on a later page, after following `more` to the end. The
device therefore clears its op queue on the first 2xx — the server has already
applied them — and keeps paging. The X4's RTC
drift is therefore irrelevant to sync correctness.

### Limits

- Note ≤ 4 KB (400 otherwise). Title ≤ 200 bytes. `ops` ≤ 50 per request.
- Response page ≤ 200 changed tasks (a burst of edits can exceed the 120-task
  list cap, which is why the two numbers differ).
- 120 active tasks; a further `add` is refused. The server reports it per-op in
  the header (`"rejected":[{"id":"d3f9a1c04","reason":"full"}]`) rather than
  failing the whole request, and the device **drops** that op and names the task
  on the sync summary. Keeping it queued would retry forever against a condition
  only the user can clear.

## Security

### Pairing

1. Device: Settings → Tasks → *Pair*. 16 bytes from `esp_random()`, rendered as
   26 Crockford-base32 characters, shown as a QR by the existing
   `QrDisplayActivity`, with the grouped text below it as a no-camera fallback.
2. Web (session-authenticated): the pairing page scans it and `POST`s to
   `/api/v1/tasks/pair`, which stores **`sha256(secret)`** — never the secret.
3. Device stores the secret on SD, XOR-obfuscated with the hardware MAC and
   base64-encoded, using `lib/Serialization/ObfuscationUtils` — the same
   treatment `OpdsServerStore` gives passwords.

The inversion matters: the entropy lives in a 128-bit secret, not in a short
code. A short human-typed pairing code would cap the permanent secret at ~40
bits, which is not acceptable for an endpoint exposed to the internet.

### Device → server

`Authorization: Bearer <secret>`, compared with
`timingSafeEqual(sha256(given), stored_hash)` — the shape already in
`api.ts:51-58`. TLS is Caddy's Let's Encrypt certificate, and the device pins
**ISRG Root X1**, the anchor `RaindropSyncActivity` already carries
(`src/network/IsrgRootX1.h`). No new trust material.

### Human → web

Single password from `.env`, exchanged for an HMAC-signed cookie
(`HttpOnly; Secure; SameSite=Strict`, 30 days) using `node:crypto` plus
`@fastify/cookie` for parsing. Without this, the pairing page would let anyone
who finds the URL pair their own device — which is precisely the threat this
feature is meant to close.

Rate limits: the existing 60/min globally, tightened to **10/min on `/pair` and
on login**.

Revocation deletes the stored hash; the device's next sync gets 401 and the
Tasks screen says pairing is required.

### Deferred by decision: per-request HMAC

Designed, not built in v1. `X-Sig = HMAC-SHA256(secret, method|path|body|seq)`
with a strictly increasing `X-Seq` persisted on the device, behind a server flag
`TASK_REQUIRE_SIG=1`. It buys three things over the bearer: the secret never
appears in reverse-proxy logs, the body is integrity-checked, and replays are
refused — using a counter rather than a timestamp, so it stays clock-free. It
costs ~60 lines on each side. Behind a real certificate the bearer is enough,
so this waits for a reason to exist.

## Device UI

Portrait 480 × 800 logical, 1-bit, bezel-safe margins from
`GfxRenderer::VIEWABLE_MARGIN_*`. Fonts are built-ins only; every size below is
a real embedded face.

- **List** — one uniform body size (LexendDeca 14, `advanceY` 35), circular
  bullets, no rules between rows. **Priority is carried by weight**: semibold /
  regular / light. Sorted by priority. Completed tasks are hidden; the fourth
  action slot shows `☑ n` and toggles them into view at the bottom.
  A task ticked during this visit **stays in place, struck through**, until the
  screen is left — so an accidental tick is undone without hunting for it.
- **Detail** — title in LexendDeca 16, weight matching its priority (no
  priority label: the weight already says it). Note in **LexendDeca 10 light**
  in a bounded, paginated region. A framed *Pomodoro* block is anchored above
  the action bar, showing the duration that will start and the position in the
  series. Header carries `n / total`.
  Bitter was tested and rejected for the note: at 10, its serifs fall to one
  pixel, and a 1-bit panel with no antialiasing loses exactly that detail first.
- **Add** — `KeyboardEntryActivity` then `OptionSelectionActivity` for priority.
  Both already exist; no new screen.
- **Pomodoro** — existing `PomodoroActivity` with one added parameter: the title
  to display.
- **Sync** — progress, then a summary of both directions, then `silentRestart()`.
- **Pair** — QR plus grouped fallback code.

Two new screens only: detail and sync summary.

## Device sync flow

`NetworkBootTarget::TASK_SYNC = 9` (next free value in `src/SilentRestart.h:10`).
Settings action and Home entry call `silentRestartToNetwork(TASK_SYNC)`; the
minimal network boot runs `TaskSyncActivity`, which:

1. Runs `WifiSelectionActivity`.
2. Pushes the RTC into the system clock before TLS — without it wolfSSL fails
   certificate date checks with `ASN_BEFORE_DATE_E`, the bug already solved in
   `RaindropSyncActivity.cpp:58-83`.
3. Checks the heap gate (35 KB free / 20 KB contiguous) and aborts with a clear
   message rather than aborting inside TLS.
4. Sends one request, parses the response line by line, writes `index.json` and
   the note files, truncates `ops.ndjson`, stores the new cursor.
5. Shows the summary, then `silentRestart()` back into the full app — the Wi-Fi
   session leaves the heap fragmented, and this is already the house pattern.

## Web UI

New in **crossdrop** (which has no front end today): `web/templates/list.html`,
`web/templates/pair.html`, `web/assets/app.css`, and a vendored
`web/assets/alpine.min.js` (~15 KB, served locally — no CDN, so no network
dependency at load).

- Monochrome on paper, LexendDeca for UI, **Bitter for note bodies** — the serif
  returns here because a browser antialiases and the 1-bit constraint does not
  apply.
- Desktop: two panes, list left, editor right, detail pane open by default.
  The compose field sits at the top with the three priority pills inline.
- Mobile: **one breakpoint at 860 px**, same template. The panes stack, the
  detail becomes a full-screen layer Alpine shows on selection, and the compose
  field moves to the **bottom**, under the thumb. This is the only deliberate
  divergence between the two sizes.
- Autosave 1 s after the last keystroke. No save button, no modal.
- The editor footer states that the note will render in LexendDeca 10 on the
  reader — writing for a screen you cannot see needs that cue.
- Note excerpts are shown under titles in the list.

Camera scanning requires a secure origin. Caddy already provides one; the
consequence is that the manual code field is a genuine fallback, not decoration.

## Error handling

- Every failure path logs before returning, per `AGENTS.md`.
- `LOG_ERR` + `return false` for parse, storage and transport failures; the sync
  screen shows the reason and leaves `ops.ndjson` **untruncated** so nothing is lost.
- 401 → "pairing required", and the stored secret is kept (the user may have
  revoked by accident; deleting it would force a re-pair on a transient error).
- A malformed NDJSON line is skipped with a log, and the cursor is **not**
  advanced past it, so the next sync retries.
- The server treats every field of the request as hostile: ids are regex-checked,
  titles and notes length-capped, and no path is ever built from client input.

## Testing

- **crossdrop (Vitest, existing suite):** `taskDb.test.ts` — seq bumping only
  on visible change, tombstones, purge, the tombstone floor. `task.api.test.ts` —
  op application order, ops-before-diff confirmation, empty cursor as snapshot,
  `reset` for a stale cursor, cursor paging and `more`, NDJSON framing, auth
  rejection, pairing, revocation, per-op rejection when full, limits.
- **firmware (native CTest, `test/task_sync/`):** the response framing reader
  (metadata line → raw note block → next line, including a note split across two
  network chunks), the ops-queue round trip, and the priority sort — all pure, no
  I/O, host-tested in the style of `CountdownClock` and `PomodoroSchedule`.
- **On hardware:** pair from the phone; add a task on the web and sync; tick it
  on the device and sync back; add a task on the device offline, sync, confirm
  it appears on the web with a `d…` id; replay a sync interrupted mid-response
  and confirm no duplicate. Check free heap and `maxAllocHeap` on the sync
  screen of an X3 with an SD-card font loaded.

## Out of scope

Due dates, projects, subtasks, recurring tasks, reminders, pomodoro counters or
time tracking, note editing on the device, background or automatic sync, and
multi-device fan-out. The protocol happens to tolerate several devices, but that
is neither tested nor claimed.

## Delivery order

Each step is independently verifiable.

1. **CrossDrop core** — table, migration, sync endpoint, limits, tests. No UI.
2. **CrossDrop web** — login, list and editor, pairing page, responsive, tests.
3. **Firmware store** — `TaskStore`, ops queue, NDJSON parser, host tests.
4. **Firmware screens** — list, detail, add, pomodoro title parameter.
5. **Firmware sync** — `TaskSyncActivity`, boot target, pairing QR, settings.
6. **Finishing** — i18n keys in `english.yaml` / `french.yaml` then
   `python3 scripts/gen_i18n.py`, `CHANGELOG.md` entry under *Added*, and a
   `docs/` page for the protocol.
