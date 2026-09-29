#include "TaskStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(SIMULATOR)
#include <esp_system.h>
#endif

namespace {

constexpr char TAG[] = "TASKS";

// Meme plafond que le protocole de sync (TaskSyncReader::TASK_SYNC_CURSOR_MAX) :
// duplique ici plutot qu'inclus pour ne pas coupler le stockage local au
// lecteur de flux reseau, mais c'est la meme spec des deux cotes.
constexpr size_t TASK_CURSOR_MAX_LEN = 32;

// ^[wd][0-9a-f]{8}$ — 'w' pour une tache creee cote serveur (web), 'd' pour
// une tache creee sur l'appareil. Aucune autre forme ne doit jamais atteindre
// notePath() ni etre indexee : voir le commentaire de notePath() dans le .h.
bool isValidTaskId(const char* id) {
  if (id == nullptr) return false;
  if (id[0] != 'w' && id[0] != 'd') return false;
  for (size_t i = 1; i < TASK_ID_LEN; i++) {
    const char c = id[i];
    const bool isHexDigit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!isHexDigit) return false;
  }
  return id[TASK_ID_LEN] == '\0';
}

bool clearDirectoryFiles(const char* dirPath) {
  if (!Storage.exists(dirPath)) return true;
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    LOG_ERR(TAG, "Could not open %s to clear it", dirPath);
    return false;
  }

  bool ok = true;
  char name[32];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    file.close();
    const std::string path = std::string(dirPath) + "/" + name;
    if (!Storage.remove(path.c_str())) {
      LOG_ERR(TAG, "Failed to remove note file: %s", path.c_str());
      ok = false;
    }
  }
#ifndef SIMULATOR  // le HalFile du simulateur n'expose pas iterationFailed()
  // Une lecture de repertoire ratee ressemble a une fin propre : sans ce
  // controle, des fichiers resteraient sans que personne le sache.
  if (dir.iterationFailed()) ok = false;
#endif
  dir.close();
  return ok;
}

// Petit fichier texte (curseur, compteur, secret) ecrit comme l'index : .tmp
// complet et referme, puis remplacement.
bool writeSmallFileAtomic(const char* path, const char* tmpPath, const char* data, size_t len) {
  HalFile file;
  if (!Storage.openFileForWrite(TAG, tmpPath, file)) {
    LOG_ERR(TAG, "Could not open %s", tmpPath);
    return false;
  }
  bool ok = file.write(data, len) == len;
  ok = file.sync() && ok;
  ok = file.close() && ok;
  if (!ok) {
    LOG_ERR(TAG, "Short write of %s", tmpPath);
    Storage.remove(tmpPath);
    return false;
  }
  return PersistableStoreBase::replaceWithTmp(tmpPath, path);
}

// Lit un petit fichier texte dans `out` (termine, espaces de fin retires).
bool readSmallFile(const char* path, char* out, size_t size) {
  out[0] = '\0';
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) return false;
  const int got = file.read(out, size - 1);
  file.close();
  if (got <= 0) return false;
  int len = got;
  out[len] = '\0';
  while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) out[--len] = '\0';
  return len > 0;
}

void removeIfExists(const char* path) {
  if (Storage.exists(path)) Storage.remove(path);
}

// Controles de promotion d'un .tmp orphelin (recoverReplacedFile) : chacun
// applique la regle de lecture de son fichier. Un .tmp qui ne se lit pas est
// jete plutot que promu ; pour son lecteur, c'est la meme absence.
bool indexParses(const char* path) {
  JsonDocument doc;
  return PersistableStoreBase::readDocFromFile(path, doc);
}

bool cursorParses(const char* path) {
  char buf[TASK_CURSOR_BUF];
  return readSmallFile(path, buf, sizeof(buf)) && TaskStore::isSafeCursor(buf);
}

bool ackedOpsParse(const char* path) {
  char buf[16];
  if (!readSmallFile(path, buf, sizeof(buf))) return false;
  char* end = nullptr;
  std::strtoul(buf, &end, 10);
  return end != buf && *end == '\0';
}

