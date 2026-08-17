#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <HalStorage.h>

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Viewer des articles Raindrop : liste /Articles depuis l'index TSV que la
// sync ecrit (/Articles/.index, snapshot serveur : fichier, titre, tags,
// octets, date). Filtre par tag, recherche par titre, marquer-lu — sans
// jamais rouvrir un .md. Si l'index manque (jamais synchronise depuis la
// mise a jour), il est synthetise depuis le dossier, sans tags.
class ArticlesActivity final : public Activity {
  using UiApp = freeink::ui::FreeInkApp<20, 4>;

  struct Row {
    std::string file;      // nom du .md dans /Articles
    std::string title;
    std::string subtitle;  // "date · ≈N p. · tags"
    std::string tags;      // CSV, pour le menu de filtre
  };

  // Offsets des lignes de l'index qui survivent au filtre courant, dans
  // l'ordre du fichier (le serveur ecrit les recents d'abord).
  std::vector<uint32_t> visible;
  std::vector<std::string> hiddenNames;  // .done + marques-lus cette session
  std::string filterTag;                 // vide = tous
  std::string searchQuery;               // vide = pas de recherche
  std::string highlightFile;             // selection initiale (retour de lecture)

  static constexpr size_t ROW_CACHE = 12;
  std::array<Row, ROW_CACHE> rowCache;
  std::array<uint32_t, ROW_CACHE> rowCacheKeys{};
  // Handle garde ouvert entre les lignes affichees : ouvrir .index par nom
  // rebalaie le repertoire FAT a chaque fois. Ferme dans onExit.
  HalFile indexFile;
  bool indexOpen = false;

  ButtonNavigator buttonNavigator;
  size_t selectorIndex = 0;
  int topIndex = 0;
  int visibleRowCount = 1;
  bool lockNextConfirmRelease = false;
  bool longPressConfirmHandled = false;
  // L'e-ink est lent : un second appui pendant la transition d'activite
  // arriverait comme un clic valide. Les entrees sont absorbees tant que le
  // premier rendu n'est pas a l'ecran, plus une marge.
  std::atomic<unsigned long> firstRenderDoneMs{0};

  freeink::ui::GfxRendererTarget uiTarget;  // doit preceder `app`
  UiApp app;
  std::atomic<bool> uiReady{false};

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  bool ensureIndexFile();
  void loadHiddenNames();
  void rebuildVisible();
  const Row& rowAt(size_t displayRow);
  bool readIndexLine(HalFile& file, uint32_t offset, Row& out) const;
  bool isHidden(const std::string& file) const;
  void activateSelected();
  void openActionsMenu();
  void openTagFilterMenu();
  void openSearch();
  void markSelectedDone();
  void clearRowCache();

 public:
  explicit ArticlesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string highlightFile = {});
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
