#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(path, json)) {
    LOG_ERR("PERSIST", "Failed to write %s", path);
    return false;
  }
  return true;
}

namespace {

// serializeJson() on a bare Print writes byte by byte, and every HalFile byte
// write takes the storage lock. 128 bytes of stack batch it into a handful of
// SD writes without ever holding the whole document in RAM.
class BufferedFilePrint : public Print {
 public:
  explicit BufferedFilePrint(HalFile& file) : file_(file) {}
  size_t write(uint8_t c) override {
    buffer_[used_++] = c;
    if (used_ == sizeof(buffer_)) flushBuffer();
    return 1;
  }
  // ArduinoJson hands over whole chunks when the destination accepts them.
  size_t write(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) write(data[i]);
    return len;
  }
  void flushBuffer() {
    if (used_ == 0) return;
    if (file_.write(buffer_, used_) != used_) failed_ = true;
    written_ += used_;
    used_ = 0;
  }
  bool ok() const { return !failed_; }
  size_t written() const { return written_; }

 private:
  HalFile& file_;
  uint8_t buffer_[128];
  size_t used_ = 0;
  size_t written_ = 0;
  bool failed_ = false;
};

}  // namespace

bool PersistableStoreBase::writeDocToFileAtomic(const char* path, const char* tmpPath, const JsonDocument& doc) {
  if (doc.overflowed()) {
    LOG_ERR("PERSIST", "Refusing to write %s: the JSON document overflowed", path);
    return false;
  }
  const size_t expected = measureJson(doc);
  HalFile file;
  if (!Storage.openFileForWrite("PERSIST", tmpPath, file)) {
    LOG_ERR("PERSIST", "Could not open %s", tmpPath);
    return false;
  }
  BufferedFilePrint out(file);
  serializeJson(doc, out);
  out.flushBuffer();
  bool ok = out.ok() && out.written() == expected;
  ok = file.sync() && ok;
  ok = file.close() && ok;
  if (!ok) {
    LOG_ERR("PERSIST", "Short write of %s (%u of %u bytes)", tmpPath, static_cast<unsigned>(out.written()),
            static_cast<unsigned>(expected));
    Storage.remove(tmpPath);
    return false;
  }
  return replaceWithTmp(tmpPath, path);
}

bool PersistableStoreBase::replaceWithTmp(const char* tmpPath, const char* path) {
  if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR("PERSIST", "Could not remove %s to replace it", path);
    return false;
  }
  if (!Storage.rename(tmpPath, path)) {
    LOG_ERR("PERSIST", "Could not rename %s to %s", tmpPath, path);
    return false;
  }
  return true;
}

void PersistableStoreBase::recoverReplacedFile(const char* path, const char* tmpPath) {
  if (!Storage.exists(tmpPath)) return;
  if (Storage.exists(path)) {
    Storage.remove(tmpPath);
    return;
  }
  LOG_INF("PERSIST", "Promoting %s left by an interrupted replace", tmpPath);
  if (!Storage.rename(tmpPath, path)) LOG_ERR("PERSIST", "Could not promote %s", tmpPath);
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  if (!Storage.exists(path)) {
    return false;  // Expected on first boot — not an error.
  }
  String json = Storage.readFile(path);
  if (json.isEmpty()) {
    LOG_ERR("PERSIST", "Failed to read %s (empty)", path);
    return false;
  }
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", path, error.c_str());
    return false;
  }
  return true;
}
