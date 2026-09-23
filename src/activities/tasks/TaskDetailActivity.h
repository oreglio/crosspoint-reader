#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "tasks/TaskOpQueue.h"
#include "tasks/TaskRecord.h"

// Detail d'une tache : son titre, sa note paginee, et un pomodoro a lancer
// dessus. Ouvert depuis TaskListActivity par le bouton Droite.
//
// Derive d'Activity, PAS d'UiListActivity : ce n'est pas une liste, et le
// header d'UiListActivity interdit explicitement d'en deriver pour autre
// chose. Les trois ecrans comparables du depot (BookStatsActivity,
// DictionaryDefinitionActivity, PomodoroActivity) font le meme choix.
//
// Boutons. mapLabels(back, confirm, previous, next) lie ses quatre arguments
// a Retour / Confirmer / Gauche / Droite dans cet ordre — ce n'est pas une
// barre de quatre cases libres, et Haut/Bas n'y figurent pas. D'ou :
// Retour = sortir, Confirmer = cocher, Gauche = modifier, Droite = pomodoro,
// et Haut/Bas tournent les pages de la note. C'est precisement parce que la
// barre ne contient pas Haut/Bas que la pagination peut les prendre sans
// entrer en conflit avec un libelle.
//
// L'index des taches. TASK_STORE.ensureLoaded() en entree, et JAMAIS
// unload() : TaskListActivity est toujours sur la pile en dessous et tient
// des indices dans le vecteur du store (ordre + lignes). Le decharger d'ici
// laisserait ces indices pendants. unload() appartient a
// TaskListActivity::onExit(), qui est le seul detenteur.
class TaskDetailActivity final : public Activity {
 public:
  TaskDetailActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* taskId);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Ce que la note a donne. Distinguer "vide" de "illisible" evite d'annoncer
  // qu'une tache n'a pas de note quand c'est la carte SD qui a refuse.
  // Trois echecs distincts, pas un seul : "il n'y a pas de note", "l'index en
  // annonce une mais le fichier a disparu" et "la carte a refuse la lecture" ne
  // disent pas la meme chose a l'utilisateur.
  enum class NoteState : uint8_t { Empty, Ready, Missing, Unreadable };

  // Lignes de note materialisees par page. La region la plus haute
  // envisageable (800 px de haut) tient 30 lignes a 26 px d'advanceY, donc 32
  // borne le tableau sans jamais mordre sur un cas reel ; linesPerPage y est
  // ecrete.
  static constexpr size_t kNoteMaxLines = 32;
  // Bornes du tampon de page. Il est dimensionne a partir de la region (voir
  // computeLayout) mais jamais au-dela de kNotePoolMaxBytes : au pire une page
  // se termine plus tot et la note gagne une page — aucun octet n'est perdu.
  static constexpr size_t kNotePoolMinBytes = 256;
  static constexpr size_t kNotePoolMaxBytes = 1024;
  // Avance du glyphe le plus etroit de LexendDeca 10, utilisee UNIQUEMENT
  // pour majorer le nombre d'octets qu'une ligne peut contenir. Sous-estimer
  // ne corrompt rien : ca rallonge le tampon, et il est de toute facon ecrete.
  static constexpr int kNarrowestAdvancePx = 4;
  // Bornes le vecteur d'offsets. Une note fait au plus TASK_NOTE_MAX octets ;
  // 256 pages couvrent meme une note qui ne serait faite que de lignes d'un
  // caractere.
  static constexpr size_t kMaxNotePages = 256;

  // --- donnees ---
  std::string taskId;
  // Copie, pas un pointeur dans le vecteur du store : upsert() remplace en
  // place pour un id existant, mais s'appuyer sur ca a distance est une
  // reference pendante qui attend son jour. 218 octets dans une activite qui
  // vit sur le tas.
  TaskRecord record{};
  bool hasRecord = false;
  // Derniere ecriture refusee par la file d'ops : l'ecran le dit, pas
  // seulement le port serie (meme choix que la sous-ligne d'etat de la liste).
  bool writeFailed = false;
  // Au moins une op appliquee pendant cette visite. Rendu a la liste en
  // sortant : elle n'est jamais re-entree (pas de onEnter() sur le chemin de
  // depilement), donc son ordre et ses lignes restent ceux d'avant et elle
  // afficherait un titre ou une priorite perimes. Un simple coup d'oeil, lui,
  // ne doit rien lui couter.
  bool changedAnything = false;

  // Titre deja enroule, recalcule a l'entree et apres une modification — pas a
  // chaque rendu : render() tourne sur la tache de rendu et ne doit pas
  // allouer par image.
  std::vector<std::string> titleLines;

  // --- geometrie, calculee une fois en entree ---
  // Gardee en entiers plutot qu'en Rect pour que ce header n'ait pas a tirer
  // tout components/themes.
  int contentX = 0;
  int contentWidth = 0;
  int titleTop = 0;
  // Ligne d'etat sous le titre : elle porte "faite" et, le cas echeant, le
  // refus d'ecriture. Le depot n'a pas de toast reutilisable hors du lecteur,
  // et la liste utilise deja sa sous-ligne d'en-tete pour la meme chose.
  int statusTop = 0;
  int pomodoroX = 0;
  int pomodoroY = 0;
  int pomodoroWidth = 0;
  int pomodoroHeight = 0;

  // --- note ---
  // Region bornee du texte, en coordonnees ecran.
  int noteX = 0;
  int noteY = 0;
  int noteWidth = 0;
  int noteHeight = 0;
  int noteAdvanceY = 0;
  int noteLinesPerPage = 1;
  NoteState noteState = NoteState::Empty;
  // Tampon de la page courante. +1 octet jamais rempli : l'adaptateur de
  // mesure termine temporairement la chaine en place, donc window[prefixLen]
  // doit rester ecrivable meme pour prefixLen == taille lue.
  std::unique_ptr<char[]> notePool;
  size_t notePoolCapacity = 0;
  size_t notePoolLength = 0;
  uint16_t lineStart[kNoteMaxLines] = {};
  uint16_t lineLength[kNoteMaxLines] = {};
  size_t lineCount = 0;
  // Offset de depart de chaque page, calcule une fois a l'entree. Une note
  // plafonne a TASK_NOTE_MAX (4096) octets, donc uint16_t suffit.
  std::vector<uint16_t> pageOffsets;
  size_t currentPage = 0;

  // --- rafraichissement ---
  // Une passe complete coute pres d'une seconde sur ces dalles : elle est
  // reservee a l'entree et au retour d'un ecran enfant, qui a repeint tout le
  // panneau. Une coche ou une tournee de page passe en rapide, avec une passe
  // complete periodique pour le residu d'encre (meme cadence que le pomodoro,
  // rapportee au nombre bien plus faible de repeints de cet ecran).
  static constexpr int kRepaintsPerFullRefresh = 8;
  bool pendingFullRefresh = true;
  int repaintsSinceFullRefresh = 0;

  // --- helpers ---
  bool refreshRecord();
  void rebuildTitleLines();
  void computeLayout();
  bool allocateNotePool();
  // Chacune ouvre et referme le fichier de note dans sa propre portee : aucun
  // handle n'est detenu entre deux images, ce qui compte parce que onExit() ne
  // tourne pas tant qu'un ecran enfant est au-dessus (un pomodoro tiendrait le
  // handle des heures, et a travers HalStorage::shutdown()).
  //
  // La fermeture elle-meme vient du destructeur de HalFile, qui est RAII
  // (lib/hal/HalStorage.cpp:280) : toute sortie ferme, `return` precoce
  // compris. La regle "aucun `return` entre l'ouverture et le close()" reste
  // tenue parce qu'elle rend la discipline verifiable a la lecture — la seule
  // verification disponible ici, SdFat n'autorisant qu'un handle par chemin sur
  // le materiel.
  void paginateNote();
  void loadPage(size_t page);
  // Decoupe `notePoolLength` octets deja lus en au plus noteLinesPerPage
  // lignes. Rend le nombre d'octets consommes ; remplit lineStart/lineLength
  // seulement si `keepLines`.
  size_t wrapLoadedWindow(bool windowIsFinal, bool keepLines);
  void showPage(size_t page);

  // Seule porte de sortie : elle pose le resultat avant de depiler, pour qu'il
  // n'y ait pas un chemin de sortie qui le pose et un autre qui l'oublie.
  void finishWithResult();
  void toggleDone();
  void editTitle();
  void editPriority();
  void startPomodoro();
  // Ecrit les deux fichiers SD PUIS prend le verrou de rendu pour appliquer la
  // mutation en memoire : appendOp() ajoute une ligne et upsert() reserialise
  // tout l'index, et render() ne lit rien de tout ca. Les tenir sous le verrou
  // bloquait la tache de rendu pendant deux ecritures SD sans rien protéger.
  // `rewrapTitle` fait rentrer rebuildTitleLines() dans ce meme verrou, pour
  // qu'aucune image ne puisse voir le nouveau titre avec l'ancien enroulement.
  //
  // L'appelant ne doit donc PAS detenir le verrou (il est non recursif). Seul
  // onEnter() appelle rebuildTitleLines() directement, verrou tenu, et il
  // n'appelle jamais celle-ci.
  bool appendAndApply(const TaskOp& op, const TaskRecord& next, bool rewrapTitle);

  // Non-const : dessiner une ligne la termine temporairement en place dans
  // notePool, exactement comme la mesure. Le declarer const mentirait.
  void drawNote();
  void drawPomodoroBlock() const;

  // Adaptateur de mesure passe a TaskNotePager. Mesure un PREFIXE de la
  // fenetre en la terminant temporairement en place — aucune copie par
  // mesure, meme procede que KeyboardEntryActivity::measureRange.
  static int measureNotePrefix(void* ctx, const char* window, size_t prefixLen);
};
