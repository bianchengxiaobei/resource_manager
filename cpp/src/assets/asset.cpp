#include "parcel/assets/asset.hpp"

#include <utility>

namespace parcel::assets {

Result<void> TypeRegistry::Register(std::shared_ptr<const ITypeSerializer> serializer) {
    if (!serializer) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "asset serializer is null");
    }
    if (serializer->Type() == kInvalidTypeId || serializer->TypeName().empty() ||
        serializer->Version() == 0) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "asset serializer is invalid");
    }
    for (const auto& existing : serializers_) {
        if (existing->Type() == serializer->Type()) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "duplicate asset type id");
        }
        if (existing->TypeName() == serializer->TypeName()) {
            return core::MakeError(core::ErrorCode::InvalidArgument, "duplicate asset type name");
        }
    }
    serializers_.push_back(std::move(serializer));
    return {};
}

const ITypeSerializer* TypeRegistry::Find(TypeId type) const noexcept {
    for (const auto& serializer : serializers_) {
        if (serializer->Type() == type) {
            return serializer.get();
        }
    }
    return nullptr;
}

const ITypeSerializer* TypeRegistry::Find(std::string_view name) const noexcept {
    for (const auto& serializer : serializers_) {
        if (serializer->TypeName() == name) {
            return serializer.get();
        }
    }
    return nullptr;
}

} // namespace parcel::assets
