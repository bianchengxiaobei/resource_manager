#include "parcel/remote/remote.hpp"

#include "parcel/assets/json.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/core/platform_file.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace parcel::remote {
namespace {

using core::ErrorCode;
using core::MakeError;

std::string_view Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

const assets::JsonValue* Field(const assets::JsonValue& object, std::string_view name) {
    if (!object.IsObject()) {
        return nullptr;
    }
    return object.Find(name);
}

Result<std::string> OptionalString(const assets::JsonValue& object, std::string_view name) {
    const auto* value = Field(object, name);
    if (value == nullptr || value->IsNull()) {
        return std::string{};
    }
    if (!value->IsString()) {
        return MakeError(ErrorCode::Corrupt, "remote config field must be a string")
            .WithPath(std::string(name));
    }
    return value->string_value;
}

Result<bool> OptionalBool(const assets::JsonValue& object, std::string_view name, bool fallback) {
    const auto* value = Field(object, name);
    if (value == nullptr || value->IsNull()) {
        return fallback;
    }
    if (!value->IsBool()) {
        return MakeError(ErrorCode::Corrupt, "remote config field must be a boolean")
            .WithPath(std::string(name));
    }
    return value->bool_value;
}

Result<double> OptionalNumber(const assets::JsonValue& object, std::string_view name,
                              double fallback) {
    const auto* value = Field(object, name);
    if (value == nullptr || value->IsNull()) {
        return fallback;
    }
    if (!value->IsNumber()) {
        return MakeError(ErrorCode::Corrupt, "remote config field must be a number")
            .WithPath(std::string(name));
    }
    return value->number_value;
}

Result<std::vector<std::string>> OptionalStringArray(const assets::JsonValue& object,
                                                     std::string_view name) {
    std::vector<std::string> result;
    const auto* value = Field(object, name);
    if (value == nullptr || value->IsNull()) {
        return result;
    }
    if (!value->IsArray()) {
        return MakeError(ErrorCode::Corrupt, "remote config field must be an array")
            .WithPath(std::string(name));
    }
    result.reserve(value->array_value.size());
    for (const auto& item : value->array_value) {
        if (!item.IsString()) {
            return MakeError(ErrorCode::Corrupt, "remote config array entries must be strings")
                .WithPath(std::string(name));
        }
        result.push_back(item.string_value);
    }
    return result;
}

bool ContainsString(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

Result<std::string> NormalizeManifestHash(std::string_view value) {
    value = Trim(value);
    constexpr std::string_view kPrefix = "sha256:";
    if (value.size() >= kPrefix.size() && value.substr(0, kPrefix.size()) == kPrefix) {
        value.remove_prefix(kPrefix.size());
    }
    if (value.empty()) {
        return std::string{};
    }
    auto hash = core::Hash256::FromHex(value);
    if (!hash) {
        return MakeError(ErrorCode::InvalidArgument, "manifest hash is not valid SHA-256")
            .WithPath(std::string(value));
    }
    return hash.Value().ToHex();
}

Result<RemoteConfig> ParseRemoteConfig(std::string_view json) {
    auto parsed = assets::ParseJson(json);
    if (!parsed) {
        return std::move(parsed).TakeError();
    }
    const auto& root = parsed.Value();
    if (!root.IsObject()) {
        return MakeError(ErrorCode::Corrupt, "remote config must be a JSON object");
    }

    RemoteConfig config;
    auto schema = OptionalNumber(root, "schema", 1.0);
    if (!schema) {
        return std::move(schema).TakeError();
    }
    if (schema.Value() < 0 || schema.Value() > 1.0 ||
        schema.Value() != static_cast<double>(static_cast<int>(schema.Value()))) {
        return MakeError(ErrorCode::Unsupported, "unsupported remote config schema");
    }
    config.schema = static_cast<int>(schema.Value());
    if (config.schema != 1) {
        return MakeError(ErrorCode::Unsupported, "unsupported remote config schema");
    }

    PARCEL_TRY_ASSIGN(config.package, OptionalString(root, "package"));
    PARCEL_TRY_ASSIGN(config.channel, OptionalString(root, "channel"));
    PARCEL_TRY_ASSIGN(config.version, OptionalString(root, "version"));
    PARCEL_TRY_ASSIGN(config.manifest_hash, OptionalString(root, "manifest_hash"));
    if (!config.manifest_hash.empty()) {
        PARCEL_TRY_ASSIGN(config.manifest_hash, NormalizeManifestHash(config.manifest_hash));
    }
    if (config.package.empty()) {
        return MakeError(ErrorCode::Corrupt, "remote config package is empty");
    }

    PARCEL_TRY_ASSIGN(config.cdn, OptionalStringArray(root, "cdn"));
    PARCEL_TRY_ASSIGN(config.sign_url, OptionalString(root, "sign_url"));

    if (const auto* auth = Field(root, "auth"); auth != nullptr && !auth->IsNull()) {
        if (!auth->IsObject()) {
            return MakeError(ErrorCode::Corrupt, "remote config auth must be an object");
        }
        PARCEL_TRY_ASSIGN(config.auth.mode, OptionalString(*auth, "mode"));
        PARCEL_TRY_ASSIGN(config.auth.header, OptionalString(*auth, "header"));
        PARCEL_TRY_ASSIGN(config.auth.token_url, OptionalString(*auth, "token_url"));
    }
    if (config.auth.header.empty()) {
        config.auth.header = "Authorization";
    }

    if (const auto* gray = Field(root, "gray"); gray != nullptr && !gray->IsNull()) {
        if (!gray->IsObject()) {
            return MakeError(ErrorCode::Corrupt, "remote config gray must be an object");
        }
        PARCEL_TRY_ASSIGN(config.gray.salt, OptionalString(*gray, "salt"));
        const auto* rules = Field(*gray, "rules");
        if (rules != nullptr && !rules->IsNull()) {
            if (!rules->IsArray()) {
                return MakeError(ErrorCode::Corrupt, "remote config gray.rules must be an array");
            }
            for (const auto& item : rules->array_value) {
                if (!item.IsObject()) {
                    return MakeError(ErrorCode::Corrupt, "gray rule must be an object");
                }
                GrayRule rule;
                PARCEL_TRY_ASSIGN(rule.version, OptionalString(item, "version"));
                PARCEL_TRY_ASSIGN(rule.whitelist, OptionalStringArray(item, "whitelist"));
                PARCEL_TRY_ASSIGN(rule.platforms, OptionalStringArray(item, "platforms"));
                PARCEL_TRY_ASSIGN(rule.min_app_version, OptionalString(item, "min_app_version"));
                PARCEL_TRY_ASSIGN(rule.enabled, OptionalBool(item, "enabled", true));
                auto percentage = OptionalNumber(item, "percentage", 0.0);
                if (!percentage) {
                    return std::move(percentage).TakeError();
                }
                if (percentage.Value() < 0.0 || percentage.Value() > 100.0) {
                    return MakeError(ErrorCode::InvalidArgument,
                                     "gray rule percentage must be within [0, 100]")
                        .WithPath(rule.version);
                }
                rule.percentage = percentage.Value();
                config.gray.rules.push_back(std::move(rule));
            }
        }
    }

    if (const auto* rollback = Field(root, "rollback");
        rollback != nullptr && !rollback->IsNull()) {
        if (!rollback->IsObject()) {
            return MakeError(ErrorCode::Corrupt, "remote config rollback must be an object");
        }
        PARCEL_TRY_ASSIGN(config.rollback.previous_version,
                          OptionalString(*rollback, "previous_version"));
    }
    return config;
}

FileRemoteConfigProvider::FileRemoteConfigProvider(std::string path) : path_(std::move(path)) {}

Result<RemoteConfig> FileRemoteConfigProvider::Load() {
    if (path_.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "remote config path is empty");
    }
    core::PlatformFile platform;
    PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file, platform.OpenRead(path_));
    auto bytes = core::ReadAll(*file);
    file->Close();
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    return ParseRemoteConfig(std::string_view(reinterpret_cast<const char*>(bytes.Value().data()),
                                              bytes.Value().size()));
}

