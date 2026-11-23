#include "parcel/remote/manifest.hpp"

#include "parcel/core/hash.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace parcel::remote {
namespace {

using core::ErrorCode;
using core::MakeError;
using core::Result;

std::string_view Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

bool IsSafeVersion(std::string_view version) {
    if (version.empty() || version.size() > 128) {
        return false;
    }
    for (const char c : version) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '.' && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

Result<std::vector<std::byte>> ReadFile(std::string_view path) {
    core::PlatformFile platform;
    PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file, platform.OpenRead(path));
    auto bytes = core::ReadAll(*file);
    file->Close();
    return bytes;
}

Result<void> WriteFile(std::string_view path, std::span<const std::byte> bytes) {
    core::PlatformFile platform;
    const std::size_t slash = std::string_view(path).find_last_of('/');
    if (slash != std::string_view::npos) {
        PARCEL_TRY(platform.CreateDirectories(std::string_view(path).substr(0, slash)));
    }
    PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file, platform.OpenWrite(path));
    PARCEL_TRY(core::WriteAll(*file, bytes));
    PARCEL_TRY(file->Flush());
    file->Close();
    return {};
}

std::filesystem::path FsPath(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

Result<core::Hash256> ContentHash(std::span<const std::byte> bytes) {
    if (bytes.size() < serialize::kManifestHeaderSize) {
        return MakeError(ErrorCode::Corrupt, "manifest is smaller than its header");
    }
    return core::Sha256(bytes.subspan(serialize::kManifestHeaderSize));
}

bool IsRetryableStatus(int status) noexcept {
    if (status == 0 || status == 408 || status == 429) {
        return true;
    }
    return status >= 500 && status < 600;
}

} // namespace

ManifestCache::ManifestCache(std::string root, std::string package)
    : root_(core::path::Normalize(root)), package_(std::move(package)) {}

Result<std::string> ManifestCache::BytesPath(std::string_view version) const {
    if (!IsSafeVersion(version)) {
        return MakeError(ErrorCode::InvalidArgument, "manifest version is not safe")
            .WithPath(std::string(version));
    }
    if (package_.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "manifest package is empty");
    }
    return core::path::ResolveWithin(root_, package_ + "_" + std::string(version) + ".bytes");
}

Result<std::string> ManifestCache::HashPath(std::string_view version) const {
    if (!IsSafeVersion(version)) {
        return MakeError(ErrorCode::InvalidArgument, "manifest version is not safe")
            .WithPath(std::string(version));
    }
    if (package_.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "manifest package is empty");
    }
    return core::path::ResolveWithin(root_, package_ + "_" + std::string(version) + ".hash");
}

Result<std::string> ManifestCache::VersionPath() const {
    if (package_.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "manifest package is empty");
    }
    return core::path::ResolveWithin(root_, package_ + ".version");
}

