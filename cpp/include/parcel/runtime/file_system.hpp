#pragma once

#include "parcel/core/error.hpp"
#include "parcel/core/guid.hpp"
#include "parcel/core/result.hpp"
#include "parcel/serialize/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace parcel::runtime {

using ManifestPtr = std::shared_ptr<const serialize::ManifestView>;

inline constexpr std::string_view kBundleCacheSidecarExtension = ".cache";
inline constexpr std::string_view kApplicationFootprintFileName = "ApplicationFootprint.bytes";

enum class InstallCleanupMode : std::uint8_t {
    None,
    Bundles,
    Manifests,
    All,
};

enum class CacheClearMode : std::uint8_t {
    All,
    Unused,
    ByTags,
    ByLocations,
    OldVersions,
};

struct CacheClearOptions {
    CacheClearMode mode = CacheClearMode::All;
    std::vector<std::string> file_names;
    std::vector<std::string> tags;
    std::vector<std::string> locations;
    std::vector<std::string> keep_file_names;
};

struct CacheStats {
    std::uint64_t bytes = 0;
    std::size_t files = 0;
};

enum class CacheVerifyLevel : std::uint8_t {
    Metadata,
    Size,
    Content,
};

enum class CacheVerifyStatus : std::uint8_t {
    Unsupported,
    Missing,
    Valid,
    Corrupt,
};

struct FileSystemOptions {
    std::string package_name;
    std::string root;
    std::string app_build_id;
    InstallCleanupMode install_cleanup_mode = InstallCleanupMode::None;
    bool read_only = false;
    bool verify = true;
    bool write_sidecar = true;
};

class IFileSystem {
  public:
    virtual ~IFileSystem() = default;

    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
    [[nodiscard]] virtual core::Result<void> Initialize(const FileSystemOptions& options) = 0;
    [[nodiscard]] virtual core::Result<std::string> RequestVersion() = 0;
    [[nodiscard]] virtual core::Result<ManifestPtr> LoadManifest() = 0;

    [[nodiscard]] virtual bool CanAccept(const serialize::ManifestBundleView& bundle) const = 0;
    [[nodiscard]] virtual bool
    IsDownloadRequired(const serialize::ManifestBundleView& bundle) const = 0;

    [[nodiscard]] virtual core::Result<std::vector<std::byte>>
    LoadBundle(const serialize::ManifestBundleView& bundle) = 0;

    [[nodiscard]] virtual core::Result<void>
    EnsureBundle(const serialize::ManifestBundleView& bundle) {
        (void)bundle;
        return core::MakeError(core::ErrorCode::Unsupported,
                               "this file system cannot ensure bundles");
    }

    [[nodiscard]] virtual core::Result<void>
    DownloadBundle(const serialize::ManifestBundleView& bundle) {
        (void)bundle;
        return core::MakeError(core::ErrorCode::Unsupported,
                               "this file system cannot download bundles");
    }

    [[nodiscard]] virtual core::Result<void>
    InvalidateBundle(const serialize::ManifestBundleView& bundle) {
        (void)bundle;
        return core::MakeError(core::ErrorCode::Unsupported,
                               "this file system cannot invalidate bundles");
    }

    [[nodiscard]] virtual bool SupportsBundleDownload() const noexcept { return false; }

    [[nodiscard]] virtual core::Result<std::string>
    DownloadTempPath(const serialize::ManifestBundleView& bundle) {
        (void)bundle;
        return core::MakeError(core::ErrorCode::Unsupported,
                               "this file system cannot download bundles");
    }

    [[nodiscard]] virtual core::Result<void>
    CommitDownloadedBundle(const serialize::ManifestBundleView& bundle,
                           std::string_view temp_path) {
        (void)bundle;
        (void)temp_path;
        return core::MakeError(core::ErrorCode::Unsupported,
                               "this file system cannot install downloaded bundles");
    }