Result<std::string> JoinUrl(std::string_view base, std::string_view file_name) {
    if (base.empty() || file_name.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "URL base or file name is empty");
    }
    if (file_name.find('\\') != std::string_view::npos ||
        file_name.find("..") != std::string_view::npos || file_name.front() == '/') {
        return MakeError(ErrorCode::InvalidArgument, "download file name is not safe")
            .WithPath(std::string(file_name));
    }
    if (base.find("{file}") != std::string_view::npos) {
        std::string result(base);
        result.replace(result.find("{file}"), 6, file_name);
        return result;
    }
    std::string result(base);
    while (!result.empty() && result.back() == '/') {
        result.pop_back();
    }
    result += '/';
    result += file_name;
    return result;
}

namespace {

bool IsUnreservedUrlChar(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' ||
           value == '~';
}

std::string EscapeUrlComponent(std::string_view value, bool keep_slashes) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char c : value) {
        if (IsUnreservedUrlChar(c) || (keep_slashes && c == '/')) {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(kHex[c >> 4]);
            result.push_back(kHex[c & 0x0F]);
        }
    }
    return result;
}

Result<std::string> SignedPathFor(const RemoteRequestContext& context) {
    if (context.package.empty() || context.version.empty() || context.file_name.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "signed URL request has incomplete path");
    }
    if (context.file_name.find('\\') != std::string::npos ||
        context.file_name.find("..") != std::string::npos ||
        context.file_name.front() == '/') {
        return MakeError(ErrorCode::InvalidArgument, "signed URL file name is not safe")
            .WithPath(context.file_name);
    }
    std::string path = "/files/";
    path += EscapeUrlComponent(context.package, false);
    path += '/';
    path += EscapeUrlComponent(context.version, false);
    path += '/';
    path += EscapeUrlComponent(context.file_name, true);
    return path;
}

