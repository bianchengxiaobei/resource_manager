#include "parcel/core/platform_file.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>

namespace parcel::core {
namespace {

std::string Win32Message(DWORD code) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer),
        0, nullptr);

    if (length == 0 || buffer == nullptr) {
        return "win32 error " + std::to_string(code);
    }

    std::wstring wide(buffer, length);
    LocalFree(buffer);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n')) {
        wide.pop_back();
    }
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return "win32 error " + std::to_string(code);
    }
    std::string message(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), message.data(),
                            needed, nullptr, nullptr) <= 0) {
        return "win32 error " + std::to_string(code);
    }
    return message;
}

Error Win32Error(DWORD code, std::string_view operation, std::string_view path) {
    const ErrorCode error_code = (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                                     ? ErrorCode::NotFound
                                     : ErrorCode::Io;
    std::string message(operation);
    message += " failed: ";
    message += Win32Message(code);
    return Error(error_code, std::move(message)).WithPath(std::string(path));
}

Result<std::wstring> Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return std::wstring{};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return MakeError(ErrorCode::InvalidArgument, "path is too long");
    }

    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) {
        return MakeError(ErrorCode::InvalidArgument, "path is not valid UTF-8");
    }

    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), needed) <= 0) {
        return MakeError(ErrorCode::InvalidArgument, "path is not valid UTF-8");
    }
    return result;
}

Result<std::wstring> AbsolutePathW(std::string_view path) {
    auto wide_result = Utf8ToWide(path);
    if (!wide_result) {
        return std::move(wide_result).TakeError();
    }
    const std::wstring& wide = wide_result.Value();

    const DWORD needed = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return Win32Error(GetLastError(), "GetFullPathNameW", path);
    }

    std::wstring buffer(static_cast<std::size_t>(needed) + 1, L'\0');
    const DWORD written =
        GetFullPathNameW(wide.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (written == 0) {
        return Win32Error(GetLastError(), "GetFullPathNameW", path);
    }
    if (written >= buffer.size()) {
        return MakeError(ErrorCode::Io, "GetFullPathNameW returned an invalid length")
            .WithPath(std::string(path));
    }
    buffer.resize(written);
    return buffer;
}

std::wstring ToExtendedPath(const std::wstring& absolute) {
    if (absolute.rfind(L"\\\\?\\", 0) == 0) {
        return absolute;
    }
    if (absolute.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + absolute.substr(2);
    }
    return L"\\\\?\\" + absolute;
}

struct WidePath {
    std::wstring absolute;
    std::wstring extended;
};

Result<WidePath> MakeWidePath(std::string_view path) {
    auto absolute_result = AbsolutePathW(path);
    if (!absolute_result) {
        return std::move(absolute_result).TakeError();
    }

    WidePath result;
    result.absolute = std::move(absolute_result).Value();
    result.extended = ToExtendedPath(result.absolute);
    return result;
}

class Win32File final : public IFile {
  public:
    Win32File(HANDLE handle, std::string path) : handle_(handle), path_(std::move(path)) {}

    ~Win32File() override { Close(); }

