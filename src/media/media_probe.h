#pragma once

#include "model/types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace loki::media {

// Reads container/stream metadata. Blocking (opens the file): never call on the UI thread.
// Throws MediaError for unreadable files or files without audio/video streams.
[[nodiscard]] model::MediaInfo probeMedia(const std::filesystem::path& path);

// Extensions offered by the import dialog (lowercase, with dot). Probing decides the rest.
[[nodiscard]] const std::vector<std::string>& importableExtensions();

} // namespace loki::media
