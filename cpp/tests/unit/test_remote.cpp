#include <catch_amalgamated.hpp>

#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/remote/manifest.hpp"
#include "parcel/remote/remote.hpp"
#include "parcel/remote/win_http.hpp"
#include "parcel/runtime/runtime.hpp"
#include "parcel/serialize/manifest.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace parcel;
using parcel::core::AssetGuid;
using parcel::core::Hash256;
using parcel::test::TempDir;
using parcel::test::ToUtf8;

namespace {

using ByteVector = std::vector<std::byte>;

AssetGuid GuidFrom(std::string_view name) {
    const Hash256 hash = core::Sha256("parcel.remote.test:" + std::string(name));
    std::array<std::byte, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(hash.bytes[i]);
    }
    return core::Guid128::FromBytes(std::span<const std::byte, 16>(bytes));
}

ByteVector Bytes(std::string_view value) {
    ByteVector result(value.size());
    std::transform(value.begin(), value.end(), result.begin(),
                   [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });
    return result;
}

void WriteFile(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file);
}

ByteVector ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    ByteVector result(size);
    file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(size));
    REQUIRE(file);
    return result;
}

std::string HeaderValue(const runtime::HttpRequest& request, std::string_view name) {
    for (const auto& header : request.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

struct MockEntry {
    ByteVector body;
    int status = 200;
    bool range_supported = true;
    int transport_failures = 0;
    int unauthorized_once = 0;
    int forbidden_once = 0;
    int delay_ms = 0;
    std::string required_token;
};

class MockTransport final : public runtime::IHttpTransport {
  public:
    void Set(std::string url, ByteVector body, int status = 200) {
        std::lock_guard lock(mutex_);
        entries_[std::move(url)] = MockEntry{std::move(body), status};
    }

    void SetStatus(std::string url, int status) {
        std::lock_guard lock(mutex_);
        entries_[std::move(url)].status = status;
    }

    void SetRangeSupport(std::string url, bool supported) {
        std::lock_guard lock(mutex_);
        entries_[std::move(url)].range_supported = supported;
    }

    void SetTransportFailures(std::string url, int count) {
        std::lock_guard lock(mutex_);
        entries_[std::move(url)].transport_failures = count;
    }

    void SetUnauthorizedOnce(std::string url, int count) {
        std::lock_guard lock(mutex_);
        entries_[std::move(url)].unauthorized_once = count;
    }

    void SetForbiddenOnce(std::string url, int count) {
        std::lock_guard lock(mutex_);
        entries_[std::move(url)].forbidden_once = count;
    }

    void SetRequiredToken(std::string url, std::string token) {
        std::lock_guard lock(mutex_);
        entries_[std::move(url)].required_token = std::move(token);
    }

    void SetDelay(std::string url, int delay_ms) {
        std::lock_guard lock(mutex_);
        entries_[std::move(url)].delay_ms = delay_ms;
    }

    [[nodiscard]] std::vector<runtime::HttpRequest> Requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    core::Result<runtime::HttpResponse> Send(const runtime::HttpRequest& request) override {
        std::lock_guard lock(mutex_);
        requests_.push_back(request);
        auto found = entries_.find(request.url);
        if (found == entries_.end()) {
            return runtime::HttpResponse{404, {}, {}, core::Error{core::ErrorCode::Http}};
        }
        MockEntry& entry = found->second;
        if (entry.delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(entry.delay_ms));
        }
        if (entry.transport_failures > 0) {
            --entry.transport_failures;
            return core::MakeError(core::ErrorCode::Http, "mock transport failure");
        }
        if (entry.forbidden_once > 0) {
            --entry.forbidden_once;
            return runtime::HttpResponse{403, {}, {}, core::Error{core::ErrorCode::Ok}};
        }
        if (entry.status == 401 || entry.status == 403 || entry.unauthorized_once > 0) {
            if (entry.unauthorized_once > 0) {
                --entry.unauthorized_once;
            }
            return runtime::HttpResponse{
                entry.status == 200 ? 401 : entry.status, {}, {}, core::Error{core::ErrorCode::Ok}};
        }
        if (!entry.required_token.empty() &&
            HeaderValue(request, "Authorization") != entry.required_token) {
            return runtime::HttpResponse{401, {}, {}, core::Error{core::ErrorCode::Ok}};
        }
        if (entry.status != 200) {
            return runtime::HttpResponse{
                entry.status, {}, entry.body, core::Error{core::ErrorCode::Ok}};
        }
        runtime::HttpResponse response;
        response.status = 200;
        if (request.range_offset && entry.range_supported) {
            const std::uint64_t offset = *request.range_offset;
            if (offset >= entry.body.size()) {
                response.status = 416;
                return response;
            }
            response.status = 206;
            response.body.assign(entry.body.begin() + static_cast<std::ptrdiff_t>(offset),
                                 entry.body.end());
            return response;
        }
        response.body = entry.body;
        return response;
    }

  private:
    mutable std::mutex mutex_;
    std::map<std::string, MockEntry> entries_;
    std::vector<runtime::HttpRequest> requests_;
};

class WatchdogTransport final : public runtime::IHttpTransport {
  public:
    WatchdogTransport(ByteVector body, bool emit_progress)
        : body_(std::move(body)), emit_progress_(emit_progress) {}

    core::Result<runtime::HttpResponse> Send(const runtime::HttpRequest& request) override {
        ++requests_;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        auto last_progress = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = std::chrono::steady_clock::now();
            if (emit_progress_ && now - last_progress >= std::chrono::milliseconds(10)) {
                if (request.on_bytes_received) {
                    request.on_bytes_received(1);
                }
                last_progress = now;
            }
            if (request.should_abort && request.should_abort()) {
                return core::MakeError(core::ErrorCode::Timeout, "watchdog abort");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (request.on_bytes_received) {
            request.on_bytes_received(body_.size());
        }
        runtime::HttpResponse response;
        response.status = 200;
        response.body = body_;
        return response;
    }

    [[nodiscard]] int Requests() const noexcept { return requests_; }

  private:
    ByteVector body_;
    bool emit_progress_ = false;
    int requests_ = 0;
};

class FlakyCommitSandbox final : public runtime::SandboxFileSystem {
  public:
    explicit FlakyCommitSandbox(runtime::FileSystemOptions options)
        : runtime::SandboxFileSystem(std::move(options)) {}

    void FailCommits(int count) { fail_commits_.store(count, std::memory_order_release); }

    core::Result<void>
    CommitDownloadedBundle(const serialize::ManifestBundleView& bundle,
                           std::string_view temp_path) override {
        if (fail_commits_.load(std::memory_order_acquire) > 0) {
            fail_commits_.fetch_sub(1, std::memory_order_acq_rel);
            return core::MakeError(core::ErrorCode::Io, "injected disk full");
        }
        return runtime::SandboxFileSystem::CommitDownloadedBundle(bundle, temp_path);
    }

  private:
    std::atomic<int> fail_commits_{0};
};


class TestUrlProvider final : public runtime::IDownloadUrlProvider {
  public:
    explicit TestUrlProvider(std::vector<std::string> urls) : urls_(std::move(urls)) {}

    core::Result<std::vector<std::string>>
    GetUrls(const runtime::DownloadUrlRequest& request) override {
        (void)request;
        std::lock_guard lock(mutex_);
        return urls_;
    }

    core::Result<std::vector<runtime::HttpHeader>>
    GetHeaders(const runtime::DownloadUrlRequest& request) override {
        (void)request;
        std::lock_guard lock(mutex_);
        if (token_.empty()) {
            return std::vector<runtime::HttpHeader>{};
        }
        return std::vector<runtime::HttpHeader>{{"Authorization", token_}};
    }

    bool OnUnauthorized(const runtime::DownloadUrlRequest& request, std::string_view url) override {
        (void)request;
        (void)url;
        std::lock_guard lock(mutex_);
        if (refreshed_) {
            return false;
        }
        refreshed_ = true;
        token_ = "Bearer refreshed";
        return true;
    }

    [[nodiscard]] int UnauthorizedCalls() const {
        std::lock_guard lock(mutex_);
        return refreshed_ ? 1 : 0;
    }

    void SetToken(std::string token) {
        std::lock_guard lock(mutex_);
        token_ = std::move(token);
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::string> urls_;
    std::string token_;
    bool refreshed_ = false;
};

class MappedUrlProvider final : public runtime::IDownloadUrlProvider {
  public:
    explicit MappedUrlProvider(std::map<std::string, std::string> urls)
        : urls_(std::move(urls)) {}

    core::Result<std::vector<std::string>>
    GetUrls(const runtime::DownloadUrlRequest& request) override {
        auto found = urls_.find(std::string(request.file_name));
        if (found == urls_.end()) {
            return core::MakeError(core::ErrorCode::NotFound, "no mapped download URL");
        }
        return std::vector<std::string>{found->second};
    }

    core::Result<std::vector<runtime::HttpHeader>>
    GetHeaders(const runtime::DownloadUrlRequest& request) override {
        (void)request;
        return std::vector<runtime::HttpHeader>{};
    }

    bool OnUnauthorized(const runtime::DownloadUrlRequest& request, std::string_view url) override {
        (void)request;
        (void)url;
        return false;
    }

  private:
    std::map<std::string, std::string> urls_;
};

struct PackageFixture {
    TempDir directory{"remote_package"};
    std::filesystem::path builtin = directory.Path() / "builtin";
    std::filesystem::path sandbox = directory.Path() / "sandbox";
    serialize::ManifestDesc manifest;
    ByteVector manifest_bytes;
    ByteVector bundle_bytes;
    std::string bundle_file;
    AssetGuid bundle_guid;
    std::string bundle_name = "main";

    PackageFixture() {
        bundle_guid = GuidFrom("main-bundle");
        bundle_file = bundle_guid.ToHex() + ".bundle";
        bundle_bytes = Bytes("fake-bundle-payload-for-remote-test");
        manifest.package_name = "game";
        manifest.package_version = "1.0.0";
        serialize::ManifestBundleDesc bundle;
        bundle.bundle_guid = bundle_guid;
        bundle.bundle_name = bundle_name;
        bundle.file_name = bundle_file;
        bundle.file_size = bundle_bytes.size();
        bundle.file_sha256 = core::Sha256(bundle_bytes);
        bundle.file_crc = core::Crc32(bundle_bytes);
        manifest.bundles.push_back(std::move(bundle));
        auto bytes = serialize::WriteManifest(manifest);
        REQUIRE(bytes);
        manifest_bytes = std::move(bytes).Value();
        WriteFile(builtin / "manifest.bytes", manifest_bytes);
        WriteFile(builtin / "game.version", Bytes("1.0.0"));
    }

    struct ExtraBundle {
        AssetGuid guid;
        std::string file_name;
        ByteVector bytes;
    };

    ExtraBundle AddBundle(std::string_view name) {
        ExtraBundle extra;
        extra.guid = GuidFrom(name);
        extra.file_name = extra.guid.ToHex() + ".bundle";
        extra.bytes = Bytes("fake-bundle-payload-for-remote-test:" + std::string(name));
        serialize::ManifestBundleDesc bundle;
        bundle.bundle_guid = extra.guid;
        bundle.bundle_name = std::string(name);
        bundle.file_name = extra.file_name;
        bundle.file_size = extra.bytes.size();
        bundle.file_sha256 = core::Sha256(extra.bytes);
        bundle.file_crc = core::Crc32(extra.bytes);
        manifest.bundles.push_back(std::move(bundle));
        auto bytes = serialize::WriteManifest(manifest);
        REQUIRE(bytes);
        manifest_bytes = std::move(bytes).Value();
        WriteFile(builtin / "manifest.bytes", manifest_bytes);
        return extra;
    }


    runtime::ResourcePackageOptions
    Options(std::shared_ptr<runtime::IHttpTransport> transport,
            std::shared_ptr<runtime::IDownloadUrlProvider> provider) const {
        runtime::ResourcePackageOptions options;
        options.package_name = "game";
        options.builtin_root = ToUtf8(builtin);
        options.sandbox_root = ToUtf8(sandbox);
        options.download_transport = std::move(transport);
        options.download_url_provider = std::move(provider);
        options.download_options.max_retries = 3;
        options.download_options.retry_base_delay = std::chrono::milliseconds(0);
        options.download_options.retry_max_delay = std::chrono::milliseconds(0);
        options.download_options.resume_minimum_size = 1;
        return options;
    }

    std::string BundleUrl(std::string_view base) const {
        return std::string(base) + "/" + bundle_file;
    }
};

bool WaitDownloader(const std::shared_ptr<runtime::ResourceDownloader>& downloader,
                    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!downloader->IsDone() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return downloader->IsDone();
}

std::int64_t FutureUnixSeconds(int seconds = 300) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               (std::chrono::system_clock::now() + std::chrono::seconds(seconds))
                   .time_since_epoch())
        .count();
}

std::string SignRequestUrl(std::string_view sign_url, std::string_view signed_path) {
    std::string escaped;
    escaped.reserve(signed_path.size() * 3);
    constexpr char kHex[] = "0123456789ABCDEF";
    for (const unsigned char c : signed_path) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            escaped.push_back(static_cast<char>(c));
        } else {
            escaped.push_back('%');
            escaped.push_back(kHex[c >> 4]);
            escaped.push_back(kHex[c & 0x0F]);
        }
    }
    std::string result(sign_url);
    result += sign_url.find('?') == std::string_view::npos ? '?' : '&';
    result += "path=";
    result += escaped;
    return result;
}

} // namespace

