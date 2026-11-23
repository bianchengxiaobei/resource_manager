#pragma once

#include "parcel/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace parcel::core {

struct FileStat {
    bool exists = false;
    bool is_directory = false;
    std::uint64_t size = 0;
    std::chrono::system_clock::time_point modified{};
};

class IFile {
  public:
    virtual ~IFile() = default;

    [[nodiscard]] virtual Result<std::size_t> Read(std::span<std::byte> buffer) = 0;
    [[nodiscard]] virtual Result<std::size_t> ReadAt(std::uint64_t offset,
                                                     std::span<std::byte> buffer) = 0;
    [[nodiscard]] virtual Result<std::size_t> Write(std::span<const std::byte> buffer) = 0;
    [[nodiscard]] virtual Result<void> Flush() = 0;
    [[nodiscard]] virtual Result<std::uint64_t> Size() const = 0;
    virtual void Close() noexcept = 0;
    [[nodiscard]] virtual bool IsOpen() const noexcept = 0;
};

class IPlatformFile {
  public:
    virtual ~IPlatformFile() = default;

    [[nodiscard]] virtual Result<std::unique_ptr<IFile>> OpenRead(std::string_view path) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<IFile>> OpenWrite(std::string_view path,
                                                                   bool append = false) = 0;
    [[nodiscard]] virtual Result<std::uint64_t> Size(std::string_view path) = 0;
    [[nodiscard]] virtual Result<bool> Exists(std::string_view path) = 0;
    [[nodiscard]] virtual Result<void> Delete(std::string_view path) = 0;
    [[nodiscard]] virtual Result<void> Rename(std::string_view from, std::string_view to,
                                              bool overwrite = false) = 0;
    [[nodiscard]] virtual Result<void> CreateDirectories(std::string_view path) = 0;
    [[nodiscard]] virtual Result<FileStat> Stat(std::string_view path) = 0;
    [[nodiscard]] virtual Result<std::uint64_t> FreeSpace(std::string_view path) = 0;
};

class PlatformFile final : public IPlatformFile {
  public:
    [[nodiscard]] Result<std::unique_ptr<IFile>> OpenRead(std::string_view path) override;
    [[nodiscard]] Result<std::unique_ptr<IFile>> OpenWrite(std::string_view path,
                                                           bool append = false) override;
    [[nodiscard]] Result<std::uint64_t> Size(std::string_view path) override;
    [[nodiscard]] Result<bool> Exists(std::string_view path) override;
    [[nodiscard]] Result<void> Delete(std::string_view path) override;
    [[nodiscard]] Result<void> Rename(std::string_view from, std::string_view to,
                                      bool overwrite = false) override;
    [[nodiscard]] Result<void> CreateDirectories(std::string_view path) override;
    [[nodiscard]] Result<FileStat> Stat(std::string_view path) override;
    [[nodiscard]] Result<std::uint64_t> FreeSpace(std::string_view path) override;
};

[[nodiscard]] Result<void> WriteAll(IFile& file, std::span<const std::byte> data);
[[nodiscard]] Result<std::vector<std::byte>> ReadAll(IFile& file);

} // namespace parcel::core