    [[nodiscard]] virtual core::Result<void> ClearCache(const CacheClearOptions& options) {
        (void)options;
        return core::MakeError(core::ErrorCode::Unsupported,
                               "this file system cannot clear its cache");
    }

    [[nodiscard]] virtual core::Result<CacheStats> QueryCache() const { return CacheStats{}; }

    [[nodiscard]] virtual core::Result<CacheVerifyStatus>
    VerifyCachedBundle(const serialize::ManifestBundleView& bundle,
                       CacheVerifyLevel level) const {
        (void)bundle;
        (void)level;
        return CacheVerifyStatus::Unsupported;
    }

    virtual void Shutdown() noexcept {}
};

class BuiltinFileSystem final : public IFileSystem {
  public:
    BuiltinFileSystem() = default;
    explicit BuiltinFileSystem(FileSystemOptions options);

    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] core::Result<void> Initialize(const FileSystemOptions& options) override;
    [[nodiscard]] core::Result<std::string> RequestVersion() override;
    [[nodiscard]] core::Result<ManifestPtr> LoadManifest() override;
    [[nodiscard]] bool CanAccept(const serialize::ManifestBundleView& bundle) const override;
    [[nodiscard]] bool
    IsDownloadRequired(const serialize::ManifestBundleView& bundle) const override;
    [[nodiscard]] core::Result<std::vector<std::byte>>
    LoadBundle(const serialize::ManifestBundleView& bundle) override;
    [[nodiscard]] core::Result<void>
    EnsureBundle(const serialize::ManifestBundleView& bundle) override;
    [[nodiscard]] core::Result<CacheVerifyStatus>
    VerifyCachedBundle(const serialize::ManifestBundleView& bundle,
                       CacheVerifyLevel level) const override;

    [[nodiscard]] std::string_view Root() const noexcept { return root_; }
    [[nodiscard]] bool HasCatalog() const noexcept { return catalog_loaded_; }
    [[nodiscard]] std::size_t CatalogEntryCount() const noexcept {
        return catalog_paths_.size();
    }

  private:
    [[nodiscard]] core::Result<std::string> BundlePath(std::string_view file_name) const;
    [[nodiscard]] core::Result<std::string> ManifestPath() const;
    [[nodiscard]] core::Result<void> LoadBuiltinCatalog();

    FileSystemOptions options_;
    std::string root_;
    std::unordered_map<std::string, std::string> catalog_paths_;
    std::unordered_map<core::AssetGuid, std::string, core::Guid128Hash> catalog_guid_paths_;
    bool catalog_loaded_ = false;
    bool initialized_ = false;
};

class SandboxFileSystem : public IFileSystem {
  public:
    SandboxFileSystem() = default;
    explicit SandboxFileSystem(FileSystemOptions options);
    ~SandboxFileSystem() override = default;

    [[nodiscard]] std::string_view Name() const noexcept override;
    [[nodiscard]] core::Result<void> Initialize(const FileSystemOptions& options) override;
    [[nodiscard]] core::Result<std::string> RequestVersion() override;
    [[nodiscard]] core::Result<ManifestPtr> LoadManifest() override;
    [[nodiscard]] bool CanAccept(const serialize::ManifestBundleView& bundle) const override;
    [[nodiscard]] bool
    IsDownloadRequired(const serialize::ManifestBundleView& bundle) const override;
    [[nodiscard]] core::Result<std::vector<std::byte>>
    LoadBundle(const serialize::ManifestBundleView& bundle) override;
    [[nodiscard]] core::Result<void>
    EnsureBundle(const serialize::ManifestBundleView& bundle) override;
    [[nodiscard]] core::Result<void>
    InvalidateBundle(const serialize::ManifestBundleView& bundle) override;
    [[nodiscard]] bool SupportsBundleDownload() const noexcept override { return initialized_; }
    [[nodiscard]] core::Result<std::string>
    DownloadTempPath(const serialize::ManifestBundleView& bundle) override;
    [[nodiscard]] core::Result<void>
    CommitDownloadedBundle(const serialize::ManifestBundleView& bundle,
                           std::string_view temp_path) override;
    [[nodiscard]] core::Result<void> ClearCache(const CacheClearOptions& options) override;
    [[nodiscard]] core::Result<CacheStats> QueryCache() const override;
    [[nodiscard]] core::Result<CacheVerifyStatus>
    VerifyCachedBundle(const serialize::ManifestBundleView& bundle,
                       CacheVerifyLevel level) const override;

