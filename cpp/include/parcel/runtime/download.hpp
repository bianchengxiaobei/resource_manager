#pragma once

#include "parcel/core/error.hpp"
#include "parcel/core/guid.hpp"
#include "parcel/core/result.hpp"
#include "parcel/serialize/manifest.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace parcel::runtime {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string url;
    std::vector<HttpHeader> headers;
    std::optional<std::uint64_t> range_offset;
    std::chrono::milliseconds timeout{0};
    std::chrono::milliseconds watchdog_timeout{0};
    std::function<void(std::uint64_t)> on_bytes_received;
    std::function<bool()> should_abort;
};

struct HttpResponse {
    int status = 0;
    std::vector<HttpHeader> headers;
    std::vector<std::byte> body;
    core::Error error{core::ErrorCode::Ok};

    [[nodiscard]] bool IsSuccess() const noexcept {
        return error.IsOk() && status >= 200 && status < 300;
    }

    [[nodiscard]] std::optional<std::string_view> Header(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> ContentLength() const noexcept;
};

class IHttpTransport {
  public:
    virtual ~IHttpTransport() = default;

    [[nodiscard]] virtual core::Result<HttpResponse> Send(const HttpRequest& request) = 0;
};

struct DownloadUrlRequest {
    std::string package;
    std::string version;
    std::string bundle_name;
    std::string file_name;
    std::uint64_t file_size = 0;
    int attempt = 0;
};

class IDownloadUrlProvider {
  public:
    virtual ~IDownloadUrlProvider() = default;

    [[nodiscard]] virtual core::Result<std::vector<std::string>>
    GetUrls(const DownloadUrlRequest& request) = 0;

    [[nodiscard]] virtual core::Result<std::vector<HttpHeader>>
    GetHeaders(const DownloadUrlRequest& request) {
        (void)request;
        return std::vector<HttpHeader>{};
    }

    [[nodiscard]] virtual bool OnUnauthorized(const DownloadUrlRequest& request,
                                              std::string_view url) {
        (void)request;
        (void)url;
        return false;
    }

    virtual void OnResult(const DownloadUrlRequest& request, std::string_view url, bool success,
                          int status) {
        (void)request;
        (void)url;
        (void)success;
        (void)status;
    }

    [[nodiscard]] virtual bool ShouldRetryStatus(int status) const noexcept {
        (void)status;
        return false;
    }
};

enum class DownloadEventType : std::uint8_t {
    Started,
    FileStarted,
    Progress,
    FileCompleted,
    FileFailed,
    RetryScheduled,
    Completed,
    Canceled,
};

struct DownloadEvent {
    DownloadEventType type = DownloadEventType::Started;
    std::string bundle_name;
    std::string file_name;
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t total_bytes = 0;
    int attempt = 0;
    core::Error error{core::ErrorCode::Ok};
};

struct DownloadOptions {
    int max_concurrency = 4;
    int max_requests_per_frame = 8;
    std::chrono::milliseconds scheduler_frame_interval{16};
    int max_retries = 3;
    std::chrono::milliseconds retry_base_delay{200};
    std::chrono::milliseconds retry_max_delay{5000};
    bool retry_jitter = true;
    std::chrono::milliseconds timeout{30000};
    std::chrono::milliseconds watchdog_timeout{0};
    std::uint64_t resume_minimum_size = 1;
    std::uint64_t max_file_bytes = 0;
    bool verify_sha256 = true;
    bool remove_temp_on_cancel = true;
    std::function<void(const DownloadEvent&)> on_event;
};

struct UnpackOptions {
    std::vector<std::string> tags;
    int max_concurrency = 4;
};

struct BundleImportInfo {
    std::string file_path;
    std::string bundle_name;
    core::AssetGuid bundle_guid;
};

struct BundleImportOptions {
    std::vector<BundleImportInfo> bundles;
    int max_concurrency = 4;
};

} // namespace parcel::runtime
