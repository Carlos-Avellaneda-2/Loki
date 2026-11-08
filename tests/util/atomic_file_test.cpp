#include "util/atomic_file.h"

#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <system_error>

namespace {

namespace fs = std::filesystem;
using loki::util::readFile;
using loki::util::writeFileAtomic;

TEST(AtomicFileTest, WritesNewFile) {
    loki::test::TempDir dir;
    const auto path = dir.path() / "a.txt";
    writeFileAtomic(path, "hello");
    EXPECT_EQ(readFile(path), "hello");
    EXPECT_FALSE(fs::exists(path.string() + ".tmp"));
}

TEST(AtomicFileTest, ReplacesExistingFile) {
    loki::test::TempDir dir;
    const auto path = dir.path() / "a.txt";
    writeFileAtomic(path, "first version, longer");
    writeFileAtomic(path, "second");
    EXPECT_EQ(readFile(path), "second");
}

TEST(AtomicFileTest, PreservesBinaryContent) {
    loki::test::TempDir dir;
    const auto path = dir.path() / "bin.dat";
    std::string data;
    for (int i = 0; i < 256; ++i) {
        data.push_back(static_cast<char>(i));
    }
    data += "\r\n\n\r";
    writeFileAtomic(path, data);
    EXPECT_EQ(readFile(path), data);
}

TEST(AtomicFileTest, HandlesNonAsciiPaths) {
    loki::test::TempDir dir;
    const auto path = dir.path() / fs::path(u8"proyecto_ñandú_日本.loki");
    writeFileAtomic(path, "ok");
    EXPECT_EQ(readFile(path), "ok");
}

TEST(AtomicFileTest, FailureLeavesOriginalUntouched) {
    loki::test::TempDir dir;
    const auto missingDir = dir.path() / "does_not_exist" / "a.txt";
    EXPECT_THROW(writeFileAtomic(missingDir, "x"), std::system_error);

    // Target is a directory: the rename must fail and the directory stays.
    const auto target = dir.path() / "folder";
    fs::create_directories(target);
    EXPECT_THROW(writeFileAtomic(target, "x"), std::system_error);
    EXPECT_TRUE(fs::is_directory(target));
    EXPECT_FALSE(fs::exists(target.string() + ".tmp"));
}

TEST(AtomicFileTest, ReadMissingFileThrows) {
    loki::test::TempDir dir;
    EXPECT_THROW((void)readFile(dir.path() / "missing.txt"), std::system_error);
}

} // namespace