Result<std::string> UrlQuery(std::string_view url) {
    const std::size_t position = url.find('?');
    if (position == std::string_view::npos || position + 1 >= url.size()) {
        return MakeError(ErrorCode::Corrupt, "signed URL response has no signature query");
    }
    return std::string(url.substr(position + 1));
}

std::int64_t UnixSecondsNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

SignedUrlProvider::SignedUrlProvider(std::string sign_url, std::vector<std::string> base_urls,
                                     std::shared_ptr<runtime::IHttpTransport> transport,
                                     std::shared_ptr<IAuthProvider> auth_provider)
    : sign_url_(std::move(sign_url)), base_urls_(std::move(base_urls)),
      transport_(std::move(transport)), auth_provider_(std::move(auth_provider)) {}

Result<std::string> SignedUrlProvider::Sign(const RemoteRequestContext& context) {
    if (sign_url_.empty() || !transport_) {
        return MakeError(ErrorCode::InvalidArgument, "signed URL provider is not configured");
    }
    auto path = SignedPathFor(context);
    if (!path) {
        return std::move(path).TakeError();
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        runtime::HttpRequest request;
        request.url = sign_url_;
        request.url += (sign_url_.find('?') == std::string::npos) ? '?' : '&';
        request.url += "path=";
        request.url += EscapeUrlComponent(path.Value(), false);
        request.timeout = std::chrono::seconds(30);
        if (auth_provider_) {
            auto headers = auth_provider_->GetAuth(context);
            if (!headers) {
                return std::move(headers).TakeError();
            }
            request.headers = std::move(headers).Value();
        }
        auto response = transport_->Send(request);
        if (!response) {
            return std::move(response).TakeError();
        }
        if (response.Value().status == 401 && attempt == 0 && auth_provider_) {
            auto refreshed = auth_provider_->OnUnauthorized(context);
            if (refreshed && refreshed.Value()) {
                continue;
            }
        }
        if (!response.Value().IsSuccess()) {
            return MakeError(ErrorCode::Http, "signed URL request failed")
                .WithHttpStatus(response.Value().status)
                .WithPath(path.Value());
        }
        const auto& body = response.Value().body;
        auto parsed = assets::ParseJson(std::string_view(
            reinterpret_cast<const char*>(body.data()), body.size()));
        if (!parsed) {
            return std::move(parsed).TakeError();
        }
        const auto& root = parsed.Value();
        const assets::JsonValue* url = Field(root, "url");
        const assets::JsonValue* expires = Field(root, "expires");
        if (url == nullptr || !url->IsString() || url->string_value.empty() ||
            expires == nullptr || !expires->IsNumber()) {
            return MakeError(ErrorCode::Corrupt, "signed URL response is malformed")
                .WithPath(path.Value());
        }
        if (!std::isfinite(expires->number_value) || expires->number_value <= 0.0 ||
            expires->number_value > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return MakeError(ErrorCode::Corrupt, "signed URL expiry is invalid")
                .WithPath(path.Value());
        }
        const auto expires_at = static_cast<std::int64_t>(expires->number_value);
        if (expires_at <= UnixSecondsNow()) {
            return MakeError(ErrorCode::Corrupt, "signed URL response is already expired")
                .WithPath(path.Value());
        }
        auto query = UrlQuery(url->string_value);
        if (!query) {
            return std::move(query).TakeError();
        }
        signed_urls_[path.Value()] = url->string_value;
        expires_[path.Value()] = expires_at;
        return url->string_value;
    }
    return MakeError(ErrorCode::Http, "signed URL request was not authorized");
}

