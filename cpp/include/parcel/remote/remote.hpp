#pragma once

#include "parcel/core/result.hpp"
#include "parcel/runtime/download.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::remote {

using core::Result;

struct RemoteAuthConfig {
    std::string mode;
    std::string header = "Authorization";
    std::string token_url;
};

struct GrayRule {
    std::string version;
    double percentage = 0.0;
    std::vector<std::string> whitelist;
    std::vector<std::string> platforms;
    std::string min_app_version;
    bool enabled = true;
};

struct GrayConfig {
    std::string salt;
    std::vector<GrayRule> rules;
};

struct RollbackConfig {
    std::string previous_version;
};

struct RemoteConfig {
    int schema = 1;
    std::string package;
    std::string channel;
    std::string version;
    std::string manifest_hash;
    std::vector<std::string> cdn;
    std::string sign_url;
    RemoteAuthConfig auth;
    GrayConfig gray;
    RollbackConfig rollback;
};

[[nodiscard]] Result<RemoteConfig> ParseRemoteConfig(std::string_view json);

class IRemoteConfigProvider {
  public:
    virtual ~IRemoteConfigProvider() = default;
    [[nodiscard]] virtual Result<RemoteConfig> Load() = 0;
};

class FileRemoteConfigProvider final : public IRemoteConfigProvider {
  public:
    explicit FileRemoteConfigProvider(std::string path);
    [[nodiscard]] Result<RemoteConfig> Load() override;

  private:
    std::string path_;
};

struct RemoteRequestContext {
    std::string package;
    std::string version;
    std::string file_name;
    std::string player_id;
    std::string platform;
    std::string app_version;
    std::string channel;
    int attempt = 0;
};

class IUrlProvider {
  public:
    virtual ~IUrlProvider() = default;
    [[nodiscard]] virtual Result<std::vector<std::string>>
    GetUrls(const RemoteRequestContext& context) = 0;
    virtual void OnResult(const RemoteRequestContext& context, std::string_view url, bool success,
                          int status) {
        (void)context;
        (void)url;
        (void)success;
        (void)status;
    }

    [[nodiscard]] virtual bool ShouldRetryStatus(int status) const noexcept {
        (void)status;
        return false;
    }
};

class IAuthProvider {
  public:
    virtual ~IAuthProvider() = default;
    [[nodiscard]] virtual Result<std::vector<runtime::HttpHeader>>
    GetAuth(const RemoteRequestContext& context) = 0;
    [[nodiscard]] virtual Result<bool> OnUnauthorized(const RemoteRequestContext& context) = 0;
};

class SignedUrlProvider final : public IUrlProvider {
  public:
    SignedUrlProvider(std::string sign_url, std::vector<std::string> base_urls,
                      std::shared_ptr<runtime::IHttpTransport> transport,
                      std::shared_ptr<IAuthProvider> auth_provider = nullptr);

    [[nodiscard]] Result<std::vector<std::string>>
    GetUrls(const RemoteRequestContext& context) override;
    void OnResult(const RemoteRequestContext& context, std::string_view url, bool success,
                  int status) override;
    [[nodiscard]] bool ShouldRetryStatus(int status) const noexcept override;

  private:
    [[nodiscard]] Result<std::string> Sign(const RemoteRequestContext& context);

    std::string sign_url_;
    std::vector<std::string> base_urls_;
    std::shared_ptr<runtime::IHttpTransport> transport_;
    std::shared_ptr<IAuthProvider> auth_provider_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> signed_urls_;
    std::map<std::string, std::int64_t> expires_;
};

class CdnUrlProvider final : public IUrlProvider {
  public:
    explicit CdnUrlProvider(std::vector<std::string> base_urls);
    [[nodiscard]] Result<std::vector<std::string>>
    GetUrls(const RemoteRequestContext& context) override;
    void OnResult(const RemoteRequestContext& context, std::string_view url, bool success,
                  int status) override;

  private:
    std::vector<std::string> base_urls_;
    std::vector<int> failures_;
    mutable std::mutex mutex_;
};

[[nodiscard]] Result<std::string> JoinUrl(std::string_view base, std::string_view file_name);

class StaticAuthProvider final : public IAuthProvider {
  public:
    StaticAuthProvider(std::string header, std::string value);
    [[nodiscard]] Result<std::vector<runtime::HttpHeader>>
    GetAuth(const RemoteRequestContext& context) override;
    [[nodiscard]] Result<bool> OnUnauthorized(const RemoteRequestContext& context) override;

  private:
    std::string header_;
    std::string value_;
};

class TokenAuthProvider final : public IAuthProvider {
  public:
    using TokenFetcher = std::function<Result<std::string>()>;

    TokenAuthProvider(std::string header, TokenFetcher fetcher);
    [[nodiscard]] Result<std::vector<runtime::HttpHeader>>
    GetAuth(const RemoteRequestContext& context) override;
    [[nodiscard]] Result<bool> OnUnauthorized(const RemoteRequestContext& context) override;

  private:
    std::string header_;
    TokenFetcher fetcher_;
    std::string token_;
    mutable std::mutex mutex_;
};

class RemoteDownloadProvider final : public runtime::IDownloadUrlProvider {
  public:
    RemoteDownloadProvider(std::shared_ptr<IUrlProvider> url_provider,
                           std::shared_ptr<IAuthProvider> auth_provider = nullptr);

    [[nodiscard]] Result<std::vector<std::string>>
    GetUrls(const runtime::DownloadUrlRequest& request) override;
    [[nodiscard]] Result<std::vector<runtime::HttpHeader>>
    GetHeaders(const runtime::DownloadUrlRequest& request) override;
    [[nodiscard]] bool OnUnauthorized(const runtime::DownloadUrlRequest& request,
                                      std::string_view url) override;
    void OnResult(const runtime::DownloadUrlRequest& request, std::string_view url, bool success,
                  int status) override;
    [[nodiscard]] bool ShouldRetryStatus(int status) const noexcept override;

  private:
    [[nodiscard]] static RemoteRequestContext
    MakeContext(const runtime::DownloadUrlRequest& request);

    std::shared_ptr<IUrlProvider> url_provider_;
    std::shared_ptr<IAuthProvider> auth_provider_;
};

struct VersionContext {
    std::string package;
    std::string channel;
    std::string player_id;
    std::string platform;
    std::string app_version;
    std::string current_version;
    std::string local_version;
    std::string service_version;
    bool offline = false;
    bool force_rollback = false;
};

struct VersionDecision {
    std::string version;
    std::string manifest_hash;
    bool rollback = false;
    std::string reason;
};

class IVersionResolver {
  public:
    virtual ~IVersionResolver() = default;
    [[nodiscard]] virtual Result<VersionDecision> Resolve(const VersionContext& context) = 0;
};

class GrayVersionResolver final : public IVersionResolver {
  public:
    explicit GrayVersionResolver(RemoteConfig config);
    [[nodiscard]] Result<VersionDecision> Resolve(const VersionContext& context) override;

    [[nodiscard]] static int CompareVersions(std::string_view lhs, std::string_view rhs);
    [[nodiscard]] static std::uint32_t PercentageBucket(std::string_view player_id,
                                                        std::string_view package,
                                                        std::string_view salt) noexcept;

  private:
    RemoteConfig config_;
};

[[nodiscard]] Result<std::string> NormalizeManifestHash(std::string_view value);

} // namespace parcel::remote



