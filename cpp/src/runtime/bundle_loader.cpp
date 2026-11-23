#include "parcel/runtime/bundle_loader.hpp"

#include "parcel/assets/asset_codec.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/serialize/asset_file.hpp"
#include "parcel/serialize/binary.hpp"
#include "parcel/serialize/bundle.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace parcel::runtime {
namespace {

using core::ErrorCode;
using core::MakeError;
using core::Result;

class AssetReadContext final : public assets::IAssetReadContext {
  public:
    AssetReadContext(const serialize::AssetFileReader& reader, assets::IObjectResolver* resolver)
        : reader_(reader), resolver_(resolver) {}

    Result<std::span<const std::byte>> Blob(std::uint64_t id) const override {
        const auto blobs = reader_.Blobs();
        if (id >= blobs.size()) {
            return MakeError(ErrorCode::Corrupt, "asset blob id is out of range");
        }
        return blobs[static_cast<std::size_t>(id)].data;
    }

    Result<std::shared_ptr<assets::AssetObject>> Resolve(core::AssetGuid guid) override {
        if (resolver_ == nullptr) {
            return MakeError(ErrorCode::NotFound, "no object resolver is available");
        }
        return resolver_->Resolve(guid);
    }

  private:
    const serialize::AssetFileReader& reader_;
    assets::IObjectResolver* resolver_ = nullptr;
};

struct DecodedAssets {
    std::vector<std::shared_ptr<assets::AssetObject>> objects;
    std::vector<std::uint32_t> local_ids;
    std::vector<std::uint32_t> sub_asset_ids;
};

Result<DecodedAssets> DecodeAssets(std::span<const std::byte> data,
                                   const assets::TypeRegistry& registry,
                                   assets::IObjectResolver* resolver) {
    auto opened = serialize::AssetFileReader::Open(data, true);
    if (!opened) {
        return std::move(opened).TakeError();
    }
    serialize::AssetFileReader reader = std::move(opened).Value();
    PARCEL_TRY(reader.ReadAll(true));
    if (reader.Objects().empty()) {
        return MakeError(ErrorCode::Corrupt, "asset file has no objects");
    }

    DecodedAssets decoded;
    decoded.objects.reserve(reader.Objects().size());
    decoded.local_ids.reserve(reader.Objects().size());
    AssetReadContext context(reader, resolver);
    for (const auto& object : reader.Objects()) {
        const assets::ITypeSerializer* serializer = registry.Find(object.type_id);
        if (serializer == nullptr) {
            return MakeError(ErrorCode::Unsupported, "unknown asset type id");
        }
        bool declared = false;
        for (const auto& type : reader.Types()) {
            if (type.type_id == object.type_id &&
                type.serializer_version == serializer->Version()) {
                declared = true;
                break;
            }
        }
        if (!declared) {
            return MakeError(ErrorCode::Unsupported, "unsupported asset serializer version");
        }
        serialize::BinaryReader metadata(object.metadata);
        auto asset = serializer->Deserialize(metadata, context);
        if (!asset) {
            return std::move(asset).TakeError();
        }
        if (!metadata.IsAtEnd()) {
            return MakeError(ErrorCode::Corrupt, "asset metadata has trailing bytes");
        }
        if (asset.Value()->Type() != object.type_id) {
            return MakeError(ErrorCode::Corrupt, "asset object type does not match its serializer");
        }
        decoded.objects.push_back(std::move(asset).Value());
        decoded.local_ids.push_back(object.local_id);
    }
    decoded.sub_asset_ids.reserve(reader.SubAssets().size());
    for (const auto& sub_asset : reader.SubAssets()) {
        decoded.sub_asset_ids.push_back(sub_asset.local_object_id);
    }
    return decoded;
}

} // namespace

struct BundleLoader::Impl {
    Impl(std::shared_ptr<FileSystemHost> file_system_host, ManifestPtr active_manifest,
         std::uint32_t bundle_index, bool verify_file)
        : host(std::move(file_system_host)), manifest(std::move(active_manifest)),
          index(bundle_index), verify(verify_file) {
        if (manifest && index < manifest->Bundles().size()) {
            info = manifest->Bundles()[index];
        }
    }

    std::shared_ptr<FileSystemHost> host;
    ManifestPtr manifest;
    std::uint32_t index = 0;
    bool verify = true;
    serialize::ManifestBundleView info;
    std::shared_ptr<std::vector<std::byte>> bytes;
    std::unique_ptr<serialize::BundleReader> reader;
    core::Error last_error{ErrorCode::Ok};
    std::mutex mutex;
    std::condition_variable condition;
    std::uint32_t references = 0;
    bool loading = false;
    bool loaded = false;
};