Result<std::vector<std::string>> SignedUrlProvider::GetUrls(const RemoteRequestContext& context) {
    auto path = SignedPathFor(context);
    if (!path) {
        return std::move(path).TakeError();
    }
    std::lock_guard lock(mutex_);
    const auto now = UnixSecondsNow();
    auto found = signed_urls_.find(path.Value());
    if (found == signed_urls_.end() || expires_[path.Value()] <= now + 30) {
        auto signed_url = Sign(context);
        if (!signed_url) {
            return std::move(signed_url).TakeError();
        }
        found = signed_urls_.find(path.Value());
    }
    const std::string& signed_url = found->second;
    if (base_urls_.empty()) {
        return std::vector<std::string>{signed_url};
    }
    auto query = UrlQuery(signed_url);
    if (!query) {
        return std::move(query).TakeError();
    }
    std::vector<std::string> urls;
    urls.reserve(base_urls_.size());
    for (const auto& base : base_urls_) {
        auto joined = JoinUrl(base, context.file_name);
        if (!joined) {
            return std::move(joined).TakeError();
        }
        std::string value = std::move(joined).Value();
        value += value.find('?') == std::string::npos ? '?' : '&';
        value += query.Value();
        urls.push_back(std::move(value));
    }
    return urls;
}

void SignedUrlProvider::OnResult(const RemoteRequestContext& context, std::string_view url,
                                 bool success, int status) {
    (void)url;
    if (success || (status != 401 && status != 403 && status != 410)) {
        return;
    }
    auto path = SignedPathFor(context);
    if (!path) {
        return;
    }
    std::lock_guard lock(mutex_);
    signed_urls_.erase(path.Value());
    expires_.erase(path.Value());
}

bool SignedUrlProvider::ShouldRetryStatus(int status) const noexcept {
    return status == 403 || status == 410;
}

CdnUrlProvider::CdnUrlProvider(std::vector<std::string> base_urls)
    : base_urls_(std::move(base_urls)), failures_(base_urls_.size(), 0) {}

Result<std::vector<std::string>> CdnUrlProvider::GetUrls(const RemoteRequestContext& context) {
    if (base_urls_.empty()) {
        return MakeError(ErrorCode::NotFound, "remote config has no CDN URL");
    }
    std::lock_guard lock(mutex_);
    std::vector<std::size_t> order(base_urls_.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [this](std::size_t lhs, std::size_t rhs) {
        return failures_[lhs] < failures_[rhs];
    });
    std::vector<std::string> urls;
    urls.reserve(order.size());
    for (const std::size_t index : order) {
        auto url = JoinUrl(base_urls_[index], context.file_name);
        if (!url) {
            return std::move(url).TakeError();
        }
        urls.push_back(std::move(url).Value());
    }
    return urls;
}

void CdnUrlProvider::OnResult(const RemoteRequestContext& context, std::string_view url,
                              bool success, int status) {
    (void)context;
    (void)url;
    (void)status;
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < base_urls_.size(); ++i) {
        if (url.rfind(base_urls_[i], 0) == 0) {
            if (success) {
                failures_[i] = 0;
            } else {
                ++failures_[i];
            }
            break;
        }
    }
}

StaticAuthProvider::StaticAuthProvider(std::string header, std::string value)
    : header_(std::move(header)), value_(std::move(value)) {
    if (header_.empty()) {
        header_ = "Authorization";
    }
}