    [[nodiscard]] Result<std::size_t> Read(std::span<std::byte> buffer) override {
        std::lock_guard lock(mutex_);
        if (!open_.load(std::memory_order_acquire)) {
            return MakeError(ErrorCode::Io, "file is closed").WithPath(path_);
        }
        if (buffer.empty()) {
            return 0;
        }

        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(buffer.size(), std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (ReadFile(handle_, buffer.data(), requested, &read, nullptr) == FALSE) {
            return Win32Error(GetLastError(), "ReadFile", path_);
        }
        return static_cast<std::size_t>(read);
    }

    [[nodiscard]] Result<std::size_t> ReadAt(std::uint64_t offset,
                                             std::span<std::byte> buffer) override {
        std::lock_guard lock(mutex_);
        if (!open_.load(std::memory_order_acquire)) {
            return MakeError(ErrorCode::Io, "file is closed").WithPath(path_);
        }
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) {
            return MakeError(ErrorCode::InvalidArgument, "file offset is out of range")
                .WithPath(path_);
        }
        if (buffer.empty()) {
            return 0;
        }

        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) == FALSE) {
            return Win32Error(GetLastError(), "SetFilePointerEx", path_);
        }

        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(buffer.size(), std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (ReadFile(handle_, buffer.data(), requested, &read, nullptr) == FALSE) {
            return Win32Error(GetLastError(), "ReadFile", path_);
        }
        return static_cast<std::size_t>(read);
    }

    [[nodiscard]] Result<std::size_t> Write(std::span<const std::byte> buffer) override {
        std::lock_guard lock(mutex_);
        if (!open_.load(std::memory_order_acquire)) {
            return MakeError(ErrorCode::Io, "file is closed").WithPath(path_);
        }
        if (buffer.empty()) {
            return 0;
        }

        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(buffer.size(), std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (WriteFile(handle_, buffer.data(), requested, &written, nullptr) == FALSE) {
            return Win32Error(GetLastError(), "WriteFile", path_);
        }
        return static_cast<std::size_t>(written);
    }

    [[nodiscard]] Result<void> Flush() override {
        std::lock_guard lock(mutex_);
        if (!open_.load(std::memory_order_acquire)) {
            return MakeError(ErrorCode::Io, "file is closed").WithPath(path_);
        }
        if (FlushFileBuffers(handle_) == FALSE) {
            return Win32Error(GetLastError(), "FlushFileBuffers", path_);
        }
        return {};
    }

    [[nodiscard]] Result<std::uint64_t> Size() const override {
        std::lock_guard lock(mutex_);
        if (!open_.load(std::memory_order_acquire)) {
            return MakeError(ErrorCode::Io, "file is closed").WithPath(path_);
        }
        LARGE_INTEGER size{};
        if (GetFileSizeEx(handle_, &size) == FALSE) {
            return Win32Error(GetLastError(), "GetFileSizeEx", path_);
        }
        return static_cast<std::uint64_t>(size.QuadPart);
    }

    void Close() noexcept override {
        std::lock_guard lock(mutex_);
        if (open_.exchange(false, std::memory_order_acq_rel)) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    [[nodiscard]] bool IsOpen() const noexcept override {
        return open_.load(std::memory_order_acquire);
    }

  private:
    mutable std::mutex mutex_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::string path_;
    std::atomic_bool open_{true};
};

} // namespace

Result<std::unique_ptr<IFile>> PlatformFile::OpenRead(std::string_view path) {
    auto wide_result = MakeWidePath(path);
    if (!wide_result) {
        return std::move(wide_result).TakeError();
    }

    HANDLE handle =
        CreateFileW(wide_result.Value().extended.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Win32Error(GetLastError(), "CreateFileW(open read)", path);
    }

    std::unique_ptr<IFile> result = std::make_unique<Win32File>(handle, std::string(path));
    return result;
}

Result<std::unique_ptr<IFile>> PlatformFile::OpenWrite(std::string_view path, bool append) {
    auto wide_result = MakeWidePath(path);
    if (!wide_result) {
        return std::move(wide_result).TakeError();
    }

    HANDLE handle =
        CreateFileW(wide_result.Value().extended.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Win32Error(GetLastError(), "CreateFileW(open write)", path);
    }

    if (append) {
        LARGE_INTEGER end{};
        if (SetFilePointerEx(handle, end, nullptr, FILE_END) == FALSE) {
            const Error error = Win32Error(GetLastError(), "SetFilePointerEx", path);
            CloseHandle(handle);
            return error;
        }
    }

    std::unique_ptr<IFile> result = std::make_unique<Win32File>(handle, std::string(path));
    return result;
}

Result<std::uint64_t> PlatformFile::Size(std::string_view path) {
    auto stat_result = Stat(path);
    if (!stat_result) {
        return std::move(stat_result).TakeError();
    }
    if (stat_result.Value().is_directory) {
        return MakeError(ErrorCode::InvalidArgument, "path is a directory")
            .WithPath(std::string(path));
    }
    return stat_result.Value().size;
}

Result<bool> PlatformFile::Exists(std::string_view path) {
    auto wide_result = MakeWidePath(path);
    if (!wide_result) {
        return std::move(wide_result).TakeError();
    }

    const DWORD attributes = GetFileAttributesW(wide_result.Value().extended.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        return Win32Error(code, "GetFileAttributesW", path);
    }
    return true;
}

