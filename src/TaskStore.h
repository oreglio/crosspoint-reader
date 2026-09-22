#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

#include "tasks/TaskOpQueue.h"
#include "tasks/TaskRecord.h"

// Taille du tampon que l'appelant de readCursor() doit fournir. Le pire cas
// mesure cote serveur fait 22 caracteres, la spec en plafonne 32 ; la marge
// couvre un terminateur et une eventuelle evolution mineure du format.
inline constexpr size_t TASK_CURSOR_BUF = 40;

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
 * exactement comme OpdsServerStore traite les mots de passe — et reste sous
 * cette forme meme en RAM, pour qu'un LOG_ERR maladroit n'affiche jamais que
 * du base64 illisible plutot que le secret en clair.
 */
class TaskStore : public PersistableStore<TaskStore> {
 private:
  std::vector<TaskRecord> records;
  std::string secretObfuscated;

  TaskStore() = default;
  friend class PersistableStore<TaskStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/tasks/index.json"; }
  static const char* opsPath() { return "/.crosspoint/tasks/ops.ndjson"; }
  static const char* cursorPath() { return "/.crosspoint/tasks/cursor.txt"; }
  static const char* notesDir() { return "/.crosspoint/tasks/n"; }

  // Cree l'arborescence /.crosspoint/tasks (et son sous-dossier n/) avant de
  // deleguer au chargement standard : writeDocToFile() ne garantit que
  // /.crosspoint, donc sans ce mkdir recursif la toute premiere sauvegarde
  // (upsert, writeCursor, appendOp...) echouerait avant meme le premier
  // appairage.
  bool loadFromFile();

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::vector<TaskRecord>& all() const { return records; }
  // Rend vraiment la memoire de l'index (jusqu'a 120 x 216 octets = ~26 Ko,
  // 7 % de la RAM d'un C3) et arme le prochain ensureLoaded() pour qu'il
  // relise la carte. main.cpp charge ce store a chaque demarrage, donc sans
  // cet appel les 26 Ko restaient residents toute la session, y compris
  // pendant la lecture d'un livre. Seul TaskListActivity::onExit() l'appelle :
  // c'est le seul detenteur d'indices dans ce vecteur.
  //
  // ATTENTION aux ecrivains : toute methode qui appelle saveToFile()
  // reserialise `records`, donc ecrire dans un store decharge ecraserait
  // index.json avec une liste vide. Les mutateurs concernes (upsert, remove,
  // writeSecret, clearSecret) appellent ensureLoaded() en entree pour cette
  // raison, replaceAll() COMPRIS : il remplace bien tout l'ensemble des
  // taches, mais toJson() ecrit aussi doc["secret"], qu'il ne remplace pas.
  void unload();
  void replaceAll(std::vector<TaskRecord> next);
  void upsert(const TaskRecord& rec);
  void remove(const char* id);
  const TaskRecord* find(const char* id) const;

  bool appendOp(const TaskOp& op);
  // `skip` saute les premieres ops valides : une sync envoie au plus
  // TASK_MAX_OPS_PER_SYNC ops par requete et lit les suivantes par tranches,
  // sans toucher au fichier avant d'avoir tout envoye.
  size_t readOps(TaskOp* out, size_t max, size_t skip = 0) const;
  void clearOps();

  bool readCursor(char* out, size_t size) const;
  void writeCursor(const char* cursor);
  // Alphabet base64url, non vide, au plus 32 caracteres — le meme controle
  // que RaindropSyncActivity applique deja a son propre curseur opaque.
  static bool isSafeCursor(const char* cursor);

  // Refuse tout ce qui ne colle pas a ^[wd][0-9a-f]{8}$ avant de batir un
  // chemin : c'est le seul rempart entre un serveur compromis et une ecriture
  // arbitraire sur la carte SD.
  static bool notePath(const char* id, char* out, size_t size);
  void clearAllNotes() const;

  // --- Mise en attente d'une reponse de sync ---
  //
  // La sync applique la reponse du serveur au fil de l'eau, mais rien ne doit
  // atteindre la carte avant qu'elle soit entierement acceptee : un corps coupe
  // a mi-chemin ne doit changer ni index.json, ni les notes, ni le curseur, ni
  // la file d'ops. Les stage*() modifient donc `records` en RAM SANS
  // sauvegarder, et les notes recues s'ecrivent sous stagedNotesDir(). Sur
  // succes, l'appelant sauvegarde (saveToFile) puis commitStagedNotes() ; sur
  // echec, discardStaged() relit la carte et jette les notes en attente.
  static const char* stagedNotesDir() { return "/.crosspoint/tasks/ns"; }
  static bool stagedNotePath(const char* id, char* out, size_t size);
  void stageReset();
  bool stageUpsert(const TaskRecord& rec);
  bool stageRemove(const char* id);
  void clearStagedNotes() const;
  // Deplace les notes en attente dans notesDir(), puis supprime toute note
  // dont la tache n'est plus indexee ou n'a plus de note (noteBytes == 0).
  void commitStagedNotes() const;
  void discardStaged();

  bool hasSecret() const { return !secretObfuscated.empty(); }
  bool readSecret(char* out, size_t size) const;
  void writeSecret(const char* secret);
  void clearSecret();

  // 'd' + 8 hex tires du generateur materiel : pas de table de correspondance
  // cote serveur, et rejouer un ajout apres coupure est sans effet.
  static std::string newDeviceId();
};

#define TASK_STORE TaskStore::getInstance()