Result<std::vector<runtime::HttpHeader>>
StaticAuthProvider::GetAuth(const RemoteRequestContext& context) {
    (void)context;
    return std::vector<runtime::HttpHeader>{runtime::HttpHeader{header_, value_}};
}

Result<bool> StaticAuthProvider::OnUnauthorized(const RemoteRequestContext& context) {
    (void)context;
    return false;
}

TokenAuthProvider::TokenAuthProvider(std::string header, TokenFetcher fetcher)
    : header_(std::move(header)), fetcher_(std::move(fetcher)) {
    if (header_.empty()) {
        header_ = "Authorization";
    }
}

Result<std::vector<runtime::HttpHeader>>
TokenAuthProvider::GetAuth(const RemoteRequestContext& context) {
    (void)context;
    std::lock_guard lock(mutex_);
    if (token_.empty()) {
        if (!fetcher_) {
            return MakeError(ErrorCode::InvalidArgument, "token fetcher is not configured");
        }
        auto token = fetcher_();
        if (!token) {
            return std::move(token).TakeError();
        }
        token_ = std::move(token).Value();
    }
    return std::vector<runtime::HttpHeader>{runtime::HttpHeader{header_, token_}};
}

Result<bool> TokenAuthProvider::OnUnauthorized(const RemoteRequestContext& context) {
    (void)context;
    std::lock_guard lock(mutex_);
    if (!fetcher_) {
        return false;
    }
    token_.clear();
    auto token = fetcher_();
    if (!token) {
        return std::move(token).TakeError();
    }
    token_ = std::move(token).Value();
    return true;
}

RemoteDownloadProvider::RemoteDownloadProvider(std::shared_ptr<IUrlProvider> url_provider,
                                               std::shared_ptr<IAuthProvider> auth_provider)
    : url_provider_(std::move(url_provider)), auth_provider_(std::move(auth_provider)) {}

RemoteRequestContext
RemoteDownloadProvider::MakeContext(const runtime::DownloadUrlRequest& request) {
    RemoteRequestContext context;
    context.package = request.package;
    context.version = request.version;
    context.file_name = request.file_name;
    context.attempt = request.attempt;
    return context;
}

Result<std::vector<std::string>>
RemoteDownloadProvider::GetUrls(const runtime::DownloadUrlRequest& request) {
    if (!url_provider_) {
        return MakeError(ErrorCode::InvalidArgument, "URL provider is not configured");
    }
    return url_provider_->GetUrls(MakeContext(request));
}

Result<std::vector<runtime::HttpHeader>>
RemoteDownloadProvider::GetHeaders(const runtime::DownloadUrlRequest& request) {
    if (!auth_provider_) {
        return std::vector<runtime::HttpHeader>{};
    }
    return auth_provider_->GetAuth(MakeContext(request));
}

bool RemoteDownloadProvider::OnUnauthorized(const runtime::DownloadUrlRequest& request,
                                            std::string_view url) {
    if (!auth_provider_) {
        return false;
    }
    (void)url;
    auto refreshed = auth_provider_->OnUnauthorized(MakeContext(request));
    return refreshed && refreshed.Value();
}

void RemoteDownloadProvider::OnResult(const runtime::DownloadUrlRequest& request,
                                      std::string_view url, bool success, int status) {
    if (url_provider_) {
        url_provider_->OnResult(MakeContext(request), url, success, status);
    }
}

bool RemoteDownloadProvider::ShouldRetryStatus(int status) const noexcept {
    return url_provider_ && url_provider_->ShouldRetryStatus(status);
}