TEST_CASE("remote downloader installs a verified bundle atomically", "[remote]") {
    PackageFixture fixture;
    auto transport = std::make_shared<MockTransport>();
    transport->Set(fixture.BundleUrl("https://cdn-a.test"), fixture.bundle_bytes);
    auto provider = std::make_shared<TestUrlProvider>(
        std::vector<std::string>{fixture.BundleUrl("https://cdn-a.test")});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    REQUIRE(downloader.Value()->Summary().total_count == 1);
    REQUIRE(downloader.Value()->TotalBytes() == fixture.bundle_bytes.size());
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    INFO(downloader.Value()->LastError().ToString());
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(downloader.Value()->Summary().completed_count == 1);
    REQUIRE(downloader.Value()->Progress() == Catch::Approx(1.0f));
    REQUIRE(downloader.Value()->DownloadedBytes() == fixture.bundle_bytes.size());

    const auto installed = fixture.sandbox / "bundles" / fixture.bundle_file;
    REQUIRE(std::filesystem::exists(installed));
    REQUIRE(ReadFile(installed) == fixture.bundle_bytes);
    REQUIRE(std::filesystem::exists(installed.string() + ".cache"));
    REQUIRE_FALSE(std::filesystem::exists(fixture.sandbox / "temp" /
                                          (fixture.bundle_guid.ToHex() + ".part")));
    REQUIRE(package.Value()->GetDownloadSize() == 0);
    REQUIRE(package.Value()->VerifyCache());
    auto cache_stats = package.Value()->QueryCache();
    REQUIRE(cache_stats);
    REQUIRE(cache_stats.Value().files >= 2);
    REQUIRE(cache_stats.Value().bytes > 0);

    const auto stale = fixture.sandbox / "bundles" / "stale.bundle";
    WriteFile(stale, fixture.bundle_bytes);
    runtime::CacheClearOptions old_options;
    old_options.mode = runtime::CacheClearMode::OldVersions;
    REQUIRE(package.Value()->ClearCache(old_options));
    REQUIRE(std::filesystem::exists(installed));
    REQUIRE_FALSE(std::filesystem::exists(stale));

    REQUIRE(package.Value()->ClearCache(runtime::CacheClearOptions{}));
    REQUIRE_FALSE(std::filesystem::exists(installed));

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("signed URL provider caches and refreshes server signatures", "[remote]") {
    PackageFixture fixture;
    auto transport = std::make_shared<MockTransport>();
    const std::string sign_url = "https://api.test/api/v1/client/url";
    const std::string signed_path = "/files/game/1.0.0/" + fixture.bundle_file;
    const auto expires = FutureUnixSeconds();
    const std::string signed_url = "https://cdn.test" + signed_path + "?expires=" +
                                   std::to_string(expires) + "&key_id=k1&signature=sig";
    const std::string sign_request = SignRequestUrl(sign_url, signed_path);
    const std::string sign_body =
        R"({"url":")" + signed_url + R"(","expires":)" + std::to_string(expires) + "}";
    transport->Set(sign_request, Bytes(sign_body));
    transport->SetRequiredToken(sign_request, "Bearer token");

    auto auth = std::make_shared<remote::StaticAuthProvider>("Authorization", "Bearer token");
    remote::SignedUrlProvider provider(
        sign_url,
        {"https://cdn-a.test/files/game/1.0.0", "https://cdn-b.test/files/game/1.0.0"},
        transport, auth);
    remote::RemoteRequestContext context;
    context.package = "game";
    context.version = "1.0.0";
    context.file_name = fixture.bundle_file;

    auto urls = provider.GetUrls(context);
    REQUIRE(urls);
    REQUIRE(urls.Value().size() == 2);
    const std::string query = signed_url.substr(signed_url.find('?'));
    REQUIRE(urls.Value()[0] == "https://cdn-a.test/files/game/1.0.0/" + fixture.bundle_file +
                                   query);
    REQUIRE(urls.Value()[1] == "https://cdn-b.test/files/game/1.0.0/" + fixture.bundle_file +
                                   query);
    REQUIRE(transport->Requests().size() == 1);
    REQUIRE(HeaderValue(transport->Requests()[0], "Authorization") == "Bearer token");

    REQUIRE(provider.GetUrls(context));
    REQUIRE(transport->Requests().size() == 1);

    provider.OnResult(context, urls.Value()[0], false, 403);
    REQUIRE(provider.GetUrls(context));
    REQUIRE(transport->Requests().size() == 2);
    REQUIRE(provider.ShouldRetryStatus(403));
    REQUIRE(provider.ShouldRetryStatus(410));
    REQUIRE_FALSE(provider.ShouldRetryStatus(404));
}

TEST_CASE("signed URL downloader re-signs after a forbidden response", "[remote]") {
    PackageFixture fixture;
    auto transport = std::make_shared<MockTransport>();
    const std::string sign_url = "https://api.test/api/v1/client/url";
    const std::string signed_path = "/files/game/1.0.0/" + fixture.bundle_file;
    const auto expires = FutureUnixSeconds();
    const std::string file_url = "https://cdn.test" + signed_path + "?expires=" +
                                 std::to_string(expires) + "&key_id=k1&signature=sig";
    const std::string sign_request = SignRequestUrl(sign_url, signed_path);
    const std::string sign_body =
        R"({"url":")" + file_url + R"(","expires":)" + std::to_string(expires) + "}";
    transport->Set(sign_request, Bytes(sign_body));
    transport->SetRequiredToken(sign_request, "Bearer token");
    transport->Set(file_url, fixture.bundle_bytes);
    transport->SetForbiddenOnce(file_url, 1);

    auto auth = std::make_shared<remote::StaticAuthProvider>("Authorization", "Bearer token");
    auto signed_provider = std::make_shared<remote::SignedUrlProvider>(
        sign_url, std::vector<std::string>{}, transport, auth);
    auto provider = std::make_shared<remote::RemoteDownloadProvider>(signed_provider, auth);

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    INFO(downloader.Value()->LastError().ToString());
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    const auto installed = fixture.sandbox / "bundles" / fixture.bundle_file;
    REQUIRE(std::filesystem::exists(installed));
    REQUIRE(ReadFile(installed) == fixture.bundle_bytes);
    REQUIRE(transport->Requests().size() == 4);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("download summary reports elapsed time and throughput", "[remote]") {
    PackageFixture fixture;
    const std::string url = fixture.BundleUrl("https://cdn-a.test");
    auto transport = std::make_shared<MockTransport>();
    transport->Set(url, fixture.bundle_bytes);
    transport->SetDelay(url, 20);
    auto provider = std::make_shared<TestUrlProvider>(std::vector<std::string>{url});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    const auto summary = downloader.Value()->Summary();
    REQUIRE(summary.downloaded_bytes == fixture.bundle_bytes.size());
    REQUIRE(summary.elapsed.count() >= 10);
    REQUIRE(summary.bytes_per_second > 0);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote cache verification checks metadata and content", "[remote]") {
    PackageFixture fixture;
    auto transport = std::make_shared<MockTransport>();
    transport->Set(fixture.BundleUrl("https://cdn-a.test"), fixture.bundle_bytes);
    auto provider = std::make_shared<TestUrlProvider>(
        std::vector<std::string>{fixture.BundleUrl("https://cdn-a.test")});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);

    const auto installed = fixture.sandbox / "bundles" / fixture.bundle_file;
    REQUIRE(std::filesystem::exists(installed));
    auto corrupt = ReadFile(installed);
    REQUIRE(corrupt.size() == fixture.bundle_bytes.size());
    corrupt[0] = static_cast<std::byte>(static_cast<unsigned char>(corrupt[0]) ^ 0xFF);
    WriteFile(installed, corrupt);

    runtime::CacheVerifyOptions metadata_options;
    metadata_options.level = runtime::CacheVerifyLevel::Metadata;
    auto metadata = package.Value()->VerifyCacheFiles(metadata_options);
    REQUIRE(metadata);
    REQUIRE(metadata.Value().total == 1);
    REQUIRE(metadata.Value().valid == 1);
    REQUIRE(metadata.Value().invalid == 0);
    REQUIRE(std::filesystem::exists(installed));

    auto content = package.Value()->VerifyCacheFiles();
    REQUIRE(content);
    REQUIRE(content.Value().total == 1);
    REQUIRE(content.Value().valid == 0);
    REQUIRE(content.Value().invalid == 1);
    REQUIRE(content.Value().removed == 1);
    REQUIRE(content.Value().failed == 0);
    REQUIRE_FALSE(std::filesystem::exists(installed));
    REQUIRE_FALSE(std::filesystem::exists(installed.string() + ".cache"));
    REQUIRE(package.Value()->GetDownloadSize() == fixture.bundle_bytes.size());

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote cache verification leaves missing bundles downloadable", "[remote]") {
    PackageFixture fixture;
    auto transport = std::make_shared<MockTransport>();
    transport->Set(fixture.BundleUrl("https://cdn-a.test"), fixture.bundle_bytes);
    auto provider = std::make_shared<TestUrlProvider>(
        std::vector<std::string>{fixture.BundleUrl("https://cdn-a.test")});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);

    const auto installed = fixture.sandbox / "bundles" / fixture.bundle_file;
    REQUIRE(std::filesystem::exists(installed));
    auto present = package.Value()->VerifyCacheFiles();
    REQUIRE(present);
    REQUIRE(present.Value().valid == 1);
    REQUIRE(present.Value().missing == 0);
    REQUIRE(present.Value().valid_bytes == fixture.bundle_bytes.size());

    REQUIRE(std::filesystem::remove(installed));
    REQUIRE(std::filesystem::remove(installed.string() + ".cache"));
    auto missing = package.Value()->VerifyCacheFiles();
    REQUIRE(missing);
    REQUIRE(missing.Value().total == 1);
    REQUIRE(missing.Value().valid == 0);
    REQUIRE(missing.Value().missing == 1);
    REQUIRE(missing.Value().failed == 0);
    REQUIRE(package.Value()->VerifyCache());
    REQUIRE(package.Value()->GetDownloadSize() == fixture.bundle_bytes.size());

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}


