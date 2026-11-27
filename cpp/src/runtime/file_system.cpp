#include "parcel/runtime/file_system.hpp"

#include "parcel/core/hash.hpp"
#include "parcel/core/path.hpp"
#include "parcel/core/platform_file.hpp"
#include "parcel/serialize/binary.hpp"
#include "parcel/serialize/builtin_catalog.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <system_error>
#include <utility>

namespace parcel::runtime {
namespace {

using core::ErrorCode;
using core::MakeError;
using core::Result;

constexpr std::array<std::byte, 4> kSidecarMagic = {std::byte{'P'}, std::byte{'R'}, std::byte{'C'},
                                                    std::byte{'C'}};
constexpr std::uint16_t kSidecarVersion = 1;
std::atomic<std::uint64_t> g_import_sequence{0};

Result<std::vector<std::byte>> ReadFile(std::string_view path) {
    core::PlatformFile platform;
    PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file, platform.OpenRead(path));
    auto bytes = core::ReadAll(*file);
    file->Close();
    return bytes;
}

Result<void> WriteFile(std::string_view path, std::span<const std::byte> bytes) {
    core::PlatformFile platform;
    PARCEL_TRY_DECL(std::unique_ptr<core::IFile>, file, platform.OpenWrite(path));
    PARCEL_TRY(core::WriteAll(*file, bytes));
    PARCEL_TRY(file->Flush());
    file->Close();
    return {};
}

bool Exists(std::string_view path) {
    core::PlatformFile platform;
    auto result = platform.Exists(path);
    return result && result.Value();
}

Result<std::uint64_t> FileSize(std::string_view path) {
    core::PlatformFile platform;
    return platform.Size(path);
}

Result<ManifestPtr> OpenManifest(std::vector<std::byte> bytes) {
    auto storage = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    auto opened = serialize::ManifestView::Open(storage, true);
    if (!opened) {
        return std::move(opened).TakeError();
    }
    return std::make_shared<const serialize::ManifestView>(std::move(opened).Value());
}

Result<ManifestPtr> LoadManifestFileFromRoot(std::string_view root) {
    auto path = core::path::ResolveWithin(root, "manifest.bytes");
    if (!path) {
        return std::move(path).TakeError();
    }
    auto bytes = ReadFile(path.Value());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    return OpenManifest(std::move(bytes).Value());
}

Result<std::string> ReadVersionFile(std::string_view root, std::string_view package_name) {
    if (package_name.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "package name is empty");
    }
    const std::string file_name = std::string(package_name) + ".version";
    auto path = core::path::ResolveWithin(root, file_name);
    if (!path) {
        return std::move(path).TakeError();
    }
    if (!Exists(path.Value())) {
        return MakeError(ErrorCode::NotFound, "package version file was not found")
            .WithPath(path.Value());
    }
    auto bytes = ReadFile(path.Value());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    std::string value(reinterpret_cast<const char*>(bytes.Value().data()), bytes.Value().size());
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
                              value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

struct Sidecar {
    core::AssetGuid guid;
    std::uint64_t size = 0;
    core::Hash256 sha256;
    std::uint32_t crc32 = 0;
    std::uint32_t flags = 0;
};

Result<Sidecar> ReadSidecar(std::string_view path) {
    auto bytes = ReadFile(path);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    if (bytes.Value().size() != 72) {
        return MakeError(ErrorCode::Corrupt, "cache sidecar has an invalid size")
            .WithPath(std::string(path));
    }
    serialize::BinaryReader reader(bytes.Value());
    PARCEL_TRY_DECL(std::span<const std::byte>, magic, reader.ReadBytes(4));
    if (!std::equal(magic.begin(), magic.end(), kSidecarMagic.begin())) {
        return MakeError(ErrorCode::Corrupt, "cache sidecar has an invalid magic")
            .WithPath(std::string(path));
    }
    Sidecar sidecar;
    PARCEL_TRY_DECL(std::uint16_t, version, reader.ReadU16());
    if (version != kSidecarVersion) {
        return MakeError(ErrorCode::Unsupported, "unsupported cache sidecar version")
            .WithPath(std::string(path));
    }
    PARCEL_TRY(reader.Skip(2));
    PARCEL_TRY_ASSIGN(sidecar.guid, reader.ReadGuid128());
    PARCEL_TRY_ASSIGN(sidecar.size, reader.ReadU64());
    PARCEL_TRY_ASSIGN(sidecar.sha256, reader.ReadHash256());
    PARCEL_TRY_ASSIGN(sidecar.crc32, reader.ReadU32());
    PARCEL_TRY_ASSIGN(sidecar.flags, reader.ReadU32());
    if (!reader.IsAtEnd()) {
        return MakeError(ErrorCode::Corrupt, "cache sidecar has trailing bytes")
            .WithPath(std::string(path));
    }
    return sidecar;
}

Result<void> VerifyBytes(const serialize::ManifestBundleView& bundle,
                         std::span<const std::byte> bytes) {
    if (bundle.file_size != 0 && bytes.size() != bundle.file_size) {
        return MakeError(ErrorCode::HashMismatch, "bundle size does not match the manifest")
            .WithBundle(std::string(bundle.bundle_name))
            .WithPath(std::string(bundle.file_name));
    }
    if (!bundle.file_sha256.IsZero() && core::Sha256(bytes) != bundle.file_sha256) {
        return MakeError(ErrorCode::HashMismatch, "bundle SHA-256 does not match the manifest")
            .WithBundle(std::string(bundle.bundle_name))
            .WithPath(std::string(bundle.file_name));
    }
    if (bundle.file_crc != 0 && core::Crc32(bytes) != bundle.file_crc) {
        return MakeError(ErrorCode::HashMismatch, "bundle CRC32 does not match the manifest")
            .WithBundle(std::string(bundle.bundle_name))
            .WithPath(std::string(bundle.file_name));
    }
    return {};
}

Result<void> VerifySidecar(const serialize::ManifestBundleView& bundle, const Sidecar& sidecar) {
    if (sidecar.guid != bundle.bundle_guid) {
        return MakeError(ErrorCode::Corrupt, "cache sidecar GUID does not match the bundle")
            .WithBundle(std::string(bundle.bundle_name));
    }
    if (sidecar.size != bundle.file_size && bundle.file_size != 0) {
        return MakeError(ErrorCode::Corrupt, "cache sidecar size does not match the bundle")
            .WithBundle(std::string(bundle.bundle_name));
    }
    if (sidecar.sha256 != bundle.file_sha256) {
        return MakeError(ErrorCode::Corrupt, "cache sidecar hash does not match the bundle")
            .WithBundle(std::string(bundle.bundle_name));
    }
    if (bundle.file_crc != 0 && sidecar.crc32 != bundle.file_crc) {
        return MakeError(ErrorCode::Corrupt, "cache sidecar CRC does not match the bundle")
            .WithBundle(std::string(bundle.bundle_name));
    }
    return {};
}

bool IsCacheFile(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    return extension == ".bundle" || extension == kBundleCacheSidecarExtension;
}

bool IsTempFile(const std::filesystem::path& path) { return path.extension().string() == ".part"; }

} // namespace