bool secretParses(const char* path) {
  char buf[128];
  if (!readSmallFile(path, buf, sizeof(buf))) return false;
  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  std::string plaintext = obfuscation::deobfuscateFromBase64(buf, &status);
  std::fill(plaintext.begin(), plaintext.end(), '\0');
  return status == obfuscation::DecodeStatus::VALIDATED || status == obfuscation::DecodeStatus::LEGACY;
}

// Parcourt la file avec les regles de TaskOpLineSplitter : seules les ops
// valides sont remises. `visit` rend false pour arreter.
void forEachQueuedOp(const char* path, bool (*visit)(void* ctx, TaskOp& op), void* ctx) {
  HalFile file;
  if (!Storage.openFileForRead(TAG, path, file)) return;
  TaskOpLineSplitter splitter;
  TaskOp op;
  while (file.available()) {
    const int byte = file.read();
    if (byte < 0) break;
    const bool usable = splitter.feed(static_cast<char>(byte), op);
    if (splitter.lastLineTooLong()) LOG_ERR(TAG, "Skipping an ops line too long to parse");
    if (usable && !visit(ctx, op)) break;
  }
  file.close();
}

}  // namespace

bool TaskStore::loadFromFile() {
  // pFlag=true (le defaut de HalStorage::mkdir) cree aussi les parents : un
  // seul appel suffit pour /.crosspoint, /.crosspoint/tasks et
  // /.crosspoint/tasks/n. Sans danger a rejouer a chaque boot : le reste du
  // code appelle deja Storage.mkdir() sans garder de dossiers existants (cf.
  // PersistableStoreBase::writeDocToFile).
  Storage.mkdir(notesDir());

  recoverReplacedFile(getFilePath(), indexTmpPath(), indexParses);
  recoverReplacedFile(cursorPath(), cursorTmpPath(), cursorParses);
  recoverReplacedFile(ackedOpsPath(), ackedOpsTmpPath(), ackedOpsParse);
  // Un compteur sans file est un clearOps() interrompu : il ferait sauter les
  // prochaines ops ajoutees.
  if (!Storage.exists(opsPath())) removeIfExists(ackedOpsPath());

  ensureSecretLoaded();

  const bool ok = PersistableStore<TaskStore>::loadFromFile();
  if (!ok) {
    // Pas d'index lisible, pas de curseur : un delta depuis l'ancien curseur ne
    // renverrait jamais les taches plus anciennes que lui.
    records.clear();
    clearCursor();
  }
  return ok;
}

void TaskStore::ensureSecretLoaded() {
  recoverReplacedFile(secretPath(), secretTmpPath(), secretParses);
  // Le secret a son propre fichier : il ne depend jamais de la lecture de
  // l'index. Un secret illisible reste vide, ce qui se lit « non appaire ».
  char secretBuf[128];
  if (readSmallFile(secretPath(), secretBuf, sizeof(secretBuf))) {
    secretObfuscated = secretBuf;
  } else {
    secretObfuscated.clear();
  }
}

bool TaskStore::saveIndex() const {
  std::lock_guard<std::mutex> lock(storeMutex);
  JsonDocument doc;
  toJson(doc);
  return writeDocToFileAtomic(getFilePath(), indexTmpPath(), doc);
}

void TaskStore::toJson(JsonDocument& doc) const {
  doc["schema"] = 1;
  JsonArray arr = doc["tasks"].to<JsonArray>();
  for (const auto& rec : records) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = rec.id;
    obj["t"] = rec.title;
    obj["p"] = rec.priority;
    obj["d"] = rec.done;
    obj["n"] = rec.noteBytes;
    // Absent tant que le serveur n'a pas classe la tache : un index sans "o"
    // (y compris d'avant l'ordre manuel) se relit en TASK_ORDER_UNSET.
    if (rec.order != TASK_ORDER_UNSET) obj["o"] = rec.order;
  }
}

