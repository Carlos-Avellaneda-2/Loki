#include "util/logging.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace fs = std::filesystem;

class LoggingTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_dir = fs::temp_directory_path() / "loki_tests" / (std::string(info->name()) + "_" + std::to_string(stamp));
    }

    void TearDown() override {
        loki::util::shutdownLogging();
        std::error_code ec;
        fs::remove_all(m_dir, ec);
    }

    static std::string readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    fs::path m_dir;
};

TEST_F(LoggingTest, CreatesLogFileInMissingDirectory) {
    loki::util::LogConfig config;
    config.directory = m_dir / "nested" / "logs";
    config.console = false;

    const auto result = loki::util::initLogging(config);

    EXPECT_TRUE(result.fileLogging);
    EXPECT_EQ(result.logFilePath, config.directory / "loki.log");
    EXPECT_TRUE(fs::exists(result.logFilePath));
}

TEST_F(LoggingTest, WritesMessagesToFile) {
    loki::util::LogConfig config;
    config.directory = m_dir;
    config.console = false;
    const auto result = loki::util::initLogging(config);
    ASSERT_TRUE(result.fileLogging);

    spdlog::info("hello from test {}", 42);
    spdlog::default_logger()->flush();

    EXPECT_NE(readFile(result.logFilePath).find("hello from test 42"), std::string::npos);
}

TEST_F(LoggingTest, RespectsLogLevel) {
    loki::util::LogConfig config;
    config.directory = m_dir;
    config.console = false;
    config.level = spdlog::level::warn;
    const auto result = loki::util::initLogging(config);
    ASSERT_TRUE(result.fileLogging);

    spdlog::info("filtered-out");
    spdlog::warn("kept-in");
    spdlog::default_logger()->flush();

    const auto content = readFile(result.logFilePath);
    EXPECT_EQ(content.find("filtered-out"), std::string::npos);
    EXPECT_NE(content.find("kept-in"), std::string::npos);
}

TEST_F(LoggingTest, FallsBackWhenDirectoryIsAFile) {
    fs::create_directories(m_dir);
    const auto blocker = m_dir / "not_a_dir";
    std::ofstream(blocker) << "x";

    loki::util::LogConfig config;
    config.directory = blocker;
    config.console = false;

    const auto result = loki::util::initLogging(config);

    EXPECT_FALSE(result.fileLogging);
    ASSERT_NE(spdlog::default_logger(), nullptr);
    EXPECT_NO_THROW(spdlog::info("still logging"));
}

TEST_F(LoggingTest, EmptyDirectoryDisablesFileLogging) {
    loki::util::LogConfig config;
    config.console = false;

    const auto result = loki::util::initLogging(config);

    EXPECT_FALSE(result.fileLogging);
    EXPECT_TRUE(result.logFilePath.empty());
}

TEST_F(LoggingTest, ShutdownIsIdempotent) {
    loki::util::LogConfig config;
    config.directory = m_dir;
    config.console = false;
    loki::util::initLogging(config);

    EXPECT_NO_THROW(loki::util::shutdownLogging());
    EXPECT_NO_THROW(loki::util::shutdownLogging());
}

TEST_F(LoggingTest, GuardShutsDownOnScopeExit) {
    loki::util::LogConfig config;
    config.directory = m_dir;
    config.console = false;
    {
        const loki::util::LoggingGuard guard(config);
        EXPECT_TRUE(guard.result().fileLogging);
        EXPECT_NE(spdlog::get("loki"), nullptr);
    }
    EXPECT_EQ(spdlog::get("loki"), nullptr);
}

} // namespace
