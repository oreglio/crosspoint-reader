#pragma once

#include <string>

#include "activities/Activity.h"

struct Rect;

// Downloads the article shelf published by a CrossDrop companion server
// (Raindrop.io sync) into /Articles as .md files the Library indexes.
//
// Runs only from a minimal network boot (NetworkBootTarget::RAINDROP_SYNC):
// connect wifi, walk the paginated manifest, fetch what is missing or changed,
// then reboot back to the full app. Nothing here survives the sync session, so
// the whole activity stays out of the steady-state RAM budget.
class RaindropSyncActivity : public Activity {
 public:
  explicit RaindropSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The sync is synchronous and blocks the main loop until it completes, so
  // the manager never polls this mid-transfer; it matters for the end screens.
  bool preventAutoSleep() override { return state_ != State::COMPLETE && state_ != State::ERROR; }
  bool skipLoopDelay() override { return state_ == State::CONNECTED; }

 private:
  enum class State {
    WIFI_SELECTION,
    CONNECTED,  // wifi up, sync not started yet: next loop() runs it
    SYNCING,
    COMPLETE,
    ERROR,
  };

  void onWifiSelectionComplete(bool connected);
  void runSync();
  // One manifest page: download, parse, fetch its articles. Returns false when
  // the walk must stop (error or last page); `cursor` carries the pagination.
  bool syncOnePage(std::string& cursor, bool& morePages);
  bool downloadArticle(const std::string& fileName, size_t expectedBytes);
  bool pollCancel();

  State state_ = State::WIFI_SELECTION;
  // Counters drawn on the progress and summary screens.
  int newCount_ = 0;
  int keptCount_ = 0;
  int failedCount_ = 0;
  int pageCount_ = 0;
  bool cancelRequested_ = false;
  unsigned long lastProgressDrawMs_ = 0;
  std::string errorMessage_;
};