bool TaskStore::fromJson(JsonVariantConst doc) {
  // Tolerer une cle 'tasks' absente/invalide (liste vide) ; seule une erreur
  // de parsing JSON est fatale — meme convention qu'OpdsServerStore::fromJson.
  records.clear();

  JsonArrayConst arr = doc["tasks"].as<JsonArrayConst>();
  records.reserve(std::min(arr.size(), MAX_TASKS));

  for (JsonObjectConst obj : arr) {
    if (records.size() >= MAX_TASKS) {
      LOG_ERR(TAG, "index.json has more than %zu tasks; ignoring the rest", MAX_TASKS);
      break;
    }

    // Tronquer un id ou un titre trop long via snprintf le ramenerait parfois
    // a une forme valide (meme piege que l'op sink de TaskOpQueue.cpp) : on
    // verifie la longueur reelle avant toute copie, jamais apres.
    const char* idRaw = obj["id"] | "";
    if (strlen(idRaw) != TASK_ID_LEN || !isValidTaskId(idRaw)) {
      LOG_ERR(TAG, "Skipping task with a malformed id in index.json");
      continue;
    }
    const bool duplicate =
        std::any_of(records.begin(), records.end(), [idRaw](const TaskRecord& r) { return strcmp(r.id, idRaw) == 0; });
    if (duplicate) {
      LOG_ERR(TAG, "Skipping duplicate task id in index.json: %s", idRaw);
      continue;
    }

    const char* titleRaw = obj["t"] | "";
    if (strlen(titleRaw) > TASK_TITLE_MAX) {
      LOG_ERR(TAG, "Skipping task %s: title exceeds %zu bytes", idRaw, TASK_TITLE_MAX);
      continue;
    }

    TaskRecord rec{};
    std::snprintf(rec.id, sizeof(rec.id), "%s", idRaw);
    std::snprintf(rec.title, sizeof(rec.title), "%s", titleRaw);
    const long priority = obj["p"] | static_cast<long>(TASK_PRIORITY_NORMAL);
    rec.priority = (priority == TASK_PRIORITY_HIGH || priority == TASK_PRIORITY_LOW) ? static_cast<uint8_t>(priority)
                                                                                     : TASK_PRIORITY_NORMAL;
    rec.done = obj["d"] | false;
    const long noteBytes = obj["n"] | 0L;
    rec.noteBytes = (noteBytes < 0 || noteBytes > UINT16_MAX) ? 0 : static_cast<uint16_t>(noteBytes);
    const long order = obj["o"] | static_cast<long>(TASK_ORDER_UNSET);
    rec.order = (order < 0 || order >= TASK_ORDER_UNSET) ? TASK_ORDER_UNSET : static_cast<uint16_t>(order);
    records.push_back(rec);
  }

  return true;
}

void TaskStore::unload() {
  // clear() seul garderait la capacite reservee : shrink_to_fit() est ce qui
  // rend les octets au tas. `secretObfuscated` reste en RAM (quelques dizaines
  // d'octets, et l'appairage doit survivre a la fermeture de l'ecran).
  records.clear();
  records.shrink_to_fit();
  markUnloaded();
}

void TaskStore::replaceAll(std::vector<TaskRecord> next) {
  // Charger d'abord, meme pour tout remplacer : un store jamais charge serait
  // relu par le prochain ensureLoaded(), qui ecraserait ce remplacement.
  ensureLoaded();
  // upsert() et fromJson() refusent deja un id malforme ; replaceAll() n'a
  // aujourd'hui qu'un seul appelant, la visite simulateur (la sync passe par
  // les stage*()), mais la defense en profondeur ne coute rien et vaut pour
  // tout appelant a venir. Rejeter en silence serait pire que la faille : un
  // seul log resume ce qui a saute.
  const size_t before = next.size();
  next.erase(std::remove_if(next.begin(), next.end(), [](const TaskRecord& r) { return !isValidTaskId(r.id); }),
             next.end());
  if (next.size() != before) {
    LOG_ERR(TAG, "Dropped %zu task(s) with a malformed id from a full sync snapshot", before - next.size());
  }

  if (next.size() > MAX_TASKS) {
    LOG_ERR(TAG, "Clamping full sync snapshot from %zu to %zu tasks", next.size(), MAX_TASKS);
    next.resize(MAX_TASKS);
  }
  records = std::move(next);
  saveIndex();
}

bool TaskStore::upsert(const TaskRecord& rec) {
  if (!stageUpsert(rec)) return false;
  if (!saveIndex()) {
    LOG_ERR(TAG, "Could not save the task index after an edit of %s", rec.id);
    return false;
  }
  return true;
}

