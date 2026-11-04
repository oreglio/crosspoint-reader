# CrossTasks — « N faites » dans l'en-tête — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sur la liseuse, remplacer l'accordéon « N faites » du bas de liste par un compteur à droite de l'en-tête, atteignable par Haut, qui bascule la liste entre tâches ouvertes et tâches faites.

**Architecture:** Firmware seul, aucune dépendance serveur. Le modèle pur (`TaskListModel`) passe d'un booléen `showDone` (accordéon) à une vue exclusive `TaskListView { Open, Done }`. L'écran (`TaskListActivity`) gagne un état « focus sur l'en-tête » hors de la liste fui, géré dans `navigateButtons()` et dessiné dans `drawChrome()`.

**Tech Stack:** C++20, ESP32-C3/S3, freeink-sdk (FreeInkUI), GoogleTest (CMake/CTest), simulateur PlatformIO.

**Spec:** `docs/superpowers/specs/2026-11-04-crosstasks-groups-design.md` — Partie 1.

## Global Constraints

- C3 d'abord : pas de PSRAM, pas de tactile supposés ; aucune allocation qui survit à `onExit()`.
- Tout libellé visible passe par `tr(STR_*)`, avec la clé dans `lib/I18n/translations/english.yaml` **et** `french.yaml` ; ne pas éditer `lib/I18n/I18nKeys.h`/`I18nStrings.*` (générés par `scripts/gen_i18n.py`).
- Libellés de pied en minuscules, comme l'existant (`ajouter`, `déplier`) ; chaque case ≤ ~70 px en `inter_8_regular` (`scripts/measure_label.py`) ; libellé droit d'en-tête ≤ 200 px en `inter_8_regular`.
- Le web ne change pas dans ce plan.
- Branche : `feat/crosstasks-groups` depuis `main`, commits `<type>: <résumé>`.

## Review Focus

- Toutes les tâches cochées d'un coup (0 ouverte, N faites) : la vue ouverte montre la ligne « + Ajouter une tâche », le compteur reste atteignable.
- Dernière tâche faite décochée depuis la vue « faites » : retour automatique à la vue ouverte, curseur valide (jamais -1 sur une liste non vide).
- Bascule de vue en mode sélection : les tâches qui ne sont plus visibles sont retirées de la sélection, le compteur « Sélection (N) » se met à jour.
- Ops en attente + faites : « 3 faites ↻ » tient en largeur en français, sur les trois thèmes.
- Appareil tactile (`sticky`/`x4-pro`) : toucher le compteur bascule ; toucher une ligne ne bascule jamais.

---

### Task 1: Modèle — vue exclusive Open / Done