BuiltinFileSystem::BuiltinFileSystem(FileSystemOptions options) { options_ = std::move(options); }

std::string_view BuiltinFileSystem::Name() const noexcept { return "builtin"; }

Result<void> BuiltinFileSystem::Initialize(const FileSystemOptions& options) {
    FileSystemOptions merged = options_;
    if (!options.root.empty()) {
        merged.root = options.root;
    }
    if (!options.package_name.empty()) {
        merged.package_name = options.package_name;
    }
    options_ = std::move(merged);
    options_.read_only = true;
    options_.write_sidecar = false;
    root_ = core::path::Normalize(options_.root);
    if (root_.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "builtin file system root is empty");
    }
    PARCEL_TRY(LoadBuiltinCatalog());
    initialized_ = true;
    return {};
}

Result<void> BuiltinFileSystem::LoadBuiltinCatalog() {
    catalog_paths_.clear();
    catalog_guid_paths_.clear();
    catalog_loaded_ = false;
    auto catalog_path = core::path::ResolveWithin(root_, serialize::kBuiltinCatalogFileName);
    if (!catalog_path) {
        return std::move(catalog_path).TakeError();
    }
    if (!Exists(catalog_path.Value())) {
        return {};
    }

    auto bytes = ReadFile(catalog_path.Value());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    auto catalog = serialize::DecodeBuiltinCatalog(bytes.Value());
    if (!catalog) {
        return std::move(catalog).TakeError().WithPath(catalog_path.Value());
    }
    if (!options_.package_name.empty() && catalog.Value().package_name != options_.package_name) {
        return MakeError(ErrorCode::Corrupt, "builtin catalog package name does not match")
            .WithPackage(options_.package_name)
            .WithPath(catalog_path.Value());
    }

    std::unordered_map<std::string, std::string> paths;
    paths.reserve(catalog.Value().entries.size());
    std::unordered_map<core::AssetGuid, std::string, core::Guid128Hash> guid_paths;
    guid_paths.reserve(catalog.Value().entries.size());
    for (const serialize::BuiltinCatalogEntry& entry : catalog.Value().entries) {
        auto direct = core::path::ResolveWithin(root_, entry.file_name);
        if (!direct) {
            return std::move(direct).TakeError();
        }
        std::string path = direct.Value();
        if (!Exists(path)) {
            auto bundled =
                core::path::ResolveWithin(root_, std::string("bundles/") + entry.file_name);
            if (!bundled) {
                return std::move(bundled).TakeError();
            }
            path = std::move(bundled).Value();
        }
        if (!paths.emplace(entry.file_name, path).second) {
            return MakeError(ErrorCode::Corrupt, "builtin catalog contains a duplicate file name")
                .WithPath(catalog_path.Value());
        }
        if (!guid_paths.emplace(entry.bundle_guid, std::move(path)).second) {
            return MakeError(ErrorCode::Corrupt, "builtin catalog contains a duplicate GUID")
                .WithPath(catalog_path.Value());
        }
    }
    catalog_paths_ = std::move(paths);
    catalog_guid_paths_ = std::move(guid_paths);
    catalog_loaded_ = true;
    return {};
}

Result<std::string> BuiltinFileSystem::ManifestPath() const {
    return core::path::ResolveWithin(root_, "manifest.bytes");
}