void TaskStore::remove(const char* id) {
  if (stageRemove(id)) saveIndex();
}

bool TaskStore::reserveFullCapacity() {
  if (records.capacity() >= MAX_TASKS) return true;
  // La marge couvre l'en-tete et l'alignement de l'allocateur.
  constexpr uint32_t RECORDS_BYTES = MAX_TASKS * sizeof(TaskRecord);
  constexpr uint32_t ALLOCATOR_MARGIN = 1024;
  const uint32_t largest = ESP.getMaxAllocHeap();
  if (largest < RECORDS_BYTES + ALLOCATOR_MARGIN) {
    LOG_ERR(TAG, "No contiguous block for the task index: need %u, largest %u",
            static_cast<unsigned>(RECORDS_BYTES + ALLOCATOR_MARGIN), static_cast<unsigned>(largest));
    return false;
  }
  records.reserve(MAX_TASKS);
  return true;
}

bool TaskStore::stageUpsert(const TaskRecord& rec) {
  // saveIndex() reserialise tout `records` : ecrire dans un store decharge
  // (unload()) reduirait index.json a cette seule tache. Voir unload().
  ensureLoaded();
  if (!isValidTaskId(rec.id)) {
    LOG_ERR(TAG, "Refusing to index a task with a malformed id");
    return false;
  }
  for (auto& existing : records) {
    if (strcmp(existing.id, rec.id) == 0) {
      existing = rec;
      return true;
    }
  }
  if (records.size() >= MAX_TASKS) {
    LOG_ERR(TAG, "Dropping task %s: index already at the %zu-task cap", rec.id, MAX_TASKS);
    return false;
  }
  records.push_back(rec);
  return true;
}

bool TaskStore::stageRemove(const char* id) {
  if (id == nullptr) return false;
  ensureLoaded();  // meme raison que dans stageUpsert()
  const auto it =
      std::find_if(records.begin(), records.end(), [id](const TaskRecord& r) { return strcmp(r.id, id) == 0; });
  if (it == records.end()) return false;
  records.erase(it);
  return true;
}

void TaskStore::stageReset() {
  ensureLoaded();  // meme raison que dans replaceAll()
  records.clear();
}

void TaskStore::discardStaged() {
  // Vider soi-meme avant de relire : fromJson() ne tourne que si index.json se
  // lit, et a la toute premiere sync il n'existe pas encore. Le secret, lui,
  // n'est pas touche : un echec de lecture ne doit pas desappairer en RAM.
  records.clear();
  loadFromFile();
  clearStagedNotes();
}

const TaskRecord* TaskStore::find(const char* id) const {
  if (id == nullptr) return nullptr;
  for (const auto& rec : records) {
    if (strcmp(rec.id, id) == 0) return &rec;
  }
  return nullptr;
}

