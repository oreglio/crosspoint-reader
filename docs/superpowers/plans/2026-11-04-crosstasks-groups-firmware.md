# CrossTasks — Groupes, firmware — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recevoir les groupes du serveur (sync schema 2), les afficher en sections sur la liste des tâches, sauter de groupe en groupe, ajouter une tâche dans un groupe, et changer le groupe d'une tâche depuis la liseuse.

**Architecture:** `TaskRecord` porte l'id de son groupe ; `TaskStore` porte un petit tableau `TaskGroup` (≤ 16), persisté dans `index.json`, chargé/déchargé avec l'écran. Le lecteur de sync accepte schema 1 et 2. Le modèle pur ordonne par groupe et produit des lignes `GroupHeader`. L'écran gère les titres de groupe comme des lignes sélectionnables.

**Tech Stack:** C++20, ESP32-C3/S3, ArduinoJson (`PersistableStore`), `StreamingJsonParser`, FreeInkUI, GoogleTest, simulateur PlatformIO, e2e Python.

**Spec:** `docs/superpowers/specs/2026-11-04-crosstasks-groups-design.md` — Partie 2 (liseuse, synchro, budget mémoire).
**Prérequis :** `docs/superpowers/plans/2026-11-04-crosstasks-done-header.md` livré (cet écran consomme `TaskListView`, `view`, `headerFocused`, `switchView()`), et le plan serveur `crossdrop/docs/superpowers/plans/2026-11-04-crosstasks-groups-server.md` Task 4 (contrat schema 2) disponible pour l'e2e.

## Global Constraints

- Contrat serveur schema 2 (plan serveur, Task 4), recopié : ligne de groupe `{"group":"g1a2b3c4","name":"MAISON","position":2}`, tombstone `{"group":"g1a2b3c4","deleted":true}` (jamais de clé `id`, jamais de note) ; ligne de tâche + `"group":"g…"` optionnelle ; op `add` + `"group"` optionnel ; op `{"op":"group","id":"…","group":"g…"|null}`.
- `MAX_GROUPS = 16`, `TASK_GROUP_NAME_MAX = 32` octets ; id de groupe `'g'` + 8 hex (`TASK_ID_LEN`).
- Budget : ≤ 2 Ko de RAM en plus à l'écran Tasks ; rien de résident hors de l'écran ; aucun coût au démarrage.
- La liseuse accepte une réponse `schema: 1` (serveur pas à jour) et `schema: 2`.
- Première sync en schema 2 : effacer le curseur stocké avant la requête ; le marqueur « groupes synchronisés » n'est posé qu'après une sync complète dont l'en-tête dit `schema: 2`.
- Tâche dont le groupe est inconnu (pas encore reçu, ou au-delà des 16) : affichée sans groupe.
- Libellés de pied en minuscules, ≤ ~70 px en `inter_8_regular` ; `tr(STR_*)` partout ; clés dans `english.yaml` et `french.yaml`.
- Le nom du groupe s'affiche tel que reçu (pas de mise en capitales).

## Review Focus

- Une tâche arrive (page 1) avant son groupe (page 2) : visible sans groupe après la page 1, rangée après la page 2 (test dans Task 2).
- Tombstone de groupe reçu alors que ses tâches libérées ne sont pas encore arrivées : aucune tâche ne garde l'id mort (test dans Task 1).
- 17e groupe reçu : ignoré avec `LOG_ERR`, ses tâches sans groupe (test dans Task 1).
- Op `group` en file vers un groupe supprimé ensuite sur le web : sync OK, tâche sans groupe (e2e, Task 5).
- Mode sélection : Haut/Bas ne s'arrêtent jamais sur un titre de groupe ; Gauche/Droite gardent effacer/cocher (test dans Task 3).

---

### Task 1: Types et persistance des groupes

