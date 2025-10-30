#include "util/files.hpp"
#include <cstdlib>
#include <fstream>
#include <random>

// Platform-specific includes for fsync
#ifdef __APPLE__
#include <fcntl.h>
#include <unistd.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <io.h>
#include <windows.h>
#endif

namespace coinbasechain {
namespace util {

namespace {

// fsync wrapper that works cross-platform
bool sync_file(int fd) {
#if defined(__APPLE__) || defined(__linux__)
  return fsync(fd) == 0;
#elif defined(_WIN32)
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  return FlushFileBuffers(h) != 0;
#else
  // Fallback: no-op (not safe but at least compiles)
  return true;
#endif
}

// Sync directory to ensure rename is durable
bool sync_directory(const std::filesystem::path &dir) {
#if defined(__APPLE__) || defined(__linux__)
  int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    return false;
  bool result = fsync(fd) == 0;
  close(fd);
  return result;
#elif defined(_WIN32)
  // Windows doesn't need explicit directory sync
  return true;
#else
  return true;
#endif
}

// Generate random suffix for temp file
std::string random_suffix() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, 0xFFFF);
  char buf[8];
  snprintf(buf, sizeof(buf), "%04x", dis(gen));
  return std::string(buf);
}

} // anonymous namespace

bool atomic_write_file(const std::filesystem::path &path,
                       const std::vector<uint8_t> &data) {
  // Create parent directory if needed
  auto parent = path.parent_path();
  if (!parent.empty() && !ensure_directory(parent)) {
    return false;
  }

  // Generate temp file path
  auto temp_path = path;
  temp_path += ".tmp." + random_suffix();

  // Open temp file for writing
  std::ofstream temp_file(temp_path, std::ios::binary | std::ios::trunc);
  if (!temp_file) {
    return false;
  }

  // Write data
  temp_file.write(reinterpret_cast<const char *>(data.data()), data.size());
  if (!temp_file) {
    temp_file.close();
    std::filesystem::remove(temp_path);
    return false;
  }

  // Flush and sync to disk
  temp_file.flush();

  // Get file descriptor for fsync
#if defined(__APPLE__) || defined(__linux__)
  int fd = fileno(std::fopen(temp_path.c_str(), "r"));
  if (fd >= 0) {
    sync_file(fd);
    close(fd);
  }
#endif

  temp_file.close();

  // Sync directory to ensure rename will be durable
  if (!parent.empty()) {
    sync_directory(parent);
  }

  // Atomic rename
  std::error_code ec;
  std::filesystem::rename(temp_path, path, ec);
  if (ec) {
    std::filesystem::remove(temp_path);
    return false;
  }

  return true;
}

bool atomic_write_file(const std::filesystem::path &path,
                       const std::string &data) {
  std::vector<uint8_t> vec(data.begin(), data.end());
  return atomic_write_file(path, vec);
}

std::vector<uint8_t> read_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return {};
  }

  auto size = file.tellg();
  if (size < 0) {
    return {};
  }

  std::vector<uint8_t> data(size);
  file.seekg(0);
  file.read(reinterpret_cast<char *>(data.data()), size);

  if (!file) {
    return {};
  }

  return data;
}

std::string read_file_string(const std::filesystem::path &path) {
  auto data = read_file(path);
  return std::string(data.begin(), data.end());
}

bool ensure_directory(const std::filesystem::path &dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return !ec || std::filesystem::exists(dir);
}

std::filesystem::path get_default_datadir() {
  const char *home = nullptr;

#ifdef _WIN32
  home = std::getenv("APPDATA");
  if (home) {
    return std::filesystem::path(home) / "coinbasechain";
  }
#else
  home = std::getenv("HOME");
  if (home) {
    return std::filesystem::path(home) / ".coinbasechain";
  }
#endif

  // Fallback to current directory
  return std::filesystem::current_path() / ".coinbasechain";
}

} // namespace util
} // namespace coinbasechain