int GrayVersionResolver::CompareVersions(std::string_view lhs, std::string_view rhs) {
    std::size_t lhs_pos = 0;
    std::size_t rhs_pos = 0;
    while (lhs_pos < lhs.size() || rhs_pos < rhs.size()) {
        std::uint64_t lhs_value = 0;
        std::uint64_t rhs_value = 0;
        bool lhs_numeric = false;
        bool rhs_numeric = false;
        while (lhs_pos < lhs.size() && lhs[lhs_pos] >= '0' && lhs[lhs_pos] <= '9') {
            lhs_numeric = true;
            lhs_value = lhs_value * 10 + static_cast<std::uint64_t>(lhs[lhs_pos] - '0');
            ++lhs_pos;
        }
        while (rhs_pos < rhs.size() && rhs[rhs_pos] >= '0' && rhs[rhs_pos] <= '9') {
            rhs_numeric = true;
            rhs_value = rhs_value * 10 + static_cast<std::uint64_t>(rhs[rhs_pos] - '0');
            ++rhs_pos;
        }
        if (lhs_numeric && rhs_numeric) {
            if (lhs_value != rhs_value) {
                return lhs_value < rhs_value ? -1 : 1;
            }
        } else if (lhs_numeric != rhs_numeric) {
            return lhs_numeric ? 1 : -1;
        }
        if (lhs_pos < lhs.size() && lhs[lhs_pos] == '.') {
            ++lhs_pos;
        }
        if (rhs_pos < rhs.size() && rhs[rhs_pos] == '.') {
            ++rhs_pos;
        }
        if (!lhs_numeric && !rhs_numeric && lhs_pos < lhs.size() && rhs_pos < rhs.size()) {
            if (lhs[lhs_pos] != rhs[rhs_pos]) {
                return lhs[lhs_pos] < rhs[rhs_pos] ? -1 : 1;
            }
            ++lhs_pos;
            ++rhs_pos;
        }
    }
    return 0;
}

std::uint32_t GrayVersionResolver::PercentageBucket(std::string_view player_id,
                                                    std::string_view package,
                                                    std::string_view salt) noexcept {
    std::string value;
    value.reserve(player_id.size() + package.size() + salt.size());
    value.append(player_id);
    value.append(package);
    value.append(salt);
    const auto hash = core::Sha256(value);
    const std::uint32_t bucket = (static_cast<std::uint32_t>(hash.bytes[0]) << 24U) |
                                 (static_cast<std::uint32_t>(hash.bytes[1]) << 16U) |
                                 (static_cast<std::uint32_t>(hash.bytes[2]) << 8U) |
                                 static_cast<std::uint32_t>(hash.bytes[3]);
    return bucket % 10000U;
}

GrayVersionResolver::GrayVersionResolver(RemoteConfig config) : config_(std::move(config)) {}

Result<VersionDecision> GrayVersionResolver::Resolve(const VersionContext& context) {
    if (context.offline) {
        if (context.local_version.empty()) {
            return MakeError(ErrorCode::NotFound, "offline version is not available");
        }
        return VersionDecision{context.local_version, {}, false, "offline"};
    }
    if (context.force_rollback) {
        if (config_.rollback.previous_version.empty()) {
            return MakeError(ErrorCode::NotFound, "rollback version is not configured");
        }
        return VersionDecision{config_.rollback.previous_version, {}, true, "rollback"};
    }
    if (!context.service_version.empty()) {
        const std::string hash =
            config_.version == context.service_version ? config_.manifest_hash : std::string{};
        return VersionDecision{context.service_version, hash, false, "server"};
    }

    for (const auto& rule : config_.gray.rules) {
        if (!rule.enabled || rule.version.empty()) {
            continue;
        }
        if (!rule.platforms.empty() && std::find(rule.platforms.begin(), rule.platforms.end(),
                                                 context.platform) == rule.platforms.end()) {
            continue;
        }
        if (!rule.min_app_version.empty() &&
            CompareVersions(context.app_version, rule.min_app_version) < 0) {
            continue;
        }
        bool selected = ContainsString(rule.whitelist, context.player_id);
        if (!selected && rule.percentage > 0.0) {
            const auto bucket =
                PercentageBucket(context.player_id, config_.package, config_.gray.salt);
            selected = bucket < static_cast<std::uint32_t>(rule.percentage * 100.0);
        }
        if (!selected) {
            continue;
        }
        const std::string hash =
            config_.version == rule.version ? config_.manifest_hash : std::string{};
        return VersionDecision{rule.version, hash, false, "gray"};
    }

    if (!config_.version.empty()) {
        return VersionDecision{config_.version, config_.manifest_hash, false, "config"};
    }
    if (!context.current_version.empty()) {
        return VersionDecision{context.current_version, {}, false, "current"};
    }
    return MakeError(ErrorCode::NotFound, "no remote version was resolved");
}

} // namespace parcel::remote





