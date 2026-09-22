#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "tasks/TaskSecret.h"

// Appairage avec le serveur CrossDrop : l'appareil fabrique son propre secret
// et l'affiche en QR et en clair, groupe par quatre ; l'utilisateur le scanne
// ou le tape sur la page web du serveur. Ouvert depuis Reglages > Systeme.
//
// Derive d'Activity, PAS de QrDisplayActivity : celle-ci est `final`, ne prend
// qu'une charge utile et n'offre aucun point d'accroche pour une action de
// pied de page. Sa moitie reutilisable, QrUtils::drawQrCode, est appelee
// directement.
//
// Le secret stocke est reaffiche a chaque entree : en tirer un nouveau a
// chaque visite casserait l'appairage de quiconque revient simplement relire
// son code. Seul le bouton Droite (« changer ») en tire un neuf — et c'est
// Droite, pas Confirmer, parce que Confirmer se presse par reflexe comme un
// « OK » et que cette action oblige a refaire l'appairage sur le web.
//
// Le secret n'est JAMAIS journalise, a aucun niveau, pas meme tronque : c'est
// le seul identifiant que l'appareil detienne et le port serie n'est pas prive.
//
// Seule la forme canonique (26 caracteres, sans separateur) atteint
// TASK_STORE.writeSecret() et le QR : voir tasks/TaskSecret.h pour la raison.
// Le groupement par quatre ne vit que dans codeLines, pour l'affichage.
class TaskPairActivity final : public Activity {
 public:
  TaskPairActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TaskPair", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // 7 groupes : au pire un groupe par ligne, jamais plus.
  static constexpr size_t kMaxCodeLines = 7;
  static constexpr size_t kHintMaxLines = 2;

  // Forme canonique, la seule que l'on stocke et encode dans le QR.
  char secret[TASK_SECRET_LEN + 1] = {};
  // Meme contenu pour QrUtils::drawQrCode, qui prend un std::string : construit
  // une fois par secret plutot qu'a chaque image sur la tache de rendu.
  std::string qrPayload;
  // Forme groupee, coupee entre deux groupes selon la largeur disponible dans
  // la police du code. Affichage seulement.
  char codeLines[kMaxCodeLines][TASK_SECRET_GROUPED_LEN + 1] = {};
  size_t codeLineCount = 0;
  // Vrai apres « changer » : l'ecran rappelle qu'il faut refaire l'appairage.
  bool renewed = false;

  std::vector<std::string> hintLines;

  // --- geometrie, calculee une fois en entree ---
  int contentX = 0;
  int contentWidth = 0;
  int hintTop = 0;
  int qrTop = 0;
  int qrAreaHeight = 0;
  int qrSize = 0;
  int codeTop = 0;
  int statusTop = 0;

  void computeLayout();
  // Relit le secret stocke ; en tire un neuf s'il manque ou n'est pas
  // canonique (un secret non canonique ne passerait jamais l'en-tete Bearer).
  void loadOrCreateSecret();
  void renewSecret();
  void adoptSecret(const char* canonical);
  void layoutCode();
};
