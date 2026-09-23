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
 * du base64 illisible plutot que le secret en clair. Il vit dans SON PROPRE
 * fichier (secretPath), ecrit seulement a l'appairage : aucune reecriture de
 * l'index — la sync en fait une par tour — ne peut donc toucher l'identifiant.
 *
 * Tous les fichiers que la sync reecrit (index, curseur, compteur d'ops
 * acquittees, secret) passent par un fichier .tmp puis un remplacement, et
 * loadFromFile() promeut un .tmp orphelin s'il se lit. Un index illisible au chargement
 * efface le curseur : la sync suivante repart d'un instantane complet au lieu
 * d'un delta qui laisserait manquer toutes les taches plus anciennes.
 */
class TaskStore : public PersistableStore<TaskStore> {
 private:
  std::vector<TaskRecord> records;
  std::string secretObfuscated;

  TaskStore() = default;
  friend class PersistableStore<TaskStore>;
  // Masque : saveToFile() supprime puis reecrit via un String que le manque de
  // memoire tronque en silence. Toute ecriture de l'index passe par saveIndex().
  using PersistableStore<TaskStore>::saveToFile;

 public:
  static const char* getFilePath() { return "/.crosspoint/tasks/index.json"; }
  static const char* opsPath() { return "/.crosspoint/tasks/ops.ndjson"; }
  static const char* indexTmpPath() { return "/.crosspoint/tasks/index.tmp"; }
  static const char* cursorPath() { return "/.crosspoint/tasks/cursor.txt"; }
  static const char* cursorTmpPath() { return "/.crosspoint/tasks/cursor.tmp"; }
  static const char* ackedOpsPath() { return "/.crosspoint/tasks/acked.txt"; }
  static const char* ackedOpsTmpPath() { return "/.crosspoint/tasks/acked.tmp"; }
  static const char* secretPath() { return "/.crosspoint/tasks/secret"; }
  static const char* secretTmpPath() { return "/.crosspoint/tasks/secret.tmp"; }
  static const char* notesDir() { return "/.crosspoint/tasks/n"; }

  // Cree l'arborescence /.crosspoint/tasks (et son sous-dossier n/), promeut
  // les .tmp orphelins, lit le secret, puis delegue au chargement standard.
  // Un index absent ou illisible efface le curseur (voir l'en-tete).
  bool loadFromFile();
  // Relit SEULEMENT le fichier du secret, sans l'index : l'ecran d'appairage
  // n'a besoin de rien d'autre, et charger l'index lui laisserait ses ~26 Ko
  // en RAM pour le reste de la session, lecture comprise. Ne marque pas le
  // store charge : un lecteur de l'index fait toujours ensureLoaded().
  void ensureSecretLoaded();
  // Ecriture sure de l'index (voir PersistableStoreBase::writeDocToFileAtomic).
  bool saveIndex() const;

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::vector<TaskRecord>& all() const { return records; }
  // Rend vraiment la memoire de l'index (jusqu'a 120 x 218 octets = ~26 Ko,
  // 7 % de la RAM d'un C3) et arme le prochain ensureLoaded() pour qu'il
  // relise la carte. Seul TaskListActivity::onExit() l'appelle : c'est le seul
  // detenteur d'indices dans ce vecteur.
  //
  // RIEN ne charge ce store a un demarrage NORMAL (pour ne pas payer ces 26 Ko
  // pendant la lecture) ; seul le demarrage reseau TASK_SYNC le charge, dans
  // main.cpp. Tout autre LECTEUR doit appeler ensureLoaded() lui-meme, ou
  // ensureSecretLoaded() s'il ne lit que le secret : hasSecret() et
  // readSecret() ne chargent rien, et sur un store jamais charge ils repondent
  // « pas de secret » alors que la carte en contient un. C'est ainsi que
  // l'ecran d'appairage desappairait l'appareil apres un redemarrage.
  //
  // ATTENTION aux ecrivains : toute methode qui appelle saveIndex()
  // reserialise `records`, donc ecrire dans un store decharge ecraserait
  // index.json avec une liste vide. Les mutateurs concernes (upsert, remove)
  // appellent ensureLoaded() en entree pour cette raison. replaceAll() et
  // stageReset() aussi : ils remplacent tout, mais un store non marque charge
  // serait relu par le prochain ensureLoaded(), qui ecraserait leur resultat.
  void unload();
  void replaceAll(std::vector<TaskRecord> next);
  // False si l'enregistrement est refuse (id malforme, plafond) ou si l'index
  // n'a pas pu etre ecrit — la RAM porte alors deja le changement, pas la
  // carte : l'appelant le dit a l'ecran.
  bool upsert(const TaskRecord& rec);
  void remove(const char* id);
  const TaskRecord* find(const char* id) const;