Result<void> PlatformFile::Delete(std::string_view path) {
    auto wide_result = MakeWidePath(path);
    if (!wide_result) {
        return std::move(wide_result).TakeError();
    }

    if (DeleteFileW(wide_result.Value().extended.c_str()) == FALSE) {
        return Win32Error(GetLastError(), "DeleteFileW", path);
    }
    return {};
}

Result<void> PlatformFile::Rename(std::string_view from, std::string_view to, bool overwrite) {
    auto from_result = MakeWidePath(from);
    if (!from_result) {
        return std::move(from_result).TakeError();
    }
    auto to_result = MakeWidePath(to);
    if (!to_result) {
        return std::move(to_result).TakeError();
    }

    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (overwrite) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (MoveFileExW(from_result.Value().extended.c_str(), to_result.Value().extended.c_str(),
                    flags) == FALSE) {
        return Win32Error(GetLastError(), "MoveFileExW", from);
    }
    return {};
}

Result<void> PlatformFile::CreateDirectories(std::string_view path) {
    if (path.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "directory path is empty");
    }

    auto wide_result = MakeWidePath(path);
    if (!wide_result) {
        return std::move(wide_result).TakeError();
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(wide_result.Value().extended), ec);
    if (ec) {
        return Error(ErrorCode::Io, "create_directories failed: " + ec.message())
            .WithPath(std::string(path));
    }

    if (!std::filesystem::is_directory(std::filesystem::path(wide_result.Value().extended), ec) ||
        ec) {
        return MakeError(ErrorCode::Io, "path exists but is not a directory")
            .WithPath(std::string(path));
    }
    return {};
}

Result<FileStat> PlatformFile::Stat(std::string_view path) {
    auto wide_result = MakeWidePath(path);
    if (!wide_result) {
        return std::move(wide_result).TakeError();
    }

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(wide_result.Value().extended.c_str(), GetFileExInfoStandard, &data) ==
        FALSE) {
        return Win32Error(GetLastError(), "GetFileAttributesExW", path);
    }

    FileStat result;
    result.exists = true;
    result.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    result.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) |
                  static_cast<std::uint64_t>(data.nFileSizeLow);

    ULARGE_INTEGER ticks{};
    ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
    constexpr std::uint64_t kWindowsEpochTicks = 116444736000000000ULL;
    if (ticks.QuadPart >= kWindowsEpochTicks) {
        const auto since_epoch = static_cast<std::int64_t>(ticks.QuadPart - kWindowsEpochTicks);
        const auto duration = std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>(since_epoch));
        result.modified = std::chrono::system_clock::time_point(duration);
    }
    return result;
}

Result<std::uint64_t> PlatformFile::FreeSpace(std::string_view path) {
    auto absolute_result = AbsolutePathW(path);
    if (!absolute_result) {
        return std::move(absolute_result).TakeError();
    }

    const std::filesystem::path absolute(absolute_result.Value());
    std::wstring root = absolute.root_path().wstring();
    if (root.empty()) {
        root = L".\\";
    }

    ULARGE_INTEGER free_for_caller{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER total_free{};
    if (GetDiskFreeSpaceExW(root.c_str(), &free_for_caller, &total, &total_free) == FALSE) {
        return Win32Error(GetLastError(), "GetDiskFreeSpaceExW", path);
    }
    return static_cast<std::uint64_t>(free_for_caller.QuadPart);
}

Result<void> WriteAll(IFile& file, std::span<const std::byte> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        auto result = file.Write(data.subspan(offset));
        if (!result) {
            return std::move(result).TakeError();
        }
        const std::size_t written = result.Value();
        if (written == 0) {
            return MakeError(ErrorCode::Io, "file write returned zero bytes");
        }
        offset += written;
    }
    return {};
}

Result<std::vector<std::byte>> ReadAll(IFile& file) {
    auto size_result = file.Size();
    if (!size_result) {
        return std::move(size_result).TakeError();
    }
    const std::uint64_t size = size_result.Value();
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return MakeError(ErrorCode::Io, "file is too large to read into memory");
    }

    std::vector<std::byte> result(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    while (offset < result.size()) {
        auto read_result = file.Read(std::span(result).subspan(offset));
        if (!read_result) {
            return std::move(read_result).TakeError();
        }
        const std::size_t read = read_result.Value();
        if (read == 0) {
            return MakeError(ErrorCode::Io, "unexpected end of file");
        }
        offset += read;
    }
    return result;
}

} // namespace parcel::core
