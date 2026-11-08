#include "util/logging.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <exception>
#include <memory>
#include <vector>

namespace loki::util {

namespace {
constexpr const char* kLoggerName = "loki";
constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [t%t] %v";
} // namespace

LogInitResult initLogging(const LogConfig& config) noexcept {
    LogInitResult result;
    try {
        std::vector<spdlog::sink_ptr> sinks;
        if (config.console) {
            sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        }

        std::string fileError;
        if (!config.directory.empty()) {
            try {
                std::filesystem::create_directories(config.directory);
                const auto path = config.directory / config.fileName;
                sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    path.string(), config.maxFileBytes, config.maxFiles));
                result.fileLogging = true;
                result.logFilePath = path;
            } catch (const std::exception& e) {
                fileError = e.what();
            }
        }

        spdlog::drop(kLoggerName);
        auto logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
        logger->set_level(config.level);
        logger->set_pattern(kPattern);
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);

        if (!fileError.empty()) {
            spdlog::warn("File logging disabled: {}", fileError);
        }
    } catch (...) {
        // Logging must never take the application down.
        result = {};
    }
    return result;
}

void shutdownLogging() noexcept {
    try {
        if (auto logger = spdlog::get(kLoggerName)) {
            logger->flush();
        }
        spdlog::shutdown();
    } catch (...) {
    }
}

} // namespace loki::util