Result<std::string> BuiltinFileSystem::BundlePath(std::string_view file_name) const {
    if (catalog_loaded_) {
        const auto found = catalog_paths_.find(std::string(file_name));
        if (found == catalog_paths_.end()) {
            return MakeError(ErrorCode::NotFound, "bundle is not listed in the builtin catalog")
                .WithPath(std::string(file_name));
        }
        return found->second;
    }
    auto direct = core::path::ResolveWithin(root_, file_name);
    if (!direct) {
        return std::move(direct).TakeError();
    }
    if (Exists(direct.Value())) {
        return direct;
    }
    auto bundled =
        core::path::ResolveWithin(root_, std::string("bundles/") + std::string(file_name));
    if (!bundled) {
        return std::move(bundled).TakeError();
    }
    return bundled;
}

Result<std::string> BuiltinFileSystem::RequestVersion() {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "builtin file system is not initialized");
    }
    auto version = ReadVersionFile(root_, options_.package_name);
    if (version) {
        return version;
    }
    auto manifest = LoadManifest();
    if (!manifest) {
        return std::move(version).TakeError();
    }
    return std::string(manifest.Value()->PackageVersion());
}

Result<ManifestPtr> BuiltinFileSystem::LoadManifest() {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "builtin file system is not initialized");
    }
    return LoadManifestFileFromRoot(root_);
}

bool BuiltinFileSystem::CanAccept(const serialize::ManifestBundleView& bundle) const {
    if (!initialized_) {
        return false;
    }
    if (catalog_loaded_) {
        const auto found = catalog_guid_paths_.find(bundle.bundle_guid);
        if (found == catalog_guid_paths_.end()) {
            return false;
        }
        auto path = BundlePath(bundle.file_name);
        return path && Exists(path.Value()) && path.Value() == found->second;
    }
    auto path = BundlePath(bundle.file_name);
    return path && Exists(path.Value());
}

bool BuiltinFileSystem::IsDownloadRequired(const serialize::ManifestBundleView& bundle) const {
    return !CanAccept(bundle);
}

Result<std::vector<std::byte>>
BuiltinFileSystem::LoadBundle(const serialize::ManifestBundleView& bundle) {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "builtin file system is not initialized");
    }
    PARCEL_TRY_DECL(std::string, path, BundlePath(bundle.file_name));
    auto bytes = ReadFile(path);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    PARCEL_TRY(VerifyBytes(bundle, bytes.Value()));
    return bytes;
}

Result<void> BuiltinFileSystem::EnsureBundle(const serialize::ManifestBundleView& bundle) {
    if (!CanAccept(bundle)) {
        return MakeError(ErrorCode::NotFound, "builtin bundle is not available")
            .WithBundle(std::string(bundle.bundle_name))
            .WithPath(std::string(bundle.file_name));
    }
    return {};
}

Result<CacheVerifyStatus>
BuiltinFileSystem::VerifyCachedBundle(const serialize::ManifestBundleView& bundle,
                                      CacheVerifyLevel level) const {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "builtin file system is not initialized");
    }
    auto path = BundlePath(bundle.file_name);
    if (!path) {
        return std::move(path).TakeError();
    }
    if (!Exists(path.Value())) {
        return CacheVerifyStatus::Missing;
    }
    auto size = FileSize(path.Value());
    if (!size) {
        return std::move(size).TakeError();
    }
    if (bundle.file_size != 0 && size.Value() != bundle.file_size) {
        return CacheVerifyStatus::Corrupt;
    }
    if (level == CacheVerifyLevel::Metadata || level == CacheVerifyLevel::Size) {
        return CacheVerifyStatus::Valid;
    }
    auto bytes = ReadFile(path.Value());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    auto check = VerifyBytes(bundle, bytes.Value());
    if (!check) {
        return CacheVerifyStatus::Corrupt;
    }
    return CacheVerifyStatus::Valid;
}

SandboxFileSystem::SandboxFileSystem(FileSystemOptions options) { options_ = std::move(options); }

std::string_view SandboxFileSystem::Name() const noexcept { return name_; }

Result<void> SandboxFileSystem::Initialize(const FileSystemOptions& options) {
    FileSystemOptions merged = options_;
    if (!options.root.empty()) {
        merged.root = options.root;
    }
    if (!options.package_name.empty()) {
        merged.package_name = options.package_name;
    }
    if (!options.write_sidecar) {
        merged.write_sidecar = options_.write_sidecar;
    }
    if (!options.app_build_id.empty()) {
        merged.app_build_id = options.app_build_id;
    }
    if (options.install_cleanup_mode != InstallCleanupMode::None) {
        merged.install_cleanup_mode = options.install_cleanup_mode;
    }
    options_ = std::move(merged);
    options_.read_only = false;
    root_ = core::path::Normalize(options_.root);
    if (root_.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system root is empty");
    }
    core::PlatformFile platform;
    PARCEL_TRY(platform.CreateDirectories(root_));
    PARCEL_TRY(CheckFootprint());
    initialized_ = true;
    return {};
}

