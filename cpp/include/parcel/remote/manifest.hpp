#pragma once

#include "parcel/core/result.hpp"
#include "parcel/remote/remote.hpp"
#include "parcel/runtime/download.hpp"
#include "parcel/serialize/manifest.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::remote {

using ManifestPtr = std::shared_ptr<const serialize::ManifestView>;

class ManifestCache {
  public:
    ManifestCache(std::string root, std::string package);

    [[nodiscard]] const std::string& Root() const noexcept { return root_; }
    [[nodiscard]] const std::string& Package() const noexcept { return package_; }

    [[nodiscard]] core::Result<ManifestPtr> Load(std::string_view version,
                                                 std::string_view expected_hash = {}) const;
    [[nodiscard]] core::Result<std::vector<std::byte>> LoadBytes(std::string_view version) const;
    [[nodiscard]] core::Result<std::string> LoadHash(std::string_view version) const;
    [[nodiscard]] core::Result<std::string> LoadVersion() const;
    [[nodiscard]] core::Result<void> SaveVersion(std::string_view version) const;
    [[nodiscard]] core::Result<void> Save(std::string_view version, std::string_view hash,
                                          std::span<const std::byte> bytes) const;
    [[nodiscard]] core::Result<void> Remove(std::string_view version) const;
    [[nodiscard]] core::Result<std::vector<std::string>> ListVersions() const;
    [[nodiscard]] core::Result<void>
    ClearOldVersions(std::span<const std::string> keep_versions) const;

  private:
    [[nodiscard]] core::Result<std::string> BytesPath(std::string_view version) const;
    [[nodiscard]] core::Result<std::string> HashPath(std::string_view version) const;
    [[nodiscard]] core::Result<std::string> VersionPath() const;

    std::string root_;
    std::string package_;
};

struct RemoteManifestOptions {
    std::chrono::milliseconds timeout{30000};
    std::chrono::milliseconds retry_delay{100};
    int max_retries = 2;
    std::size_t max_manifest_bytes = 64 * 1024 * 1024;
};

class RemoteManifestService {
  public:
    RemoteManifestService(std::string package, ManifestCache cache,
                          std::shared_ptr<runtime::IHttpTransport> transport,
                          std::shared_ptr<runtime::IDownloadUrlProvider> url_provider,
                          RemoteManifestOptions options = {});

    [[nodiscard]] core::Result<std::string> RequestVersion(const RemoteRequestContext& context);
    [[nodiscard]] core::Result<std::string>
    RequestManifestHash(std::string_view version, const RemoteRequestContext& context);
    [[nodiscard]] core::Result<ManifestPtr> LoadManifest(std::string_view version,
                                                         const RemoteRequestContext& context);
    [[nodiscard]] core::Result<ManifestPtr> LoadManifest(std::string_view version,
                                                         std::string_view expected_hash,
                                                         const RemoteRequestContext& context);

    [[nodiscard]] ManifestPtr ActiveManifest() const;
    [[nodiscard]] std::string ActiveVersion() const;
    [[nodiscard]] ManifestCache& Cache() noexcept { return cache_; }

  private:
    [[nodiscard]] core::Result<std::vector<std::byte>>
    FetchBytes(std::string_view file_name, std::string_view version,
               const RemoteRequestContext& context);
    [[nodiscard]] core::Result<std::string> FetchText(std::string_view file_name,
                                                      std::string_view version,
                                                      const RemoteRequestContext& context);
    void Activate(ManifestPtr manifest, std::string version);

    std::string package_;
    ManifestCache cache_;
    std::shared_ptr<runtime::IHttpTransport> transport_;
    std::shared_ptr<runtime::IDownloadUrlProvider> url_provider_;
    RemoteManifestOptions options_;
    mutable std::mutex mutex_;
    ManifestPtr active_manifest_;
    std::string active_version_;
};

} // namespace parcel::remote
