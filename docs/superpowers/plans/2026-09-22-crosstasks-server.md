# CrossTasks — Server & Web Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a task list to CrossDrop — SQLite storage, a one-call two-way sync endpoint for the reader, and a responsive web UI with QR pairing — without touching the existing Raindrop article pipeline.

**Architecture:** Three new modules beside the existing ones. `taskDb.ts` owns the table and the monotonic `task_seq` diff cursor, reusing the exact "bump seq only on visible change" rule from `db.ts`. `taskApi.ts` registers the device-facing routes on the existing Fastify instance, keeping its bearer auth. `web.ts` serves two server-rendered templates plus a vendored Alpine; the browser talks to a small session-authenticated JSON API, entirely separate from the device's bearer-authenticated one.

**Tech Stack:** Node 22+, TypeScript (ESM), Fastify 5, better-sqlite3, Ajv, Vitest, Alpine.js 3 (vendored), Caddy (already deployed).

**Spec:** `docs/superpowers/specs/2026-09-22-crosstasks-design.md` — in the **CrossInkLibrary** repo. Read it before starting; this plan argues from it.

**Repository:** All paths below are relative to `/Users/aurelien-edusign/Code/xteink/crossdrop` (a different repo from the spec's).

## Global Constraints

- **Do not modify the article pipeline.** `src/raindrop.ts`, `src/pipeline.ts`, `src/extract.ts`, `src/summarize.ts`, `src/bundle.ts`, `src/syncLoop.ts` and `src/markdown.ts` must be untouched. `src/db.ts`, `src/api.ts`, `src/config.ts` and `src/index.ts` are extended only additively.
- **Two separate auth realms.** The device uses `Authorization: Bearer <secret>` compared with `timingSafeEqual(sha256(...))`. The browser uses an HMAC-signed session cookie. A device secret must never be accepted as a browser session, nor the reverse.
- **Priority encoding is `0 = high, 1 = normal, 2 = low`** so that `ORDER BY priority ASC` is the display order.
- **Ids:** `'w' + 8 lowercase hex` for web-created, `'d' + 8 lowercase hex` for device-created. Regex `/^[wd][0-9a-f]{8}$/`. The device may only create `d…`.
- **Limits:** title ≤ 200 bytes UTF-8, note ≤ 4096 bytes, ≤ 50 ops per request, ≤ 200 changed tasks per response page, ≤ 120 active (not done, not deleted) tasks.
- **Test style:** Vitest, `app.inject()`, `new Db(':memory:')`, `loadConfig({...})` with an env literal, French `describe`/`it` descriptions — match `test/api.test.ts` exactly.
- **ESM imports always carry the `.js` extension**, even for `.ts` sources (`import { Db } from './db.js'`).
- Run `npm run typecheck` before every commit. Run `npm test` at every step that says so.
- Never log a secret, a session cookie or a note body.

---

### Task 1: The `tasks` table and its diff cursor

**Files:**
- Create: `src/taskDb.ts`
- Create: `test/taskDb.test.ts`

**Interfaces:**
- Consumes: `Database` from `better-sqlite3`.
- Produces: `class TaskDb` with `constructor(d: Database.Database)`, and methods
  `upsert(t: TaskInput): void`, `get(id: string): TaskRow | undefined`,
  `changesSince(cursor: number, limit: number): TaskRow[]`,
  `activeCount(): number`, `markDeleted(id: string): void`,
  `purgeTombstones(beforeIso: string): void`, `tombstoneFloor(): number`,
  `snapshot(cursor: number, limit: number): TaskRow[]`, `maxSeq(): number`,
  `listAll(): TaskRow[]` (display order for the web UI: open first, then
  priority ascending — Task 6 depends on it).
  Types `TaskRow` and `TaskInput` are exported.

- [ ] **Step 1: Write the failing test**

Create `test/taskDb.test.ts`:

```ts
import Database from 'better-sqlite3';
import { beforeEach, describe, expect, it } from 'vitest';
import { TaskDb } from '../src/taskDb.js';

let db: TaskDb;
const NOW = '2026-09-22T09:00:00.000Z';

beforeEach(() => { db = new TaskDb(new Database(':memory:')); });

function add(id: string, over: Partial<Parameters<TaskDb['upsert']>[0]> = {}) {
  db.upsert({ id, title: `T${id}`, note: '', priority: 1, done: 0, deleted: 0, updated_at: NOW, ...over });
}

describe('taskDb', () => {
  it('assigne un seq croissant et le rend via changesSince', () => {
    add('w00000001'); add('w00000002');
    const rows = db.changesSince(0, 200);
    expect(rows.map(r => r.id)).toEqual(['w00000001', 'w00000002']);
    expect(rows[0].seq).toBeLessThan(rows[1].seq);
  });

  it('ne bouge pas le seq quand rien de visible ne change', () => {
    add('w00000001');
    const before = db.get('w00000001')!.seq;
    add('w00000001');
    expect(db.get('w00000001')!.seq).toBe(before);
  });

  it('bouge le seq quand le titre, la priorité, le fait ou la note changent', () => {
    add('w00000001');
    let seq = db.get('w00000001')!.seq;
    for (const over of [{ title: 'autre' }, { priority: 0 }, { done: 1 }, { note: 'x' }]) {
      add('w00000001', over);
      const next = db.get('w00000001')!.seq;
      expect(next).toBeGreaterThan(seq);
      seq = next;
    }
  });

  it('markDeleted pose un tombstone visible dans le diff', () => {
    add('w00000001');
    db.markDeleted('w00000001');
    const row = db.changesSince(0, 200).at(-1)!;
    expect(row).toMatchObject({ id: 'w00000001', deleted: 1 });
  });

  it('activeCount ignore les faites et les supprimées', () => {
    add('w00000001'); add('w00000002', { done: 1 }); add('w00000003');
    db.markDeleted('w00000003');
    expect(db.activeCount()).toBe(1);
  });

  it('snapshot ne renvoie que les vivantes, changesSince renvoie aussi les tombstones', () => {
    add('w00000001'); add('w00000002');
    db.markDeleted('w00000002');
    expect(db.snapshot(0, 200).map(r => r.id)).toEqual(['w00000001']);
    expect(db.changesSince(0, 200).map(r => r.id)).toContain('w00000002');
  });

  it('purgeTombstones supprime les vieilles et remonte le plancher', () => {
    add('w00000001', { updated_at: '2026-01-01T00:00:00.000Z' });
    db.markDeleted('w00000001');
    const floorBefore = db.tombstoneFloor();
    db.purgeTombstones('2026-06-01T00:00:00.000Z');
    expect(db.get('w00000001')).toBeUndefined();
    expect(db.tombstoneFloor()).toBeGreaterThan(floorBefore);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npx vitest run test/taskDb.test.ts`
Expected: FAIL — `Cannot find module '../src/taskDb.js'`.

- [ ] **Step 3: Write the implementation**

Create `src/taskDb.ts`:

```ts
import type Database from 'better-sqlite3';

export interface TaskRow {
  id: string; title: string; note: string; priority: number;
  done: number; deleted: number; updated_at: string; seq: number;
}
export type TaskInput = Omit<TaskRow, 'seq'>;

const SCHEMA = `
CREATE TABLE IF NOT EXISTS tasks (
  id         TEXT PRIMARY KEY,
  title      TEXT NOT NULL,
  note       TEXT NOT NULL DEFAULT '',
  priority   INTEGER NOT NULL DEFAULT 1,
  done       INTEGER NOT NULL DEFAULT 0,
  deleted    INTEGER NOT NULL DEFAULT 0,
  updated_at TEXT NOT NULL,
  seq        INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_tasks_seq ON tasks(seq);
CREATE TABLE IF NOT EXISTS task_kv (key TEXT PRIMARY KEY, value TEXT);
`;

export class TaskDb {
  private d: Database.Database;

  constructor(d: Database.Database) {
    this.d = d;
    this.d.exec(SCHEMA);
  }

  private kvGet(key: string): string | null {
    const r = this.d.prepare('SELECT value FROM task_kv WHERE key = ?').get(key) as { value: string } | undefined;
    return r?.value ?? null;
  }
  private kvSet(key: string, value: string): void {
    this.d.prepare('INSERT INTO task_kv(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value')
      .run(key, value);
  }
  private nextSeq(): number {
    const n = Number(this.kvGet('task_seq') ?? '0') + 1;
    this.kvSet('task_seq', String(n));
    return n;
  }

  maxSeq(): number { return Number(this.kvGet('task_seq') ?? '0'); }
  tombstoneFloor(): number { return Number(this.kvGet('task_tombstone_floor') ?? '0'); }

  upsert(t: TaskInput): void {
    const tx = this.d.transaction(() => {
      // Même règle que upsertArticle : le seq ne bouge que si ce que la liseuse
      // voit change. Une réécriture à l'identique ne doit pas re-servir la tâche.
      const prev = this.d.prepare(
        'SELECT title, note, priority, done, deleted, seq FROM tasks WHERE id = ?',
      ).get(t.id) as Omit<TaskRow, 'id' | 'updated_at'> | undefined;
      const unchanged = prev !== undefined && prev.title === t.title && prev.note === t.note
        && prev.priority === t.priority && prev.done === t.done && prev.deleted === t.deleted;
      const seq = unchanged ? prev.seq : this.nextSeq();
      this.d.prepare(`
        INSERT INTO tasks (id,title,note,priority,done,deleted,updated_at,seq)
        VALUES (@id,@title,@note,@priority,@done,@deleted,@updated_at,@seq)
        ON CONFLICT(id) DO UPDATE SET
          title=excluded.title, note=excluded.note, priority=excluded.priority,
          done=excluded.done, deleted=excluded.deleted,
          updated_at=excluded.updated_at, seq=excluded.seq
      `).run({ ...t, seq });
    });
    tx();
  }

  get(id: string): TaskRow | undefined {
    return this.d.prepare('SELECT * FROM tasks WHERE id = ?').get(id) as TaskRow | undefined;
  }

  changesSince(cursor: number, limit: number): TaskRow[] {
    return this.d.prepare('SELECT * FROM tasks WHERE seq > ? ORDER BY seq LIMIT ?')
      .all(cursor, limit) as TaskRow[];
  }

  snapshot(cursor: number, limit: number): TaskRow[] {
    return this.d.prepare('SELECT * FROM tasks WHERE deleted = 0 AND seq > ? ORDER BY seq LIMIT ?')
      .all(cursor, limit) as TaskRow[];
  }

  activeCount(): number {
    const r = this.d.prepare('SELECT COUNT(*) AS n FROM tasks WHERE done = 0 AND deleted = 0').get() as { n: number };
    return r.n;
  }

  markDeleted(id: string): void {
    const row = this.get(id);
    if (!row) return;
    this.upsert({ ...row, deleted: 1 });
  }

  purgeTombstones(beforeIso: string): void {
    const tx = this.d.transaction(() => {
      const doomed = this.d.prepare('SELECT MAX(seq) AS s FROM tasks WHERE deleted = 1 AND updated_at < ?')
        .get(beforeIso) as { s: number | null };
      if (doomed.s === null) return;
      this.d.prepare('DELETE FROM tasks WHERE deleted = 1 AND updated_at < ?').run(beforeIso);
      this.kvSet('task_tombstone_floor', String(doomed.s));
    });
    tx();
  }

  listAll(): TaskRow[] {
    return this.d.prepare('SELECT * FROM tasks WHERE deleted = 0 ORDER BY done ASC, priority ASC, seq ASC')
      .all() as TaskRow[];
  }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npx vitest run test/taskDb.test.ts && npm run typecheck`
Expected: 7 tests pass, no type errors.

- [ ] **Step 5: Commit**

```bash
git add src/taskDb.ts test/taskDb.test.ts
git commit -m "feat(tasks): tasks table with a monotonic diff cursor"
```

---

### Task 2: Pairing secrets

**Files:**
- Modify: `src/taskDb.ts` (add the pairing methods)
- Create: `src/taskAuth.ts`
- Modify: `test/taskDb.test.ts` (add one describe block)

**Interfaces:**
- Consumes: `TaskDb` from Task 1.
- Produces: `TaskDb.setPairing(secretHashHex: string, label: string, pairedAtIso: string): void`,
  `TaskDb.getPairing(): { hash: string; label: string; paired_at: string; last_sync: string | null } | null`,
  `TaskDb.clearPairing(): void`, `TaskDb.touchLastSync(iso: string): void`,
  and `hashSecret(s: string): string` (hex sha256) from the new `src/taskAuth.ts`.

`hashSecret` lives here, not beside the session helpers, because it is how the
pairing secret is *stored* — Task 4 needs it to authenticate the device, long
before Task 5 adds browser sessions to the same file.

- [ ] **Step 1: Write the failing test**

Append to `test/taskDb.test.ts`:

```ts
describe('appairage', () => {
  it('stocke, relit, horodate et révoque', () => {
    expect(db.getPairing()).toBeNull();
    db.setPairing('a'.repeat(64), 'Xteink X4', NOW);
    expect(db.getPairing()).toMatchObject({ hash: 'a'.repeat(64), label: 'Xteink X4', last_sync: null });
    db.touchLastSync('2026-09-22T10:00:00.000Z');
    expect(db.getPairing()!.last_sync).toBe('2026-09-22T10:00:00.000Z');
    db.clearPairing();
    expect(db.getPairing()).toBeNull();
  });

  it('un nouvel appairage remplace le précédent', () => {
    db.setPairing('a'.repeat(64), 'Un', NOW);
    db.setPairing('b'.repeat(64), 'Deux', NOW);
    expect(db.getPairing()!.label).toBe('Deux');
  });

  it('hashSecret est un sha256 hex stable', () => {
    expect(hashSecret('abc')).toBe('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  });
});
```

Add `import { hashSecret } from '../src/taskAuth.js';` to the top of the file.

- [ ] **Step 2: Run the test to verify it fails**

Run: `npx vitest run test/taskDb.test.ts`
Expected: FAIL — `db.setPairing is not a function`.

- [ ] **Step 3: Write the implementation**

Add to the `SCHEMA` string in `src/taskDb.ts`, before the closing backtick:

```sql
CREATE TABLE IF NOT EXISTS task_pairing (
  id        INTEGER PRIMARY KEY CHECK (id = 1),
  hash      TEXT NOT NULL,
  label     TEXT NOT NULL,
  paired_at TEXT NOT NULL,
  last_sync TEXT
);
```

Add these methods to `class TaskDb`:

```ts
  setPairing(secretHashHex: string, label: string, pairedAtIso: string): void {
    this.d.prepare(`
      INSERT INTO task_pairing (id,hash,label,paired_at,last_sync) VALUES (1,?,?,?,NULL)
      ON CONFLICT(id) DO UPDATE SET hash=excluded.hash, label=excluded.label,
                                    paired_at=excluded.paired_at, last_sync=NULL
    `).run(secretHashHex, label, pairedAtIso);
  }

  getPairing(): { hash: string; label: string; paired_at: string; last_sync: string | null } | null {
    const r = this.d.prepare('SELECT hash,label,paired_at,last_sync FROM task_pairing WHERE id = 1').get();
    return (r as { hash: string; label: string; paired_at: string; last_sync: string | null } | undefined) ?? null;
  }

  clearPairing(): void { this.d.prepare('DELETE FROM task_pairing WHERE id = 1').run(); }

  touchLastSync(iso: string): void {
    this.d.prepare('UPDATE task_pairing SET last_sync = ? WHERE id = 1').run(iso);
  }
```

Create `src/taskAuth.ts`:

```ts
import { createHash } from 'node:crypto';

/** Le secret d'appairage n'est jamais stocke en clair : seul son sha256 l'est. */
export function hashSecret(s: string): string {
  return createHash('sha256').update(s, 'utf8').digest('hex');
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npx vitest run test/taskDb.test.ts && npm run typecheck`
Expected: 10 tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/taskDb.ts src/taskAuth.ts test/taskDb.test.ts
git commit -m "feat(tasks): store the paired device secret as a sha256 hash"
```

---

### Task 3: Op validation and application

**Files:**
- Create: `src/taskOps.ts`
- Create: `test/taskOps.test.ts`

**Interfaces:**
- Consumes: `TaskDb`, `TaskRow` from Task 1.
- Produces: `applyOps(db: TaskDb, ops: unknown[], nowIso: string): Rejection[]` where
  `interface Rejection { id: string; reason: 'full' | 'unknown' | 'badid' }`, and
  `export const MAX_ACTIVE = 120`.

This task is separate from the route because the ordering rule — ops applied
before the diff, per-op rejection rather than whole-request failure — is the
heart of the protocol and deserves its own tests without HTTP in the way.

- [ ] **Step 1: Write the failing test**

Create `test/taskOps.test.ts`:

```ts
import Database from 'better-sqlite3';
import { beforeEach, describe, expect, it } from 'vitest';
import { TaskDb } from '../src/taskDb.js';
import { applyOps, MAX_ACTIVE } from '../src/taskOps.js';

let db: TaskDb;
const NOW = '2026-09-22T09:00:00.000Z';
beforeEach(() => { db = new TaskDb(new Database(':memory:')); });

describe('applyOps', () => {
  it('ajoute une tâche créée par la liseuse', () => {
    const rejected = applyOps(db, [{ op: 'add', id: 'd0000abc1', title: 'Café', priority: 0 }], NOW);
    expect(rejected).toEqual([]);
    expect(db.get('d0000abc1')).toMatchObject({ title: 'Café', priority: 0, done: 0 });
  });

  it('refuse un id du domaine web dans un add', () => {
    const rejected = applyOps(db, [{ op: 'add', id: 'w0000abc1', title: 'X', priority: 1 }], NOW);
    expect(rejected).toEqual([{ id: 'w0000abc1', reason: 'badid' }]);
    expect(db.get('w0000abc1')).toBeUndefined();
  });

  it('rejouer le même add est sans effet', () => {
    const op = { op: 'add', id: 'd0000abc1', title: 'Café', priority: 0 };
    applyOps(db, [op], NOW);
    const seq = db.get('d0000abc1')!.seq;
    expect(applyOps(db, [op], NOW)).toEqual([]);
    expect(db.get('d0000abc1')!.seq).toBe(seq);
  });

  it('done, prio et title modifient une tâche existante', () => {
    applyOps(db, [{ op: 'add', id: 'd0000abc1', title: 'Café', priority: 2 }], NOW);
    applyOps(db, [
      { op: 'done', id: 'd0000abc1', done: true },
      { op: 'prio', id: 'd0000abc1', priority: 0 },
      { op: 'title', id: 'd0000abc1', title: 'Café en grains' },
    ], NOW);
    expect(db.get('d0000abc1')).toMatchObject({ done: 1, priority: 0, title: 'Café en grains' });
  });

  it('une op sur un id inconnu est rejetée sans faire échouer les suivantes', () => {
    const rejected = applyOps(db, [
      { op: 'done', id: 'd0000ffff', done: true },
      { op: 'add', id: 'd0000abc1', title: 'Café', priority: 1 },
    ], NOW);
    expect(rejected).toEqual([{ id: 'd0000ffff', reason: 'unknown' }]);
    expect(db.get('d0000abc1')).toBeDefined();
  });

  it(`refuse un add au-delà de ${MAX_ACTIVE} tâches actives`, () => {
    for (let i = 0; i < MAX_ACTIVE; i++) {
      db.upsert({ id: `w${String(i).padStart(8, '0')}`, title: 'x', note: '', priority: 1, done: 0, deleted: 0, updated_at: NOW });
    }
    const rejected = applyOps(db, [{ op: 'add', id: 'd0000abc1', title: 'Café', priority: 1 }], NOW);
    expect(rejected).toEqual([{ id: 'd0000abc1', reason: 'full' }]);
  });

  it('cocher une tâche libère une place pour un add ultérieur', () => {
    for (let i = 0; i < MAX_ACTIVE; i++) {
      db.upsert({ id: `w${String(i).padStart(8, '0')}`, title: 'x', note: '', priority: 1, done: 0, deleted: 0, updated_at: NOW });
    }
    const rejected = applyOps(db, [
      { op: 'done', id: 'w00000000', done: true },
      { op: 'add', id: 'd0000abc1', title: 'Café', priority: 1 },
    ], NOW);
    expect(rejected).toEqual([]);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npx vitest run test/taskOps.test.ts`
Expected: FAIL — `Cannot find module '../src/taskOps.js'`.

- [ ] **Step 3: Write the implementation**

Create `src/taskOps.ts`:

```ts
import type { TaskDb } from './taskDb.js';

export const MAX_ACTIVE = 120;
export const MAX_TITLE_BYTES = 200;

export interface Rejection { id: string; reason: 'full' | 'unknown' | 'badid' }

const DEVICE_ID = /^d[0-9a-f]{8}$/;
const ANY_ID = /^[wd][0-9a-f]{8}$/;

function clampPriority(p: unknown): number {
  // Pas Number(p) : Number(null) vaut 0, donc un `priority: null` explicite
  // deviendrait « haute » alors qu'un champ absent retombe sur « normale ».
  if (typeof p !== 'number' || !Number.isInteger(p)) return 1;
  return p === 0 || p === 2 ? p : 1;
}

/**
 * Tronque a `maxBytes` OCTETS UTF-8, sans jamais couper un caractere en deux.
 * slice() compterait des unites UTF-16 : 150 « e » accentues font 150 unites
 * mais 300 octets, donc aucune troncature ne se declencherait et la limite
 * serait depassee de moitie (pire en cyrillique ou en CJK).
 */
function truncateUtf8(s: string, maxBytes: number): string {
  if (Buffer.byteLength(s, 'utf8') <= maxBytes) return s;
  let out = '';
  let bytes = 0;
  // for…of itere par POINTS DE CODE : une paire de substituts ne peut pas etre
  // coupee en deux, contrairement a slice() qui compte des unites UTF-16 et
  // laisserait un substitut orphelin — mesure a tort comme 3 octets, donc une
  // boucle qui s'arrete trop tot en croyant etre sous la limite.
  for (const ch of s) {
    const w = Buffer.byteLength(ch, 'utf8');
    if (bytes + w > maxBytes) break;
    out += ch;
    bytes += w;
  }
  return out;
}

/**
 * Applique les ops de la liseuse dans l'ordre du tableau, AVANT que le diff ne
 * soit calculé : c'est ce qui rend le protocole sans horloge, puisque les
 * écritures du device lui reviennent confirmées dans la même réponse.
 * Une op fautive est rejetée individuellement, jamais au prix de la requête.
 */
export function applyOps(db: TaskDb, ops: unknown[], nowIso: string): Rejection[] {
  const rejected: Rejection[] = [];
  for (const raw of ops) {
    // `raw as ...` n'est qu'un cast de compilation : sans cette garde, un
    // `{"ops":[null]}` — du JSON parfaitement valide venu du reseau — leverait
    // une TypeError hors de applyOps et ferait perdre toutes les ops valides
    // de la meme requete.
    if (typeof raw !== 'object' || raw === null || Array.isArray(raw)) {
      rejected.push({ id: '', reason: 'badid' });
      continue;
    }
    const o = raw as Record<string, unknown>;
    const id = typeof o.id === 'string' ? o.id : '';
    if (!ANY_ID.test(id)) { rejected.push({ id, reason: 'badid' }); continue; }

    if (o.op === 'add') {
      if (!DEVICE_ID.test(id)) { rejected.push({ id, reason: 'badid' }); continue; }
      if (db.get(id)) continue;                       // rejeu : sans effet
      if (db.activeCount() >= MAX_ACTIVE) { rejected.push({ id, reason: 'full' }); continue; }
      const title = truncateUtf8(String(o.title ?? ''), MAX_TITLE_BYTES);
      db.upsert({ id, title, note: '', priority: clampPriority(o.priority), done: 0, deleted: 0, updated_at: nowIso });
      continue;
    }

    const row = db.get(id);
    if (!row || row.deleted === 1) { rejected.push({ id, reason: 'unknown' }); continue; }

    if (o.op === 'done') db.upsert({ ...row, done: o.done === true ? 1 : 0, updated_at: nowIso });
    else if (o.op === 'prio') db.upsert({ ...row, priority: clampPriority(o.priority), updated_at: nowIso });
    else if (o.op === 'title') db.upsert({ ...row, title: truncateUtf8(String(o.title ?? ''), MAX_TITLE_BYTES), updated_at: nowIso });
    else rejected.push({ id, reason: 'unknown' });
  }
  return rejected;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `npx vitest run test/taskOps.test.ts && npm run typecheck`
Expected: 7 tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/taskOps.ts test/taskOps.test.ts
git commit -m "feat(tasks): apply device ops before the diff, rejecting per-op"
```

---

### Task 4: The sync endpoint and its framing

**Files:**
- Create: `src/taskApi.ts`
- Create: `test/taskApi.test.ts`
- Modify: `src/api.ts` (register the routes, and exempt them from the article bearer hook)

**Interfaces:**
- Consumes: `TaskDb` (Tasks 1–2), `hashSecret` (Task 2), `applyOps` / `MAX_ACTIVE` (Task 3), `Config`.
- Produces: `registerTaskRoutes(app: FastifyInstance, tasks: TaskDb, cfg: Config): void`,
  serving `POST /api/v1/tasks/sync`. Exports `encodeCursor(seq: number): string`
  and `decodeTaskCursor(raw: string | undefined): number | null` for the web module.

**Authentication — read this before the tests.** These routes do **not** use the
article pipeline's `DEVICE_TOKEN`. A reader authenticates with the secret it
generated at pairing, and the server compares `sha256(bearer)` against the hash
stored by `setPairing`. So `registerTaskRoutes` installs its own `onRequest`
hook scoped to `/api/v1/tasks/`, and `src/api.ts` exempts that prefix from the
existing global hook. An unpaired server answers 401 to every task route.

**Framing contract (this is the interface the firmware implements):** the
response is `application/x-ndjson`. Line 1 is the header. Then, per task, one
metadata line; when its `noteBytes > 0`, exactly that many raw bytes follow,
then a `\n` that is **not** counted in `noteBytes`.

- [ ] **Step 1: Write the failing test**

Create `test/taskApi.test.ts`:

```ts
import Database from 'better-sqlite3';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { beforeEach, describe, expect, it } from 'vitest';
import { buildServer } from '../src/api.js';
import { loadConfig } from '../src/config.js';
import { Db } from '../src/db.js';
import { TaskDb } from '../src/taskDb.js';
import { hashSecret } from '../src/taskAuth.js';

const TOKEN = 'a'.repeat(48);                 // jeton des articles, PAS celui des taches
const SECRET = 'K7M2P9XQ4VTB1234567890ABCD';   // secret d'appairage, 26 caracteres
const AUTH = { authorization: `Bearer ${SECRET}` };
const NOW = '2026-09-22T09:00:00.000Z';
let db: Db; let tasks: TaskDb; let cfg: ReturnType<typeof loadConfig>;

beforeEach(() => {
  db = new Db(':memory:');
  tasks = new TaskDb(new Database(':memory:'));
  tasks.setPairing(hashSecret(SECRET), 'Xteink X4', NOW);
  cfg = loadConfig({ RAINDROP_TOKEN: 't', DEVICE_TOKEN: TOKEN, DOMAIN: 'd.fr', DATA_DIR: mkdtempSync(join(tmpdir(), 'crossdrop-')) });
});

/** Découpe le corps NDJSON en { header, rows, notes } selon le cadrage.
    Le champ s'appelle `rows` et non `tasks` : `tasks` est déjà la TaskDb du
    fichier, et la déstructurer l'ombrerait — trois tests tombaient en TDZ. */
function parse(body: string) {
  const lines = body.split('\n');
  const header = JSON.parse(lines[0]);
  const rows: Record<string, unknown>[] = [];
  const notes: Record<string, string> = {};
  let i = 1;
  while (i < lines.length && lines[i] !== '') {
    const t = JSON.parse(lines[i]) as Record<string, unknown>;
    rows.push(t); i += 1;
    const n = Number(t.noteBytes ?? 0);
    if (n > 0) { notes[t.id as string] = lines[i]; i += 1; }
  }
  return { header, rows, notes };
}

async function sync(body: object) {
  const app = await buildServer(db, cfg, { tasks });
  return app.inject({ method: 'POST', url: '/api/v1/tasks/sync', headers: AUTH, payload: body });
}

describe('tasks/sync', () => {
  it('401 sans jeton', async () => {
    const app = await buildServer(db, cfg, { tasks });
    const res = await app.inject({ method: 'POST', url: '/api/v1/tasks/sync', payload: { schema: 1 } });
    expect(res.statusCode).toBe(401);
  });

  it('401 avec le jeton des articles : il n’ouvre pas les routes taches', async () => {
    const app = await buildServer(db, cfg, { tasks });
    const res = await app.inject({ method: 'POST', url: '/api/v1/tasks/sync',
      headers: { authorization: `Bearer ${TOKEN}` }, payload: { schema: 1 } });
    expect(res.statusCode).toBe(401);
  });

  it('401 quand aucune liseuse n’est appairée', async () => {
    tasks.clearPairing();
    const app = await buildServer(db, cfg, { tasks });
    const res = await app.inject({ method: 'POST', url: '/api/v1/tasks/sync', headers: AUTH, payload: { schema: 1 } });
    expect(res.statusCode).toBe(401);
  });

  it('curseur absent = instantané complet', async () => {
    tasks.upsert({ id: 'w00000001', title: 'Notaire', note: '', priority: 0, done: 0, deleted: 0, updated_at: NOW });
    const { header, rows } = parse((await sync({ schema: 1 })).body);
    expect(header).toMatchObject({ schema: 1, more: false, reset: false, count: 1 });
    expect(rows[0]).toMatchObject({ id: 'w00000001', title: 'Notaire', priority: 0, done: false, noteBytes: 0 });
  });

  it('cadre la note en octets bruts après la ligne de métadonnées', async () => {
    const note = 'Curseur + file d’ops.\nDeuxième ligne.';
    tasks.upsert({ id: 'w00000001', title: 'Spec', note, priority: 1, done: 0, deleted: 0, updated_at: NOW });
    const res = await sync({ schema: 1 });
    const bodyBuf = Buffer.from(res.rawPayload);
    const firstNl = bodyBuf.indexOf(0x0a);
    const secondNl = bodyBuf.indexOf(0x0a, firstNl + 1);
    const meta = JSON.parse(bodyBuf.subarray(firstNl + 1, secondNl).toString('utf8'));
    expect(meta.noteBytes).toBe(Buffer.byteLength(note, 'utf8'));
    expect(bodyBuf.subarray(secondNl + 1, secondNl + 1 + meta.noteBytes).toString('utf8')).toBe(note);
  });

  it('les ops sont appliquées avant le diff et reviennent confirmées', async () => {
    const { rows } = parse((await sync({
      schema: 1, ops: [{ op: 'add', id: 'd0000abc1', title: 'Café', priority: 0 }],
    })).body);
    expect(rows.map(t => t.id)).toContain('d0000abc1');
    expect(tasks.get('d0000abc1')).toBeDefined();
  });

  it('un curseur rend uniquement les changements suivants', async () => {
    tasks.upsert({ id: 'w00000001', title: 'A', note: '', priority: 1, done: 0, deleted: 0, updated_at: NOW });
    const first = parse((await sync({ schema: 1 })).body);
    tasks.upsert({ id: 'w00000002', title: 'B', note: '', priority: 1, done: 0, deleted: 0, updated_at: NOW });
    const second = parse((await sync({ schema: 1, cursor: first.header.cursor })).body);
    expect(second.rows.map(t => t.id)).toEqual(['w00000002']);
  });

  it('pagine avec more:true quand il y a plus de tâches que la limite', async () => {
    for (let i = 0; i < 3; i++) {
      tasks.upsert({ id: `w0000000${i}`, title: `T${i}`, note: '', priority: 1, done: 0, deleted: 0, updated_at: NOW });
    }
    const { header } = parse((await sync({ schema: 1, limit: 2 })).body);
    expect(header.more).toBe(true);
  });

  it('un curseur plus vieux que le plancher de purge déclenche reset', async () => {
    tasks.upsert({ id: 'w00000001', title: 'A', note: '', priority: 1, done: 0, deleted: 0, updated_at: '2026-01-01T00:00:00.000Z' });
    tasks.markDeleted('w00000001');
    tasks.purgeTombstones('2026-06-01T00:00:00.000Z');
    tasks.upsert({ id: 'w00000002', title: 'B', note: '', priority: 1, done: 0, deleted: 0, updated_at: NOW });
    const { header, rows } = parse((await sync({ schema: 1, cursor: Buffer.from('1', 'utf8').toString('base64url') })).body);
    expect(header.reset).toBe(true);
    expect(rows.map(t => t.id)).toEqual(['w00000002']);
  });

  it('remonte les rejets dans l’en-tête sans faire échouer la requête', async () => {
    const { header } = parse((await sync({
      schema: 1, ops: [{ op: 'done', id: 'd0000ffff', done: true }],
    })).body);
    expect(header.rejected).toEqual([{ id: 'd0000ffff', reason: 'unknown' }]);
  });

  it('refuse un corps mal formé', async () => {
    const res = await sync({ schema: 1, ops: 'pas un tableau' });
    expect(res.statusCode).toBe(400);
  });

  it('refuse plus de 50 ops', async () => {
    const ops = Array.from({ length: 51 }, (_, i) => ({ op: 'done', id: `d0000${String(i).padStart(4, '0')}`, done: true }));
    expect((await sync({ schema: 1, ops })).statusCode).toBe(400);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npx vitest run test/taskApi.test.ts`
Expected: FAIL — `Cannot find module '../src/taskDb.js'` is resolved, but `buildServer` rejects the unknown `tasks` option / the route 404s.

- [ ] **Step 3: Write the implementation**

Create `src/taskApi.ts`:

```ts
import { timingSafeEqual } from 'node:crypto';
import { Ajv } from 'ajv';
import type { FastifyInstance } from 'fastify';
import type { Config } from './config.js';
import { hashSecret } from './taskAuth.js';
import type { TaskDb, TaskRow } from './taskDb.js';
import { applyOps } from './taskOps.js';

const MAX_PAGE = 200;
const MAX_OPS = 50;

export const encodeCursor = (seq: number) => Buffer.from(String(seq), 'utf8').toString('base64url');

export function decodeTaskCursor(raw: string | undefined): number | null {
  if (!raw) return 0;
  const s = Buffer.from(raw, 'base64url').toString('utf8');
  if (!/^\d{1,15}$/.test(s)) return null;
  return Number(s);
}

function metaLine(r: TaskRow): string {
  if (r.deleted === 1) return JSON.stringify({ id: r.id, deleted: true });
  return JSON.stringify({
    id: r.id, title: r.title, priority: r.priority,
    done: r.done === 1, noteBytes: Buffer.byteLength(r.note, 'utf8'),
  });
}

export function registerTaskRoutes(app: FastifyInstance, tasks: TaskDb, _cfg: Config): void {
  // Domaine liseuse. Le jeton n'est PAS DEVICE_TOKEN (celui des articles) mais le
  // secret que la liseuse s'est fabrique a l'appairage : on ne stocke que son
  // sha256, et une instance non appairee refuse tout.
  app.addHook('onRequest', async (req, reply) => {
    if (!req.url.startsWith('/api/v1/tasks/')) return;
    const pairing = tasks.getPairing();
    if (!pairing) return reply.code(401).send({ error: 'aucune liseuse appairee' });
    const header = req.headers.authorization ?? '';
    const given = Buffer.from(hashSecret(header.startsWith('Bearer ') ? header.slice(7) : ''));
    const want = Buffer.from(pairing.hash);
    if (given.length !== want.length || !timingSafeEqual(given, want)) {
      return reply.code(401).send({ error: 'unauthorized' });
    }
  });

  const ajv = new Ajv({ coerceTypes: false });
  const validate = ajv.compile({
    type: 'object',
    properties: {
      schema: { type: 'integer' },
      cursor: { type: 'string', maxLength: 64 },
      limit: { type: 'integer', minimum: 1, maximum: MAX_PAGE },
      ops: { type: 'array', maxItems: MAX_OPS, items: { type: 'object' } },
    },
    additionalProperties: false,
  });

  app.post<{ Body: { cursor?: string; limit?: number; ops?: unknown[] } }>(
    '/api/v1/tasks/sync',
    async (req, reply) => {
      if (!validate(req.body ?? {})) return reply.code(400).send({ error: 'corps invalide' });
      const body = req.body ?? {};
      const cursor = decodeTaskCursor(body.cursor);
      if (cursor === null) return reply.code(400).send({ error: 'cursor invalide' });

      const now = new Date().toISOString();
      // L'ordre est le cœur du protocole : ops d'abord, diff ensuite.
      const rejected = applyOps(tasks, body.ops ?? [], now);

      // Un curseur antérieur au plancher de purge a manqué des suppressions :
      // on ne peut plus lui servir un diff honnête, donc on le renvoie à zéro.
      const reset = cursor > 0 && cursor < tasks.tombstoneFloor();
      const effective = reset ? 0 : cursor;
      const limit = Math.min(body.limit ?? MAX_PAGE, MAX_PAGE);
      // `effective === 0`, pas `reset` : un curseur ABSENT (premier appairage) doit
      // lui aussi passer par snapshot(), sinon changesSince(0) renvoie les
      // tombstones de taches que la liseuse n'a jamais connues.
      const rows = effective === 0 ? tasks.snapshot(0, limit) : tasks.changesSince(effective, limit);

      const last = rows.at(-1);
      const header = {
        schema: 1,
        // encodeCursor(effective), pas body.cursor : apres un reset sans aucune
        // ligne, renvoyer le curseur perime tel quel ferait rejouer le reset a
        // chaque sync, indefiniment.
        cursor: last ? encodeCursor(last.seq) : encodeCursor(effective),
        more: rows.length === limit,
        reset,
        count: rows.length,
        rejected,
      };

      tasks.touchLastSync(now);
      reply.header('content-type', 'application/x-ndjson; charset=utf-8');

      const chunks: Buffer[] = [Buffer.from(`${JSON.stringify(header)}\n`, 'utf8')];
      for (const r of rows) {
        chunks.push(Buffer.from(`${metaLine(r)}\n`, 'utf8'));
        if (r.deleted !== 1 && r.note.length > 0) {
          chunks.push(Buffer.from(r.note, 'utf8'), Buffer.from('\n', 'utf8'));
        }
      }
      return reply.send(Buffer.concat(chunks));
    },
  );
}
```

In `src/api.ts`, extend `ServerOpts` and register the routes. Add to the interface:

```ts
  // Liste de tâches ; absente, les routes /task ne sont simplement pas montées.
  tasks?: TaskDb;
```

Add the import at the top (`import type { TaskDb } from './taskDb.js';` and
`import { registerTaskRoutes } from './taskApi.js';`), then after the existing
`app.setErrorHandler(...)` block insert:

```ts
  if (opts.tasks) registerTaskRoutes(app, opts.tasks, cfg);
```

Finally, exempt the task routes from the article bearer hook — they carry their
own. Make this the first line of the existing `onRequest` hook body in
`src/api.ts`:

```ts
    if (req.url.startsWith('/api/v1/tasks/')) return;  // domaine liseuse, auth par secret d'appairage
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm test && npm run typecheck`
Expected: the 10 new tests pass and **every pre-existing test still passes** — that is the check that the article pipeline was not disturbed.

- [ ] **Step 5: Commit**

```bash
git add src/taskApi.ts src/api.ts test/taskApi.test.ts
git commit -m "feat(tasks): one-call sync endpoint with length-prefixed notes"
```

---

### Task 5: Pairing route and browser session auth

**Files:**
- Modify: `src/taskAuth.ts` (append the session helpers beside `hashSecret` from Task 2)
- Create: `test/taskAuth.test.ts`
- Modify: `src/config.ts` (add `webPassword`, `sessionSecret`)
- Modify: `src/taskApi.ts` (bypass the bearer hook for the browser realm)
- Modify: `.env.example`

**Interfaces:**
- Consumes: `TaskDb` (Task 2), `Config`.
- Produces: `signSession(secret: string, iat: number): string`,
  `verifySession(secret: string, cookie: string | undefined, nowMs: number): boolean`,
  `registerWebAuth(app, tasks, cfg)` serving `POST /web/login`, `POST /web/pair`,
  `POST /web/unpair`.

**Why this is its own task:** the device realm (bearer) and the browser realm
(cookie) must never authenticate each other. Keeping the browser realm in a
separate module with its own tests is what makes that reviewable.

- [ ] **Step 1: Write the failing test**

Create `test/taskAuth.test.ts`:

```ts
import { describe, expect, it } from 'vitest';
import { signSession, verifySession } from '../src/taskAuth.js';

const SECRET = 's'.repeat(32);
const T0 = 1_790_000_000_000;

describe('session', () => {
  it('un cookie signé est accepté, un cookie bricolé ne l’est pas', () => {
    const c = signSession(SECRET, T0);
    expect(verifySession(SECRET, c, T0 + 1000)).toBe(true);
    expect(verifySession(SECRET, `${c}x`, T0 + 1000)).toBe(false);
    expect(verifySession(SECRET, undefined, T0)).toBe(false);
  });

  it('expire au-delà de 30 jours', () => {
    const c = signSession(SECRET, T0);
    expect(verifySession(SECRET, c, T0 + 29 * 86400_000)).toBe(true);
    expect(verifySession(SECRET, c, T0 + 31 * 86400_000)).toBe(false);
  });

  it('un autre secret invalide la signature', () => {
    expect(verifySession('a'.repeat(32), signSession(SECRET, T0), T0)).toBe(false);
  });

});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npx vitest run test/taskAuth.test.ts`
Expected: FAIL — `signSession is not a function` (the module exists since Task 2,
but carries only `hashSecret`).

- [ ] **Step 3: Write the implementation**

Append to `src/taskAuth.ts` (it already exports `hashSecret` from Task 2; add
`createHmac` and `timingSafeEqual` to its existing `node:crypto` import):

```ts
const MAX_AGE_MS = 30 * 86_400_000;

/** Cookie = "<iat>.<hmac>" ; aucune donnée de session, juste une date signée. */
export function signSession(secret: string, iatMs: number): string {
  const mac = createHmac('sha256', secret).update(String(iatMs)).digest('base64url');
  return `${iatMs}.${mac}`;
}

export function verifySession(secret: string, cookie: string | undefined, nowMs: number): boolean {
  if (!cookie) return false;
  const dot = cookie.indexOf('.');
  if (dot <= 0) return false;
  const iat = Number(cookie.slice(0, dot));
  if (!Number.isInteger(iat) || nowMs - iat > MAX_AGE_MS || nowMs - iat < -60_000) return false;
  const given = Buffer.from(cookie.slice(dot + 1));
  const want = Buffer.from(createHmac('sha256', secret).update(String(iat)).digest('base64url'));
  return given.length === want.length && timingSafeEqual(given, want);
}
```

In `src/config.ts`, add to the `Config` interface:

```ts
  webPassword: string | null; sessionSecret: string;
```

and inside the object returned by `loadConfig`:

```ts
    webPassword: env.WEB_PASSWORD?.trim() || null,
    sessionSecret: env.SESSION_SECRET?.trim() || deviceToken,
```

Append to `.env.example`:

```
# Liste de tâches (optionnel : sans WEB_PASSWORD, l'interface web reste fermée)
WEB_PASSWORD=              # mot de passe unique de l'interface web
SESSION_SECRET=            # clé de signature des cookies, openssl rand -hex 32
```

Append to `src/taskApi.ts` — the browser realm, plus the hook exemption:

```ts
import { hashSecret, signSession, verifySession } from './taskAuth.js';

/**
 * Le domaine navigateur. Volontairement distinct du Bearer de la liseuse :
 * un secret d'appareil ne doit jamais ouvrir une session web, ni l'inverse.
 */
export function registerWebAuth(app: FastifyInstance, tasks: TaskDb, cfg: Config): void {
  const cookieOf = (req: { headers: Record<string, unknown> }) => {
    const raw = String(req.headers.cookie ?? '');
    const hit = raw.split(';').map(s => s.trim()).find(s => s.startsWith('cd_session='));
    return hit?.slice('cd_session='.length);
  };
  const authed = (req: { headers: Record<string, unknown> }) =>
    verifySession(cfg.sessionSecret, cookieOf(req), Date.now());

  app.post<{ Body: { password?: string } }>('/web/login', async (req, reply) => {
    if (!cfg.webPassword) return reply.code(503).send({ error: 'interface web désactivée' });
    const given = Buffer.from(hashSecret(String(req.body?.password ?? '')));
    const want = Buffer.from(hashSecret(cfg.webPassword));
    if (!timingSafeEqual(given, want)) return reply.code(401).send({ error: 'mot de passe invalide' });
    reply.header('set-cookie',
      `cd_session=${signSession(cfg.sessionSecret, Date.now())}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=2592000`);
    return { ok: true };
  });

  app.post<{ Body: { secret?: string; label?: string } }>('/web/pair', async (req, reply) => {
    if (!authed(req)) return reply.code(401).send({ error: 'non connecté' });
    const secret = String(req.body?.secret ?? '').trim().toUpperCase().replace(/[^0-9A-HJKMNP-TV-Z]/g, '');
    if (secret.length !== 26) return reply.code(400).send({ error: 'code invalide' });
    tasks.setPairing(hashSecret(secret), String(req.body?.label ?? 'Liseuse').slice(0, 60), new Date().toISOString());
    return { ok: true };
  });

  app.post('/web/unpair', async (req, reply) => {
    if (!authed(req)) return reply.code(401).send({ error: 'non connecté' });
    tasks.clearPairing();
    return { ok: true };
  });
}
```

Add `import { timingSafeEqual } from 'node:crypto';` to the top of `src/taskApi.ts`,
and call `registerWebAuth(app, opts.tasks, cfg)` next to `registerTaskRoutes` in `src/api.ts`.

In `src/api.ts`, exempt the browser realm from the article bearer hook. **Add a
second exemption line beside the one Task 4 already put there — do not replace
it.** Dropping Task 4's line would let the article hook intercept
`/api/v1/tasks/*` before the pairing hook runs, and a valid pairing secret
(always different from `DEVICE_TOKEN`) would be rejected 401 by the wrong hook,
taking the whole reader sync down:

```ts
    if (req.url.startsWith('/api/v1/tasks/')) return;              // domaine liseuse (Task 4)
    if (req.url.startsWith('/web/') || req.url === '/') return;    // domaine navigateur, auth par cookie
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm test && npm run typecheck`
Expected: all tests pass, including the pre-existing `auth` describe in
`test/api.test.ts` — the exemption must not have opened `/api/v1/*`.

- [ ] **Step 5: Add a regression test that the realms stay separate**

Append to `test/taskApi.test.ts`:

```ts
describe('cloison entre les deux domaines', () => {
  it('le Bearer de la liseuse n’ouvre pas /web/pair', async () => {
    const app = await buildServer(db, cfg, { tasks });
    const res = await app.inject({ method: 'POST', url: '/web/pair', headers: AUTH, payload: { secret: 'A'.repeat(26) } });
    expect(res.statusCode).toBe(401);
  });

  it('les routes /api/v1 restent fermées sans Bearer', async () => {
    const app = await buildServer(db, cfg, { tasks });
    expect((await app.inject({ url: '/api/v1/health' })).statusCode).toBe(401);
  });
});
```

Run: `npm test`
Expected: both pass.

- [ ] **Step 6: Commit**

```bash
git add src/taskAuth.ts src/taskApi.ts src/api.ts src/config.ts .env.example test/taskAuth.test.ts test/taskApi.test.ts
git commit -m "feat(tasks): browser session realm and QR pairing, separate from the device bearer"
```

---

### Task 6: Browser JSON API for the web UI

**Files:**
- Modify: `src/taskApi.ts` (add `registerWebData`)
- Modify: `src/api.ts` (one call)
- Modify: `test/taskApi.test.ts` (add a describe block)

**Interfaces:**
- Consumes: `TaskDb`, `verifySession`.
- Produces: `registerWebData(app, tasks, cfg)` serving
  `GET /web/api/tasks`, `POST /web/api/tasks`, `PATCH /web/api/tasks/:id`,
  `DELETE /web/api/tasks/:id`, `GET /web/api/status`.

- [ ] **Step 1: Write the failing test**

Append to `test/taskApi.test.ts`:

```ts
import { signSession } from '../src/taskAuth.js';

describe('api navigateur', () => {
  const session = () => ({ cookie: `cd_session=${signSession(cfg.sessionSecret, Date.now())}` });

  it('liste, crée, modifie et supprime', async () => {
    const app = await buildServer(db, cfg, { tasks });
    let res = await app.inject({ method: 'POST', url: '/web/api/tasks', headers: session(),
      payload: { title: 'Notaire', priority: 0 } });
    expect(res.statusCode).toBe(200);
    const id = res.json().id as string;
    expect(id).toMatch(/^w[0-9a-f]{8}$/);

    res = await app.inject({ url: '/web/api/tasks', headers: session() });
    expect(res.json().tasks).toHaveLength(1);

    res = await app.inject({ method: 'PATCH', url: `/web/api/tasks/${id}`, headers: session(),
      payload: { note: 'Prendre le dossier.', done: true } });
    expect(res.statusCode).toBe(200);
    expect(tasks.get(id)).toMatchObject({ note: 'Prendre le dossier.', done: 1 });

    res = await app.inject({ method: 'DELETE', url: `/web/api/tasks/${id}`, headers: session() });
    expect(res.statusCode).toBe(200);
    expect(tasks.get(id)!.deleted).toBe(1);
  });

  it('refuse sans session', async () => {
    const app = await buildServer(db, cfg, { tasks });
    expect((await app.inject({ url: '/web/api/tasks' })).statusCode).toBe(401);
  });

  it('refuse une note de plus de 4096 octets', async () => {
    const app = await buildServer(db, cfg, { tasks });
    const id = (await app.inject({ method: 'POST', url: '/web/api/tasks', headers: session(),
      payload: { title: 'X', priority: 1 } })).json().id;
    const res = await app.inject({ method: 'PATCH', url: `/web/api/tasks/${id}`, headers: session(),
      payload: { note: 'a'.repeat(4097) } });
    expect(res.statusCode).toBe(400);
  });

  it('status expose l’appairage sans jamais rendre le secret', async () => {
    tasks.setPairing('a'.repeat(64), 'Xteink X4', NOW);
    const app = await buildServer(db, cfg, { tasks });
    const body = (await app.inject({ url: '/web/api/status', headers: session() })).json();
    expect(body.pairing).toMatchObject({ label: 'Xteink X4' });
    expect(JSON.stringify(body)).not.toContain('a'.repeat(64));
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `npx vitest run test/taskApi.test.ts`
Expected: FAIL — the `/web/api/*` routes 404.

- [ ] **Step 3: Write the implementation**

Append to `src/taskApi.ts`:

```ts
import { randomBytes } from 'node:crypto';
import { MAX_ACTIVE, MAX_TITLE_BYTES } from './taskOps.js';

const MAX_NOTE_BYTES = 4096;
const newWebId = () => `w${randomBytes(4).toString('hex')}`;

export function registerWebData(app: FastifyInstance, tasks: TaskDb, cfg: Config): void {
  const guard = (req: { headers: Record<string, unknown> }) => {
    const raw = String(req.headers.cookie ?? '');
    const hit = raw.split(';').map(s => s.trim()).find(s => s.startsWith('cd_session='));
    return verifySession(cfg.sessionSecret, hit?.slice('cd_session='.length), Date.now());
  };

  app.get('/web/api/tasks', async (req, reply) => {
    if (!guard(req)) return reply.code(401).send({ error: 'non connecté' });
    return { tasks: tasks.listAll().map(r => ({
      id: r.id, title: r.title, note: r.note, priority: r.priority, done: r.done === 1,
    })) };
  });

  app.post<{ Body: { title?: string; priority?: number } }>('/web/api/tasks', async (req, reply) => {
    if (!guard(req)) return reply.code(401).send({ error: 'non connecté' });
    const title = String(req.body?.title ?? '').trim();
    if (!title) return reply.code(400).send({ error: 'titre vide' });
    if (Buffer.byteLength(title, 'utf8') > MAX_TITLE_BYTES) return reply.code(400).send({ error: 'titre trop long' });
    if (tasks.activeCount() >= MAX_ACTIVE) return reply.code(409).send({ error: 'liste pleine' });
    const id = newWebId();
    const p = Number(req.body?.priority);
    tasks.upsert({ id, title, note: '', priority: p === 0 || p === 2 ? p : 1,
                   done: 0, deleted: 0, updated_at: new Date().toISOString() });
    return { id };
  });

  app.patch<{ Params: { id: string }; Body: { title?: string; note?: string; priority?: number; done?: boolean } }>(
    '/web/api/tasks/:id', async (req, reply) => {
      if (!guard(req)) return reply.code(401).send({ error: 'non connecté' });
      const row = tasks.get(req.params.id);
      if (!row || row.deleted === 1) return reply.code(404).send({ error: 'inconnue' });
      const b = req.body ?? {};
      if (b.note !== undefined && Buffer.byteLength(String(b.note), 'utf8') > MAX_NOTE_BYTES) {
        return reply.code(400).send({ error: 'note trop longue' });
      }
      if (b.title !== undefined && Buffer.byteLength(String(b.title), 'utf8') > MAX_TITLE_BYTES) {
        return reply.code(400).send({ error: 'titre trop long' });
      }
      tasks.upsert({
        ...row,
        title: b.title !== undefined ? String(b.title) : row.title,
        note: b.note !== undefined ? String(b.note) : row.note,
        priority: b.priority === 0 || b.priority === 1 || b.priority === 2 ? b.priority : row.priority,
        done: b.done !== undefined ? (b.done ? 1 : 0) : row.done,
        updated_at: new Date().toISOString(),
      });
      return { ok: true };
    });

  app.delete<{ Params: { id: string } }>('/web/api/tasks/:id', async (req, reply) => {
    if (!guard(req)) return reply.code(401).send({ error: 'non connecté' });
    if (!tasks.get(req.params.id)) return reply.code(404).send({ error: 'inconnue' });
    tasks.markDeleted(req.params.id);
    return { ok: true };
  });

  app.get('/web/api/status', async (req, reply) => {
    if (!guard(req)) return reply.code(401).send({ error: 'non connecté' });
    const p = tasks.getPairing();
    return {
      active: tasks.activeCount(),
      // Le hash ne sort jamais : la page n'a besoin que de l'étiquette et des dates.
      pairing: p ? { label: p.label, paired_at: p.paired_at, last_sync: p.last_sync } : null,
    };
  });
}
```

Call `registerWebData(app, opts.tasks, cfg)` beside the other two in `src/api.ts`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `npm test && npm run typecheck`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/taskApi.ts src/api.ts test/taskApi.test.ts
git commit -m "feat(tasks): browser CRUD API behind the session cookie"
```

---

### Task 7: Templates, stylesheet and Alpine wiring

**Files:**
- Create: `web/templates/list.html`, `web/templates/pair.html`, `web/templates/login.html`
- Create: `web/assets/app.css`
- Create: `web/assets/alpine.min.js` (vendored)
- Create: `src/web.ts`
- Modify: `src/api.ts` (one call), `Dockerfile` (copy `web/`)

**Interfaces:**
- Consumes: the `/web/api/*` routes from Task 6.
- Produces: `registerWeb(app: FastifyInstance, cfg: Config): void` serving
  `GET /`, `GET /pair`, `GET /assets/*`.

**Design rules, copied from the spec and non-negotiable:** priority is carried by
**font weight** (600 / 400 / 300), never a badge or a colour; completed tasks are
collapsed behind a counter; one breakpoint at **860 px**, below which the panes
stack and the compose field moves to the bottom; autosave **1 s** after the last
keystroke; monochrome on paper (`#f7f6f3` ground, `#14130f` ink, `#e6e3dd` rules);
Lexend Deca for UI, Bitter for note bodies.

- [ ] **Step 1: Vendor Alpine**

```bash
mkdir -p web/assets web/templates
curl -sL https://cdn.jsdelivr.net/npm/alpinejs@3.14.9/dist/cdn.min.js -o web/assets/alpine.min.js
test -s web/assets/alpine.min.js && head -c 40 web/assets/alpine.min.js
```

Expected: a non-empty file. It is served locally — **never reference a CDN from
the templates**, so the page has no third-party dependency at load.

- [ ] **Step 2: Write the static-serving module**

Create `src/web.ts`:

```ts
import { createReadStream, existsSync, readFileSync } from 'node:fs';
import { basename, join, resolve } from 'node:path';
import type { FastifyInstance } from 'fastify';
import type { Config } from './config.js';

const WEB_DIR = resolve(process.env.WEB_DIR ?? 'web');
const page = (name: string) => readFileSync(join(WEB_DIR, 'templates', `${name}.html`), 'utf8');

const TYPES: Record<string, string> = { '.css': 'text/css; charset=utf-8', '.js': 'text/javascript; charset=utf-8' };

export function registerWeb(app: FastifyInstance, _cfg: Config): void {
  for (const [route, tpl] of [['/', 'list'], ['/pair', 'pair'], ['/login', 'login']] as const) {
    app.get(route, async (_req, reply) => reply.type('text/html; charset=utf-8').send(page(tpl)));
  }

  app.get<{ Params: { '*': string } }>('/assets/*', async (req, reply) => {
    const name = basename(req.params['*']);            // jamais de chemin venant du client
    const ext = name.slice(name.lastIndexOf('.'));
    if (!TYPES[ext]) return reply.code(404).send({ error: 'inconnu' });
    const path = join(WEB_DIR, 'assets', name);
    if (!existsSync(path)) return reply.code(404).send({ error: 'inconnu' });
    return reply.type(TYPES[ext]).send(createReadStream(path));
  });
}
```

Call `registerWeb(app, cfg)` in `src/api.ts` alongside the others, and widen the
**browser** exemption line from Task 5 with the static routes. Leave Task 4's
`/api/v1/tasks/` line exactly as it is — the two exemptions are separate realms
and must both survive:

```ts
    if (req.url.startsWith('/api/v1/tasks/')) return;              // domaine liseuse (Task 4)
    if (req.url.startsWith('/web/') || req.url.startsWith('/assets/')
        || req.url === '/' || req.url === '/pair' || req.url === '/login') return;
```

In `Dockerfile`, add `COPY web ./web` to the **runtime stage**, immediately after
`COPY --from=build /app/dist ./dist`. It belongs there and not in the build
stage: `web/` is not a build product, so nothing compiles it and the runtime
image is what serves it.

- [ ] **Step 3: Write the stylesheet**

Create `web/assets/app.css`:

```css
@import url('https://fonts.googleapis.com/css2?family=Lexend+Deca:wght@300;400;500;600&family=Bitter:wght@400;600&display=swap');

:root{ --paper:#f7f6f3; --card:#fff; --ink:#14130f; --muted:#7a766e; --rule:#e6e3dd; --tint:#f4f2ee; }
*{ box-sizing:border-box; }
body{ margin:0; background:var(--paper); color:var(--ink); font-family:'Lexend Deca',system-ui,sans-serif; }
[x-cloak]{ display:none !important; }

.top{ height:64px; background:var(--card); border-bottom:1px solid var(--rule);
      display:flex; align-items:center; justify-content:space-between; padding:0 26px; }
.wordmark{ font-size:19px; font-weight:600; }
.wordmark i{ font-style:normal; font-weight:300; color:var(--muted); margin-left:10px; }
.chip{ display:flex; align-items:center; gap:9px; font-size:13px; color:var(--muted);
       border:1px solid var(--rule); border-radius:999px; padding:6px 13px 6px 10px; }
.chip b{ width:7px; height:7px; border-radius:50%; background:var(--ink); display:block; }

.body{ display:flex; height:calc(100vh - 64px); }
.pane-list{ width:452px; flex:0 0 452px; border-right:1px solid var(--rule); background:var(--card);
            display:flex; flex-direction:column; min-height:0; }
.pane-detail{ flex:1; display:flex; flex-direction:column; min-height:0; background:var(--card); }

.addrow{ padding:20px 26px 16px; border-bottom:1px solid #f0eee9; }
.addfield{ display:flex; align-items:center; gap:13px; }
.addfield input{ border:0; outline:0; font:inherit; font-size:15.5px; width:100%; background:transparent; }
.prio{ display:flex; gap:7px; margin:13px 0 0 32px; }
.prio button{ font:inherit; font-size:11.5px; letter-spacing:.09em; text-transform:uppercase; font-weight:500;
              color:var(--muted); background:var(--card); border:1px solid var(--rule);
              border-radius:999px; padding:4px 11px; cursor:pointer; }
.prio button.on{ background:var(--ink); color:#fff; border-color:var(--ink); }

.tasks{ flex:1; overflow-y:auto; padding:8px 0; }
.t{ display:flex; gap:13px; padding:12px 26px; position:relative; align-items:flex-start; cursor:pointer; }
.t .ring{ width:19px; height:19px; border:1.5px solid var(--ink); border-radius:50%;
          flex:0 0 19px; margin-top:3px; cursor:pointer; }
.t .ring.full{ background:var(--ink); }
/* La priorité est portée par la graisse, jamais par une pastille de couleur. */
.t .ttl{ font-size:15.5px; line-height:23px; }
.t.p0 .ttl{ font-weight:600; } .t.p1 .ttl{ font-weight:400; }
.t.p2 .ttl{ font-weight:300; color:#4d4a44; }
.t .exc{ font-family:'Bitter',Georgia,serif; font-size:13px; line-height:20px; color:#8a857c;
         margin-top:4px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
.t.on{ background:var(--tint); }
.t.on::before{ content:""; position:absolute; left:0; top:0; bottom:0; width:3px; background:var(--ink); }
.t.done .ttl{ text-decoration:line-through; color:var(--muted); }

.donebar{ border-top:1px solid var(--rule); padding:15px 26px; display:flex; align-items:center;
          justify-content:space-between; font-size:13.5px; color:var(--muted); background:#fcfbf9; cursor:pointer; }
.donebar em{ font-style:normal; width:19px; height:19px; border-radius:50%; background:var(--ink); color:#fff;
             font-size:11px; display:inline-flex; align-items:center; justify-content:center; margin-right:10px; }

.d-head{ padding:34px 40px 0; }
.d-title{ font-size:29px; line-height:38px; font-weight:600; border:0; outline:0; width:100%;
          font-family:inherit; background:transparent; color:inherit; }
.d-prio{ display:flex; gap:8px; margin-top:20px; }
.d-hair{ height:1px; background:var(--rule); margin:26px 40px 0; }
.d-note{ flex:1; padding:24px 40px; font-family:'Bitter',Georgia,serif; font-size:16.5px; line-height:28px;
         border:0; outline:0; resize:none; background:transparent; color:#23211c; }
.d-foot{ border-top:1px solid var(--rule); padding:14px 40px; display:flex; align-items:center;
         justify-content:space-between; font-size:12.5px; color:#8a857c; background:#fcfbf9; }

@media (max-width: 860px){
  .body{ display:block; height:auto; }
  .pane-list{ width:100%; flex:none; border-right:0; height:calc(100vh - 64px); }
  /* Le détail devient un calque plein écran, pas un volet. */
  .pane-detail{ position:fixed; inset:0; z-index:10; }
  .addrow{ order:99; position:fixed; left:0; right:0; bottom:0; background:var(--card);
           border-top:1px solid var(--rule); border-bottom:0; padding-bottom:26px; }
  .tasks{ padding-bottom:132px; }
  .d-head{ padding:20px 22px 0; } .d-note{ padding:22px 22px; } .d-foot{ padding:13px 22px 26px; }
}
```

- [ ] **Step 4: Write the list template**

Create `web/templates/list.html`:

```html
<!doctype html>
<html lang="fr"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Tâches — CrossDrop</title>
<link rel="stylesheet" href="/assets/app.css">
<script defer src="/assets/alpine.min.js"></script>
</head><body>
<div x-data="taskApp()" x-init="load()" x-cloak>
  <div class="top">
    <div class="wordmark">Tâches<i>crossdrop</i></div>
    <div style="display:flex;align-items:center;gap:22px">
      <div class="chip" x-show="status.pairing"><b></b>
        <span x-text="status.pairing ? status.pairing.label : ''"></span>
      </div>
      <a href="/pair" style="font-size:13.5px;color:inherit">Appairer</a>
    </div>
  </div>

  <div class="body">
    <div class="pane-list">
      <div class="addrow">
        <div class="addfield">
          <span class="ring" style="width:19px;height:19px;border:1.5px dashed #b8b2a7;border-radius:50%"></span>
          <input placeholder="Ajouter une tâche…" x-model="draft" @keydown.enter="add()">
        </div>
        <div class="prio">
          <template x-for="(lbl, p) in ['Haute','Normale','Basse']" :key="p">
            <button :class="draftPrio === p ? 'on' : ''" @click="draftPrio = p" x-text="lbl"></button>
          </template>
        </div>
      </div>

      <div class="tasks">
        <template x-for="t in open" :key="t.id">
          <div class="t" :class="['p' + t.priority, sel && sel.id === t.id ? 'on' : '']" @click="select(t)">
            <span class="ring" @click.stop="toggle(t)"></span>
            <div>
              <div class="ttl" x-text="t.title"></div>
              <div class="exc" x-show="t.note" x-text="t.note"></div>
            </div>
          </div>
        </template>
      </div>

      <div class="donebar" @click="showDone = !showDone">
        <span><em x-text="done.length"></em> faites</span>
        <span x-text="showDone ? '▴' : '▾'"></span>
      </div>
      <div x-show="showDone" style="max-height:34vh;overflow-y:auto">
        <template x-for="t in done" :key="t.id">
          <div class="t done" @click="select(t)">
            <span class="ring full" @click.stop="toggle(t)"></span>
            <div class="ttl" x-text="t.title"></div>
          </div>
        </template>
      </div>
    </div>

    <div class="pane-detail" x-show="sel">
      <div class="d-head">
        <input class="d-title" x-model="sel.title" @input="queueSave()">
        <div class="d-prio prio">
          <template x-for="(lbl, p) in ['Haute','Normale','Basse']" :key="p">
            <button :class="sel.priority === p ? 'on' : ''" @click="sel.priority = p; queueSave()" x-text="lbl"></button>
          </template>
        </div>
      </div>
      <div class="d-hair"></div>
      <textarea class="d-note" x-model="sel.note" @input="queueSave()"
                placeholder="Note… elle se lira sur la liseuse."></textarea>
      <div class="d-foot">
        <span x-text="saved ? 'Enregistré · se lira en Lexend 10 sur la liseuse' : 'Modification en cours…'"></span>
        <a href="#" @click.prevent="remove()" style="color:inherit">Supprimer</a>
      </div>
    </div>
  </div>
</div>

<script>
function taskApp() {
  return {
    tasks: [], sel: null, draft: '', draftPrio: 1, showDone: false, saved: true, status: {}, timer: null,
    get open() { return this.tasks.filter(t => !t.done); },
    get done() { return this.tasks.filter(t => t.done); },
    async load() {
      this.tasks = (await (await fetch('/web/api/tasks')).json()).tasks;
      this.status = await (await fetch('/web/api/status')).json();
      if (!this.sel) this.sel = this.open[0] ?? null;
    },
    select(t) { this.sel = t; },
    async add() {
      const title = this.draft.trim();
      if (!title) return;
      const res = await fetch('/web/api/tasks', { method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ title, priority: this.draftPrio }) });
      if (!res.ok) { alert(res.status === 409 ? 'Liste pleine (120 tâches).' : 'Échec.'); return; }
      this.draft = ''; await this.load();
    },
    async toggle(t) {
      t.done = !t.done;
      await fetch('/web/api/tasks/' + t.id, { method: 'PATCH',
        headers: { 'content-type': 'application/json' }, body: JSON.stringify({ done: t.done }) });
    },
    // Autosave 1 s après la dernière frappe : ni mode, ni bouton « enregistrer ».
    queueSave() {
      this.saved = false;
      clearTimeout(this.timer);
      this.timer = setTimeout(async () => {
        await fetch('/web/api/tasks/' + this.sel.id, { method: 'PATCH',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify({ title: this.sel.title, note: this.sel.note, priority: this.sel.priority }) });
        this.saved = true;
      }, 1000);
    },
    async remove() {
      if (!confirm('Supprimer cette tâche ?')) return;
      await fetch('/web/api/tasks/' + this.sel.id, { method: 'DELETE' });
      this.sel = null; await this.load();
    },
  };
}
</script>
</body></html>
```

- [ ] **Step 5: Write the pairing and login templates**

Create `web/templates/pair.html`:

```html
<!doctype html>
<html lang="fr"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Appairer une liseuse — CrossDrop</title>
<link rel="stylesheet" href="/assets/app.css">
<script defer src="/assets/alpine.min.js"></script>
</head><body>
<div x-data="pairApp()" x-init="load()" x-cloak>
  <div class="top">
    <div class="wordmark">Tâches<i>crossdrop</i></div>
    <a href="/" style="font-size:13.5px;color:inherit">← Retour aux tâches</a>
  </div>
  <div style="max-width:620px;margin:0 auto;padding:46px 22px">
    <h1 style="font-size:31px;font-weight:600;margin:0">Appairer une liseuse</h1>
    <p style="font-size:15px;line-height:25px;color:var(--muted)">
      La liseuse fabrique elle-même son secret et te le montre. Rien n'est tapé, ni d'un côté ni de l'autre.
      Ouvre <strong>Réglages → Tâches → Appairer</strong> sur la liseuse, puis vise le QR.
    </p>

    <div style="margin-top:26px">
      <video x-ref="cam" style="width:100%;max-height:300px;background:#14130f;border-radius:10px" muted playsinline></video>
      <p style="font-size:12.5px;color:var(--muted)" x-text="camMsg"></p>
    </div>

    <div style="margin-top:22px">
      <div style="font-size:12px;letter-spacing:.14em;text-transform:uppercase;color:#a5a096">ou colle le code</div>
      <input x-model="code" placeholder="K7M2 P9XQ 4VTB …" style="width:100%;margin-top:10px;padding:15px 17px;
             font:inherit;font-size:19px;letter-spacing:.11em;border:1px solid #d9d5cd;border-radius:7px">
      <button @click="pair()" style="margin-top:12px;font:inherit;padding:11px 20px;border-radius:999px;
              border:0;background:var(--ink);color:#fff;cursor:pointer">Appairer</button>
      <p style="font-size:12.5px;color:var(--muted);line-height:20px">
        128 bits tirés du générateur matériel de la liseuse. Il ne quitte jamais l'appareil autrement que par ce QR.
      </p>
    </div>

    <div x-show="status.pairing" style="margin-top:30px;border:1px solid var(--rule);border-radius:9px;
         padding:18px 20px;display:flex;align-items:center;justify-content:space-between">
      <div>
        <div style="font-size:15px;font-weight:500" x-text="status.pairing && status.pairing.label"></div>
        <div style="font-size:12.5px;color:var(--muted)"
             x-text="status.pairing ? 'Appairée le ' + status.pairing.paired_at.slice(0,10)
                     + ' · dernière sync ' + (status.pairing.last_sync ? status.pairing.last_sync.slice(0,16).replace('T',' ') : 'jamais') : ''"></div>
      </div>
      <a href="#" @click.prevent="unpair()" style="font-size:13px;color:var(--muted)">Révoquer</a>
    </div>
  </div>
</div>

<script>
function pairApp() {
  return {
    code: '', status: {}, camMsg: 'Démarrage de la caméra…',
    async load() { this.status = await (await fetch('/web/api/status')).json(); this.startCam(); },
    async startCam() {
      // getUserMedia et BarcodeDetector exigent une origine sécurisée : derrière
      // Caddy c'est acquis, en HTTP local le champ manuel reste le seul chemin.
      if (!window.isSecureContext) { this.camMsg = 'Caméra indisponible hors HTTPS — utilise le code.'; return; }
      if (!('BarcodeDetector' in window)) { this.camMsg = 'Ce navigateur ne sait pas lire un QR — utilise le code.'; return; }
      try {
        const stream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: 'environment' } });
        this.$refs.cam.srcObject = stream; await this.$refs.cam.play();
        this.camMsg = 'Vise le QR de la liseuse.';
        const det = new BarcodeDetector({ formats: ['qr_code'] });
        const tick = async () => {
          try {
            const hits = await det.detect(this.$refs.cam);
            if (hits.length) { this.code = hits[0].rawValue; await this.pair(); return; }
          } catch (e) { /* image pas encore prête */ }
          requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
      } catch (e) { this.camMsg = 'Caméra refusée — utilise le code.'; }
    },
    async pair() {
      const res = await fetch('/web/pair', { method: 'POST', headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ secret: this.code, label: 'Xteink X4' }) });
      if (!res.ok) { alert('Code invalide.'); return; }
      this.code = ''; await this.load();
    },
    async unpair() {
      if (!confirm('Révoquer cette liseuse ?')) return;
      await fetch('/web/unpair', { method: 'POST' }); await this.load();
    },
  };
}
</script>
</body></html>
```

Create `web/templates/login.html`:

```html
<!doctype html>
<html lang="fr"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>Connexion — CrossDrop</title>
<link rel="stylesheet" href="/assets/app.css">
</head><body>
<div style="max-width:340px;margin:18vh auto;padding:0 22px">
  <div style="font-size:19px;font-weight:600">Tâches</div>
  <form method="dialog" onsubmit="return false" style="margin-top:22px">
    <input id="pw" type="password" placeholder="Mot de passe" autofocus
           style="width:100%;padding:13px 15px;font:inherit;border:1px solid #d9d5cd;border-radius:7px">
    <button onclick="go()" style="width:100%;margin-top:12px;padding:12px;font:inherit;border:0;
            border-radius:7px;background:#14130f;color:#fff;cursor:pointer">Entrer</button>
    <p id="err" style="color:#8a857c;font-size:13px"></p>
  </form>
</div>
<script>
async function go() {
  const res = await fetch('/web/login', { method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ password: document.getElementById('pw').value }) });
  if (res.ok) location.href = '/';
  else document.getElementById('err').textContent = 'Mot de passe invalide.';
}
</script>
</body></html>
```

- [ ] **Step 6: Wire the TaskDb into the process**

In `src/index.ts`, after the existing `const db = new Db(...)` line, add:

```ts
import Database from 'better-sqlite3';
import { TaskDb } from './taskDb.js';
// …
const tasksDbFile = new Database(join(cfg.dataDir, 'tasks.db'));
// Meme journal que la base des articles (src/db.ts) : sans WAL, un lecteur
// bloque un ecrivain, et les deux bases du service n'auraient pas la meme
// garantie de durabilite pour aucune raison.
tasksDbFile.pragma('journal_mode = WAL');
const tasks = new TaskDb(tasksDbFile);
```

and pass it to `buildServer`:

```ts
const app = await buildServer(db, cfg, { refresh: () => loop.pollFresh(), tasks });
```

- [ ] **Step 7: Verify the whole thing runs**

```bash
npm run typecheck && npm test
DEVICE_TOKEN=$(openssl rand -hex 24) RAINDROP_TOKEN=x DOMAIN=localhost \
  DATA_DIR=./data WEB_PASSWORD=test SESSION_SECRET=$(openssl rand -hex 32) \
  npx tsx src/index.ts &
sleep 2
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:3000/
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:3000/assets/app.css
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:3000/web/api/tasks
kill %1
```

Expected: `200`, `200`, `401` — the last one proves the browser API is closed
without a session.

- [ ] **Step 8: Commit**

```bash
git add web src/web.ts src/api.ts src/index.ts Dockerfile
git commit -m "feat(tasks): responsive web UI on Alpine, served without a build step"
```

---

### Task 8: Tombstone purge on the existing timer

**Files:**
- Modify: `src/index.ts`
- Modify: `test/taskDb.test.ts` (one test)

**Interfaces:**
- Consumes: `TaskDb.purgeTombstones` (Task 1).
- Produces: nothing new; a daily `setInterval` in the process.

- [ ] **Step 1: Write the failing test**

Append to `test/taskDb.test.ts`:

```ts
describe('horizon de purge', () => {
  it('purge à 30 jours et remonte le plancher au seq purgé le plus haut', () => {
    add('w00000001', { updated_at: '2026-08-01T00:00:00.000Z' });
    db.markDeleted('w00000001');
    const purgedSeq = db.get('w00000001')!.seq;
    add('w00000002');
    db.purgeTombstones('2026-09-01T00:00:00.000Z');
    expect(db.tombstoneFloor()).toBe(purgedSeq);
    expect(db.get('w00000002')).toBeDefined();
  });
});
```

- [ ] **Step 2: Run the test**

Run: `npx vitest run test/taskDb.test.ts`
Expected: PASS (Task 1 already implemented `purgeTombstones`; this pins the floor
semantics the `reset` path depends on). If it fails, fix `purgeTombstones` — the
floor must be the highest purged `seq`, not the count.

- [ ] **Step 3: Schedule it**

In `src/index.ts`, after `loop.start();`:

```ts
// Les tombstones ne servent qu'à faire découvrir les suppressions aux liseuses.
// Passé 30 jours, une liseuse restée éteinte repart d'un instantané (header reset).
const purgeTimer = setInterval(() => {
  const cutoff = new Date(Date.now() - 30 * 86_400_000).toISOString();
  tasks.purgeTombstones(cutoff);
}, 86_400_000);
purgeTimer.unref();
```

- [ ] **Step 4: Verify**

Run: `npm run typecheck && npm test`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/index.ts test/taskDb.test.ts
git commit -m "feat(tasks): purge tombstones daily and keep the reset floor honest"
```

---

### Task 9: Deployment notes

**Files:**
- Modify: `README.md`
- Modify: `docker-compose.yml` (no new service; one env passthrough)

- [ ] **Step 1: Document the new endpoints and variables**

Append to the `## API` section of `README.md`:

```markdown
### Liste de tâches

Domaine liseuse (`Authorization: Bearer <secret d'appairage>`) :

- `POST /api/v1/tasks/sync` — curseur + file d'ops dans un sens, NDJSON dans
  l'autre. Une note n'est pas une chaîne JSON : quand `noteBytes > 0`, ce nombre
  d'octets bruts suit la ligne de métadonnées, puis un `\n` non compté.

Domaine navigateur (cookie de session) :

- `GET /` — la liste · `GET /pair` — appairage · `GET /login`
- `GET|POST /web/api/tasks`, `PATCH|DELETE /web/api/tasks/:id`, `GET /web/api/status`
- `POST /web/login`, `POST /web/pair`, `POST /web/unpair`

Variables : `WEB_PASSWORD` (sans elle l'interface web répond 503) et
`SESSION_SECRET` (`openssl rand -hex 32`). Les tâches vivent dans
`$DATA_DIR/tasks.db`, à sauvegarder comme `crossdrop.db`.
```

- [ ] **Step 2: Pass the variables through compose**

In `docker-compose.yml`, add to the app service's `environment:` block:

```yaml
      - WEB_PASSWORD=${WEB_PASSWORD}
      - SESSION_SECRET=${SESSION_SECRET}
```

- [ ] **Step 3: Verify the image still builds**

Run: `docker compose build`
Expected: build succeeds and `web/` is present in the image
(`docker compose run --rm app ls web/templates`).

- [ ] **Step 4: Commit**

```bash
git add README.md docker-compose.yml
git commit -m "docs: document the task list endpoints and its two env vars"
```

---

## Done when

- `npm test` is green, including every pre-existing article test.
- `POST /api/v1/tasks/sync` answers a bearer-authenticated request with a valid
  framed body, and 401s without one.
- The web UI lists, creates, edits, completes and deletes tasks, and collapses
  completed ones behind a counter.
- The layout stacks below 860 px with the compose field at the bottom.
- `/pair` stores a scanned secret and shows the paired device; revoking clears it.
- The firmware plan (`2026-09-22-crosstasks-firmware.md`) can be started: its only
  dependency is the framing contract in Task 4.