Result<void> SandboxFileSystem::CheckFootprint() {
    if (options_.app_build_id.empty()) {
        return {};
    }
    auto footprint = core::path::ResolveWithin(root_, kApplicationFootprintFileName);
    if (!footprint) {
        return std::move(footprint).TakeError();
    }
    core::PlatformFile platform;
    auto exists = platform.Exists(footprint.Value());
    if (!exists) {
        return std::move(exists).TakeError();
    }
    bool dirty = !exists.Value();
    if (exists.Value()) {
        auto bytes = ReadFile(footprint.Value());
        dirty = !bytes || bytes.Value().size() != options_.app_build_id.size() ||
                !std::equal(bytes.Value().begin(), bytes.Value().end(),
                            reinterpret_cast<const std::byte*>(options_.app_build_id.data()));
        if (dirty) {
            PARCEL_TRY(ApplyInstallCleanup());
        }
    }
    if (dirty || !exists.Value()) {
        PARCEL_TRY(WriteFile(footprint.Value(),
                             std::span<const std::byte>(
                                 reinterpret_cast<const std::byte*>(options_.app_build_id.data()),
                                 options_.app_build_id.size())));
    }
    return {};
}

Result<void> SandboxFileSystem::ApplyInstallCleanup() {
    if (options_.install_cleanup_mode == InstallCleanupMode::None) {
        return {};
    }
    const std::filesystem::path root(root_);
    std::error_code error;
    const bool remove_bundles = options_.install_cleanup_mode == InstallCleanupMode::Bundles ||
                                options_.install_cleanup_mode == InstallCleanupMode::All;
    const bool remove_manifests = options_.install_cleanup_mode == InstallCleanupMode::Manifests ||
                                  options_.install_cleanup_mode == InstallCleanupMode::All;
    const bool remove_all = options_.install_cleanup_mode == InstallCleanupMode::All;

    if (remove_bundles) {
        std::filesystem::remove_all(root / "bundles", error);
        if (error) {
            return MakeError(ErrorCode::Io, "failed to remove cached bundles").WithPath(root_);
        }
        std::filesystem::remove_all(root / "temp", error);
        if (error) {
            return MakeError(ErrorCode::Io, "failed to remove temporary bundles").WithPath(root_);
        }
        if (!remove_all) {
            std::filesystem::directory_iterator iterator(root, error);
            if (error) {
                return MakeError(ErrorCode::Io, "failed to enumerate the sandbox").WithPath(root_);
            }
            const std::filesystem::directory_iterator end;
            for (; iterator != end; iterator.increment(error)) {
                if (error) {
                    return MakeError(ErrorCode::Io, "failed to enumerate the sandbox")
                        .WithPath(root_);
                }
                if (!iterator->is_regular_file(error) || error) {
                    error.clear();
                    continue;
                }
                const auto extension = iterator->path().extension().string();
                if (extension != ".bundle" && extension != kBundleCacheSidecarExtension) {
                    continue;
                }
                std::filesystem::remove(iterator->path(), error);
                if (error) {
                    return MakeError(ErrorCode::Io, "failed to remove cached bundles")
                        .WithPath(root_);
                }
            }
        }
    }

    if (remove_all) {
        std::filesystem::directory_iterator iterator(root, error);
        if (error) {
            return MakeError(ErrorCode::Io, "failed to enumerate the sandbox").WithPath(root_);
        }
        const std::filesystem::directory_iterator end;
        for (; iterator != end; iterator.increment(error)) {
            if (error) {
                return MakeError(ErrorCode::Io, "failed to enumerate the sandbox").WithPath(root_);
            }
            if (iterator->path().filename() == kApplicationFootprintFileName) {
                continue;
            }
            std::filesystem::remove_all(iterator->path(), error);
            if (error) {
                return MakeError(ErrorCode::Io, "failed to remove sandbox cache").WithPath(root_);
            }
        }
        return {};
    }

    if (remove_manifests) {
        std::filesystem::remove(root / "manifest.bytes", error);
        if (error) {
            return MakeError(ErrorCode::Io, "failed to remove the cached manifest").WithPath(root_);
        }
        error.clear();
        std::filesystem::directory_iterator iterator(root, error);
        if (error) {
            return MakeError(ErrorCode::Io, "failed to enumerate the sandbox").WithPath(root_);
        }
        const std::filesystem::directory_iterator end;
        for (; iterator != end; iterator.increment(error)) {
            if (error) {
                return MakeError(ErrorCode::Io, "failed to enumerate the sandbox").WithPath(root_);
            }
            const auto extension = iterator->path().extension().string();
            if (extension != ".version" && extension != ".hash") {
                continue;
            }
            std::filesystem::remove(iterator->path(), error);
            if (error) {
                return MakeError(ErrorCode::Io, "failed to remove cached manifest metadata")
                    .WithPath(root_);
            }
        }
    }
    return {};
}

Result<std::string> SandboxFileSystem::ManifestPath() const {
    return core::path::ResolveWithin(root_, "manifest.bytes");
}

Result<std::string> SandboxFileSystem::BundlePath(std::string_view file_name) const {
    auto direct = core::path::ResolveWithin(root_, file_name);
    if (!direct) {
        return std::move(direct).TakeError();
    }
    if (Exists(direct.Value())) {
        return direct;
    }
    return core::path::ResolveWithin(root_, std::string("bundles/") + std::string(file_name));
}