TEST_CASE("remote cache verification continues across corrupt and missing bundles", "[remote]") {
    PackageFixture fixture;
    const auto extra = fixture.AddBundle("side");
    const std::string first_url = fixture.BundleUrl("https://cdn-a.test");
    const std::string extra_url = "https://cdn-a.test/" + extra.file_name;
    auto transport = std::make_shared<MockTransport>();
    transport->Set(first_url, fixture.bundle_bytes);
    transport->Set(extra_url, extra.bytes);
    auto provider = std::make_shared<TestUrlProvider>(
        std::vector<std::string>{first_url, extra_url});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    REQUIRE(downloader.Value()->Summary().total_count == 2);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);

    const auto installed = fixture.sandbox / "bundles" / fixture.bundle_file;
    const auto extra_installed = fixture.sandbox / "bundles" / extra.file_name;
    REQUIRE(std::filesystem::exists(installed));
    REQUIRE(std::filesystem::exists(extra_installed));
    auto corrupt = ReadFile(installed);
    corrupt[0] = static_cast<std::byte>(static_cast<unsigned char>(corrupt[0]) ^ 0xFF);
    WriteFile(installed, corrupt);
    REQUIRE(std::filesystem::remove(extra_installed));
    REQUIRE(std::filesystem::remove(extra_installed.string() + ".cache"));

    runtime::CacheVerifyOptions report_options;
    report_options.max_concurrency = 4;
    auto report = package.Value()->VerifyCacheFiles(report_options);
    REQUIRE(report);
    REQUIRE(report.Value().total == 2);
    REQUIRE(report.Value().valid == 0);
    REQUIRE(report.Value().invalid == 1);
    REQUIRE(report.Value().removed == 1);
    REQUIRE(report.Value().missing == 1);
    REQUIRE(report.Value().failed == 0);
    REQUIRE_FALSE(std::filesystem::exists(installed));
    REQUIRE(package.Value()->VerifyCache());

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}


