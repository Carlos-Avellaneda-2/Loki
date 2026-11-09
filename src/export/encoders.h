#pragma once

#include "export/media_writer.h"

#include <string>
#include <vector>

namespace loki::exporter {

// Encoder names tried for a codec, best first (D23: hardware first, then LGPL software; never
// libx264/libx265).
[[nodiscard]] const std::vector<std::string>& encoderCandidates(VideoCodec codec);

// First candidate that opens on this machine for a small test picture (hardware encoders fail to
// open without a device/driver). Result is cached per codec. Empty if none works.
[[nodiscard]] std::string pickEncoder(VideoCodec codec);

// Whether `name` opens on this machine (not cached).
[[nodiscard]] bool encoderWorks(const std::string& name);

} // namespace loki::exporter