Result<std::string> SandboxFileSystem::SidecarPath(std::string_view file_name) const {
    auto path = BundlePath(file_name);
    if (!path) {
        return std::move(path).TakeError();
    }
    std::string sidecar = path.Value();
    sidecar += kBundleCacheSidecarExtension;
    return sidecar;
}

Result<std::string> SandboxFileSystem::RequestVersion() {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system is not initialized");
    }
    auto version = ReadVersionFile(root_, options_.package_name);
    if (version) {
        return version;
    }
    auto manifest = LoadManifest();
    if (!manifest) {
        return std::move(version).TakeError();
    }
    return std::string(manifest.Value()->PackageVersion());
}

Result<ManifestPtr> SandboxFileSystem::LoadManifest() {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system is not initialized");
    }
    return LoadManifestFileFromRoot(root_);
}

bool SandboxFileSystem::CanAccept(const serialize::ManifestBundleView& bundle) const {
    (void)bundle;
    return initialized_;
}

bool SandboxFileSystem::IsDownloadRequired(const serialize::ManifestBundleView& bundle) const {
    if (!initialized_) {
        return true;
    }
    auto path = BundlePath(bundle.file_name);
    if (!path || !Exists(path.Value())) {
        return true;
    }
    auto sidecar = SidecarPath(bundle.file_name);
    if (sidecar && Exists(sidecar.Value())) {
        auto parsed = ReadSidecar(sidecar.Value());
        return !parsed || !VerifySidecar(bundle, parsed.Value());
    }
    auto size = FileSize(path.Value());
    return !size || (bundle.file_size != 0 && size.Value() != bundle.file_size);
}

Result<void> SandboxFileSystem::VerifyBundle(const serialize::ManifestBundleView& bundle) const {
    auto path = BundlePath(bundle.file_name);
    if (!path) {
        return std::move(path).TakeError();
    }
    if (!Exists(path.Value())) {
        return MakeError(ErrorCode::NotFound, "sandbox bundle was not found")
            .WithBundle(std::string(bundle.bundle_name))
            .WithPath(path.Value());
    }
    auto sidecar_path = SidecarPath(bundle.file_name);
    if (!sidecar_path) {
        return std::move(sidecar_path).TakeError();
    }
    if (Exists(sidecar_path.Value())) {
        auto sidecar = ReadSidecar(sidecar_path.Value());
        if (!sidecar) {
            return std::move(sidecar).TakeError();
        }
        return VerifySidecar(bundle, sidecar.Value());
    }
    auto size = FileSize(path.Value());
    if (!size) {
        return std::move(size).TakeError();
    }
    if (bundle.file_size != 0 && size.Value() != bundle.file_size) {
        return MakeError(ErrorCode::Corrupt, "sandbox bundle size is invalid")
            .WithBundle(std::string(bundle.bundle_name))
            .WithPath(path.Value());
    }
    return {};
}

Result<void> SandboxFileSystem::WriteSidecar(const serialize::ManifestBundleView& bundle,
                                             std::span<const std::byte> bytes) const {
    if (!options_.write_sidecar || options_.read_only) {
        return {};
    }
    auto path = SidecarPath(bundle.file_name);
    if (!path) {
        return std::move(path).TakeError();
    }
    serialize::BinaryWriter writer(72);
    PARCEL_TRY(writer.WriteBytes(kSidecarMagic));
    PARCEL_TRY(writer.WriteU16(kSidecarVersion));
    PARCEL_TRY(writer.WriteU16(0));
    PARCEL_TRY(writer.WriteGuid128(bundle.bundle_guid));
    PARCEL_TRY(writer.WriteU64(bytes.size()));
    PARCEL_TRY(writer.WriteHash256(core::Sha256(bytes)));
    PARCEL_TRY(writer.WriteU32(core::Crc32(bytes)));
    PARCEL_TRY(writer.WriteU32(0));
    return WriteFile(path.Value(), writer.Data());
}

Result<std::vector<std::byte>>
SandboxFileSystem::LoadBundle(const serialize::ManifestBundleView& bundle) {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system is not initialized");
    }
    PARCEL_TRY_DECL(std::string, path, BundlePath(bundle.file_name));
    auto bytes = ReadFile(path);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }

    auto manifest_check = VerifyBytes(bundle, bytes.Value());
    if (!manifest_check) {
        (void)RemoveBundleFiles(bundle.file_name);
        return std::move(manifest_check).TakeError();
    }

    auto sidecar_path = SidecarPath(bundle.file_name);
    if (!sidecar_path) {
        return std::move(sidecar_path).TakeError();
    }
    if (Exists(sidecar_path.Value())) {
        auto sidecar = ReadSidecar(sidecar_path.Value());
        if (!sidecar || !VerifySidecar(bundle, sidecar.Value())) {
            (void)RemoveBundleFiles(bundle.file_name);
            if (!sidecar) {
                return std::move(sidecar).TakeError();
            }
            return MakeError(ErrorCode::Corrupt, "sandbox cache sidecar is invalid")
                .WithBundle(std::string(bundle.bundle_name))
                .WithPath(path);
        }
    }
    auto sidecar_write = WriteSidecar(bundle, bytes.Value());
    if (!sidecar_write) {
        return std::move(sidecar_write).TakeError();
    }
    return bytes;
}