**Files:**
- Modify: `src/tasks/TaskRecord.h`
- Modify: `src/TaskStore.h`, `src/TaskStore.cpp` (`toJson`/`fromJson`, `unload`, `replaceAll`)
- Test: `test/task_record/TaskRecordTest.cpp` (et le test de persistance existant du store s'il y en a un ; sinon un test hôte de `toJson`/`fromJson` dans `test/task_record`)

**Interfaces:**
- Produces:
  - `inline constexpr size_t MAX_GROUPS = 16; inline constexpr size_t TASK_GROUP_NAME_MAX = 32;`
  - `TaskRecord::group[TASK_ID_LEN + 1]` (chaîne vide = sans groupe)
  - `struct TaskGroup { char id[TASK_ID_LEN + 1]; char name[TASK_GROUP_NAME_MAX + 1]; uint16_t position; };`
  - `TaskStore` : `const std::vector<TaskGroup>& groups() const;` `bool upsertGroup(const TaskGroup& g);` (refuse et `LOG_ERR` au-delà de 16 nouveaux) ; `void removeGroup(const char* id);` (retire le groupe **et** vide `group` des tâches qui le portent) ; `const TaskGroup* findGroup(const char* id) const;` ; `bool groupsSynced() const;` `void setGroupsSynced(bool);`
  - `index.json` : clé `"g"` par tâche (absente si vide), tableau `"groups": [{"id","name","pos"}]`, booléen `"gs"`. Un index sans ces clés se charge (tâches sans groupe, `gs = false`).
  - `unload()` vide et `shrink_to_fit()` aussi le vecteur de groupes.

- [ ] **Step 1: Tests** — aller-retour JSON avec groupes ; ancien index sans `g`/`groups`/`gs` ; `removeGroup` vide les tâches ; 17e `upsertGroup` refusé ; `upsertGroup` d'un id existant met à jour nom et position sans compter dans la limite.
- [ ] **Step 2: Run** `cmake --build build/test --target TaskRecordTest && ctest --test-dir build/test -R TaskRecord --output-on-failure` → FAIL.
- [ ] **Step 3: Implémenter.** `std::vector<TaskGroup>` réservé à `MAX_GROUPS` au chargement (≈ 16 × 46 o).
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(tasks): a group per task and the group list in the task index`

---

### Task 2: Sync schema 2 — lecture, ops, premier passage

**Files:**
- Modify: `src/tasks/TaskSyncReader.h`, `src/tasks/TaskSyncReader.cpp` (en-tête :337, `parseTaskLine`)
- Modify: `src/tasks/TaskOpQueue.h`, `src/tasks/TaskOpQueue.cpp` (`TaskOpKind`, `TaskOp`, sérialisation/lecture des lignes, en-tête de requête :302, constante de pire ligne)
- Modify: `src/activities/tasks/TaskSyncActivity.cpp` (callbacks, curseur :395-446)
- Test: `test/task_sync_reader/`, `test/task_op_queue/TaskOpQueueTest.cpp`

**Interfaces:**
- Consumes: Task 1.
- Produces:
  - `inline constexpr long TASK_SYNC_SCHEMA_MIN = 1, TASK_SYNC_SCHEMA_MAX = 2, TASK_SYNC_REQUEST_SCHEMA = 2;` (remplacent `TASK_SYNC_SCHEMA`)
  - `TaskSyncCallbacks` : `onHeader(void* ctx, const char* cursor, bool more, bool reset, long schema)` ; nouveaux `void (*onGroup)(void* ctx, const TaskGroup& g);` et `void (*onGroupDeleted)(void* ctx, const char* id);`
  - Une ligne avec clé `group` et sans clé `id` est une ligne de groupe ; sur une ligne de tâche, `group` remplit `TaskRecord::group` (id mal formé → vide).
  - `TaskOpKind::Group` ; `TaskOp::group[TASK_ID_LEN + 1]` ; `add` sérialise `"group"` si non vide ; `Group` sérialise `"group":null` si vide.
  - `TaskSyncActivity` : avant la première requête, si `!TASK_STORE.groupsSynced()` → `clearCursor()` ; `onGroup` → `upsertGroup` ; `onGroupDeleted` → `removeGroup` ; à la fin d'une sync complète avec `schema == 2` → `setGroupsSynced(true)` puis `saveIndex()`.

- [ ] **Step 1: Tests lecteur** — en-tête schema 1 et 2 acceptés, 3 refusé ; ligne de groupe vivante et tombstone ; tâche avec `group` ; tâche puis groupe dans deux `feed` successifs (Review Focus) ; nom de 32 octets échappé tient dans `LINE_BUF_SIZE`.
- [ ] **Step 2: Tests file d'ops** — aller-retour `add` avec/sans groupe, `group` avec id et avec `null` ; en-tête de requête `{"schema":2,` ; la pire ligne recalculée tient dans le tampon de relecture.
- [ ] **Step 3: Run** `ctest --test-dir build/test -R "TaskSyncReader|TaskOpQueue" --output-on-failure` → FAIL.
- [ ] **Step 4: Implémenter.**
- [ ] **Step 5: Run** → PASS ; `pio run -e default`.
- [ ] **Step 6: Commit** `feat(tasks): sync groups with schema 2 servers, still accepting schema 1`

---

### Task 3: Modèle — ordre par groupe, titres, sauts

**Files:**
- Modify: `src/activities/tasks/TaskListModel.h`, `.cpp`
- Test: `test/task_list_model/TaskListModelTest.cpp`

**Interfaces:**
- Consumes: `TaskGroup`, `TaskRecord::group` (Task 1) ; `TaskListView` (plan done-header).
- Produces:
  - `TaskRowKind::GroupHeader` ; `TaskListRow::groupIndex` (indice dans `groups`, -1 sinon).
  - `void buildTaskOrder(const std::vector<TaskRecord>& records, const std::vector<TaskGroup>& groups, std::vector<int>& order, int& openCount);` — ouvertes : rang de groupe (sans groupe ou inconnu = avant tout, puis `position`, puis id du groupe), puis `taskOrderBefore` ; faites : `taskOrderBefore` seul, groupe ignoré.
  - `buildTaskListRows(order, openCount, view, records, groups, rows)` — vue Open : tâches sans groupe sans titre, puis pour chaque groupe ayant ≥ 1 tâche ouverte un `GroupHeader` suivi de ses tâches ; vue Done inchangée (aucun titre).
  - `int taskListGroupJump(const std::vector<TaskListRow>& rows, int selected, int direction);` — indice du `GroupHeader` précédent (`-1`) ou suivant (`+1`) depuis `selected`, `-1` s'il n'y en a pas (pas de bouclage).
  - `int taskListStepSkippingHeaders(const std::vector<TaskListRow>& rows, int selected, int direction);` — pour le mode sélection, avec bouclage comme `ButtonNavigator`.
  - `int taskGroupOpenCount(const std::vector<TaskListRow>& rows, int headerRow);` — tâches qui suivent le titre jusqu'au prochain titre.

- [ ] **Step 1: Tests** — ordre sans groupe → groupes par position → priorité ; groupe inconnu traité comme sans groupe ; groupe sans tâche ouverte : pas de titre ; faites ignorent le groupe ; `taskListGroupJump` aux deux bouts ; `taskListStepSkippingHeaders` ne rend jamais un titre (Review Focus) ; compteur par groupe.
- [ ] **Step 2: Run** `ctest --test-dir build/test -R TaskListModel --output-on-failure` → FAIL.
- [ ] **Step 3: Implémenter.** Rang de groupe calculé une fois par enregistrement avant le tri (tableau local `uint8_t` de `records.size()` sur la pile ou réutilisé ; ≤ 120 o) plutôt qu'une recherche dans le comparateur.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(tasks): order open tasks by group with a selectable title per group`

---

### Task 4: Écran — titres de groupe, ajouter dedans, changer de groupe

**Files:**
- Modify: `src/activities/tasks/TaskListActivity.h`, `.cpp` (`rebuildOrder`, `buildRows`, `activateIndex`, `handleCustomInput`, `navigateButtons`, `drawFooter`, `openTaskMenu` :444-520, `createTask`, `askPriorityForNewTask` :1025)
- Modify: `src/activities/tasks/TaskDetailActivity.cpp` (flux « modifier » : titre → priorité → groupe)
- Modify: `lib/I18n/translations/english.yaml`, `french.yaml`
- Modify: `scripts/run_simulator_tasks_tour.py`, `src/simulator/SimulatorTasksTour.cpp` (jeu de données avec groupes)

**Interfaces:**
- Consumes: Tasks 1-3 ; `view`, `headerFocused`, `switchView()` (plan done-header).
- Produces : `void createTask(const char* groupId = nullptr);` ; `void chooseGroup(const std::string& taskId);` (popup « Sans groupe » + groupes, puis op `Group`, `upsert`, `refreshAfterEdit`).

- [ ] **Step 1: Clés i18n** — `STR_TASK_PREV_GROUP` « previous » / « précédent » ; `STR_TASK_NEXT_GROUP` « next » / « suivant » ; `STR_TASK_MENU_GROUP` « Group » / « Groupe » ; `STR_TASK_NO_GROUP` « No group » / « Sans groupe ». « ajouter » réutilise `STR_TASK_NEW_SHORT`. Run `python3 scripts/gen_i18n.py`, puis `python3 scripts/measure_label.py inter_8_regular "précédent" --max 70` → OK (sinon abréger, ex. « préc. »).

- [ ] **Step 2: Rendu du titre de groupe** dans `buildRows` — pas de pastille, nom en gras, nombre de tâches ouvertes à droite (`taskGroupOpenCount`), filet dessous. Plus d'air au-dessus qu'entre deux tâches : si `fui::ListProps` n'accepte pas de hauteur par ligne (vérifier `freeink-sdk` `list.h`), aligner le texte en bas de la ligne pour que l'air soit au-dessus. Libellés dans un tampon membre fixe (un par ligne visible, `winItems`), pas de `std::string` par ligne.

- [ ] **Step 3: Boutons sur un titre** — Confirmer (et toucher) → `createTask(groupId)` ; Gauche/Droite → `taskListGroupJump(-1/+1)` (rien si -1) ; appui long = Confirmer. `drawFooter` : « ajouter » / « précédent » / « suivant », case vide au bout. En mode sélection, `navigateButtons` utilise `taskListStepSkippingHeaders`.

- [ ] **Step 4: Ajout dans un groupe** — `createTask(groupId)` transmet l'id jusqu'à `askPriorityForNewTask` ; l'enregistrement et l'op `Add` portent `group`. Gauche « Nouv. » sur une tâche → sans groupe (inchangé).

- [ ] **Step 5: Changer de groupe** — entrée « Groupe » dans `openTaskMenu` (après « Modifier »), seulement si `TASK_STORE.groups()` n'est pas vide ; troisième étape du flux « modifier » de `TaskDetailActivity`, même condition.

- [ ] **Step 6: Build, analyse, tournée**

Run: `pio run -e simulator && pio run -e default && pio run -e x4-pro && pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high`, puis `python3 scripts/run_simulator_tasks_tour.py` → inspecter FR/EN, trois thèmes : air au-dessus des titres, compteur aligné, « précédent » non tronqué.

- [ ] **Step 7: Commit** `feat(tasks): group titles on the reader, add into a group, move a task between groups`

---

### Task 5: E2E, docs, vérification matérielle

**Files:**
- Modify: `scripts/run_simulator_tasks_sync_e2e.py`
- Modify: `docs/crosstasks.md`, `CHANGELOG.md` (Added)

- [ ] **Step 1: Scénarios e2e** (serveur `crossdrop` de la branche `feat/crosstasks-groups` en local) : (a) premier passage schema 2 avec un curseur existant → instantané complet, groupes reçus, ops en file envoyées ; (b) groupe renommé puis supprimé sur le serveur → titres mis à jour, tâches remontées sans groupe ; (c) op `group` vers un groupe supprimé entre-temps → sync OK, tâche sans groupe (Review Focus) ; (d) serveur en schema 1 (ancien commit) → sync OK, `gs` reste faux.
Run: `python3 scripts/run_simulator_tasks_sync_e2e.py` → tous les scénarios PASS.

- [ ] **Step 2: Docs** — `docs/crosstasks.md` : section « Groups » (sans groupe en haut, titres, boutons ajouter/précédent/suivant, menu « Group », groupes gérés sur le web) et le contrat schema 2 ; `CHANGELOG.md`. `clang-format -i` sur les fichiers C++ touchés.

- [ ] **Step 3: Commit** `docs(tasks): document task groups and the schema 2 sync`

- [ ] **Step 4: Vérification matérielle (X4, après déploiement serveur)** — flasher `default` ; heap libre (`ESP.getFreeHeap()`, `getMaxAllocHeap()` en log) à l'entrée et à la sortie de Tasks : écart ≤ 2 Ko par rapport à avant, et identique à l'arrondi près après sortie. Sync : groupes affichés dans l'ordre du web ; Droite/Gauche sautent de titre en titre ; Confirmer sur ADMINISTRATIF ajoute dedans ; menu « Groupe » déplace une tâche ; sync suivante : le web reflète le déplacement.
