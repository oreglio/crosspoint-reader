#pragma once

#include "I18nKeys.h"
#include "activities/Activity.h"
#include "network/OtaUpdater.h"

class OtaUpdateActivity : public Activity {
  enum State {
    CHANNEL_SELECTION,
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    UPDATE_IN_PROGRESS,
    NO_UPDATE,
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };

  // Can't initialize this to 0 or the first render doesn't happen
  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;

  State state = CHANNEL_SELECTION;
  OtaChannel channel = OtaChannel::Stable;
  // True when the release found is older than the one running: the confirmation
  // names the direction, and installUpdate is told the user accepted it.
  bool installingOlder = false;
  unsigned int lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  StrId failureMessage = StrId::STR_UPDATE_FAILED;
  OtaUpdater updater;

  void askChannel();
  void startWifiFlow();
  void onWifiSelectionComplete(bool success);
  void runUpdateInstall();

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OtaUpdate", renderer, mappedInput), updater() {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CHECKING_FOR_UPDATE || state == UPDATE_IN_PROGRESS; }
  bool skipLoopDelay() override { return true; }  // Prevent power-saving mode
};
