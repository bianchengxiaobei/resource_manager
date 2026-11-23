#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/guid.hpp"
#include "parcel/core/operation.hpp"
#include "parcel/core/result.hpp"
#include "parcel/runtime/asset_types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::runtime {

enum class LoadMethod : std::uint8_t {
    Asset = 0,
    SubAssets = 1,
    AllAssets = 2,
    RawFile = 3,
    BundleFile = 4,
    Scene = 5,
};

[[nodiscard]] constexpr std::string_view ToString(LoadMethod method) noexcept {
    switch (method) {
    case LoadMethod::Asset:
        return "Asset";
    case LoadMethod::SubAssets:
        return "SubAssets";
    case LoadMethod::AllAssets:
        return "AllAssets";
    case LoadMethod::RawFile:
        return "RawFile";
    case LoadMethod::BundleFile:
        return "BundleFile";
    case LoadMethod::Scene:
        return "Scene";
    }
    return "Unknown";
}

struct AssetInfo {
    core::AssetGuid asset_guid;
    std::string address;
    std::string asset_path;
    std::string type_name;
    std::vector<std::string> tags;

    [[nodiscard]] bool IsValid() const noexcept { return !asset_guid.IsZero(); }
};

class HandleState;
class ProviderBase;
struct ResourcePackageImpl;

class AssetHandle {
  public:
    AssetHandle() noexcept = default;
    ~AssetHandle() = default;
    AssetHandle(const AssetHandle&) noexcept = default;
    AssetHandle& operator=(const AssetHandle&) noexcept = default;
    AssetHandle(AssetHandle&&) noexcept = default;
    AssetHandle& operator=(AssetHandle&&) noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept { return IsValid(); }
    [[nodiscard]] bool IsValid() const noexcept;

    [[nodiscard]] core::OperationStatus Status() const noexcept;
    [[nodiscard]] bool IsDone() const noexcept;
    [[nodiscard]] float Progress() const noexcept;
    [[nodiscard]] const core::Error& LastError() const noexcept;
    [[nodiscard]] LoadMethod Method() const noexcept;
    [[nodiscard]] const AssetInfo& Info() const noexcept;

    [[nodiscard]] assets::AssetObject* GetObject() const noexcept;

    template <typename T> [[nodiscard]] T* Get() const noexcept {
        assets::AssetObject* object = GetObject();
        if (object == nullptr) {
            return nullptr;
        }
        if constexpr (assets::AssetTypeOf<T>::value != assets::kInvalidTypeId) {
            if (object->Type() != assets::AssetTypeOf<T>::value) {
                return nullptr;
            }
        }
        return static_cast<T*>(object);
    }

    [[nodiscard]] std::span<const std::shared_ptr<assets::AssetObject>> Objects() const noexcept;
    [[nodiscard]] std::span<const std::byte> Data() const noexcept;
    [[nodiscard]] core::OperationPtr Operation() const noexcept;

    [[nodiscard]] core::Result<void>
    WaitForCompletion(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) const;

    void OnComplete(std::function<void(const AssetHandle&)> callback);
    void OnComplete(std::function<void()> callback);
    void Release() noexcept;

  private:
    friend class HandleState;
    friend class ProviderBase;
    friend struct ResourcePackageImpl;

    explicit AssetHandle(std::shared_ptr<HandleState> state) noexcept;

    std::shared_ptr<HandleState> state_;
};

using SubAssetsHandle = AssetHandle;
using AllAssetsHandle = AssetHandle;
using RawFileHandle = AssetHandle;
using BundleFileHandle = AssetHandle;
using SceneHandle = AssetHandle;

} // namespace parcel::runtime
