#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace loki::util {

// Writes `content` to `path` atomically: data goes to a temporary file in the same directory,
// is flushed to disk, then replaces `path` in one rename. Readers see either the old or the new
// file, never a partial one, even if the process crashes mid-write.
// Throws std::system_error on failure (the original file is left untouched).
void writeFileAtomic(const std::filesystem::path& path, std::string_view content);

// Reads a whole file as bytes. Throws std::system_error on failure.
[[nodiscard]] std::string readFile(const std::filesystem::path& path);

} // namespace loki::util