TEST_CASE("remote cache verification matches serial and concurrent reports", "[remote]") {
    PackageFixture fixture;
    std::vector<PackageFixture::ExtraBundle> extras;
    std::map<std::string, std::string> urls;
    auto transport = std::make_shared<MockTransport>();
    const std::string main_url = fixture.BundleUrl("https://cdn-a.test");
    transport->Set(main_url, fixture.bundle_bytes);
    urls[fixture.bundle_file] = main_url;
    for (int i = 0; i < 4; ++i) {
        extras.push_back(fixture.AddBundle("extra-" + std::to_string(i)));
        const auto& extra = extras.back();
        const std::string url = "https://cdn-a.test/" + extra.file_name;
        transport->Set(url, extra.bytes);
        urls[extra.file_name] = url;
    }
    auto provider = std::make_shared<MappedUrlProvider>(urls);

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    REQUIRE(downloader.Value()->Summary().total_count == 5);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    INFO(downloader.Value()->LastError().ToString());
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);

    runtime::CacheVerifyOptions serial;
    serial.max_concurrency = 1;
    auto serial_report = package.Value()->VerifyCacheFiles(serial);
    REQUIRE(serial_report);
    runtime::CacheVerifyOptions concurrent;
    concurrent.max_concurrency = 4;
    auto concurrent_report = package.Value()->VerifyCacheFiles(concurrent);
    REQUIRE(concurrent_report);
    const auto& expected = serial_report.Value();
    const auto& actual = concurrent_report.Value();
    REQUIRE(actual.total == expected.total);
    REQUIRE(actual.valid == expected.valid);
    REQUIRE(actual.missing == expected.missing);
    REQUIRE(actual.invalid == expected.invalid);
    REQUIRE(actual.removed == expected.removed);
    REQUIRE(actual.failed == expected.failed);
    REQUIRE(actual.unsupported == expected.unsupported);
    REQUIRE(actual.valid_bytes == expected.valid_bytes);
    REQUIRE(actual.valid == 5);
    REQUIRE(actual.valid_bytes > 0);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

#if defined(_WIN32)
TEST_CASE("remote WinHTTP downloads a bundle from a loopback server", "[.winhttp]") {
    const char* base_env = std::getenv("PARCEL_WINHTTP_BASE_URL");
    const char* fixture_env = std::getenv("PARCEL_WINHTTP_FIXTURE_DIR");
    if (base_env == nullptr || fixture_env == nullptr) {
        SKIP("WinHTTP loopback environment is not configured");
    }
    const std::string base = base_env;
    const std::filesystem::path fixture_root = fixture_env;
    std::filesystem::create_directories(fixture_root);

    PackageFixture fixture;
    WriteFile(fixture_root / fixture.bundle_file, fixture.bundle_bytes);
    auto transport = std::make_shared<remote::WinHttpTransport>(false);
    auto provider = std::make_shared<TestUrlProvider>(
        std::vector<std::string>{base + "/" + fixture.bundle_file});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    REQUIRE(downloader.Value()->Summary().total_count == 1);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    INFO(downloader.Value()->LastError().ToString());
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);

    const auto installed = fixture.sandbox / "bundles" / fixture.bundle_file;
    REQUIRE(std::filesystem::exists(installed));
    REQUIRE(ReadFile(installed) == fixture.bundle_bytes);
    REQUIRE(package.Value()->VerifyCache());
    REQUIRE(package.Value()->GetDownloadSize() == 0);

    const auto before = transport->GetStats();
    runtime::HttpRequest direct;
    direct.url = base + "/" + fixture.bundle_file;
    auto direct_response = transport->Send(direct);
    REQUIRE(direct_response);
    REQUIRE(direct_response.Value().IsSuccess());
    const auto after = transport->GetStats();
    REQUIRE(after.requests > before.requests);
    REQUIRE(after.connections_reused > before.connections_reused);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}
#endif

