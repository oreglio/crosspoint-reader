#include "TaskStore.h"

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

// Pire cas mesure sur taskOpToLine() pour un op "add" (le seul qui cumule
// titre echappe et priorite) : entete+id (~30) + titre echappe 2*200 o (411)
// + priorite (15) + accolade finale (2) = 458 o. La marge couvre une legere
// derive du format sans reserver une pile de taille disproportionnee.
constexpr size_t TASK_OP_LINE_BUF = 512;

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

}  // namespace

bool TaskStore::loadFromFile() {
  // pFlag=true (le defaut de HalStorage::mkdir) cree aussi les parents : un
  // seul appel suffit pour /.crosspoint, /.crosspoint/tasks et
  // /.crosspoint/tasks/n. Sans danger a rejouer a chaque boot : le reste du
  // code appelle deja Storage.mkdir() sans garder de dossiers existants (cf.
  // PersistableStoreBase::writeDocToFile).
  Storage.mkdir(notesDir());
  return PersistableStore<TaskStore>::loadFromFile();
}

void TaskStore::toJson(JsonDocument& doc) const {
  doc["schema"] = 1;
  doc["secret"] = secretObfuscated;
  JsonArray arr = doc["tasks"].to<JsonArray>();
  for (const auto& rec : records) {
    JsonObject obj = arr.add<JsonObject>();
    obj["id"] = rec.id;
    obj["t"] = rec.title;
    obj["p"] = rec.priority;
    obj["d"] = rec.done;
    obj["n"] = rec.noteBytes;
  }
}

bool TaskStore::fromJson(JsonVariantConst doc) {
  // Tolerer une cle 'tasks' absente/invalide (liste vide) ; seule une erreur
  // de parsing JSON est fatale — meme convention qu'OpdsServerStore::fromJson.
  records.clear();
  secretObfuscated = doc["secret"] | "";

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
  // toJson() serialise AUSSI doc["secret"], pas seulement `records` : sur un
  // store decharge (ou jamais charge) le saveToFile() de fin ecrirait un
  // secret vide et desappairerait l'appareil en silence. Remplacer tout
  // l'ensemble des taches ne dispense donc pas de charger d'abord.
  ensureLoaded();
  // upsert() et fromJson() refusent deja un id malforme ; replaceAll() n'a
  // aujourd'hui qu'un seul appelant (TaskSyncReader, deja valide en amont),
  // mais la defense en profondeur cesse d'etre theorique des qu'un deuxieme
  // appelant existe (la remise a zero complete a venir). Rejeter en silence
  // serait pire que la faille : un seul log resume ce qui a saute.
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
  saveToFile();
}

void TaskStore::upsert(const TaskRecord& rec) {
  // saveToFile() reserialise tout `records` : ecrire dans un store decharge
  // (unload()) reduirait index.json a cette seule tache. Voir unload().
  ensureLoaded();
  if (!isValidTaskId(rec.id)) {
    LOG_ERR(TAG, "Refusing to index a task with a malformed id");
    return;
  }
  for (auto& existing : records) {
    if (strcmp(existing.id, rec.id) == 0) {
      existing = rec;
      saveToFile();
      return;
    }
  }
  if (records.size() >= MAX_TASKS) {
    LOG_ERR(TAG, "Dropping task %s: index already at the %zu-task cap", rec.id, MAX_TASKS);
    return;
  }
  records.push_back(rec);
  saveToFile();
}

void TaskStore::remove(const char* id) {
  if (id == nullptr) return;
  ensureLoaded();  // meme raison que dans upsert()
  const auto it =
      std::find_if(records.begin(), records.end(), [id](const TaskRecord& r) { return strcmp(r.id, id) == 0; });
  if (it == records.end()) return;
  records.erase(it);
  saveToFile();
}

const TaskRecord* TaskStore::find(const char* id) const {
  if (id == nullptr) return nullptr;
  for (const auto& rec : records) {
    if (strcmp(rec.id, id) == 0) return &rec;
  }
  return nullptr;
}