    [[nodiscard]] std::string_view Root() const noexcept { return root_; }
    [[nodiscard]] core::Result<void>
    VerifyBundle(const serialize::ManifestBundleView& bundle) const;

  protected:
    [[nodiscard]] core::Result<std::string> BundlePath(std::string_view file_name) const;
    [[nodiscard]] core::Result<std::string> SidecarPath(std::string_view file_name) const;
    [[nodiscard]] core::Result<std::string> ManifestPath() const;
    [[nodiscard]] core::Result<void> WriteSidecar(const serialize::ManifestBundleView& bundle,
                                                  std::span<const std::byte> bytes) const;
    [[nodiscard]] core::Result<void> RemoveBundleFiles(std::string_view file_name) const;
    [[nodiscard]] core::Result<void> CheckFootprint();
    [[nodiscard]] core::Result<void> ApplyInstallCleanup();

    FileSystemOptions options_;
    std::string root_;
    std::string name_ = "sandbox";
    bool initialized_ = false;
};

class CustomFileSystem final : public SandboxFileSystem {
  public:
    CustomFileSystem();
    explicit CustomFileSystem(FileSystemOptions options, std::string name = "custom");

    [[nodiscard]] std::string_view Name() const noexcept override;

  private:
    std::string custom_name_;
};

class FileSystemHost {
  public:
    explicit FileSystemHost(std::string package_name = {});
    ~FileSystemHost() = default;

    FileSystemHost(const FileSystemHost&) = delete;
    FileSystemHost& operator=(const FileSystemHost&) = delete;

    void AddFileSystem(std::shared_ptr<IFileSystem> file_system);
    [[nodiscard]] core::Result<void> Initialize();
    void Shutdown() noexcept;

    [[nodiscard]] std::string_view PackageName() const noexcept { return package_name_; }
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] const std::vector<std::shared_ptr<IFileSystem>>& FileSystems() const noexcept {
        return file_systems_;
    }
    [[nodiscard]] IFileSystem* MainFileSystem() const noexcept;

    [[nodiscard]] core::Result<std::string> RequestVersion();
    [[nodiscard]] core::Result<ManifestPtr> LoadManifest();

    [[nodiscard]] bool CanAccept(const serialize::ManifestBundleView& bundle) const;
    [[nodiscard]] bool IsDownloadRequired(const serialize::ManifestBundleView& bundle) const;

    [[nodiscard]] core::Result<std::vector<std::byte>>
    LoadBundle(const serialize::ManifestBundleView& bundle);
    [[nodiscard]] core::Result<void> EnsureBundle(const serialize::ManifestBundleView& bundle);
    [[nodiscard]] core::Result<void> InvalidateBundle(const serialize::ManifestBundleView& bundle);
    [[nodiscard]] core::Result<void> ClearCache(const CacheClearOptions& options);
    [[nodiscard]] core::Result<CacheStats> QueryCache() const;
    [[nodiscard]] core::Result<CacheVerifyStatus>
    VerifyCachedBundle(const serialize::ManifestBundleView& bundle,
                       CacheVerifyLevel level) const;

  private:
    std::string package_name_;
    std::vector<std::shared_ptr<IFileSystem>> file_systems_;
    bool initialized_ = false;
};

} // namespace parcel::runtime


