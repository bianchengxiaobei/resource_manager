#pragma once

#include "parcel/core/error.hpp"
#include "parcel/core/operation.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace parcel::runtime {

struct DownloadSummary {
    std::size_t total_count = 0;
    std::size_t completed_count = 0;
    std::size_t failed_count = 0;
    std::size_t active_count = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t downloaded_bytes = 0;
    std::chrono::milliseconds elapsed{0};
    std::uint64_t bytes_per_second = 0;
};

struct DownloadFailure {
    std::string bundle_name;
    std::string file_name;
    core::Error error;
};

class ResourceDownloader {
  public:
    virtual ~ResourceDownloader() = default;

    virtual void Start() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    virtual void Cancel() = 0;
    virtual void RetryFailed() = 0;
    [[nodiscard]] virtual bool IsDone() const = 0;
    [[nodiscard]] virtual core::OperationStatus Status() const = 0;
    [[nodiscard]] virtual float Progress() const = 0;
    [[nodiscard]] virtual const core::Error& LastError() const = 0;
    [[nodiscard]] virtual std::uint64_t DownloadedBytes() const = 0;
    [[nodiscard]] virtual std::uint64_t TotalBytes() const = 0;
    [[nodiscard]] virtual DownloadSummary Summary() const { return {}; }
    [[nodiscard]] virtual std::vector<DownloadFailure> Failures() const { return {}; }
};

class ResourceUnpacker {
  public:
    virtual ~ResourceUnpacker() = default;

    virtual void Start() = 0;
    virtual void Cancel() = 0;
    [[nodiscard]] virtual bool IsDone() const = 0;
    [[nodiscard]] virtual core::OperationStatus Status() const = 0;
    [[nodiscard]] virtual float Progress() const = 0;
    [[nodiscard]] virtual const core::Error& LastError() const = 0;
};

class ResourceImporter {
  public:
    virtual ~ResourceImporter() = default;

    virtual void Start() = 0;
    virtual void Cancel() = 0;
    [[nodiscard]] virtual bool IsDone() const = 0;
    [[nodiscard]] virtual core::OperationStatus Status() const = 0;
    [[nodiscard]] virtual float Progress() const = 0;
    [[nodiscard]] virtual const core::Error& LastError() const = 0;
};

class CustomAsyncOperation : public core::OperationBase {
  public:
    void Complete() { Succeed(); }
    void FailWith(core::Error error) { Fail(std::move(error)); }
};

} // namespace parcel::runtime

