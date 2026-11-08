#pragma once

#include "media/video_frame.h"

#include <filesystem>

namespace loki::media {

// Decodes one RGBA frame fitting in maxWidth x maxHeight at media time `at` (clamped to the
// stream). Software decoding (cheap, avoids exhausting hardware decoder sessions). Throws MediaError.
[[nodiscard]] VideoFramePtr makeThumbnail(const std::filesystem::path& path, const Rational& at, int maxWidth,
                                          int maxHeight);

} // namespace loki::media
