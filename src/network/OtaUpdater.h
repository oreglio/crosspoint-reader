#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#if __has_include(<AppVersion.h>)
#include <AppVersion.h>
#endif

// Stable reads /releases/latest, which GitHub defines as excluding prereleases.
// Beta reads /releases, which includes them — so a beta device sees whatever is
// newest, prerelease or not, and is never stranded behind the stable line.
enum class OtaChannel : uint8_t { Stable, Beta };

class OtaUpdater {
  bool updateAvailable = false;
  std::string latestVersion;
  std::string otaUrl;
  std::string otaSha256;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;
  OtaChannel channel = OtaChannel::Stable;
  bool allowOlder = false;

 public:
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    CANCELLED_ERROR,
    HASH_MISMATCH_ERROR,
    WRONG_DEVICE_ERROR,
  };

  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return processedSize; }

  size_t getTotalSize() const { return totalSize; }

  OtaUpdater() = default;

  // Carried as state rather than as arguments on purpose: the simulator build
  // excludes OtaUpdater.cpp (platformio.ini) and an adjacent repository defines
  // checkForUpdate/installUpdate out-of-line, so their signatures cannot change
  // from here. Both setters are inline for the same reason — no symbol to link.
  void setChannel(const OtaChannel value) { channel = value; }
  // Stated by the caller once the user has been shown the direction of the change
  // and accepted it. Anti-rollback is disabled in sdkconfig, so nothing after
  // this will refuse an older image.
  void setAllowOlder(const bool value) { allowOlder = value; }

  bool isUpdateNewer() const;

  // An asset was found and its tag differs from the running version, in either
  // direction. isUpdateNewer() answers the narrower question and is unchanged.
  bool isDifferentVersion() const {
#ifdef CROSSINK_VERSION
    return updateAvailable && !latestVersion.empty() && latestVersion != CROSSINK_VERSION;
#else
    return updateAvailable && !latestVersion.empty();
#endif
  }
  const std::string& getLatestVersion() const;
  OtaUpdaterError checkForUpdate();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr,
                                std::atomic<bool>* cancelRequested = nullptr);
};