TEST_CASE("remote downloader retries injected disk-full commits", "[remote]") {
    PackageFixture fixture;
    const std::string url = fixture.BundleUrl("https://cdn-a.test");
    auto transport = std::make_shared<MockTransport>();
    transport->Set(url, fixture.bundle_bytes);
    auto provider = std::make_shared<TestUrlProvider>(std::vector<std::string>{url});

    runtime::FileSystemOptions builtin_options;
    builtin_options.package_name = "game";
    builtin_options.root = ToUtf8(fixture.builtin);
    builtin_options.read_only = true;
    auto builtin_fs = std::make_shared<runtime::BuiltinFileSystem>(builtin_options);

    runtime::FileSystemOptions sandbox_options;
    sandbox_options.package_name = "game";
    sandbox_options.root = ToUtf8(fixture.sandbox);
    auto sandbox_fs = std::make_shared<FlakyCommitSandbox>(sandbox_options);

    runtime::ResourcePackageOptions options;
    options.package_name = "game";
    options.file_systems = {builtin_fs, sandbox_fs};
    options.download_file_system = sandbox_fs;
    options.download_transport = transport;
    options.download_url_provider = provider;
    options.download_options.max_retries = 3;
    options.download_options.retry_base_delay = std::chrono::milliseconds(0);
    options.download_options.retry_max_delay = std::chrono::milliseconds(0);

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(options);
    REQUIRE(package);

    const auto installed = fixture.sandbox / "bundles" / fixture.bundle_file;
    const auto temp = fixture.sandbox / "temp" / (fixture.bundle_guid.ToHex() + ".part");
    sandbox_fs->FailCommits(1);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    INFO(downloader.Value()->LastError().ToString());
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(std::filesystem::exists(installed));
    REQUIRE_FALSE(std::filesystem::exists(temp));
    REQUIRE(transport->Requests().size() >= 2);

    REQUIRE(package.Value()->ClearCache(runtime::CacheClearOptions{}));
    REQUIRE_FALSE(std::filesystem::exists(installed));
    sandbox_fs->FailCommits(4);
    auto failing = package.Value()->CreateDownloader();
    REQUIRE(failing);
    failing.Value()->Start();
    REQUIRE(WaitDownloader(failing.Value()));
    REQUIRE(failing.Value()->Status() == core::OperationStatus::Failed);
    REQUIRE(failing.Value()->LastError().code == core::ErrorCode::Io);
    REQUIRE_FALSE(std::filesystem::exists(installed));
    REQUIRE_FALSE(std::filesystem::exists(temp));

    sandbox_fs->FailCommits(0);
    auto recovered = package.Value()->CreateDownloader();
    REQUIRE(recovered);
    recovered.Value()->Start();
    REQUIRE(WaitDownloader(recovered.Value()));
    REQUIRE(recovered.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(std::filesystem::exists(installed));

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}


TEST_CASE("remote downloader resumes with Range and retries transport failures", "[remote]") {
    PackageFixture fixture;
    const std::string url = fixture.BundleUrl("https://cdn-a.test");
    auto transport = std::make_shared<MockTransport>();
    transport->Set(url, fixture.bundle_bytes);
    transport->SetTransportFailures(url, 2);
    auto provider = std::make_shared<TestUrlProvider>(std::vector<std::string>{url});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);

    const auto temp = fixture.sandbox / "temp" / (fixture.bundle_guid.ToHex() + ".part");
    std::filesystem::create_directories(temp.parent_path());
    const std::size_t prefix_size = fixture.bundle_bytes.size() / 2;
    WriteFile(temp, std::span<const std::byte>(fixture.bundle_bytes.data(), prefix_size));

    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    INFO(downloader.Value()->LastError().ToString());
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);

    bool saw_range = false;
    for (const auto& request : transport->Requests()) {
        if (request.range_offset && *request.range_offset == prefix_size) {
            saw_range = true;
        }
    }
    REQUIRE(saw_range);
    REQUIRE(ReadFile(fixture.sandbox / "bundles" / fixture.bundle_file) == fixture.bundle_bytes);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("on-demand load shares an in-flight explicit download", "[remote]") {
    PackageFixture fixture;
    const std::string url = fixture.BundleUrl("https://cdn-a.test");
    auto transport = std::make_shared<MockTransport>();
    transport->Set(url, fixture.bundle_bytes);
    transport->SetDelay(url, 100);
    auto provider = std::make_shared<TestUrlProvider>(std::vector<std::string>{url});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto handle = package.Value()->LoadBundleFile(fixture.bundle_name);
    INFO(handle.LastError().ToString());
    REQUIRE(handle.Status() == core::OperationStatus::Succeeded);
    handle.Release();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(transport->Requests().size() == 1);
    REQUIRE(ReadFile(fixture.sandbox / "bundles" / fixture.bundle_file) == fixture.bundle_bytes);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote downloaders serialize shared bundle cache writes", "[remote][stress]") {
    PackageFixture fixture;
    const std::string url = fixture.BundleUrl("https://cdn-a.test");
    auto transport = std::make_shared<MockTransport>();
    transport->Set(url, fixture.bundle_bytes);
    transport->SetDelay(url, 50);
    auto provider_a = std::make_shared<TestUrlProvider>(std::vector<std::string>{url});
    auto provider_b = std::make_shared<TestUrlProvider>(std::vector<std::string>{url});

    runtime::AssetSystem first_system;
    runtime::AssetSystem second_system;
    REQUIRE(first_system.Initialize({}));
    REQUIRE(second_system.Initialize({}));
    auto first_package = first_system.CreatePackage(fixture.Options(transport, provider_a));
    auto second_package = second_system.CreatePackage(fixture.Options(transport, provider_b));
    REQUIRE(first_package);
    REQUIRE(second_package);

    auto first = first_package.Value()->CreateDownloader();
    auto second = second_package.Value()->CreateDownloader();
    REQUIRE(first);
    REQUIRE(second);
    first.Value()->Start();
    second.Value()->Start();
    REQUIRE(WaitDownloader(first.Value()));
    REQUIRE(WaitDownloader(second.Value()));
    INFO(first.Value()->LastError().ToString());
    INFO(second.Value()->LastError().ToString());
    REQUIRE(first.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(second.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(transport->Requests().size() == 1);
    REQUIRE(ReadFile(fixture.sandbox / "bundles" / fixture.bundle_file) == fixture.bundle_bytes);
    REQUIRE_FALSE(std::filesystem::exists(fixture.sandbox / "temp" /
                                          (fixture.bundle_guid.ToHex() + ".part")));

    REQUIRE(first_system.DestroyPackage("game"));
    REQUIRE(second_system.DestroyPackage("game"));
    first_system.Shutdown();
    second_system.Shutdown();
}

TEST_CASE("remote downloader discards an oversized partial before requesting", "[remote]") {
    PackageFixture fixture;
    const std::string url = fixture.BundleUrl("https://cdn-a.test");
    auto transport = std::make_shared<MockTransport>();
    transport->Set(url, fixture.bundle_bytes);
    auto provider = std::make_shared<TestUrlProvider>(std::vector<std::string>{url});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);

    const auto temp = fixture.sandbox / "temp" / (fixture.bundle_guid.ToHex() + ".part");
    std::filesystem::create_directories(temp.parent_path());
    auto oversized = fixture.bundle_bytes;
    oversized.insert(oversized.end(), 4, std::byte{0x7f});
    WriteFile(temp, oversized);

    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(ReadFile(fixture.sandbox / "bundles" / fixture.bundle_file) == fixture.bundle_bytes);
    REQUIRE_FALSE(std::filesystem::exists(temp));
    for (const auto& request : transport->Requests()) {
        REQUIRE_FALSE(request.range_offset.has_value());
    }

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote downloader keeps a resumable partial after failure", "[remote]") {
    PackageFixture fixture;
    const std::string url = fixture.BundleUrl("https://cdn-a.test");
    auto transport = std::make_shared<MockTransport>();
    transport->SetStatus(url, 500);
    auto provider = std::make_shared<TestUrlProvider>(std::vector<std::string>{url});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto options = fixture.Options(transport, provider);
    options.download_options.max_retries = 1;
    auto package = system.CreatePackage(options);
    REQUIRE(package);

    const auto temp = fixture.sandbox / "temp" / (fixture.bundle_guid.ToHex() + ".part");
    std::filesystem::create_directories(temp.parent_path());
    const std::size_t prefix_size = fixture.bundle_bytes.size() / 2;
    WriteFile(temp, std::span<const std::byte>(fixture.bundle_bytes.data(), prefix_size));

    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Failed);
    REQUIRE(std::filesystem::exists(temp));

    transport->Set(url, fixture.bundle_bytes);
    downloader.Value()->RetryFailed();
    REQUIRE(WaitDownloader(downloader.Value()));
    INFO(downloader.Value()->LastError().ToString());
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(ReadFile(fixture.sandbox / "bundles" / fixture.bundle_file) == fixture.bundle_bytes);
    REQUIRE_FALSE(std::filesystem::exists(temp));

    bool saw_range = false;
    for (const auto& request : transport->Requests()) {
        if (request.range_offset && *request.range_offset == prefix_size) {
            saw_range = true;
        }
    }
    REQUIRE(saw_range);
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("sandbox cache ignores and clears orphan partial downloads", "[remote]") {
    PackageFixture fixture;
    auto transport = std::make_shared<MockTransport>();
    auto provider = std::make_shared<TestUrlProvider>(std::vector<std::string>{});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);

    const auto temp = fixture.sandbox / "temp" / (fixture.bundle_guid.ToHex() + ".part");
    std::filesystem::create_directories(temp.parent_path());
    WriteFile(temp, std::span<const std::byte>(fixture.bundle_bytes.data(), 4));

    auto stats = package.Value()->QueryCache();
    REQUIRE(stats);
    REQUIRE(stats.Value().files == 0);
    REQUIRE(stats.Value().bytes == 0);
    REQUIRE(package.Value()->ClearCache(runtime::CacheClearOptions{}));
    REQUIRE_FALSE(std::filesystem::exists(temp));

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote downloader falls back across CDN candidates and refreshes auth", "[remote]") {
    PackageFixture fixture;
    const std::string bad = fixture.BundleUrl("https://cdn-bad.test");
    const std::string good = fixture.BundleUrl("https://cdn-good.test");
    auto transport = std::make_shared<MockTransport>();
    transport->SetStatus(bad, 500);
    transport->Set(good, fixture.bundle_bytes);
    transport->SetUnauthorizedOnce(good, 1);
    auto provider = std::make_shared<TestUrlProvider>(std::vector<std::string>{bad, good});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    INFO(downloader.Value()->LastError().ToString());
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(provider->UnauthorizedCalls() == 1);
    REQUIRE(std::filesystem::exists(fixture.sandbox / "bundles" / fixture.bundle_file));

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote downloader pauses, resumes and cancels predictably", "[remote]") {
    PackageFixture fixture;
    const std::string url = fixture.BundleUrl("https://cdn-a.test");
    auto transport = std::make_shared<MockTransport>();
    transport->Set(url, fixture.bundle_bytes);
    transport->SetDelay(url, 30);
    auto provider = std::make_shared<TestUrlProvider>(std::vector<std::string>{url});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);

    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    downloader.Value()->Pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    downloader.Value()->Resume();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);

    const auto installed = fixture.sandbox / "bundles" / fixture.bundle_file;
    std::filesystem::remove(installed);
    std::filesystem::remove(installed.string() + ".cache");

    transport->SetDelay(url, 200);
    auto second = package.Value()->CreateDownloader();
    REQUIRE(second);
    second.Value()->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    second.Value()->Cancel();
    REQUIRE(WaitDownloader(second.Value()));
    REQUIRE(second.Value()->Status() == core::OperationStatus::Canceled);
    REQUIRE_FALSE(std::filesystem::exists(fixture.sandbox / "temp" /
                                          (fixture.bundle_guid.ToHex() + ".part")));

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime on-demand download fills a missing bundle", "[remote]") {
    PackageFixture fixture;
    auto transport = std::make_shared<MockTransport>();
    transport->Set(fixture.BundleUrl("https://cdn-a.test"), fixture.bundle_bytes);
    auto provider = std::make_shared<TestUrlProvider>(
        std::vector<std::string>{fixture.BundleUrl("https://cdn-a.test")});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);

    auto bundle = package.Value()->LoadBundleFile(fixture.bundle_name);
    INFO(bundle.LastError().ToString());
    REQUIRE(bundle.Status() == core::OperationStatus::Succeeded);
    REQUIRE(bundle.Data().size() == fixture.bundle_bytes.size());
    REQUIRE(std::filesystem::exists(fixture.sandbox / "bundles" / fixture.bundle_file));

    bundle.Release();
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("runtime on-demand download repairs a corrupted cache", "[remote]") {
    PackageFixture fixture;
    auto transport = std::make_shared<MockTransport>();
    transport->Set(fixture.BundleUrl("https://cdn-a.test"), fixture.bundle_bytes);
    auto provider = std::make_shared<TestUrlProvider>(
        std::vector<std::string>{fixture.BundleUrl("https://cdn-a.test")});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);

    auto first = package.Value()->LoadBundleFile(fixture.bundle_name);
    REQUIRE(first.Status() == core::OperationStatus::Succeeded);
    first.Release();

    const auto installed = fixture.sandbox / "bundles" / fixture.bundle_file;
    auto corrupted = ReadFile(installed);
    corrupted[0] ^= std::byte{0xff};
    WriteFile(installed, corrupted);

    auto repaired = package.Value()->LoadBundleFile(fixture.bundle_name);
    INFO(repaired.LastError().ToString());
    REQUIRE(repaired.Status() == core::OperationStatus::Succeeded);
    REQUIRE(ReadFile(installed) == fixture.bundle_bytes);

    repaired.Release();
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote downloader watchdog aborts a stalled transfer", "[remote]") {
    PackageFixture fixture;
    auto transport = std::make_shared<WatchdogTransport>(fixture.bundle_bytes, false);
    auto provider = std::make_shared<TestUrlProvider>(
        std::vector<std::string>{fixture.BundleUrl("https://cdn-a.test")});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto options = fixture.Options(transport, provider);
    options.download_options.watchdog_timeout = std::chrono::milliseconds(30);
    options.download_options.max_retries = 0;
    auto package = system.CreatePackage(options);
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value(), std::chrono::seconds(2)));
    INFO(downloader.Value()->LastError().ToString());
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Failed);
    REQUIRE(downloader.Value()->LastError().code == core::ErrorCode::Timeout);
    REQUIRE(transport->Requests() == 1);
    REQUIRE_FALSE(std::filesystem::exists(fixture.sandbox / "bundles" / fixture.bundle_file));

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote downloader watchdog is reset by received bytes", "[remote]") {
    PackageFixture fixture;
    auto transport = std::make_shared<WatchdogTransport>(fixture.bundle_bytes, true);
    auto provider = std::make_shared<TestUrlProvider>(
        std::vector<std::string>{fixture.BundleUrl("https://cdn-a.test")});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto options = fixture.Options(transport, provider);
    options.download_options.watchdog_timeout = std::chrono::milliseconds(30);
    options.download_options.max_retries = 0;
    auto package = system.CreatePackage(options);
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value(), std::chrono::seconds(2)));
    INFO(downloader.Value()->LastError().ToString());
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(transport->Requests() == 1);
    REQUIRE(ReadFile(fixture.sandbox / "bundles" / fixture.bundle_file) == fixture.bundle_bytes);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote downloader limits request starts per scheduler frame", "[remote]") {
    PackageFixture fixture;
    std::vector<PackageFixture::ExtraBundle> extras;
    for (int i = 0; i < 4; ++i) {
        extras.push_back(fixture.AddBundle("extra-" + std::to_string(i)));
    }

    auto transport = std::make_shared<MockTransport>();
    std::map<std::string, std::string> urls;
    const std::string base = "https://cdn-a.test";
    const std::string main_url = base + "/" + fixture.bundle_file;
    transport->Set(main_url, fixture.bundle_bytes);
    urls[fixture.bundle_file] = main_url;
    for (const auto& extra : extras) {
        const std::string url = base + "/" + extra.file_name;
        transport->Set(url, extra.bytes);
        urls[extra.file_name] = url;
    }
    auto provider = std::make_shared<MappedUrlProvider>(std::move(urls));

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto options = fixture.Options(transport, provider);
    options.download_options.max_concurrency = 4;
    options.download_options.max_requests_per_frame = 2;
    options.download_options.scheduler_frame_interval = std::chrono::milliseconds(1000);
    auto package = system.CreatePackage(options);
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    REQUIRE(downloader.Value()->Summary().total_count == 5);

    downloader.Value()->Start();
    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (transport->Requests().empty() && std::chrono::steady_clock::now() < first_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(transport->Requests().empty());
    REQUIRE(transport->Requests().size() <= 2);

    REQUIRE(WaitDownloader(downloader.Value(), std::chrono::seconds(5)));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(transport->Requests().size() == 5);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote downloader RetryFailed skips succeeded bundles", "[remote]") {
    PackageFixture fixture;
    const auto side = fixture.AddBundle("side");
    auto transport = std::make_shared<MockTransport>();
    std::map<std::string, std::string> urls;
    const std::string base = "https://cdn-a.test";
    const std::string main_url = base + "/" + fixture.bundle_file;
    const std::string side_url = base + "/" + side.file_name;
    transport->Set(main_url, fixture.bundle_bytes);
    transport->SetStatus(side_url, 500);
    urls[fixture.bundle_file] = main_url;
    urls[side.file_name] = side_url;
    auto provider = std::make_shared<MappedUrlProvider>(std::move(urls));

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto options = fixture.Options(transport, provider);
    options.download_options.max_retries = 0;
    auto package = system.CreatePackage(options);
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    REQUIRE(downloader.Value()->Summary().total_count == 2);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Failed);

    const auto main_installed = fixture.sandbox / "bundles" / fixture.bundle_file;
    const auto side_installed = fixture.sandbox / "bundles" / side.file_name;
    REQUIRE(std::filesystem::exists(main_installed));
    REQUIRE_FALSE(std::filesystem::exists(side_installed));
    const auto before = transport->Requests();
    const auto main_requests = static_cast<std::size_t>(std::count_if(
        before.begin(), before.end(),
        [&main_url](const runtime::HttpRequest& request) { return request.url == main_url; }));

    transport->Set(side_url, side.bytes);
    downloader.Value()->RetryFailed();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(std::filesystem::exists(side_installed));

    const auto after = transport->Requests();
    const auto main_requests_after = static_cast<std::size_t>(std::count_if(
        after.begin(), after.end(),
        [&main_url](const runtime::HttpRequest& request) { return request.url == main_url; }));
    REQUIRE(main_requests_after == main_requests);

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote downloader reports failed files and RetryFailed recovers", "[remote]") {
    PackageFixture fixture;
    const std::string url = fixture.BundleUrl("https://cdn-a.test");
    auto transport = std::make_shared<MockTransport>();
    transport->SetStatus(url, 500);
    auto provider = std::make_shared<TestUrlProvider>(std::vector<std::string>{url});

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    auto package = system.CreatePackage(fixture.Options(transport, provider));
    REQUIRE(package);
    auto downloader = package.Value()->CreateDownloader();
    REQUIRE(downloader);
    downloader.Value()->Start();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Failed);
    REQUIRE(downloader.Value()->Failures().size() == 1);
    REQUIRE_FALSE(downloader.Value()->LastError().IsOk());

    transport->Set(url, fixture.bundle_bytes);
    downloader.Value()->RetryFailed();
    REQUIRE(WaitDownloader(downloader.Value()));
    REQUIRE(downloader.Value()->Status() == core::OperationStatus::Succeeded);
    REQUIRE(downloader.Value()->Failures().empty());

    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();
}