BundleLoader::BundleLoader(std::shared_ptr<FileSystemHost> host, ManifestPtr manifest,
                           std::uint32_t bundle_index, bool verify_file)
    : impl_(std::make_shared<Impl>(std::move(host), std::move(manifest), bundle_index,
                                   verify_file)) {}

BundleLoader::~BundleLoader() { Unload(); }

Result<void> BundleLoader::Load() {
    if (!impl_->host || !impl_->manifest || impl_->index >= impl_->manifest->Bundles().size()) {
        return MakeError(ErrorCode::InvalidArgument, "bundle loader has no valid bundle");
    }

    {
        std::unique_lock lock(impl_->mutex);
        if (impl_->loaded) {
            return {};
        }
        if (impl_->loading) {
            impl_->condition.wait(lock, [this] { return !impl_->loading; });
            return impl_->loaded ? Result<void>{} : Result<void>{impl_->last_error};
        }
        impl_->loading = true;
        impl_->last_error = core::Error{ErrorCode::Ok};
    }

    core::Error failure{ErrorCode::Internal, "bundle was not loaded"};
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto loaded = impl_->host->LoadBundle(impl_->info);
        if (loaded) {
            auto bytes = std::make_shared<std::vector<std::byte>>(std::move(loaded).Value());
            if (impl_->info.file_size != 0 && bytes->size() != impl_->info.file_size) {
                failure =
                    MakeError(ErrorCode::HashMismatch, "bundle size does not match the manifest")
                        .WithBundle(std::string(impl_->info.bundle_name))
                        .WithPath(std::string(impl_->info.file_name));
            } else if (!impl_->info.file_sha256.IsZero() &&
                       core::Sha256(*bytes) != impl_->info.file_sha256) {
                failure =
                    MakeError(ErrorCode::HashMismatch, "bundle SHA-256 does not match the manifest")
                        .WithBundle(std::string(impl_->info.bundle_name))
                        .WithPath(std::string(impl_->info.file_name));
            } else if (impl_->info.file_crc != 0 && core::Crc32(*bytes) != impl_->info.file_crc) {
                failure =
                    MakeError(ErrorCode::HashMismatch, "bundle CRC32 does not match the manifest")
                        .WithBundle(std::string(impl_->info.bundle_name))
                        .WithPath(std::string(impl_->info.file_name));
            } else {
                auto reader = serialize::BundleReader::Open(*bytes, impl_->verify);
                if (!reader) {
                    failure = std::move(reader).TakeError();
                } else {
                    auto value = std::move(reader).Value();
                    if (value.Toc().bundle_guid != impl_->info.bundle_guid) {
                        failure =
                            MakeError(ErrorCode::Corrupt, "bundle GUID does not match the manifest")
                                .WithBundle(std::string(impl_->info.bundle_name));
                    } else {
                        auto reader_ptr =
                            std::make_unique<serialize::BundleReader>(std::move(value));
                        std::lock_guard lock(impl_->mutex);
                        impl_->bytes = std::move(bytes);
                        impl_->reader = std::move(reader_ptr);
                        impl_->loaded = true;
                        impl_->loading = false;
                        impl_->last_error = core::Error{ErrorCode::Ok};
                        impl_->condition.notify_all();
                        return {};
                    }
                }
            }
        } else {
            failure = std::move(loaded).TakeError();
        }

        (void)impl_->host->InvalidateBundle(impl_->info);
    }

    {
        std::lock_guard lock(impl_->mutex);
        impl_->loading = false;
        impl_->loaded = false;
        impl_->bytes.reset();
        impl_->reader.reset();
        impl_->last_error = std::move(failure);
        impl_->condition.notify_all();
    }
    return impl_->last_error;
}

void BundleLoader::Unload() noexcept {
    std::lock_guard lock(impl_->mutex);
    impl_->loaded = false;
    impl_->loading = false;
    impl_->bytes.reset();
    impl_->reader.reset();
    impl_->last_error = core::Error{ErrorCode::Ok};
    impl_->condition.notify_all();
}

bool BundleLoader::IsLoaded() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->loaded;
}

bool BundleLoader::IsLoading() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->loading;
}

const core::Error& BundleLoader::LastError() const noexcept { return impl_->last_error; }

void BundleLoader::AddReference() noexcept {
    std::lock_guard lock(impl_->mutex);
    ++impl_->references;
}

bool BundleLoader::ReleaseReference() noexcept {
    std::lock_guard lock(impl_->mutex);
    if (impl_->references > 0) {
        --impl_->references;
    }
    return impl_->references == 0;
}

std::uint32_t BundleLoader::ReferenceCount() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->references;
}

