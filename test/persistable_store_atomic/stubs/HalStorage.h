#pragma once

#include <Arduino.h>

#include <string>
#include <unordered_map>

class HalStorage;

// This fork's writeDocToFileAtomic() streams into a HalFile. The bytes land
// in the fake card when the handle closes, like a real SD write.
class HalFile {
 public:
  HalFile() = default;
  explicit operator bool() const { return owner_ != nullptr; }
  size_t write(const uint8_t* data, size_t len) {
    buffer_.append(reinterpret_cast<const char*>(data), len);
    return len;
  }
  size_t write(const void* data, size_t len) { return write(static_cast<const uint8_t*>(data), len); }
  bool sync() { return owner_ != nullptr; }
  bool close();

 private:
  friend class HalStorage;
  HalStorage* owner_ = nullptr;
  std::string path_;
  std::string buffer_;
};

class HalStorage {
 public:
  void reset() {
    files.clear();
    failRenameFrom.clear();
  }

  bool mkdir(const char*, bool = true) { return true; }
  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) { return files.erase(path) > 0; }

  bool rename(const char* from, const char* to) {
    if (failRenameFrom == from) {
      failRenameFrom.clear();
      return false;
    }
    const auto source = files.find(from);
    if (source == files.end() || files.contains(to)) return false;
    files[to] = source->second;
    files.erase(source);
    return true;
  }

  bool writeFile(const char* path, const String& content) {
    files[path] = content.c_str();
    return true;
  }

  String readFile(const char* path) const {
    const auto file = files.find(path);
    return file == files.end() ? String() : String(file->second.c_str());
  }

  void put(const std::string& path, std::string content) { files[path] = std::move(content); }

  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file.owner_ = this;
    file.path_ = path;
    file.buffer_.clear();
    return true;
  }
  void failNextRenameFrom(std::string path) { failRenameFrom = std::move(path); }

 private:
  std::unordered_map<std::string, std::string> files;
  std::string failRenameFrom;
};

inline HalStorage Storage;

inline bool HalFile::close() {
  if (owner_ == nullptr) return true;
  owner_->put(path_, buffer_);
  owner_ = nullptr;
  return true;
}