**Files:**
- Modify: `src/activities/tasks/TaskListModel.h` (enum `TaskRowKind`, `buildTaskListRows`, commentaires d'en-tête)
- Modify: `src/activities/tasks/TaskListModel.cpp:19-41`
- Test: `test/task_list_model/TaskListModelTest.cpp`

**Interfaces:**
- Produces:
  - `enum class TaskRowKind : uint8_t { Task, AddTask };` (suppression de `DoneSection`)
  - `enum class TaskListView : uint8_t { Open, Done };`
  - `void buildTaskListRows(const std::vector<int>& order, int openCount, TaskListView view, std::vector<TaskListRow>& rows);`

- [ ] **Step 1: Réécrire les tests `BuildTaskListRows*` existants pour la nouvelle signature, et ajouter :**

```cpp
TEST(BuildTaskListRows, OpenViewListsOnlyOpenTasks) {
  // order = {2, 0, 1}, openCount = 2  -> rows = Task(2), Task(0) ; aucune ligne pour 1 (faite)
}
TEST(BuildTaskListRows, DoneViewListsOnlyDoneTasksInOrder) {
  // order = {2, 0, 1}, openCount = 1  -> rows = Task(0), Task(1)
}
TEST(BuildTaskListRows, EmptyIndexIsSingleAddRowInBothViews) {
  // order = {}, openCount = 0 -> Open : {AddTask} ; Done : {AddTask}
}
TEST(BuildTaskListRows, AllDoneOpenViewShowsAddRow) {
  // order = {0, 1}, openCount = 0, Open -> {AddTask}
}
TEST(BuildTaskListRows, DoneViewWithNoDoneTasksIsEmpty) {
  // order = {0}, openCount = 1, Done -> rows.empty()
}
```

- [ ] **Step 2: Lancer et constater l'échec de compilation**

Run: `cmake -S test -B build/test && cmake --build build/test --target TaskListModelTest`
Expected: erreur de compilation (`TaskListView` inconnu).

- [ ] **Step 3: Implémenter `buildTaskListRows(order, openCount, view, rows)`**

Open : les `openCount` premières, ou `{AddTask}` s'il n'y en a aucune. Done : `order[openCount..]`, vide s'il n'y en a pas, `{AddTask}` seulement si l'index entier est vide. Mettre à jour le commentaire d'en-tête (plus de ligne « N faites » ; la ligne 0 est la sélection par défaut).

- [ ] **Step 4: Lancer les tests**

Run: `cmake --build build/test --target TaskListModelTest && ctest --test-dir build/test -R TaskListModel --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/activities/tasks/TaskListModel.* test/task_list_model/TaskListModelTest.cpp
git commit -m "refactor(tasks): an exclusive open/done view instead of the done accordion"
```

(La compilation firmware casse jusqu'à la Task 2 : `TaskListActivity.cpp` référence encore `DoneSection`/`showDone`. Les deux tâches se livrent ensemble ; ne pas pousser entre les deux.)

---

### Task 2: Écran — compteur dans l'en-tête, focus, bascule

**Files:**
- Modify: `src/activities/tasks/TaskListActivity.h` (membres `showDone`, `doneRowLabel`, `toggleDoneSection`)
- Modify: `src/activities/tasks/TaskListActivity.cpp` (`rebuildOrder` :181, `buildRows` :262-275, `activateIndex` :315, `handleButtons` :405, `onBackButton` :436, `drawChrome` :1105, `drawFooter` :1126, toute référence à `DoneSection`/`showDone`)
- Modify: `lib/I18n/translations/english.yaml`, `lib/I18n/translations/french.yaml`
- Modify: `scripts/run_simulator_tasks_tour.py` (scènes de l'accordéon remplacées par les nouvelles)
- Modify: `docs/crosstasks.md` (section « The List »), `CHANGELOG.md` (Changed)

**Interfaces:**
- Consumes: `TaskListView`, `buildTaskListRows(order, openCount, view, rows)` (Task 1).
- Produces (membres privés de `TaskListActivity`, utilisés par le plan groupes) :
  - `TaskListView view = TaskListView::Open;`
  - `bool headerFocused = false;`
  - `void switchView();` — bascule `view`, retire de `selection` les tâches qui quittent l'écran, `dirty = true`, `headerFocused = false`, sélection sur la ligne 0.
  - `bool hasHeaderCounter() const;` — vrai en vue Done, ou en vue Open avec `doneCount > 0`.
  - `void navigateButtons() override;`

- [ ] **Step 1: Clés i18n**

Ajouter (EN / FR) : `STR_TASK_DONE_TITLE` « Done » / « Faites » ; `STR_TASK_TODO_COUNT` « to do » / « à faire » ; `STR_TASK_SHOW` « view » / « voir ». Réutiliser `STR_TASK_DONE_COUNT` (« faites »). Supprimer `STR_TASK_EXPAND`/`STR_TASK_COLLAPSE` seulement si plus aucun usage (`rg STR_TASK_EXPAND src`).
Run: `python3 scripts/gen_i18n.py` → Expected: aucune erreur « missing from english.yaml ».

- [ ] **Step 2: Mesurer les libellés**

Run: `python3 scripts/measure_label.py inter_8_regular "3 faites ↻" --max 200` puis `"120 à faire ↻" --max 200` et `"voir" --max 70`.
Expected: tous OK. Sinon, raccourcir avant d'aller plus loin.

- [ ] **Step 3: Remplacer `showDone` par `view` dans `rebuildOrder()` et `buildRows()`**

Supprimer la branche `DoneSection` de `buildRows` et `doneRowLabel`. En fin de `rebuildOrder()` : si `view == Done` et `doneCount == 0`, repasser en `Open` (retour automatique, spec point 6).

- [ ] **Step 4: `drawChrome()` — titre et compteur**

Vue Open : titre « Tâches (M) », droite « N faites » si `doneCount > 0`. Vue Done : titre « Faites (N) », droite « M à faire ». Si `pendingOps`, suffixe « ↻ » au libellé droit (« 3 faites ↻ ») ; sans compteur, « ↻ » seul comme aujourd'hui. Quand `headerFocused`, le libellé droit est dessiné en vidéo inverse (rectangle plein + texte blanc), à la position où le thème pose le `subtitle` — si `GUI.drawHeader`/`TouchHeaderBackButton::draw` n'exposent pas ce rectangle, calculer la largeur avec `renderer` et dessiner par-dessus après l'appel. Libellé dans un `char[48]` membre ou local (pas de `std::string`).

- [ ] **Step 5: `navigateButtons()` — focus sur l'en-tête**

Si `headerFocused` : Bas → `headerFocused = false`, sélection 0 ; Haut → `headerFocused = false`, sélection sur la dernière ligne. Sinon : Haut sur la ligne 0 avec `hasHeaderCounter()` → `headerFocused = true` ; tout le reste délègue à `UiListActivity::navigateButtons()`. Pendant le focus, la liste ne montre aucune ligne surlignée (vérifier dans `freeink-sdk` comment `ListNav`/`ListProps` rendent « aucune sélection » ; sinon ne pas peindre la sélection de la ligne 0 tant que `headerFocused`). En mode sélection, même comportement.

- [ ] **Step 6: Confirmer, toucher, Retour**

`handleButtons()` : Confirmer relâché avec `headerFocused` → `switchView()` (appui long compris). Appui tactile dans le rectangle du compteur → `switchView()` (repérer dans `UiListActivity`/`TouchHeaderBackButton` où arrivent les touchers d'en-tête). `onBackButton()` : sélection d'abord (inchangé), puis en vue Done → `switchView()`, sinon `finish()`.

- [ ] **Step 7: `drawFooter()`**

`headerFocused` → Confirmer « voir », Gauche « ajouter » (inchangé), Droite vide. En vue Done, sur une tâche, les libellés actuels des tâches faites (« décocher », « détail »). Retirer les branches `DoneSection`.

- [ ] **Step 8: Build et analyse statique**

Run: `pio run -e simulator && pio run -e default && pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high`
Expected: succès, aucun défaut.

- [ ] **Step 9: Tournée simulateur**

Mettre à jour `scripts/run_simulator_tasks_tour.py` : scènes « compteur dans l'en-tête », « compteur focalisé », « vue faites », « 3 faites ↻ ». Run: `python3 scripts/run_simulator_tasks_tour.py` → inspecter les captures FR et EN des trois thèmes : compteur aligné sur la ligne de base du titre, rien de tronqué.

- [ ] **Step 10: Docs, CHANGELOG, format, commit**

`docs/crosstasks.md` : remplacer la puce « Finished tasks gather in an **N done** row… » par le nouveau geste (Up from the first task reaches **N done** in the header; Confirm or a tap shows the finished tasks instead; the header then reads **Done (N)** with **M to do** to come back; Back also comes back). Entrée `CHANGELOG.md` sous Changed. Run: `clang-format -i` sur les fichiers touchés.

```bash
git add src/activities/tasks lib/I18n/translations scripts/run_simulator_tasks_tour.py docs/crosstasks.md CHANGELOG.md
git commit -m "feat(tasks): done count in the header, switching the list to finished tasks"
```

- [ ] **Step 11: Vérification matérielle (X4)**

Flasher `default`. Cocher 3 tâches : « 3 faites » à droite de l'en-tête. Haut depuis la première tâche : compteur surligné, pied « voir ». Confirmer : « Faites (3) » / « M à faire ». Décocher les 3 : retour seul à la liste. Retour depuis la vue faites : liste ouverte, pas de sortie de Tasks.
