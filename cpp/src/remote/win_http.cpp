#include "parcel/remote/win_http.hpp"

#include "parcel/core/error.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace parcel::remote {
#ifdef _WIN32
namespace {

using core::Error;
using core::ErrorCode;
using core::MakeError;
using core::Result;

struct InternetHandle {
    HINTERNET value = nullptr;

    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    ~InternetHandle() {
        if (value != nullptr) {
            WinHttpCloseHandle(value);
        }
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    InternetHandle(InternetHandle&& other) noexcept : value(other.value) { other.value = nullptr; }

    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this != &other) {
            if (value != nullptr) {
                WinHttpCloseHandle(value);
            }
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return value != nullptr; }
};

struct PooledConnection {
    InternetHandle session;
    InternetHandle connection;
    std::wstring host;
    INTERNET_PORT port = 0;
    bool secure = false;
};

Result<std::wstring> ToWide(std::string_view value) {
    if (value.empty()) {
        return std::wstring{};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return MakeError(ErrorCode::InvalidArgument, "URL is not valid UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), length);
    return result;
}

INTERNET_PORT ExplicitPort(std::wstring_view url, INTERNET_PORT fallback) {
    const std::size_t scheme = url.find(L"://");
    if (scheme == std::wstring_view::npos) {
        return fallback;
    }
    const std::size_t authority = scheme + 3;
    const std::size_t slash = url.find(L'/', authority);
    const std::size_t end = slash == std::wstring_view::npos ? url.size() : slash;
    const std::size_t colon = url.find(L':', authority);
    if (colon == std::wstring_view::npos || colon >= end) {
        return fallback;
    }
    INTERNET_PORT port = 0;
    for (std::size_t i = colon + 1; i < end; ++i) {
        if (url[i] < L'0' || url[i] > L'9') {
            return fallback;
        }
        port = static_cast<INTERNET_PORT>(port * 10 + (url[i] - L'0'));
    }
    return port == 0 ? fallback : port;
}

Result<void> AddHeader(HINTERNET request, std::wstring_view line) {
    std::wstring header(line);
    header += L"\r\n";
    if (WinHttpAddRequestHeaders(request, header.c_str(), static_cast<DWORD>(header.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) == FALSE) {
        return MakeError(ErrorCode::Http, "failed to set an HTTP request header");
    }
    return {};
}

void ParseHeaders(std::wstring_view raw, runtime::HttpResponse& response) {
    std::size_t position = 0;
    bool first = true;
    while (position < raw.size()) {
        const std::size_t end = raw.find(L"\r\n", position);
        const std::wstring_view line = end == std::wstring_view::npos
                                           ? raw.substr(position)
                                           : raw.substr(position, end - position);
        position = end == std::wstring_view::npos ? raw.size() : end + 2;
        if (first) {
            first = false;
            continue;
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t colon = line.find(L':');
        if (colon == std::wstring_view::npos) {
            continue;
        }
        std::wstring name(line.substr(0, colon));
        std::wstring value(line.substr(colon + 1));
        while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) {
            value.erase(value.begin());
        }
        const int name_size = WideCharToMultiByte(
            CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
        const int value_size = WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (name_size <= 0 || value_size <= 0) {
            continue;
        }
        std::string name_utf8(static_cast<std::size_t>(name_size), '\0');
        std::string value_utf8(static_cast<std::size_t>(value_size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                            name_utf8.data(), name_size, nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                            value_utf8.data(), value_size, nullptr, nullptr);
        response.headers.push_back(
            runtime::HttpHeader{std::move(name_utf8), std::move(value_utf8)});
    }
}

Error WinHttpError(ErrorCode code, std::string_view operation) {
    const DWORD last = GetLastError();
    if (last == ERROR_WINHTTP_TIMEOUT) {
        return MakeError(ErrorCode::Timeout, std::string(operation) + " timed out");
    }
    return MakeError(code, std::string(operation) + " failed with WinHTTP error " +
                               std::to_string(static_cast<unsigned long>(last)));
}

} // namespace

struct WinHttpTransport::Impl {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> connections_created{0};
    std::atomic<std::uint64_t> connections_reused{0};
    std::mutex pool_mutex;
    std::vector<PooledConnection> idle;
};

WinHttpTransport::WinHttpTransport(bool use_system_proxy)
    : use_system_proxy_(use_system_proxy), impl_(std::make_shared<Impl>()) {}

WinHttpTransport::~WinHttpTransport() = default;

WinHttpTransport::Stats WinHttpTransport::GetStats() const noexcept {
    Stats stats;
    if (impl_) {
        stats.requests = impl_->requests.load(std::memory_order_acquire);
        stats.connections_created =
            impl_->connections_created.load(std::memory_order_acquire);
        stats.connections_reused =
            impl_->connections_reused.load(std::memory_order_acquire);
    }
    return stats;
}

Result<runtime::HttpResponse> WinHttpTransport::Send(const runtime::HttpRequest& request) {
    if (request.url.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "HTTP URL is empty");
    }
    auto wide_url = ToWide(request.url);
    if (!wide_url) {
        return std::move(wide_url).TakeError();
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (WinHttpCrackUrl(wide_url.Value().c_str(), 0, ICU_REJECT_USERPWD, &components) == FALSE) {
        return WinHttpError(ErrorCode::InvalidArgument, "WinHttpCrackUrl");
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
        return MakeError(ErrorCode::Unsupported, "only HTTP and HTTPS URLs are supported");
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring target(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        target.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;

    impl_->requests.fetch_add(1, std::memory_order_acq_rel);

    const INTERNET_PORT port = ExplicitPort(wide_url.Value(), components.nPort);
    const DWORD access_type =
        use_system_proxy_ ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY;
    auto apply_timeouts = [&request](InternetHandle& session) {
        if (request.timeout.count() > 0 || request.watchdog_timeout.count() > 0) {
            const auto timeout = static_cast<int>(request.timeout.count());
            const auto watchdog = request.watchdog_timeout.count() > 0
                                      ? static_cast<int>(request.watchdog_timeout.count())
                                      : timeout;
            (void)WinHttpSetTimeouts(session.value, timeout, timeout, watchdog, timeout);
        }
    };

    Error create_error{ErrorCode::Ok};
    auto make_connection = [&]() -> PooledConnection {
        PooledConnection pooled;
        pooled.session = InternetHandle(WinHttpOpen(L"Parcel/0.7", access_type,
                                                    WINHTTP_NO_PROXY_NAME,
                                                    WINHTTP_NO_PROXY_BYPASS, 0));
        if (!pooled.session.valid()) {
            create_error = WinHttpError(ErrorCode::Http, "WinHttpOpen");
            return pooled;
        }
        apply_timeouts(pooled.session);
        pooled.connection =
            InternetHandle(WinHttpConnect(pooled.session.value, host.c_str(), port, 0));
        if (!pooled.connection.valid()) {
            create_error = WinHttpError(ErrorCode::Http, "WinHttpConnect");
            return pooled;
        }
        pooled.host = host;
        pooled.port = port;
        pooled.secure = secure;
        return pooled;
    };

    PooledConnection pooled;
    bool reused = false;
    {
        std::lock_guard lock(impl_->pool_mutex);
        for (auto it = impl_->idle.begin(); it != impl_->idle.end(); ++it) {
            if (it->host == host && it->port == port && it->secure == secure) {
                pooled = std::move(*it);
                impl_->idle.erase(it);
                reused = true;
                impl_->connections_reused.fetch_add(1, std::memory_order_acq_rel);
                break;
            }
        }
    }
    if (!pooled.session.valid()) {
        pooled = make_connection();
        if (!pooled.session.valid() || !pooled.connection.valid()) {
            return create_error.IsOk()
                       ? MakeError(ErrorCode::Http, "failed to create a WinHTTP connection")
                       : create_error;
        }
        impl_->connections_created.fetch_add(1, std::memory_order_acq_rel);
    } else {
        apply_timeouts(pooled.session);
    }

    InternetHandle handle(WinHttpOpenRequest(pooled.connection.value, L"GET", target.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             secure ? WINHTTP_FLAG_SECURE : 0));
    if (!handle.valid() && reused) {
        pooled = make_connection();
        if (pooled.session.valid() && pooled.connection.valid()) {
            impl_->connections_created.fetch_add(1, std::memory_order_acq_rel);
            handle = InternetHandle(WinHttpOpenRequest(
                pooled.connection.value, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
        }
    }
    if (!handle.valid()) {
        return WinHttpError(ErrorCode::Http, "WinHttpOpenRequest");
    }

    for (const auto& header : request.headers) {
        auto name = ToWide(header.name);
        auto value = ToWide(header.value);
        if (!name || !value) {
            return MakeError(ErrorCode::InvalidArgument, "HTTP header is not valid UTF-8");
        }
        std::wstring line = name.Value();
        line += L": ";
        line += value.Value();
        PARCEL_TRY(AddHeader(handle.value, line));
    }
    auto watchdog_failure = [&request](std::string_view operation) -> Error {
        if (request.should_abort && request.should_abort()) {
            return MakeError(ErrorCode::Timeout, "download watchdog aborted");
        }
        return WinHttpError(ErrorCode::Http, operation);
    };
    if (request.range_offset) {
        std::wstring range = L"Range: bytes=";
        range += std::to_wstring(*request.range_offset);
        range += L"-";
        PARCEL_TRY(AddHeader(handle.value, range));
    }

    if (WinHttpSendRequest(handle.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                           0, 0, 0) == FALSE) {
        return watchdog_failure("WinHttpSendRequest");
    }
    if (WinHttpReceiveResponse(handle.value, nullptr) == FALSE) {
        return watchdog_failure("WinHttpReceiveResponse");
    }

    runtime::HttpResponse response;
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX) == FALSE) {
        return WinHttpError(ErrorCode::Http, "WinHttpQueryHeaders");
    }
    response.status = static_cast<int>(status);

    DWORD raw_size = 0;
    WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &raw_size, WINHTTP_NO_HEADER_INDEX);
    if (raw_size > 0) {
        std::wstring raw(raw_size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(handle.value, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &raw_size,
                                WINHTTP_NO_HEADER_INDEX) != FALSE) {
            ParseHeaders(raw, response);
        }
    }

    for (;;) {
        if (request.should_abort && request.should_abort()) {
            return MakeError(ErrorCode::Timeout, "download watchdog aborted");
        }
        DWORD available = 0;
        if (WinHttpQueryDataAvailable(handle.value, &available) == FALSE) {
            return watchdog_failure("WinHttpQueryDataAvailable");
        }
        if (available == 0) {
            break;
        }
        const std::size_t old_size = response.body.size();
        response.body.resize(old_size + available);
        DWORD read = 0;
        if (WinHttpReadData(handle.value, response.body.data() + old_size, available, &read) ==
            FALSE) {
            return watchdog_failure("WinHttpReadData");
        }
        response.body.resize(old_size + read);
        if (request.on_bytes_received) {
            request.on_bytes_received(response.body.size());
        }
        if (read == 0) {
            break;
        }
    }
    handle = InternetHandle{};
    {
        std::lock_guard lock(impl_->pool_mutex);
        if (impl_->idle.size() < 8U) {
            impl_->idle.push_back(std::move(pooled));
        }
    }
    return response;
}

#else

struct WinHttpTransport::Impl {
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> connections_created{0};
    std::atomic<std::uint64_t> connections_reused{0};
};

WinHttpTransport::WinHttpTransport(bool use_system_proxy)
    : use_system_proxy_(use_system_proxy), impl_(std::make_shared<Impl>()) {}

WinHttpTransport::~WinHttpTransport() = default;

WinHttpTransport::Stats WinHttpTransport::GetStats() const noexcept {
    Stats stats;
    if (impl_) {
        stats.requests = impl_->requests.load(std::memory_order_acquire);
        stats.connections_created =
            impl_->connections_created.load(std::memory_order_acquire);
        stats.connections_reused =
            impl_->connections_reused.load(std::memory_order_acquire);
    }
    return stats;
}

Result<runtime::HttpResponse> WinHttpTransport::Send(const runtime::HttpRequest& request) {
    (void)request;
    impl_->requests.fetch_add(1, std::memory_order_acq_rel);
    return core::MakeError(core::ErrorCode::Unsupported,
                           "WinHttpTransport is only available on Windows");
}

#endif
} // namespace parcel::remote