  bool appendOp(const TaskOp& op);
  // `skip` saute les premieres ops valides : une sync envoie au plus
  // TASK_MAX_OPS_PER_SYNC ops par requete et lit les suivantes par tranches,
  // sans toucher au fichier avant d'avoir tout envoye.
  size_t readOps(TaskOp* out, size_t max, size_t skip = 0) const;
  // Vrai s'il reste en file une op que le serveur n'a pas encore acceptee.
  // Lit la carte : jamais depuis le rendu.
  bool hasPendingOps() const;
  // Efface la file AVANT le compteur d'ops acquittees. Une coupure entre les
  // deux laisse un compteur sans file, que loadFromFile() et appendOp() jettent
  // avant tout ajout : il ne fait donc jamais sauter d'op. Dans l'autre ordre,
  // la meme coupure laissait une file sans compteur, rejouee en entier.
  void clearOps();
  // Nombre d'ops en tete de file que le serveur a deja acceptees : une sync en
  // plusieurs tranches l'ecrit a chaque tour valide, et la suivante s'en sert
  // comme `skip`. Une op acceptee n'est ainsi jamais renvoyee — son rejeu
  // n'est pas sans effet (done/prio/title sont des affectations, qui
  // ecraseraient une modification faite entre-temps sur le web).
  size_t readAckedOps() const;
  bool writeAckedOps(size_t count);
  // Titre d'une op add/title encore en file, pour nommer un rejet quand la
  // tache n'est plus dans l'index. Faux si aucune op de la file ne le porte.
  bool findQueuedTitle(const char* id, char* out, size_t size) const;

  bool readCursor(char* out, size_t size) const;
  bool writeCursor(const char* cursor);
  // Sans curseur, la sync suivante demande un instantane complet.
  void clearCursor();
  // Alphabet base64url, non vide, au plus 32 caracteres — le meme controle
  // que RaindropSyncActivity applique deja a son propre curseur opaque.
  static bool isSafeCursor(const char* cursor);

  // Refuse tout ce qui ne colle pas a ^[wd][0-9a-f]{8}$ avant de batir un
  // chemin : c'est le seul rempart entre un serveur compromis et une ecriture
  // arbitraire sur la carte SD.
  static bool notePath(const char* id, char* out, size_t size);
  bool clearAllNotes() const;

  // --- Mise en attente d'une reponse de sync ---
  //
  // La sync applique la reponse du serveur au fil de l'eau, mais rien ne doit
  // atteindre la carte avant qu'elle soit entierement acceptee : un corps coupe
  // a mi-chemin ne doit changer ni index.json, ni les notes, ni le curseur, ni
  // la file d'ops. Les stage*() modifient donc `records` en RAM SANS
  // sauvegarder, et les notes recues s'ecrivent sous stagedNotesDir(). Sur
  // succes, l'appelant sauvegarde (saveIndex) puis commitStagedNotes() ; sur
  // echec, discardStaged() relit la carte et jette les notes en attente.
  static const char* stagedNotesDir() { return "/.crosspoint/tasks/ns"; }
  static bool stagedNotePath(const char* id, char* out, size_t size);
  // Porte la capacite de `records` a MAX_TASKS en une fois, ou rend false sans
  // rien allouer si le tas n'a pas de bloc contigu assez grand. fromJson()
  // reserve le nombre exact de taches chargees, donc le premier ajout
  // reallouerait au double par un `new` qui aborte (jusqu'a 43 Ko pendant que
  // l'ancien bloc vit encore). stageUpsert() refuse au-dela de MAX_TASKS, donc
  // apres cet appel aucune reallocation n'est possible. Deux appelants : la
  // sync, avant la connexion (les stage*() tournent dans le rappel TLS), et la
  // creation d'une tache, avant d'ouvrir le clavier.
  bool reserveFullCapacity();
  void stageReset();
  bool stageUpsert(const TaskRecord& rec);
  bool stageRemove(const char* id);
  bool clearStagedNotes() const;
  // Deplace les notes en attente dans notesDir(), puis supprime toute note
  // dont la tache n'est plus indexee ou n'a plus de note (noteBytes == 0).
  // False si un deplacement a echoue : l'appelant ne doit alors pas avancer le
  // curseur, pour que le serveur renvoie la ligne avec sa note.
  bool commitStagedNotes() const;
  void discardStaged();

  // Ne chargent PAS le store : l'appelant fait ensureLoaded() d'abord (voir
  // unload()).
  bool hasSecret() const { return !secretObfuscated.empty(); }
  bool readSecret(char* out, size_t size) const;
  // False si la carte a refuse l'ecriture ; le secret en RAM reste alors
  // l'ancien, pour que RAM et carte disent la meme chose — sinon la session
  // enverrait un secret qu'un redemarrage oublierait.
  bool writeSecret(const char* secret);
  void clearSecret();

  // 'd' + 8 hex tires du generateur materiel : pas de table de correspondance
  // cote serveur, et rejouer un ajout apres coupure est sans effet.
  static std::string newDeviceId();
};

#define TASK_STORE TaskStore::getInstance()
