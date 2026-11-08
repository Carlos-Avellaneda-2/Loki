#include "util/atomic_file.h"

#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace loki::util {

namespace {

std::filesystem::path tempPathFor(const std::filesystem::path& path) {
    auto tmp = path;
    tmp += L".tmp";
    return tmp;
}

#ifdef _WIN32

[[noreturn]] void throwLastError(const char* what) {
    throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), what);
}

// Owns a Win32 HANDLE.
class FileHandle {
public:
    explicit FileHandle(HANDLE h) noexcept : m_handle(h) {}
    ~FileHandle() { close(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&&) = delete;
    FileHandle& operator=(FileHandle&&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept { return m_handle != INVALID_HANDLE_VALUE; }
    bool close() noexcept {
        if (!valid()) {
            return true;
        }
        const BOOL ok = ::CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return ok != FALSE;
    }

private:
    HANDLE m_handle;
};

void writeAndSync(const std::filesystem::path& tmp, std::string_view content) {
    FileHandle file(::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        throwLastError("CreateFileW failed");
    }
    const char* data = content.data();
    std::size_t remaining = content.size();
    while (remaining > 0) {
        const DWORD chunk = remaining > 0x40000000 ? 0x40000000 : static_cast<DWORD>(remaining);
        DWORD written = 0;
        if (::WriteFile(file.get(), data, chunk, &written, nullptr) == FALSE) {
            throwLastError("WriteFile failed");
        }
        data += written;
        remaining -= written;
    }
    if (::FlushFileBuffers(file.get()) == FALSE) {
        throwLastError("FlushFileBuffers failed");
    }
    if (!file.close()) {
        throwLastError("CloseHandle failed");
    }
}

void replaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        throwLastError("MoveFileExW failed");
    }
}

#else

[[noreturn]] void throwErrno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

void writeAndSync(const std::filesystem::path& tmp, std::string_view content) {
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        throwErrno("open failed");
    }
    const char* data = content.data();
    std::size_t remaining = content.size();
    while (remaining > 0) {
        const ssize_t n = ::write(fd, data, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "write failed");
        }
        data += n;
        remaining -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const int err = errno;
        ::close(fd);
        throw std::system_error(err, std::generic_category(), "fsync failed");
    }
    if (::close(fd) != 0) {
        throwErrno("close failed");
    }
}

void replaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (std::rename(from.c_str(), to.c_str()) != 0) {
        throwErrno("rename failed");
    }
}

#endif

} // namespace

void writeFileAtomic(const std::filesystem::path& path, std::string_view content) {
    const auto tmp = tempPathFor(path);
    try {
        writeAndSync(tmp, content);
        replaceFile(tmp, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        throw;
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                                "Cannot open " + path.string());
    }
    std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        throw std::system_error(std::make_error_code(std::errc::io_error), "Read failed: " + path.string());
    }
    return content;
}

} // namespace loki::util