TEST_CASE("remote config parsing and gray decisions are deterministic", "[remote]") {
    const std::string json = R"json({
        "schema": 1,
        "package": "game",
        "channel": "prod",
        "version": "1.0.0",
        "manifest_hash": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "cdn": ["https://cdn-a.test/game", "https://cdn-b.test/game"],
        "sign_url": "https://api.test/api/v1/client/url",
        "gray": {
            "salt": "game-v1",
            "rules": [
                {
                    "version": "1.1.0",
                    "percentage": 10,
                    "whitelist": ["player-001"],
                    "platforms": ["windows"],
                    "min_app_version": "1.0.0",
                    "enabled": true
                }
            ]
        },
        "rollback": {"previous_version": "0.9.0"}
    })json";
    auto parsed = remote::ParseRemoteConfig(json);
    REQUIRE(parsed);
    REQUIRE(parsed.Value().package == "game");
    REQUIRE(parsed.Value().cdn.size() == 2);
    REQUIRE(parsed.Value().sign_url == "https://api.test/api/v1/client/url");
    REQUIRE(parsed.Value().gray.rules.size() == 1);
    REQUIRE(parsed.Value().manifest_hash ==
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    remote::GrayVersionResolver resolver(parsed.Value());
    remote::VersionContext context;
    context.package = "game";
    context.platform = "windows";
    context.app_version = "1.0.0";
    context.current_version = "1.0.0";
    context.player_id = "player-001";
    auto by_whitelist = resolver.Resolve(context);
    REQUIRE(by_whitelist);
    REQUIRE(by_whitelist.Value().version == "1.1.0");
    REQUIRE(by_whitelist.Value().reason == "gray");

    context.player_id = "player-002";
    auto by_config = resolver.Resolve(context);
    REQUIRE(by_config);
    REQUIRE(by_config.Value().version == "1.0.0");
    REQUIRE(by_config.Value().reason == "config");

    context.service_version = "1.2.0";
    auto by_service = resolver.Resolve(context);
    REQUIRE(by_service);
    REQUIRE(by_service.Value().version == "1.2.0");
    REQUIRE(by_service.Value().reason == "server");

    context.force_rollback = true;
    auto by_rollback = resolver.Resolve(context);
    REQUIRE(by_rollback);
    REQUIRE(by_rollback.Value().version == "0.9.0");
    REQUIRE(by_rollback.Value().rollback);

    context.force_rollback = false;
    context.service_version.clear();
    context.offline = true;
    context.local_version = "0.8.0";
    auto by_offline = resolver.Resolve(context);
    REQUIRE(by_offline);
    REQUIRE(by_offline.Value().version == "0.8.0");
    REQUIRE(by_offline.Value().reason == "offline");

    REQUIRE(remote::GrayVersionResolver::CompareVersions("1.2.0", "1.10.0") < 0);
    REQUIRE(remote::GrayVersionResolver::PercentageBucket("player", "game", "salt") ==
            remote::GrayVersionResolver::PercentageBucket("player", "game", "salt"));
}

