#pragma once

#include <Arduino.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

// Les repertoires n'existent pas dans ce stub : exists() ne rend vrai que pour
// des fichiers, donc les parcours de dossier de TaskStore ne s'executent pas.
// Les methodes ne sont la que pour que TaskStore.cpp compile tel quel.
constexpr int O_WRONLY = 1, O_CREAT = 2, O_APPEND = 4;

class HalFile {
 public:
  explicit HalFile(String content = {}) : content(std::move(content)) {}
  explicit HalFile(std::string* output) : output(output) {}
  explicit operator bool() const { return true; }
  int read(void* out, size_t length) {
    const size_t count = std::min(length, content.size() - position);
    memcpy(out, content.data() + position, count);
    position += count;
    return static_cast<int>(count);
  }
  int read() { return position < content.size() ? static_cast<unsigned char>(content[position++]) : -1; }
  bool available() const { return position < content.size(); }
  size_t write(const uint8_t* bytes, size_t length) {
    if (output) output->append(reinterpret_cast<const char*>(bytes), length);
    return length;
  }
  size_t write(uint8_t byte) { return write(&byte, 1); }
  size_t write(const void* bytes, size_t length) { return write(static_cast<const uint8_t*>(bytes), length); }
  bool sync() { return true; }
  bool isDirectory() const { return false; }
  HalFile openNextFile() { return HalFile(); }
  void getName(char* out, size_t size) const { out[size ? 0 : 0] = '\0'; }
  bool iterationFailed() const { return false; }
  bool close() {
    content.clear();
    position = 0;
    output = nullptr;
    return true;
  }

 private:
  std::string* output = nullptr;
  String content;
  size_t position = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  void reset() {
    files.clear();
    failRenameFrom.clear();
    writeAttempts = 0;
    failWrite = false;
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
    ++writeAttempts;
    if (failWrite) {
      failWrite = false;
      files[path] = "";
      return false;
    }
    files[path] = content.c_str();
    return true;
  }

  String readFile(const char* path) const {
    const auto file = files.find(path);
    return file == files.end() ? String() : String(file->second.c_str());
  }

  void put(const std::string& path, std::string content) { files[path] = std::move(content); }

  void failNextRenameFrom(std::string path) { failRenameFrom = std::move(path); }
  void failNextWrite() { failWrite = true; }
  bool openFileForRead(const char*, const char* path, HalFile& file) const {
    if (!exists(path)) return false;
    file = HalFile(readFile(path));
    return true;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    if (!writeFile(path, "")) return false;
    file = HalFile(&files[path]);
    return true;
  }
  HalFile open(const char* path, int = 0) {
    if (exists(path)) return HalFile(readFile(path));
    return HalFile();
  }
  size_t writeAttempts = 0;

 private:
  std::unordered_map<std::string, std::string> files;
  std::string failRenameFrom;
  bool failWrite = false;
};

inline HalStorage Storage;
