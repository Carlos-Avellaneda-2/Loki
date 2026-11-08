#pragma once

#include <spdlog/spdlog.h>

#include <cstddef>
#include <filesystem>
#include <string>

namespace loki::util {

struct LogConfig {
    // Directory for rotating log files. Empty disables file logging.
    std::filesystem::path directory;
    std::string fileName = "loki.log";
    spdlog::level::level_enum level = spdlog::level::info;
    bool console = true;
    std::size_t maxFileBytes = 5 * 1024 * 1024;
    std::size_t maxFiles = 3;
};

struct LogInitResult {
    bool fileLogging = false;          // false if the file sink could not be created
    std::filesystem::path logFilePath; // valid only when fileLogging is true
};

// Installs the "loki" logger as spdlog's default logger. Never throws: if the file sink
// cannot be created, logging continues on the console only and the result reports it.
LogInitResult initLogging(const LogConfig& config) noexcept;

// Flushes and drops all loggers. Safe to call multiple times.
void shutdownLogging() noexcept;

// RAII guard that shuts logging down when it goes out of scope.
class LoggingGuard {
public:
    explicit LoggingGuard(const LogConfig& config) noexcept : m_result(initLogging(config)) {}
    ~LoggingGuard() { shutdownLogging(); }
    LoggingGuard(const LoggingGuard&) = delete;
    LoggingGuard& operator=(const LoggingGuard&) = delete;
    LoggingGuard(LoggingGuard&&) = delete;
    LoggingGuard& operator=(LoggingGuard&&) = delete;

    [[nodiscard]] const LogInitResult& result() const noexcept { return m_result; }

private:
    LogInitResult m_result;
};

} // namespace loki::util