Result<void> SandboxFileSystem::EnsureBundle(const serialize::ManifestBundleView& bundle) {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system is not initialized");
    }
    auto verification = VerifyBundle(bundle);
    if (verification) {
        return {};
    }
    (void)RemoveBundleFiles(bundle.file_name);
    return std::move(verification).TakeError();
}

Result<void> SandboxFileSystem::ImportBundle(const serialize::ManifestBundleView& bundle,
                                             std::span<const std::byte> bytes) {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system is not initialized");
    }
    if (options_.read_only) {
        return MakeError(ErrorCode::Unsupported, "sandbox file system is read-only")
            .WithBundle(std::string(bundle.bundle_name));
    }
    PARCEL_TRY(VerifyBytes(bundle, bytes));

    auto temp_root = core::path::ResolveWithin(root_, "temp");
    if (!temp_root) {
        return std::move(temp_root).TakeError();
    }
    core::PlatformFile platform;
    PARCEL_TRY(platform.CreateDirectories(temp_root.Value()));
    const std::uint64_t sequence = g_import_sequence.fetch_add(1, std::memory_order_relaxed);
    auto temp_path = core::path::ResolveWithin(
        temp_root.Value(), bundle.bundle_guid.ToHex() + "-" + std::to_string(sequence) + ".part");
    if (!temp_path) {
        return std::move(temp_path).TakeError();
    }
    PARCEL_TRY(WriteFile(temp_path.Value(), bytes));
    auto committed = CommitDownloadedBundle(bundle, temp_path.Value());
    if (!committed) {
        (void)platform.Delete(temp_path.Value());
        return std::move(committed).TakeError();
    }
    return {};
}

Result<CacheVerifyStatus>
SandboxFileSystem::VerifyCachedBundle(const serialize::ManifestBundleView& bundle,
                                      CacheVerifyLevel level) const {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system is not initialized");
    }
    auto path = BundlePath(bundle.file_name);
    if (!path) {
        return std::move(path).TakeError();
    }
    if (!Exists(path.Value())) {
        return CacheVerifyStatus::Missing;
    }

    auto sidecar_path = SidecarPath(bundle.file_name);
    if (!sidecar_path) {
        return std::move(sidecar_path).TakeError();
    }
    if (Exists(sidecar_path.Value())) {
        auto sidecar = ReadSidecar(sidecar_path.Value());
        if (!sidecar || !VerifySidecar(bundle, sidecar.Value())) {
            return CacheVerifyStatus::Corrupt;
        }
    }

    auto size = FileSize(path.Value());
    if (!size) {
        return std::move(size).TakeError();
    }
    if (bundle.file_size != 0 && size.Value() != bundle.file_size) {
        return CacheVerifyStatus::Corrupt;
    }
    if (level == CacheVerifyLevel::Metadata || level == CacheVerifyLevel::Size) {
        return CacheVerifyStatus::Valid;
    }

    auto bytes = ReadFile(path.Value());
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    auto check = VerifyBytes(bundle, bytes.Value());
    if (!check) {
        return CacheVerifyStatus::Corrupt;
    }
    return CacheVerifyStatus::Valid;
}

Result<void> SandboxFileSystem::InvalidateBundle(const serialize::ManifestBundleView& bundle) {
    return RemoveBundleFiles(bundle.file_name);
}

Result<std::string>
SandboxFileSystem::DownloadTempPath(const serialize::ManifestBundleView& bundle) {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system is not initialized");
    }
    if (bundle.bundle_guid.ToHex().empty()) {
        return MakeError(ErrorCode::InvalidArgument, "bundle GUID is empty");
    }
    auto temp_root = core::path::ResolveWithin(root_, "temp");
    if (!temp_root) {
        return std::move(temp_root).TakeError();
    }
    core::PlatformFile platform;
    PARCEL_TRY(platform.CreateDirectories(temp_root.Value()));
    return core::path::ResolveWithin(temp_root.Value(), bundle.bundle_guid.ToHex() + ".part");
}

Result<void> SandboxFileSystem::CommitDownloadedBundle(const serialize::ManifestBundleView& bundle,
                                                       std::string_view temp_path) {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system is not initialized");
    }
    if (options_.read_only) {
        return MakeError(ErrorCode::Unsupported, "sandbox file system is read-only")
            .WithBundle(std::string(bundle.bundle_name));
    }
    if (!core::path::IsWithin(root_, temp_path)) {
        return MakeError(ErrorCode::InvalidArgument, "temporary bundle path escapes the sandbox")
            .WithPath(std::string(temp_path));
    }
    auto bytes = ReadFile(temp_path);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    PARCEL_TRY(VerifyBytes(bundle, bytes.Value()));

    auto target = BundlePath(bundle.file_name);
    if (!target) {
        return std::move(target).TakeError();
    }
    core::PlatformFile platform;
    const std::size_t slash = target.Value().find_last_of('/');
    if (slash != std::string::npos) {
        PARCEL_TRY(platform.CreateDirectories(target.Value().substr(0, slash)));
    }
    if (Exists(target.Value())) {
        PARCEL_TRY(platform.Delete(target.Value()));
    }
    PARCEL_TRY(platform.Rename(temp_path, target.Value(), true));
    return WriteSidecar(bundle, bytes.Value());
}

