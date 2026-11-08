#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace loki::test {

// Unique temporary directory removed on destruction.
class TempDir {
public:
    TempDir() {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string name = info != nullptr ? std::string(info->test_suite_name()) + "_" + info->name() : "loki";
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() / "loki_tests" / (name + "_" + std::to_string(stamp));
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

} // namespace loki::test
