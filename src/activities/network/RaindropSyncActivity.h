#pragma once

#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"

struct Rect;

// Syncs the article shelf published by a CrossDrop companion server
// (Raindrop.io) into /Articles as .md files the Library indexes.
//
// One request does the whole sync: the server packs every article newer than
// the device's stored cursor into a STORED zip (single TLS handshake — the
// per-article scheme died of repeated-handshake OOM on the C3), which is then
// unpacked with the EPUB zip reader. Runs only from a minimal network boot
// (NetworkBootTarget::RAINDROP_SYNC) and reboots back to the full app.
class RaindropSyncActivity : public Activity {
 public:
  explicit RaindropSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The sync runs synchronously inside the wifi callback, so the manager only
  // polls this on the end screens, where staying awake keeps the summary up.
  bool preventAutoSleep() override { return state_ == State::SYNCING; }

 private:
  enum class State {
    WIFI_SELECTION,
    SYNCING,
    COMPLETE,
    ERROR,
  };

  void onWifiSelectionComplete(bool connected);
  void runSync();
  bool downloadBundle();
  bool unpackBundle();
  std::string readStoredCursor();
  void storeCursor(const std::string& cursor);
  bool pollCancel();

  State state_ = State::WIFI_SELECTION;
  ScreenTransitionRefresh screenTransitionRefresh_;
  int newCount_ = 0;
  int failedCount_ = 0;
  bool cancelRequested_ = false;
  bool unpacking_ = false;
  size_t downloadedBytes_ = 0;
  size_t totalBytes_ = 0;
  std::string errorMessage_;
  // Article being extracted right now, drawn on the sync screen.
  std::string currentArticle_;
};
