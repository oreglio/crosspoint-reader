#pragma once

#include <cstddef>
#include <cstdint>

#include "TaskRecord.h"

// Version de cadrage que ce lecteur sait lire. Un en-tête qui annonce autre
// chose peut cadrer différemment : mieux vaut refuser la réponse que relire une
// note comme si c'était du JSON.
inline constexpr long TASK_SYNC_SCHEMA = 1;

// Le curseur est opaque et base64url ; la spec le plafonne à 32 caractères et
// le pire cas mesuré côté serveur en fait 22.
inline constexpr size_t TASK_SYNC_CURSOR_MAX = 32;

struct TaskSyncCallbacks {
  void* ctx;
  void (*onHeader)(void* ctx, const char* cursor, bool more, bool reset);
  void (*onTask)(void* ctx, const TaskRecord& rec);
  void (*onDeleted)(void* ctx, const char* id);
  // Émis par morceaux au fil de l'arrivée réseau ; `last` marque le dernier.
  void (*onNoteChunk)(void* ctx, const char* id, const char* data, size_t len, bool last);
  // Une op refusée par le serveur, annoncée dans le tableau `rejected` de
  // l'en-tête : `reason` vaut "full", "unknown" ou "badid". Émis pendant
  // l'analyse de la ligne d'en-tête, donc avant onHeader — et avant qu'on
  // sache si cette ligne est valide : une entrée refermée juste avant une
  // coupure remonte, puis la ligne est rejetée. Le consommateur doit donc
  // jeter les rejets accumulés dès que hasError() est vrai, sinon l'écran de
  // résumé nommera une tâche refusée pour une réponse qu'on a jetée.
  void (*onRejected)(void* ctx, const char* id, const char* reason);
};

// Lit le corps cadré de POST /api/v1/tasks/sync.
//
// Le corps est du NDJSON avec une exception : quand une ligne de métadonnées
// porte noteBytes > 0, exactement ce nombre d'octets BRUTS suit, puis un \n non
// compté. Le lecteur bascule donc entre deux modes — lignes et comptage — ce
// qui permet à une note de contenir des \n et lui évite de passer par le tampon
// de jeton de 512 octets de StreamingJsonParser.
//
// Tout ce qui casse le cadrage — ligne illisible, ligne trop longue, note non
// terminée par un \n, schéma inconnu — lève hasError() et arrête la lecture :
// une fois le cadrage perdu, continuer reviendrait à relire des octets de note
// comme des métadonnées, c'est-à-dire à corrompre en silence.
//
// Aucune allocation : la ligne courante tient dans un tampon membre, les octets
// de note sont relayés sans être stockés.
class TaskSyncReader {
 public:
  // Une ligne de métadonnées vaut au pire ~480 octets (titre échappé + champs).
  static constexpr size_t LINE_BUF_SIZE = 640;

  explicit TaskSyncReader(const TaskSyncCallbacks& callbacks) : cb_(callbacks) {}

  // Chaque fin de ligne coûte une pointe de pile transitoire, mesurée à
  // ~1424 octets sur le chemin d'une ligne de tâche (~1200 pour l'en-tête) avec
  // la chaîne du dépôt : riscv32-esp-elf-g++ -Os -fno-exceptions -fstack-usage.
  // L'essentiel vient de deux cadres locaux à l'analyse d'une ligne, le
  // collecteur de champs (576 avec TaskRecord) et StreamingJsonParser (672).
  // Les deux sont de taille fixe et aucune récursion n'est possible ici : une
  // entrée pathologique ne peut donc pas creuser plus. La tâche qui alimente
  // feed() doit prévoir au moins 4 Ko, la taille retenue pour le travail
  // réseau — et la pointe survient dans le rappel de réception TLS.
  void feed(const char* data, size_t len);
  bool hasError() const { return error_; }
  bool sawHeader() const { return sawHeader_; }

  // Vrai si le corps s'est arrêté sur une frontière propre. Un corps coupé en
  // plein milieu d'une note ou d'une ligne n'est pas une erreur de cadrage,
  // mais il ne doit pas faire avancer le curseur.
  bool isComplete() const { return sawHeader_ && !error_ && mode_ == Mode::Line && lineLen_ == 0; }

 private:
  enum class Mode : uint8_t { Line, NoteBytes };

  void consumeLine(size_t len, bool dropped);
  void parseHeaderLine(size_t len);
  void parseTaskLine(size_t len);
  void beginNoteBlock(const char* id, size_t bytes);

  TaskSyncCallbacks cb_;
  Mode mode_ = Mode::Line;
  char line_[LINE_BUF_SIZE] = {};
  size_t lineLen_ = 0;
  bool overflow_ = false;
  bool error_ = false;
  bool sawHeader_ = false;

  // État de la note en cours de relais.
  char noteId_[TASK_ID_LEN + 1] = {};
  size_t noteRemaining_ = 0;
  bool noteSwallowNewline_ = false;
  // Note d'une tâche refusée : ses octets sont consommés pour garder le
  // cadrage, mais rien n'est remonté.
  bool noteDiscard_ = false;
};
