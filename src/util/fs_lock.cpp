// Copyright (c) 2024 Coinbase Chain
// Distributed under the MIT software license

#include "util/fs_lock.hpp"
#include "util/logging.hpp"
#include <cerrno>
#include <cstring>
#include <fstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace coinbasechain {
namespace util {

// Global mutex to protect dir_locks map
static std::mutex g_dir_locks_mutex;

// Map of currently held directory locks
// Key: full path to lock file
// Value: FileLock object
static std::map<std::string, std::unique_ptr<FileLock>> g_dir_locks;

// ============================================================================
// FileLock implementation
// ============================================================================

#ifndef _WIN32
// Unix/macOS implementation using fcntl

static std::string GetErrorReason() { return std::strerror(errno); }

FileLock::FileLock(const fs::path &file) {
  fd_ = open(file.c_str(), O_RDWR);
  if (fd_ == -1) {
    reason_ = GetErrorReason();
  }
}

FileLock::~FileLock() {
  if (fd_ != -1) {
    close(fd_);
  }
}

bool FileLock::TryLock() {
  if (fd_ == -1) {
    return false;
  }

  struct flock lock;
  lock.l_type = F_WRLCK; // Exclusive write lock
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0; // Lock entire file

  if (fcntl(fd_, F_SETLK, &lock) == -1) {
    reason_ = GetErrorReason();
    return false;
  }

  return true;
}

#else
// Windows implementation using LockFileEx

static std::string GetErrorReason() {
  DWORD error = GetLastError();
  char *message = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 (LPSTR)&message, 0, nullptr);
  // False positive: FormatMessageA modifies 'message' via out-parameter
  // cppcheck-suppress knownConditionTrueFalse
  std::string result(message ? message : "Unknown error");
  // cppcheck-suppress knownConditionTrueFalse
  if (message) {
    LocalFree(message);
  }
  return result;
}

FileLock::FileLock(const fs::path &file) {
  hFile_ = CreateFileW(file.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  if (hFile_ == INVALID_HANDLE_VALUE) {
    reason_ = GetErrorReason();
  }
}

FileLock::~FileLock() {
  if (hFile_ != INVALID_HANDLE_VALUE) {
    CloseHandle(hFile_);
  }
}

bool FileLock::TryLock() {
  if (hFile_ == INVALID_HANDLE_VALUE) {
    return false;
  }

  OVERLAPPED overlapped = {};
  if (!LockFileEx(hFile_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                  0, MAXDWORD, MAXDWORD, &overlapped)) {
    reason_ = GetErrorReason();
    return false;
  }

  return true;
}

#endif

// ============================================================================
// Directory locking functions
// ============================================================================

LockResult LockDirectory(const fs::path &directory,
                         const std::string &lockfile_name, bool probe_only) {
  std::lock_guard<std::mutex> lock(g_dir_locks_mutex);

  fs::path lockfile_path = directory / lockfile_name;
  std::string lockfile_str = lockfile_path.string();

  // Check if we already have a lock on this directory
  if (g_dir_locks.find(lockfile_str) != g_dir_locks.end()) {
    return LockResult::Success;
  }

  // Create empty lock file if it doesn't exist
  {
    std::ofstream lockfile(lockfile_path, std::ios::app);
    if (!lockfile) {
      LOG_CHAIN_ERROR("Failed to create lock file: {}", lockfile_path.string());
      return LockResult::ErrorWrite;
    }
  }

  // Try to acquire lock
  auto file_lock = std::make_unique<FileLock>(lockfile_path);
  if (!file_lock->TryLock()) {
    LOG_CHAIN_ERROR("Failed to lock directory {}: {}", directory.string(),
              file_lock->GetReason());
    return LockResult::ErrorLock;
  }

  if (!probe_only) {
    // Lock successful and we're not just probing, store it
    g_dir_locks.emplace(lockfile_str, std::move(file_lock));
  }

  return LockResult::Success;
}

void UnlockDirectory(const fs::path &directory,
                     const std::string &lockfile_name) {
  std::lock_guard<std::mutex> lock(g_dir_locks_mutex);

  fs::path lockfile_path = directory / lockfile_name;
  std::string lockfile_str = lockfile_path.string();

  g_dir_locks.erase(lockfile_str);
}

void ReleaseAllDirectoryLocks() {
  std::lock_guard<std::mutex> lock(g_dir_locks_mutex);
  g_dir_locks.clear();
}

} // namespace util
} // namespace coinbasechain