TEST_CASE("remote manifest service caches, verifies and repairs manifests", "[remote]") {
    PackageFixture fixture;
    TempDir cache_root{"remote_manifest_cache"};
    remote::ManifestCache cache(ToUtf8(cache_root.Path()), "game");
    auto transport = std::make_shared<MockTransport>();
    const std::string base = "https://cdn.test/game";
    transport->Set(base + "/game.version", Bytes("1.0.0\n"));
    transport->Set(base + "/game_1.0.0.hash",
                   Bytes("sha256:" + core::Sha256(std::span<const std::byte>(fixture.manifest_bytes)
                                                      .subspan(serialize::kManifestHeaderSize))
                                         .ToHex()));
    transport->Set(base + "/game_1.0.0.bytes", fixture.manifest_bytes);

    auto urls = std::make_shared<remote::CdnUrlProvider>(std::vector<std::string>{base});
    auto adapter = std::make_shared<remote::RemoteDownloadProvider>(urls, nullptr);
    remote::RemoteManifestService service("game", cache, transport, adapter);
    remote::RemoteRequestContext context;
    context.package = "game";

    auto version = service.RequestVersion(context);
    REQUIRE(version);
    REQUIRE(version.Value() == "1.0.0");
    REQUIRE(cache.LoadVersion().Value() == "1.0.0");

    auto offline_transport = std::make_shared<MockTransport>();
    offline_transport->SetStatus(base + "/game.version", 404);
    remote::RemoteManifestService offline_service("game", cache, offline_transport, adapter);
    auto cached_version = offline_service.RequestVersion(context);
    REQUIRE(cached_version);
    REQUIRE(cached_version.Value() == "1.0.0");

    auto manifest = service.LoadManifest(version.Value(), context);
    REQUIRE(manifest);
    REQUIRE(manifest.Value()->PackageVersion() == "1.0.0");
    REQUIRE(service.ActiveManifest() != nullptr);
    REQUIRE(service.ActiveVersion() == "1.0.0");
    REQUIRE(std::filesystem::exists(cache_root.Path() / "game_1.0.0.bytes"));
    REQUIRE(std::filesystem::exists(cache_root.Path() / "game_1.0.0.hash"));

    const auto bytes_path = cache_root.Path() / "game_1.0.0.bytes";
    auto corrupted = ReadFile(bytes_path);
    corrupted[serialize::kManifestHeaderSize + 16] ^= std::byte{0xff};
    WriteFile(bytes_path, corrupted);
    auto repaired = service.LoadManifest("1.0.0", context);
    REQUIRE(repaired);
    REQUIRE(ReadFile(bytes_path) == fixture.manifest_bytes);
    REQUIRE(service.ActiveVersion() == "1.0.0");

    auto second_desc = fixture.manifest;
    second_desc.package_version = "2.0.0";
    auto second_bytes = serialize::WriteManifest(second_desc);
    REQUIRE(second_bytes);
    transport->Set(base + "/game_2.0.0.hash",
                   Bytes(core::Sha256(std::span<const std::byte>(second_bytes.Value())
                                          .subspan(serialize::kManifestHeaderSize))
                             .ToHex()));
    transport->Set(base + "/game_2.0.0.bytes", second_bytes.Value());
    auto old_manifest = service.ActiveManifest();
    auto second_manifest = service.LoadManifest("2.0.0", context);
    REQUIRE(second_manifest);
    REQUIRE(second_manifest.Value()->PackageVersion() == "2.0.0");
    REQUIRE(old_manifest->PackageVersion() == "1.0.0");
    REQUIRE(service.ActiveManifest()->PackageVersion() == "2.0.0");

    runtime::AssetSystem system;
    REQUIRE(system.Initialize({}));
    runtime::ResourcePackageOptions package_options;
    package_options.package_name = "game";
    package_options.builtin_root = ToUtf8(fixture.builtin);
    auto package = system.CreatePackage(package_options);
    REQUIRE(package);
    REQUIRE(package.Value()->LoadManifest(second_manifest.Value()));
    REQUIRE(package.Value()->Version() == "2.0.0");
    REQUIRE(system.DestroyPackage("game"));
    system.Shutdown();

    auto versions = cache.ListVersions();
    REQUIRE(versions);
    REQUIRE(versions.Value().size() == 2);
    REQUIRE(cache.ClearOldVersions(std::vector<std::string>{"2.0.0"}));
    REQUIRE(cache.ListVersions().Value().size() == 1);
    REQUIRE(cache.ListVersions().Value().front() == "2.0.0");
    REQUIRE(cache.ClearOldVersions(std::vector<std::string>{}));
    REQUIRE(cache.ListVersions().Value().empty());
}