Result<void> SandboxFileSystem::RemoveBundleFiles(std::string_view file_name) const {
    core::PlatformFile platform;
    auto path = BundlePath(file_name);
    if (path && Exists(path.Value())) {
        (void)platform.Delete(path.Value());
    }
    auto sidecar = SidecarPath(file_name);
    if (sidecar && Exists(sidecar.Value())) {
        (void)platform.Delete(sidecar.Value());
    }
    return {};
}

Result<void> SandboxFileSystem::ClearCache(const CacheClearOptions& options) {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system is not initialized");
    }
    if (options.file_names.empty() &&
        (options.mode == CacheClearMode::ByTags || options.mode == CacheClearMode::ByLocations ||
         options.mode == CacheClearMode::Unused)) {
        return {};
    }
    if (options.mode == CacheClearMode::OldVersions && options.keep_file_names.empty() &&
        options.file_names.empty()) {
        return {};
    }

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(root_, error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to enumerate the sandbox cache").WithPath(root_);
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            return MakeError(ErrorCode::Io, "failed to enumerate the sandbox cache")
                .WithPath(root_);
        }
        if (!iterator->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        const std::filesystem::path path = iterator->path();
        if (IsTempFile(path)) {
            if (options.file_names.empty() && (options.mode == CacheClearMode::All ||
                                               options.mode == CacheClearMode::OldVersions)) {
                std::filesystem::remove(path, error);
                error.clear();
            }
            continue;
        }
        if (!IsCacheFile(path)) {
            continue;
        }
        const std::string file_name = path.filename().string();
        std::string cache_name = file_name;
        if (cache_name.size() > kBundleCacheSidecarExtension.size() &&
            cache_name.compare(cache_name.size() - kBundleCacheSidecarExtension.size(),
                               kBundleCacheSidecarExtension.size(),
                               kBundleCacheSidecarExtension) == 0) {
            cache_name.resize(cache_name.size() - kBundleCacheSidecarExtension.size());
        }
        if (options.mode == CacheClearMode::OldVersions) {
            if (std::find(options.keep_file_names.begin(), options.keep_file_names.end(),
                          file_name) != options.keep_file_names.end() ||
                std::find(options.keep_file_names.begin(), options.keep_file_names.end(),
                          cache_name) != options.keep_file_names.end()) {
                continue;
            }
        } else if (!options.file_names.empty()) {
            if (std::find(options.file_names.begin(), options.file_names.end(), file_name) ==
                    options.file_names.end() &&
                std::find(options.file_names.begin(), options.file_names.end(), cache_name) ==
                    options.file_names.end()) {
                continue;
            }
        }
        std::filesystem::remove(path, error);
        error.clear();
    }
    return {};
}

Result<CacheStats> SandboxFileSystem::QueryCache() const {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "sandbox file system is not initialized");
    }
    CacheStats stats;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(root_, error);
    if (error) {
        return MakeError(ErrorCode::Io, "failed to enumerate the sandbox cache").WithPath(root_);
    }
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            return MakeError(ErrorCode::Io, "failed to enumerate the sandbox cache")
                .WithPath(root_);
        }
        if (!iterator->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        const auto path = iterator->path();
        if (!IsCacheFile(path) || IsTempFile(path)) {
            continue;
        }
        const auto size = iterator->file_size(error);
        if (error) {
            error.clear();
            continue;
        }
        ++stats.files;
        stats.bytes += static_cast<std::uint64_t>(size);
    }
    return stats;
}

CustomFileSystem::CustomFileSystem() {
    name_ = "custom";
    custom_name_ = name_;
}

CustomFileSystem::CustomFileSystem(FileSystemOptions options, std::string name)
    : SandboxFileSystem(std::move(options)), custom_name_(std::move(name)) {
    if (custom_name_.empty()) {
        custom_name_ = "custom";
    }
    name_ = custom_name_;
}

std::string_view CustomFileSystem::Name() const noexcept { return custom_name_; }

FileSystemHost::FileSystemHost(std::string package_name) : package_name_(std::move(package_name)) {}

void FileSystemHost::AddFileSystem(std::shared_ptr<IFileSystem> file_system) {
    if (file_system) {
        file_systems_.push_back(std::move(file_system));
    }
}

Result<void> FileSystemHost::Initialize() {
    if (file_systems_.empty()) {
        return MakeError(ErrorCode::InvalidArgument, "file system host has no file systems");
    }
    FileSystemOptions options;
    options.package_name = package_name_;
    for (auto& file_system : file_systems_) {
        FileSystemOptions current = options;
        auto initialized = file_system->Initialize(current);
        if (!initialized) {
            return std::move(initialized).TakeError();
        }
    }
    initialized_ = true;
    return {};
}

void FileSystemHost::Shutdown() noexcept {
    for (auto& file_system : file_systems_) {
        if (file_system) {
            file_system->Shutdown();
        }
    }
    initialized_ = false;
}

IFileSystem* FileSystemHost::MainFileSystem() const noexcept {
    if (file_systems_.empty()) {
        return nullptr;
    }
    return file_systems_.front().get();
}