std::uint32_t BundleLoader::BundleIndex() const noexcept { return impl_->index; }

const serialize::ManifestBundleView& BundleLoader::Info() const noexcept { return impl_->info; }

const serialize::BundleToc* BundleLoader::Toc() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->reader ? &impl_->reader->Toc() : nullptr;
}

std::shared_ptr<const std::vector<std::byte>> BundleLoader::Bytes() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->bytes;
}

Result<std::vector<std::byte>> BundleLoader::ReadPayload(std::uint32_t payload_id) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->reader || !impl_->loaded) {
        return MakeError(ErrorCode::NotFound, "bundle is not loaded")
            .WithBundle(std::string(impl_->info.bundle_name));
    }
    for (const auto& payload : impl_->reader->Toc().payloads) {
        if (payload.payload_id == payload_id) {
            return impl_->reader->ReadLogical(payload.logical_offset, payload.size);
        }
    }
    return MakeError(ErrorCode::NotFound, "bundle payload was not found")
        .WithBundle(std::string(impl_->info.bundle_name));
}

Result<std::vector<std::byte>> BundleLoader::ReadRawFile(std::string_view name) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->reader || !impl_->loaded) {
        return MakeError(ErrorCode::NotFound, "bundle is not loaded")
            .WithBundle(std::string(impl_->info.bundle_name));
    }
    for (const auto& raw : impl_->reader->Toc().raw_files) {
        if (raw.name == name) {
            return impl_->reader->ReadLogical(raw.logical_offset, raw.size);
        }
    }
    return MakeError(ErrorCode::NotFound, "raw file was not found in the bundle")
        .WithBundle(std::string(impl_->info.bundle_name))
        .WithPath(std::string(name));
}

BundleHandle::BundleHandle(std::shared_ptr<BundleLoader> loader,
                           const assets::TypeRegistry* registry, assets::IObjectResolver* resolver,
                           std::uint32_t payload_id, std::uint32_t local_object_id)
    : loader_(std::move(loader)), registry_(registry), resolver_(resolver), payload_id_(payload_id),
      local_object_id_(local_object_id) {}

Result<std::shared_ptr<assets::AssetObject>> BundleHandle::LoadAsset() {
    if (!loader_ || registry_ == nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "bundle handle is not initialized");
    }
    auto bytes = loader_->ReadPayload(payload_id_);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    auto decoded = DecodeAssets(bytes.Value(), *registry_, resolver_);
    if (!decoded) {
        return std::move(decoded).TakeError();
    }
    for (std::size_t i = 0; i < decoded.Value().objects.size(); ++i) {
        if (decoded.Value().local_ids[i] == local_object_id_) {
            return decoded.Value().objects[i];
        }
    }
    return decoded.Value().objects.front();
}

Result<std::vector<std::shared_ptr<assets::AssetObject>>> BundleHandle::LoadAllAssets() {
    if (!loader_ || registry_ == nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "bundle handle is not initialized");
    }
    auto bytes = loader_->ReadPayload(payload_id_);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    auto decoded = DecodeAssets(bytes.Value(), *registry_, resolver_);
    if (!decoded) {
        return std::move(decoded).TakeError();
    }
    return std::move(decoded).Value().objects;
}

Result<std::vector<std::shared_ptr<assets::AssetObject>>> BundleHandle::LoadSubAssets() {
    if (!loader_ || registry_ == nullptr) {
        return MakeError(ErrorCode::InvalidArgument, "bundle handle is not initialized");
    }
    auto bytes = loader_->ReadPayload(payload_id_);
    if (!bytes) {
        return std::move(bytes).TakeError();
    }
    auto decoded = DecodeAssets(bytes.Value(), *registry_, resolver_);
    if (!decoded) {
        return std::move(decoded).TakeError();
    }
    std::vector<std::shared_ptr<assets::AssetObject>> result;
    for (const std::uint32_t local_id : decoded.Value().sub_asset_ids) {
        for (std::size_t i = 0; i < decoded.Value().objects.size(); ++i) {
            if (decoded.Value().local_ids[i] == local_id) {
                result.push_back(decoded.Value().objects[i]);
                break;
            }
        }
    }
    return result;
}

Result<std::vector<std::byte>> BundleHandle::LoadRawFile(std::string_view name) {
    if (!loader_) {
        return MakeError(ErrorCode::InvalidArgument, "bundle handle is not initialized");
    }
    return loader_->ReadRawFile(name);
}

void BundleHandle::Unload() noexcept { loader_.reset(); }

std::shared_ptr<BundleLoader> BundleHandle::Loader() const noexcept { return loader_; }

} // namespace parcel::runtime

