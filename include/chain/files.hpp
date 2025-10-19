#ifndef COINBASECHAIN_UTIL_FILES_HPP
#define COINBASECHAIN_UTIL_FILES_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace coinbasechain {
namespace util {

/**
 * Atomic file operations for crash-safe persistence
 *
 * Pattern:
 * 1. Write to temporary file (.tmp suffix)
 * 2. fsync() the file to ensure data is on disk
 * 3. fsync() the directory to ensure rename will be durable
 * 4. Atomic rename over original file
 *
 * This ensures that either the old file or new file is always valid,
 * never a half-written corrupted file.
 */

/**
 * Write data to file atomically
 * Returns true on success, false on failure
 */
bool atomic_write_file(const std::filesystem::path &path,
                       const std::vector<uint8_t> &data);

/**
 * Write string to file atomically
 */
bool atomic_write_file(const std::filesystem::path &path,
                       const std::string &data);

/**
 * Read entire file into vector
 * Returns empty vector on failure
 */
std::vector<uint8_t> read_file(const std::filesystem::path &path);

/**
 * Read entire file into string
 * Returns empty string on failure
 */
std::string read_file_string(const std::filesystem::path &path);

/**
 * Create directory if it doesn't exist (recursive)
 * Returns true on success or if already exists
 */
bool ensure_directory(const std::filesystem::path &dir);

/**
 * Get default data directory for the application
 * Returns ~/.coinbasechain on Unix
 */
std::filesystem::path get_default_datadir();

} // namespace util
} // namespace coinbasechain

#endif // COINBASECHAIN_UTIL_FILES_HPP