Result<std::string> FileSystemHost::RequestVersion() {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "file system host is not initialized");
    }
    core::Error last_error{ErrorCode::NotFound, "package version was not found"};
    for (auto& file_system : file_systems_) {
        auto result = file_system->RequestVersion();
        if (result) {
            if (!result.Value().empty()) {
                return result;
            }
            continue;
        }
        last_error = std::move(result).TakeError();
    }
    return last_error;
}

Result<ManifestPtr> FileSystemHost::LoadManifest() {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "file system host is not initialized");
    }
    core::Error last_error{ErrorCode::NotFound, "package manifest was not found"};
    for (auto& file_system : file_systems_) {
        auto result = file_system->LoadManifest();
        if (result) {
            return result;
        }
        last_error = std::move(result).TakeError();
    }
    return last_error;
}

bool FileSystemHost::CanAccept(const serialize::ManifestBundleView& bundle) const {
    for (const auto& file_system : file_systems_) {
        if (file_system->CanAccept(bundle)) {
            return true;
        }
    }
    return false;
}

bool FileSystemHost::IsDownloadRequired(const serialize::ManifestBundleView& bundle) const {
    for (const auto& file_system : file_systems_) {
        if (file_system->CanAccept(bundle) && !file_system->IsDownloadRequired(bundle)) {
            return false;
        }
    }
    for (const auto& file_system : file_systems_) {
        if (file_system->IsDownloadRequired(bundle)) {
            return true;
        }
    }
    return false;
}

Result<std::vector<std::byte>>
FileSystemHost::LoadBundle(const serialize::ManifestBundleView& bundle) {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "file system host is not initialized");
    }
    core::Error last_error{ErrorCode::NotFound, "bundle was not found"};
    for (auto& file_system : file_systems_) {
        if (!file_system->CanAccept(bundle)) {
            continue;
        }
        auto result = file_system->LoadBundle(bundle);
        if (result) {
            return result;
        }
        last_error = std::move(result).TakeError();
    }
    return last_error;
}

Result<void> FileSystemHost::EnsureBundle(const serialize::ManifestBundleView& bundle) {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "file system host is not initialized");
    }
    core::Error last_error{ErrorCode::NotFound, "bundle was not found"};
    for (auto& file_system : file_systems_) {
        if (!file_system->CanAccept(bundle)) {
            continue;
        }
        auto result = file_system->EnsureBundle(bundle);
        if (result) {
            return {};
        }
        last_error = std::move(result).TakeError();
    }
    for (auto& file_system : file_systems_) {
        if (!file_system->IsDownloadRequired(bundle)) {
            continue;
        }
        auto result = file_system->EnsureBundle(bundle);
        if (result) {
            return {};
        }
        last_error = std::move(result).TakeError();
    }
    return last_error;
}

Result<CacheVerifyStatus>
FileSystemHost::VerifyCachedBundle(const serialize::ManifestBundleView& bundle,
                                   CacheVerifyLevel level) const {
    if (!initialized_) {
        return MakeError(ErrorCode::InvalidArgument, "file system host is not initialized");
    }
    bool missing = false;
    bool valid = false;
    bool corrupt = false;
    bool has_error = false;
    core::Error first_error{ErrorCode::NotFound, "bundle was not found"};
    for (const auto& file_system : file_systems_) {
        if (!file_system->CanAccept(bundle)) {
            continue;
        }
        auto result = file_system->VerifyCachedBundle(bundle, level);
        if (!result) {
            if (!has_error) {
                first_error = std::move(result).TakeError();
                has_error = true;
            }
            continue;
        }
        switch (result.Value()) {
        case CacheVerifyStatus::Valid:
            valid = true;
            break;
        case CacheVerifyStatus::Corrupt:
            corrupt = true;
            break;
        case CacheVerifyStatus::Missing:
            missing = true;
            break;
        case CacheVerifyStatus::Unsupported:
            break;
        }
    }
    if (corrupt) {
        return CacheVerifyStatus::Corrupt;
    }
    if (valid) {
        return CacheVerifyStatus::Valid;
    }
    if (has_error) {
        return first_error;
    }
    if (missing) {
        return CacheVerifyStatus::Missing;
    }
    return CacheVerifyStatus::Unsupported;
}

Result<void> FileSystemHost::InvalidateBundle(const serialize::ManifestBundleView& bundle) {
    core::Error last_error{ErrorCode::NotFound, "bundle was not found"};
    for (auto& file_system : file_systems_) {
        if (!file_system->CanAccept(bundle)) {
            continue;
        }
        auto result = file_system->InvalidateBundle(bundle);
        if (result) {
            return {};
        }
        last_error = std::move(result).TakeError();
    }
    return last_error;
}

Result<void> FileSystemHost::ClearCache(const CacheClearOptions& options) {
    for (auto& file_system : file_systems_) {
        auto result = file_system->ClearCache(options);
        if (!result && result.GetError().code != ErrorCode::Unsupported) {
            return result;
        }
    }
    return {};
}

Result<CacheStats> FileSystemHost::QueryCache() const {
    CacheStats total;
    for (const auto& file_system : file_systems_) {
        auto result = file_system->QueryCache();
        if (!result) {
            continue;
        }
        total.bytes += result.Value().bytes;
        total.files += result.Value().files;
    }
    return total;
}

} // namespace parcel::runtime