TEST_CASE("remote manifest service refreshes authentication and joins safe URLs", "[remote]") {
    PackageFixture fixture;
    TempDir cache_root{"remote_manifest_auth"};
    remote::ManifestCache cache(ToUtf8(cache_root.Path()), "game");
    auto transport = std::make_shared<MockTransport>();
    const std::string base = "https://cdn.test/game";
    transport->Set(base + "/game.version", Bytes("1.0.0"));
    transport->Set(base + "/game_1.0.0.hash",
                   Bytes(core::Sha256(std::span<const std::byte>(fixture.manifest_bytes)
                                          .subspan(serialize::kManifestHeaderSize))
                             .ToHex()));
    transport->Set(base + "/game_1.0.0.bytes", fixture.manifest_bytes);
    transport->SetRequiredToken(base + "/game_1.0.0.hash", "token-2");
    transport->SetRequiredToken(base + "/game_1.0.0.bytes", "token-2");

    auto urls = std::make_shared<remote::CdnUrlProvider>(std::vector<std::string>{base});
    auto token_calls = std::make_shared<int>(0);
    auto token = std::make_shared<remote::TokenAuthProvider>("Authorization", [token_calls] {
        return parcel::core::Result<std::string>("token-" + std::to_string(++*token_calls));
    });
    auto adapter = std::make_shared<remote::RemoteDownloadProvider>(urls, token);
    remote::RemoteManifestService service("game", cache, transport, adapter);
    remote::RemoteRequestContext context;
    context.package = "game";

    auto loaded = service.LoadManifest("1.0.0", context);
    REQUIRE(loaded);
    REQUIRE(service.ActiveVersion() == "1.0.0");
    REQUIRE(std::filesystem::exists(cache_root.Path() / "game_1.0.0.bytes"));

    auto joined = remote::JoinUrl("https://cdn.test/game/", "bundle.bundle");
    REQUIRE(joined);
    REQUIRE(joined.Value() == "https://cdn.test/game/bundle.bundle");
    REQUIRE_FALSE(remote::JoinUrl("https://cdn.test/game", "../bundle.bundle"));
    REQUIRE_FALSE(remote::JoinUrl("", "bundle.bundle"));
}

TEST_CASE("remote config rejects malformed values", "[remote]") {
    REQUIRE_FALSE(remote::ParseRemoteConfig("{}"));
    REQUIRE_FALSE(remote::ParseRemoteConfig(R"({"schema":2,"package":"game"})"));
    REQUIRE_FALSE(remote::ParseRemoteConfig(
        R"({"schema":1,"package":"game","gray":{"rules":[{"percentage":101}]}})"));
    REQUIRE(remote::NormalizeManifestHash(
                "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")
                .Value() == "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    REQUIRE_FALSE(remote::NormalizeManifestHash("sha256:not-a-hash"));
}