Result<std::string> ManifestCache::LoadVersion() const {
    PARCEL_TRY_DECL(std::string, path, VersionPath());
    PARCEL_TRY_DECL(std::vector<std::byte>, bytes, ReadFile(path));
    const std::string version(
        Trim(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
    if (!IsSafeVersion(version)) {
        return MakeError(ErrorCode::Corrupt, "cached package version is invalid");
    }
    return version;
}

Result<void> ManifestCache::SaveVersion(std::string_view version) const {
    if (!IsSafeVersion(version)) {
        return MakeError(ErrorCode::InvalidArgument, "package version is invalid")
            .WithPath(std::string(version));
    }
    PARCEL_TRY_DECL(std::string, path, VersionPath());
    const std::string text = std::string(version) + "\n";
    return WriteFile(path, std::span<const std::byte>(
                               reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

Result<std::vector<std::byte>> ManifestCache::LoadBytes(std::string_view version) const {
    PARCEL_TRY_DECL(std::string, path, BytesPath(version));
    return ReadFile(path);
}

Result<std::string> ManifestCache::LoadHash(std::string_view version) const {
    PARCEL_TRY_DECL(std::string, path, HashPath(version));
    PARCEL_TRY_DECL(std::vector<std::byte>, bytes, ReadFile(path));
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return NormalizeManifestHash(Trim(text));
}

Result<ManifestPtr> ManifestCache::Load(std::string_view version,
                                        std::string_view expected_hash) const {
    std::string hash;
    if (expected_hash.empty()) {
        PARCEL_TRY_ASSIGN(hash, LoadHash(version));
    } else {
        PARCEL_TRY_ASSIGN(hash, NormalizeManifestHash(expected_hash));
    }
    if (hash.empty()) {
        return MakeError(ErrorCode::Corrupt, "manifest hash is empty");
    }
    auto parsed_hash = core::Hash256::FromHex(hash);
    if (!parsed_hash) {
        return std::move(parsed_hash).TakeError();
    }
    PARCEL_TRY_DECL(std::vector<std::byte>, bytes, LoadBytes(version));
    PARCEL_TRY_DECL(core::Hash256, actual, ContentHash(bytes));
    if (actual != parsed_hash.Value()) {
        return MakeError(ErrorCode::HashMismatch, "cached manifest SHA-256 does not match")
            .WithPath(std::string(version));
    }
    auto storage = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    auto opened = serialize::ManifestView::Open(storage, true);
    if (!opened) {
        return std::move(opened).TakeError();
    }
    return std::make_shared<const serialize::ManifestView>(std::move(opened).Value());
}

Result<void> ManifestCache::Save(std::string_view version, std::string_view hash,
                                 std::span<const std::byte> bytes) const {
    std::string normalized;
    PARCEL_TRY_ASSIGN(normalized, NormalizeManifestHash(hash));
    if (normalized.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "manifest hash is empty");
    }
    PARCEL_TRY_DECL(std::string, bytes_path, BytesPath(version));
    PARCEL_TRY_DECL(std::string, hash_path, HashPath(version));
    const std::string bytes_temp = bytes_path + ".tmp";
    const std::string hash_temp = hash_path + ".tmp";
    const std::string hash_text = normalized + "\n";
    const auto hash_bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(hash_text.data()), hash_text.size());
    PARCEL_TRY(WriteFile(bytes_temp, bytes));
    PARCEL_TRY(WriteFile(hash_temp, hash_bytes));
    core::PlatformFile platform;
    PARCEL_TRY(platform.Rename(bytes_temp, bytes_path, true));
    PARCEL_TRY(platform.Rename(hash_temp, hash_path, true));
    return {};
}

Result<void> ManifestCache::Remove(std::string_view version) const {
    std::string bytes_path;
    auto path = BytesPath(version);
    if (path) {
        bytes_path = path.Value();
    }
    auto hash_path = HashPath(version);
    core::PlatformFile platform;
    if (!bytes_path.empty()) {
        (void)platform.Delete(bytes_path);
        (void)platform.Delete(bytes_path + ".tmp");
    }
    if (hash_path) {
        (void)platform.Delete(hash_path.Value());
        (void)platform.Delete(hash_path.Value() + ".tmp");
    }
    return {};
}

Result<std::vector<std::string>> ManifestCache::ListVersions() const {
    std::vector<std::string> result;
    std::error_code error;
    if (!std::filesystem::exists(FsPath(root_), error)) {
        return result;
    }
    const std::string prefix = package_ + "_";
    for (const auto& entry : std::filesystem::directory_iterator(FsPath(root_), error)) {
        if (error) {
            return MakeError(ErrorCode::Io, "failed to enumerate the manifest cache")
                .WithPath(root_);
        }
        if (!entry.is_regular_file(error)) {
            error.clear();
            continue;
        }
        const std::string name = PathToUtf8(entry.path().filename());
        if (name.size() <= prefix.size() + 6 || name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - 6, 6, ".bytes") != 0) {
            continue;
        }
        std::string version = name.substr(prefix.size(), name.size() - prefix.size() - 6);
        if (IsSafeVersion(version) &&
            std::find(result.begin(), result.end(), version) == result.end()) {
            result.push_back(std::move(version));
        }
    }
    return result;
}

Result<void> ManifestCache::ClearOldVersions(std::span<const std::string> keep_versions) const {
    std::set<std::string> keep;
    for (const auto& version : keep_versions) {
        keep.insert(version);
    }
    PARCEL_TRY_DECL(std::vector<std::string>, versions, ListVersions());
    for (const auto& version : versions) {
        if (keep.find(version) == keep.end()) {
            PARCEL_TRY(Remove(version));
        }
    }
    std::error_code error;
    if (std::filesystem::exists(FsPath(root_), error)) {
        for (const auto& entry : std::filesystem::directory_iterator(FsPath(root_), error)) {
            if (error) {
                return MakeError(ErrorCode::Io, "failed to enumerate the manifest cache")
                    .WithPath(root_);
            }
            if (!entry.is_regular_file(error)) {
                error.clear();
                continue;
            }
            const std::string name = PathToUtf8(entry.path().filename());
            if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
                std::filesystem::remove(entry.path(), error);
                error.clear();
            }
        }
    }
    return {};
}

RemoteManifestService::RemoteManifestService(
    std::string package, ManifestCache cache, std::shared_ptr<runtime::IHttpTransport> transport,
    std::shared_ptr<runtime::IDownloadUrlProvider> url_provider, RemoteManifestOptions options)
    : package_(std::move(package)), cache_(std::move(cache)), transport_(std::move(transport)),
      url_provider_(std::move(url_provider)), options_(options) {}

Result<std::vector<std::byte>>
RemoteManifestService::FetchBytes(std::string_view file_name, std::string_view version,
                                  const RemoteRequestContext& context) {
    (void)context;
    if (!transport_ || !url_provider_) {
        return MakeError(ErrorCode::InvalidArgument, "remote manifest service is not configured");
    }
    bool auth_refreshed = false;
    int failures = 0;
    const int max_retries = std::max(0, options_.max_retries);
    while (true) {
        runtime::DownloadUrlRequest request;
        request.package = package_;
        request.version = std::string(version);
        request.file_name = std::string(file_name);
        request.attempt = failures;
        auto urls = url_provider_->GetUrls(request);
        if (!urls || urls.Value().empty()) {
            core::Error error = urls ? MakeError(ErrorCode::NotFound, "no remote manifest URL")
                                     : std::move(urls).TakeError();
            if (failures < max_retries) {
                ++failures;
                std::this_thread::sleep_for(options_.retry_delay);
                continue;
            }
            return error.WithPath(std::string(file_name));
        }
        const std::string& url =
            urls.Value()[static_cast<std::size_t>(failures) % urls.Value().size()];
        auto headers = url_provider_->GetHeaders(request);
        if (!headers) {
            return std::move(headers).TakeError();
        }
        runtime::HttpRequest http_request;
        http_request.url = url;
        http_request.headers = std::move(headers).Value();
        http_request.timeout = options_.timeout;
        auto response = transport_->Send(http_request);
        if (!response) {
            core::Error error = std::move(response).TakeError();
            url_provider_->OnResult(request, url, false, 0);
            if (failures < max_retries) {
                ++failures;
                std::this_thread::sleep_for(options_.retry_delay);
                continue;
            }
            return error.WithPath(std::string(file_name));
        }
        auto result = std::move(response).Value();
        if (!result.error.IsOk()) {
            url_provider_->OnResult(request, url, false, result.status);
            if (failures < max_retries) {
                ++failures;
                std::this_thread::sleep_for(options_.retry_delay);
                continue;
            }
            auto error = std::move(result.error);
            return error.WithPath(std::string(file_name)).WithHttpStatus(result.status);
        }
        if ((result.status == 401 || result.status == 403) && !auth_refreshed) {
            auth_refreshed = true;
            if (url_provider_->OnUnauthorized(request, url)) {
                continue;
            }
        }
        if (result.IsSuccess()) {
            if (result.body.size() > options_.max_manifest_bytes) {
                return MakeError(ErrorCode::InvalidArgument, "remote manifest is too large")
                    .WithPath(std::string(file_name));
            }
            url_provider_->OnResult(request, url, true, result.status);
            return std::move(result.body);
        }
        url_provider_->OnResult(request, url, false, result.status);
        core::Error error = MakeError(ErrorCode::Http, "remote manifest request failed")
                                .WithPath(std::string(file_name))
                                .WithHttpStatus(result.status);
        if (IsRetryableStatus(result.status) && failures < max_retries) {
            ++failures;
            std::this_thread::sleep_for(options_.retry_delay);
            continue;
        }
        return error;
    }
}

Result<std::string> RemoteManifestService::FetchText(std::string_view file_name,
                                                     std::string_view version,
                                                     const RemoteRequestContext& context) {
    PARCEL_TRY_DECL(std::vector<std::byte>, bytes, FetchBytes(file_name, version, context));
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

Result<std::string> RemoteManifestService::RequestVersion(const RemoteRequestContext& context) {
    auto fetched = FetchText(package_ + ".version", {}, context);
    if (fetched) {
        const auto version = std::string(Trim(fetched.Value()));
        if (!IsSafeVersion(version)) {
            return MakeError(ErrorCode::Corrupt, "remote package version is invalid")
                .WithPath(version);
        }
        PARCEL_TRY(cache_.SaveVersion(version));
        return version;
    }
    auto error = std::move(fetched).TakeError();
    auto cached = cache_.LoadVersion();
    if (cached) {
        return cached;
    }
    return error;
}

Result<std::string>
RemoteManifestService::RequestManifestHash(std::string_view version,
                                           const RemoteRequestContext& context) {
    if (!IsSafeVersion(version)) {
        return MakeError(ErrorCode::InvalidArgument, "package version is invalid")
            .WithPath(std::string(version));
    }
    PARCEL_TRY_DECL(std::string, text,
                    FetchText(package_ + "_" + std::string(version) + ".hash", version, context));
    return NormalizeManifestHash(Trim(text));
}

Result<ManifestPtr> RemoteManifestService::LoadManifest(std::string_view version,
                                                        const RemoteRequestContext& context) {
    return LoadManifest(version, {}, context);
}

Result<ManifestPtr> RemoteManifestService::LoadManifest(std::string_view version,
                                                        std::string_view expected_hash,
                                                        const RemoteRequestContext& context) {
    if (!IsSafeVersion(version)) {
        return MakeError(ErrorCode::InvalidArgument, "package version is invalid")
            .WithPath(std::string(version));
    }
    std::string hash;
    if (expected_hash.empty()) {
        auto cached_local = cache_.Load(version);
        if (cached_local) {
            Activate(cached_local.Value(), std::string(version));
            return cached_local;
        }
        PARCEL_TRY_ASSIGN(hash, RequestManifestHash(version, context));
    } else {
        PARCEL_TRY_ASSIGN(hash, NormalizeManifestHash(expected_hash));
    }
    if (hash.empty()) {
        return MakeError(ErrorCode::HashMismatch, "remote manifest hash is empty")
            .WithPath(std::string(version));
    }
    auto expected = core::Hash256::FromHex(hash);
    if (!expected) {
        return std::move(expected).TakeError();
    }

    auto cached = cache_.Load(version, hash);
    if (cached) {
        Activate(cached.Value(), std::string(version));
        return cached;
    }

    PARCEL_TRY_DECL(std::vector<std::byte>, bytes,
                    FetchBytes(package_ + "_" + std::string(version) + ".bytes", version, context));
    PARCEL_TRY_DECL(core::Hash256, actual, ContentHash(bytes));
    if (actual != expected.Value()) {
        (void)cache_.Remove(version);
        return MakeError(ErrorCode::HashMismatch, "remote manifest SHA-256 does not match")
            .WithPath(std::string(version));
    }
    auto storage = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    auto opened = serialize::ManifestView::Open(storage, true);
    if (!opened) {
        (void)cache_.Remove(version);
        return std::move(opened).TakeError();
    }
    auto manifest = std::make_shared<const serialize::ManifestView>(std::move(opened).Value());
    PARCEL_TRY(cache_.Save(version, hash, *storage));
    Activate(manifest, std::string(version));
    return manifest;
}

ManifestPtr RemoteManifestService::ActiveManifest() const {
    std::lock_guard lock(mutex_);
    return active_manifest_;
}

std::string RemoteManifestService::ActiveVersion() const {
    std::lock_guard lock(mutex_);
    return active_version_;
}

void RemoteManifestService::Activate(ManifestPtr manifest, std::string version) {
    std::lock_guard lock(mutex_);
    active_manifest_ = std::move(manifest);
    active_version_ = std::move(version);
}

} // namespace parcel::remote
