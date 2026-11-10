#pragma once

#include "playback/sources.h"

#include <filesystem>
#include <string>
#include <vector>

namespace loki::playback {

// Render previews (D42). The sequence is cut into fixed segments; a segment's key is a hash of
// everything that draws it (sequence format, the video tracks' clips around the segment, their
// media and nested sequences, transitions). A rendered segment is the file `<cache>/<key>.mov`: any
// edit that changes the picture changes the key, so stale renders are simply not found.
inline constexpr std::int64_t kPreviewSegmentSeconds = 2;

[[nodiscard]] Rational previewSegmentLength();
// Number of segments covering the sequence.
[[nodiscard]] std::size_t previewSegmentCount(const SequenceSnapshot& snapshot);
// Key of segment `index` (hex), or "" when nothing is drawn there (no video clip).
[[nodiscard]] std::string previewSegmentKey(const SequenceSnapshot& snapshot, std::size_t index);
[[nodiscard]] std::filesystem::path previewFile(const std::filesystem::path& cacheDir, const std::string& key);
// Fills snapshot.previews with the segments already rendered in `cacheDir`.
void attachPreviews(SequenceSnapshot& snapshot, const std::filesystem::path& cacheDir);

// Segment state for the timeline bar.
enum class PreviewState { Empty, Rendered, NotRendered };
[[nodiscard]] std::vector<PreviewState> previewStates(const SequenceSnapshot& snapshot,
                                                      const std::filesystem::path& cacheDir);

} // namespace loki::playback