bool TaskStore::appendOp(const TaskOp& op) {
  char line[TASK_OP_LINE_BUF];
  const size_t len = taskOpToLine(op, line, sizeof(line));
  if (len == 0) {
    LOG_ERR(TAG, "Op too large to serialize; dropped rather than written truncated");
    return false;
  }

  // Ouverture en ajout : un seul petit write, jamais une reecriture de toute
  // la file. Un tick doit couter une ecriture, pas un rechargement complet —
  // une coupure ne doit alors perdre que cette ligne.
  HalFile file = Storage.open(opsPath(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR(TAG, "Could not open the ops queue for append");
    return false;
  }
  bool ok = file.write(line, len) == len;
  if (ok) ok = file.write("\n", 1) == 1;
  file.close();
  if (!ok) LOG_ERR(TAG, "Failed to append op to the queue");
  return ok;
}

size_t TaskStore::readOps(TaskOp* out, size_t max) const {
  if (out == nullptr || max == 0) return 0;
  HalFile file;
  if (!Storage.openFileForRead(TAG, opsPath(), file)) return 0;

  size_t count = 0;
  char line[TASK_OP_LINE_BUF];
  size_t lineLen = 0;
  bool overflow = false;
  while (count < max && file.available()) {
    const int byte = file.read();
    if (byte < 0) break;
    if (byte == '\r') continue;  // jamais ecrit par appendOp, tolere en lecture
    if (byte != '\n') {
      if (lineLen < sizeof(line)) {
        line[lineLen++] = static_cast<char>(byte);
      } else {
        overflow = true;
      }
      continue;
    }
    // Une ligne sans '\n' final (coupure en plein write) n'atteint jamais ce
    // point : elle reste dans le tampon et n'est donc jamais rejouee comme un
    // op valide aux champs par defaut.
    if (overflow) {
      LOG_ERR(TAG, "Skipping an ops line too long to parse");
    } else if (lineLen > 0 && taskOpFromLine(line, lineLen, out[count])) {
      count++;
    }
    lineLen = 0;
    overflow = false;
  }
  file.close();
  return count;
}

void TaskStore::clearOps() {
  if (Storage.exists(opsPath()) && !Storage.remove(opsPath())) {
    LOG_ERR(TAG, "Failed to clear the ops queue file");
  }
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

void TaskStore::writeCursor(const char* cursor) {
  if (!isSafeCursor(cursor)) {
    LOG_ERR(TAG, "Refusing to persist an unsafe sync cursor");
    return;
  }
  HalFile file;
  if (!Storage.openFileForWrite(TAG, cursorPath(), file)) {
    LOG_ERR(TAG, "Could not persist the sync cursor");
    return;
  }
  const size_t len = strlen(cursor);
  const bool ok = file.write(cursor, len) == len;
  file.close();
  if (!ok) LOG_ERR(TAG, "Failed to write the sync cursor");
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

void TaskStore::clearAllNotes() const {
  if (!Storage.exists(notesDir())) return;
  HalFile dir = Storage.open(notesDir());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    LOG_ERR(TAG, "Could not open the notes directory to clear it");
    return;
  }

  char name[32];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    file.close();
    const std::string path = std::string(notesDir()) + "/" + name;
    if (!Storage.remove(path.c_str())) {
      LOG_ERR(TAG, "Failed to remove note file: %s", path.c_str());
    }
  }
  dir.close();
}

bool TaskStore::readSecret(char* out, size_t size) const {
  if (out == nullptr || size == 0) return false;
  out[0] = '\0';
  if (secretObfuscated.empty()) return false;

  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  const std::string plaintext = obfuscation::deobfuscateFromBase64(secretObfuscated.c_str(), &status);
  if (status != obfuscation::DecodeStatus::VALIDATED && status != obfuscation::DecodeStatus::LEGACY) {
    LOG_ERR(TAG, "Stored pairing secret failed to decode");
    return false;
  }
  std::snprintf(out, size, "%s", plaintext.c_str());
  return true;
}

void TaskStore::writeSecret(const char* secret) {
  if (secret == nullptr) return;
  ensureLoaded();  // meme raison que dans upsert()
  secretObfuscated = obfuscation::obfuscateToBase64(secret).c_str();
  saveToFile();
}

void TaskStore::clearSecret() {
  ensureLoaded();  // meme raison que dans upsert()
  secretObfuscated.clear();
  saveToFile();
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
