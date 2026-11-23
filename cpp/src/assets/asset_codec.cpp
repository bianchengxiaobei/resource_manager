#include "parcel/assets/asset_codec.hpp"

#include <cstdint>
#include <unordered_set>
#include <utility>

namespace parcel::assets {
namespace {

using core::ErrorCode;
using core::MakeError;

class WriteContext final : public IAssetWriteContext {
  public:
    explicit WriteContext(serialize::AssetFileWriter& writer) : writer_(writer) {}

    Result<std::uint64_t> AddBlob(std::span<const std::byte> data) override {
        const std::uint64_t id = static_cast<std::uint64_t>(writer_.Description().blobs.size());
        serialize::AssetBlobDesc blob;
        blob.data.assign(data.begin(), data.end());
        writer_.AddBlob(std::move(blob));
        return id;
    }

  private:
    serialize::AssetFileWriter& writer_;
};

class ReadContext final : public IAssetReadContext {
  public:
    ReadContext(const serialize::AssetFileReader& reader, IObjectResolver* resolver)
        : reader_(reader), resolver_(resolver) {}

    Result<std::span<const std::byte>> Blob(std::uint64_t id) const override {
        const auto blobs = reader_.Blobs();
        if (id >= blobs.size()) {
            return MakeError(ErrorCode::Corrupt, "asset blob id is out of range");
        }
        return blobs[static_cast<std::size_t>(id)].data;
    }

    Result<std::shared_ptr<AssetObject>> Resolve(AssetGuid guid) override {
        if (resolver_ == nullptr) {
            return MakeError(ErrorCode::NotFound, "no object resolver is available");
        }
        return resolver_->Resolve(guid);
    }

  private:
    const serialize::AssetFileReader& reader_;
    IObjectResolver* resolver_ = nullptr;
};

} // namespace

Result<serialize::AssetFileDesc> EncodeAsset(const AssetObject& object,
                                             const TypeRegistry& registry) {
    const ITypeSerializer* serializer = registry.Find(object.Type());
    if (serializer == nullptr) {
        return MakeError(ErrorCode::Unsupported, "asset object type is not registered");
    }

    serialize::AssetFileWriter writer;
    serialize::AssetTypeDesc type;
    type.type_id = serializer->Type();
    type.type_name.assign(serializer->TypeName());
    type.serializer_version = serializer->Version();
    writer.AddType(std::move(type));

    WriteContext context(writer);
    serialize::BinaryWriter metadata;
    PARCEL_TRY(serializer->Serialize(object, metadata, context));

    serialize::AssetObjectDesc object_desc;
    object_desc.local_id = 0;
    object_desc.type_id = serializer->Type();
    object_desc.metadata = metadata.Release();
    writer.AddObject(std::move(object_desc));

    std::vector<AssetGuid> references;
    PARCEL_TRY(serializer->CollectReferences(object, references));
    std::unordered_set<core::Guid128, core::Guid128Hash> seen;
    for (const AssetGuid& reference : references) {
        if (reference.IsZero() || !seen.insert(reference).second) {
            continue;
        }
        serialize::AssetExternalRefDesc ref;
        ref.asset_guid = reference;
        writer.AddExternalRef(ref);
    }
    return writer.Description();
}

Result<std::shared_ptr<AssetObject>> DecodeAsset(const serialize::AssetFileReader& reader,
                                                 const TypeRegistry& registry,
                                                 IObjectResolver* resolver) {
    const auto objects = reader.Objects();
    if (objects.empty()) {
        return MakeError(ErrorCode::Corrupt, "asset file has no objects");
    }
    if (objects.size() != 1) {
        return MakeError(ErrorCode::Unsupported, "multiple asset objects are not supported");
    }
    const serialize::AssetObjectView& object_view = objects.front();
    const ITypeSerializer* serializer = registry.Find(object_view.type_id);
    if (serializer == nullptr) {
        return MakeError(ErrorCode::Unsupported, "unknown asset type id");
    }

    bool type_declared = false;
    for (const serialize::AssetTypeView& type_view : reader.Types()) {
        if (type_view.type_id == object_view.type_id) {
            if (type_view.serializer_version != serializer->Version()) {
                return MakeError(ErrorCode::Unsupported, "unsupported asset serializer version");
            }
            type_declared = true;
            break;
        }
    }
    if (!type_declared) {
        return MakeError(ErrorCode::Corrupt, "asset object type is not declared");
    }

    ReadContext context(reader, resolver);
    serialize::BinaryReader metadata(object_view.metadata);
    PARCEL_TRY_DECL(std::shared_ptr<AssetObject>, object,
                    serializer->Deserialize(metadata, context));
    if (!object) {
        return MakeError(ErrorCode::Internal, "asset serializer returned a null object");
    }
    if (object->Type() != object_view.type_id) {
        return MakeError(ErrorCode::Corrupt, "asset object type does not match its serializer");
    }
    if (!metadata.IsAtEnd()) {
        return MakeError(ErrorCode::Corrupt, "asset metadata has trailing bytes");
    }
    return object;
}

Result<std::shared_ptr<AssetObject>> DecodeAssetBytes(std::span<const std::byte> data,
                                                      const TypeRegistry& registry,
                                                      IObjectResolver* resolver,
                                                      bool verify_content_hash) {
    auto opened = serialize::AssetFileReader::Open(data, verify_content_hash);
    if (!opened) {
        return std::move(opened).TakeError();
    }
    serialize::AssetFileReader reader = std::move(opened).Value();
    PARCEL_TRY(reader.ReadAll(verify_content_hash));
    return DecodeAsset(reader, registry, resolver);
}

Result<std::vector<std::byte>> EncodeAssetBytes(const AssetObject& object,
                                                const TypeRegistry& registry) {
    PARCEL_TRY_DECL(serialize::AssetFileDesc, desc, EncodeAsset(object, registry));
    return serialize::WriteAssetFile(desc);
}

} // namespace parcel::assets