bool TaskStore::appendOp(const TaskOp& op) {
  char record[TASK_OP_RECORD_MAX];
  const size_t len = taskOpToRecord(op, record, sizeof(record));
  if (len == 0) {
    LOG_ERR(TAG, "Op too large to serialize; dropped rather than written truncated");
    return false;
  }

  // Un compteur d'ops acquittees sans file (clearOps() interrompu) ferait
  // sauter cette op a la prochaine sync : on le jette avant d'ajouter.
  if (!Storage.exists(opsPath())) removeIfExists(ackedOpsPath());

  // Ouverture en ajout, et UN seul write pour tout l'enregistrement : un tick
  // coute une ecriture, pas une reecriture de la file. Une coupure (ou un
  // write refuse) peut laisser un fragment de cette ligne sans '\n' final ;
  // le '\n' de tete de l'enregistrement suivant le laisse seul sur sa ligne,
  // que la lecture rejette. Seule l'op interrompue est perdue, jamais la
  // suivante (voir taskOpToRecord).
  HalFile file = Storage.open(opsPath(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR(TAG, "Could not open the ops queue for append");
    return false;
  }
  const bool ok = file.write(record, len) == len;
  file.close();
  if (!ok) LOG_ERR(TAG, "Failed to append op to the queue");
  return ok;
}

size_t TaskStore::appendOpBatch(const size_t count, bool (*fill)(void* ctx, size_t i, TaskOp& out), void* ctx) {
  if (count == 0 || fill == nullptr) return 0;
  // Meme garde qu'appendOp() : un compteur d'acquittement sans file ferait
  // sauter les premieres ops du lot a la prochaine sync.
  if (!Storage.exists(opsPath())) removeIfExists(ackedOpsPath());

  HalFile file = Storage.open(opsPath(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR(TAG, "Could not open the ops queue for a batch of %u", static_cast<unsigned>(count));
    return 0;
  }
  // Une op et son enregistrement a la fois sur la pile (~740 o), jamais le
  // lot entier : 120 ops feraient ~27 Ko.
  size_t written = 0;
  for (; written < count; ++written) {
    TaskOp op{};
    if (!fill(ctx, written, op)) break;
    char record[TASK_OP_RECORD_MAX];
    const size_t len = taskOpToRecord(op, record, sizeof(record));
    if (len == 0) {
      LOG_ERR(TAG, "Op too large to serialize; batch stopped at %u", static_cast<unsigned>(written));
      break;
    }
    if (file.write(record, len) != len) {
      LOG_ERR(TAG, "Failed to append op %u of a batch of %u", static_cast<unsigned>(written),
              static_cast<unsigned>(count));
      break;
    }
  }
  file.close();
  return written;
}

size_t TaskStore::readOps(TaskOp* out, size_t max, size_t skip) const {
  if (out == nullptr || max == 0) return 0;
  // Seules les ops VALIDES comptent pour `skip`, comme pour le compte rendu :
  // sinon deux tranches successives se chevaucheraient.
  struct Ctx {
    TaskOp* out;
    size_t max;
    size_t skip;
    size_t count;
  } ctx{out, max, skip, 0};
  forEachQueuedOp(
      opsPath(),
      [](void* raw, TaskOp& op) {
        auto* c = static_cast<Ctx*>(raw);
        if (c->skip > 0) {
          c->skip--;
          return true;
        }
        c->out[c->count++] = op;
        return c->count < c->max;
      },
      &ctx);
  return ctx.count;
}

bool TaskStore::hasPendingOps() const {
  TaskOp first;
  return readOps(&first, 1, readAckedOps()) > 0;
}

bool TaskStore::findQueuedTitle(const char* id, char* out, size_t size) const {
  if (id == nullptr || out == nullptr || size == 0) return false;
  out[0] = '\0';
  struct Ctx {
    const char* id;
    char* out;
    size_t size;
    bool found;
  } ctx{id, out, size, false};
  // La derniere op qui porte un titre gagne : c'est celui que l'utilisateur a
  // vu en dernier.
  forEachQueuedOp(
      opsPath(),
      [](void* raw, TaskOp& op) {
        auto* c = static_cast<Ctx*>(raw);
        if ((op.kind == TaskOpKind::Add || op.kind == TaskOpKind::Title) && strcmp(op.id, c->id) == 0) {
          std::snprintf(c->out, c->size, "%s", op.title);
          c->found = true;
        }
        return true;
      },
      &ctx);
  return ctx.found;
}

void TaskStore::clearOps() {
  // La file d'abord : voir la declaration.
  if (Storage.exists(opsPath()) && !Storage.remove(opsPath())) {
    LOG_ERR(TAG, "Failed to clear the ops queue file; keeping the acknowledged-ops count");
    return;
  }
  removeIfExists(ackedOpsTmpPath());
  if (Storage.exists(ackedOpsPath()) && !Storage.remove(ackedOpsPath())) {
    LOG_ERR(TAG, "Failed to clear the acknowledged-ops count");
  }
}

size_t TaskStore::readAckedOps() const {
  char buf[16];
  if (!readSmallFile(ackedOpsPath(), buf, sizeof(buf))) return 0;
  char* end = nullptr;
  const unsigned long value = std::strtoul(buf, &end, 10);
  // Une valeur illisible vaut zero : renvoyer des ops deja acceptees coute un
  // rejeu, en sauter qui ne l'ont pas ete coute des coches.
  if (end == buf || *end != '\0') return 0;
  return static_cast<size_t>(value);
}

bool TaskStore::writeAckedOps(size_t count) {
  char buf[16];
  const int len = std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(count));
  if (!writeSmallFileAtomic(ackedOpsPath(), ackedOpsTmpPath(), buf, static_cast<size_t>(len))) {
    LOG_ERR(TAG, "Could not persist the acknowledged-ops count");
    return false;
  }
  return true;
}

bool TaskStore::isSafeCursor(const char* cursor) {
  if (cursor == nullptr) return false;
  const size_t len = strlen(cursor);
  if (len == 0 || len > TASK_CURSOR_MAX_LEN) return false;
  for (size_t i = 0; i < len; i++) {
    const char c = cursor[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

bool TaskStore::readCursor(char* out, size_t size) const {
  if (out == nullptr || size == 0) return false;
  out[0] = '\0';

  HalFile file;
  if (!Storage.openFileForRead(TAG, cursorPath(), file)) return false;
  char buffer[TASK_CURSOR_BUF] = {};
  const int got = file.read(buffer, sizeof(buffer) - 1);
  file.close();
  if (got <= 0) return false;

  int trimmed = got;
  buffer[trimmed] = '\0';
  while (trimmed > 0 && (buffer[trimmed - 1] == '\n' || buffer[trimmed - 1] == '\r' || buffer[trimmed - 1] == ' ')) {
    buffer[--trimmed] = '\0';
  }

  // Le fichier vit sur une carte SD editable a la main : tout ce qui echoue
  // ce controle est traite comme une absence de curseur, jamais une erreur —
  // ca coute un snapshot complet au prochain sync, ca ne corrompt rien.
  if (!isSafeCursor(buffer)) return false;

  std::snprintf(out, size, "%s", buffer);
  return true;
}

bool TaskStore::writeCursor(const char* cursor) {
  if (!isSafeCursor(cursor)) {
    LOG_ERR(TAG, "Refusing to persist an unsafe sync cursor");
    return false;
  }
  if (!writeSmallFileAtomic(cursorPath(), cursorTmpPath(), cursor, strlen(cursor))) {
    LOG_ERR(TAG, "Could not persist the sync cursor");
    return false;
  }
  return true;
}

void TaskStore::clearCursor() {
  removeIfExists(cursorTmpPath());
  if (Storage.exists(cursorPath()) && !Storage.remove(cursorPath())) {
    LOG_ERR(TAG, "Could not remove the sync cursor");
  }
}

bool TaskStore::notePath(const char* id, char* out, size_t size) {
  if (id == nullptr || out == nullptr || size == 0) return false;
  if (strlen(id) != TASK_ID_LEN || !isValidTaskId(id)) {
    LOG_ERR(TAG, "Refusing to build a note path from a malformed id");
    return false;
  }
  const int written = std::snprintf(out, size, "%s/%s.txt", notesDir(), id);
  return written > 0 && static_cast<size_t>(written) < size;
}

bool TaskStore::clearAllNotes() const { return clearDirectoryFiles(notesDir()); }

bool TaskStore::clearStagedNotes() const { return clearDirectoryFiles(stagedNotesDir()); }

bool TaskStore::stagedNotePath(const char* id, char* out, size_t size) {
  // Meme garde que notePath() : l'id vient du reseau.
  char checked[48];
  if (!notePath(id, checked, sizeof(checked))) return false;
  const int written = std::snprintf(out, size, "%s/%s.txt", stagedNotesDir(), id);
  return written > 0 && static_cast<size_t>(written) < size;
}

bool TaskStore::commitStagedNotes() const {
  char name[32];
  char from[64];
  char to[64];
  bool ok = true;

  if (Storage.exists(stagedNotesDir())) {
    HalFile dir = Storage.open(stagedNotesDir());
    if (dir && dir.isDirectory()) {
      for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
        file.getName(name, sizeof(name));
        file.close();
        std::snprintf(from, sizeof(from), "%s/%s", stagedNotesDir(), name);
        std::snprintf(to, sizeof(to), "%s/%s", notesDir(), name);
        // SdFat refuse de renommer vers un fichier existant (O_EXCL). Si la
        // suppression ou le renommage echoue, la note en place est perdue ou
        // perimee : l'appelant n'avance alors pas le curseur, et le serveur
        // renverra la ligne avec sa note a la sync suivante.
        if ((Storage.exists(to) && !Storage.remove(to)) || !Storage.rename(from, to)) {
          LOG_ERR(TAG, "Failed to move a synced note into place: %s", name);
          ok = false;
        }
      }
#ifndef SIMULATOR
      if (dir.iterationFailed()) ok = false;
#endif
    } else {
      LOG_ERR(TAG, "Could not open the staged notes directory");
      ok = false;
    }
    if (dir) dir.close();
  }

  // Une tache supprimee, ou dont la note a ete videe sur le web, laisserait
  // sinon son ancien fichier derriere elle. Un echec ici ne laisse qu'un
  // fichier que rien n'affiche : il ne fait pas echouer le commit.
  if (!Storage.exists(notesDir())) return ok;
  HalFile dir = Storage.open(notesDir());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return ok;
  }
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    file.close();
    char id[TASK_ID_LEN + 1] = {};
    const size_t len = strlen(name);
    bool keep = false;
    if (len == TASK_ID_LEN + 4 && strcmp(name + TASK_ID_LEN, ".txt") == 0) {
      std::memcpy(id, name, TASK_ID_LEN);
      const TaskRecord* rec = find(id);
      keep = rec != nullptr && rec->noteBytes > 0;
    }
    if (!keep) {
      std::snprintf(to, sizeof(to), "%s/%s", notesDir(), name);
      if (!Storage.remove(to)) LOG_ERR(TAG, "Failed to remove orphan note file: %s", name);
    }
  }
  dir.close();
  return ok;
}

bool TaskStore::readSecret(char* out, size_t size) const {
  if (out == nullptr || size == 0) return false;
  out[0] = '\0';
  if (secretObfuscated.empty()) return false;

  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  std::string plaintext = obfuscation::deobfuscateFromBase64(secretObfuscated.c_str(), &status);
  if (status != obfuscation::DecodeStatus::VALIDATED && status != obfuscation::DecodeStatus::LEGACY) {
    LOG_ERR(TAG, "Stored pairing secret failed to decode");
    return false;
  }
  std::snprintf(out, size, "%s", plaintext.c_str());
  // Efface la copie en clair avant de la rendre au tas. Les tampons internes
  // de deobfuscateFromBase64() ne sont pas atteignables d'ici.
  std::fill(plaintext.begin(), plaintext.end(), '\0');
  return true;
}

bool TaskStore::writeSecret(const char* secret) {
  if (secret == nullptr) return false;
  // Plus besoin de charger l'index : le secret ne vit plus dedans. Le
  // dossier, lui, doit exister avant le tout premier appairage.
  Storage.mkdir(notesDir());
  // Copie explicite pour pouvoir l'effacer : un const char* passe a
  // obfuscateToBase64() creerait un temporaire en clair hors de portee.
  std::string plaintext(secret);
  const std::string obfuscated = obfuscation::obfuscateToBase64(plaintext).c_str();
  std::fill(plaintext.begin(), plaintext.end(), '\0');
  if (!writeSmallFileAtomic(secretPath(), secretTmpPath(), obfuscated.c_str(), obfuscated.size())) {
    LOG_ERR(TAG, "Could not save the pairing secret; keeping the previous one");
    return false;
  }
  secretObfuscated = obfuscated;
  return true;
}

void TaskStore::clearSecret() {
  removeIfExists(secretTmpPath());
  if (Storage.exists(secretPath()) && !Storage.remove(secretPath())) {
    LOG_ERR(TAG, "Could not remove the pairing secret file");
    return;
  }
  secretObfuscated.clear();
}

std::string TaskStore::newDeviceId() {
#if !defined(SIMULATOR)
  const uint32_t value = esp_random();
#else
  const uint32_t value = static_cast<uint32_t>(rand());
#endif
  char buf[TASK_ID_LEN + 1];
  std::snprintf(buf, sizeof(buf), "d%08x", value);
  return std::string(buf);
}
