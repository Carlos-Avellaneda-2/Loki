#pragma once

#include "model/project.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace loki::model {

// On-disk format of `.loki` project files (JSON). Bump when the schema changes and add a
// migration step in project_json.cpp (vN -> vN+1).
inline constexpr int kProjectFormatVersion = 3; // 2: transitions, generated media, text params (D20)
                                                // 3: clip speed, markers, nested sequences (D28-D31)
inline constexpr std::string_view kProjectFormatName = "loki-project";
inline constexpr std::string_view kProjectFileExtension = ".loki";

// Malformed, unsupported or invalid project data. The message says what and where.
class ProjectFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// `projectFile` (may be empty) is where the project lives; it is used to store media paths
// relative to the project as well, and to relink media when the project folder moved.
[[nodiscard]] nlohmann::json toJson(const Project& project, const std::filesystem::path& projectFile = {});
[[nodiscard]] Project fromJson(const nlohmann::json& json, const std::filesystem::path& projectFile = {});

[[nodiscard]] std::string serializeProject(const Project& project, const std::filesystem::path& projectFile = {});
[[nodiscard]] Project parseProject(std::string_view text, const std::filesystem::path& projectFile = {});

// File helpers. Saving is atomic (temp file + rename). Both throw ProjectFormatError for format
// problems and std::system_error for I/O failures.
void saveProjectFile(const Project& project, const std::filesystem::path& file);
[[nodiscard]] Project loadProjectFile(const std::filesystem::path& file);

// Upgrades `json` in place to kProjectFormatVersion. Returns the version it had.
// Throws ProjectFormatError for missing/unknown/newer versions.
int migrateProjectJson(nlohmann::json& json);

// UTF-8 <-> path helpers (paths are stored as UTF-8 with '/' separators).
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view utf8);

} // namespace loki::model
