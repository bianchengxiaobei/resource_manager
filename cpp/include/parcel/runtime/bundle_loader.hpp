#pragma once

#include "parcel/assets/asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/result.hpp"
#include "parcel/runtime/file_system.hpp"
#include "parcel/serialize/bundle.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::runtime {

class IBundleHandle {
  public:
    virtual ~IBundleHandle() = default;

    [[nodiscard]] virtual core::Result<std::shared_ptr<assets::AssetObject>> LoadAsset() = 0;
    [[nodiscard]] virtual core::Result<std::vector<std::shared_ptr<assets::AssetObject>>>
    LoadAllAssets() = 0;
    [[nodiscard]] virtual core::Result<std::vector<std::shared_ptr<assets::AssetObject>>>
    LoadSubAssets() = 0;
    [[nodiscard]] virtual core::Result<std::vector<std::byte>>
    LoadRawFile(std::string_view name) = 0;
    virtual void Unload() noexcept = 0;
};

class BundleLoader final {
  public:
    BundleLoader(std::shared_ptr<FileSystemHost> host, ManifestPtr manifest,
                 std::uint32_t bundle_index, bool verify_file = true);
    ~BundleLoader();

    BundleLoader(const BundleLoader&) = delete;
    BundleLoader& operator=(const BundleLoader&) = delete;

    [[nodiscard]] core::Result<void> Load();
    void Unload() noexcept;

    [[nodiscard]] bool IsLoaded() const noexcept;
    [[nodiscard]] bool IsLoading() const noexcept;
    [[nodiscard]] const core::Error& LastError() const noexcept;

    void AddReference() noexcept;
    [[nodiscard]] bool ReleaseReference() noexcept;
    [[nodiscard]] std::uint32_t ReferenceCount() const noexcept;

    [[nodiscard]] std::uint32_t BundleIndex() const noexcept;
    [[nodiscard]] const serialize::ManifestBundleView& Info() const noexcept;
    [[nodiscard]] const serialize::BundleToc* Toc() const noexcept;
    [[nodiscard]] std::shared_ptr<const std::vector<std::byte>> Bytes() const noexcept;

    [[nodiscard]] core::Result<std::vector<std::byte>> ReadPayload(std::uint32_t payload_id);
    [[nodiscard]] core::Result<std::vector<std::byte>> ReadRawFile(std::string_view name);
    [[nodiscard]] core::Result<std::vector<std::byte>>
    ReadRawFileRange(std::string_view name, std::uint64_t offset, std::uint64_t size);

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

class BundleHandle final : public IBundleHandle {
  public:
    BundleHandle(std::shared_ptr<BundleLoader> loader, const assets::TypeRegistry* registry,
                 assets::IObjectResolver* resolver, std::uint32_t payload_id,
                 std::uint32_t local_object_id = 0);
    ~BundleHandle() override = default;

    [[nodiscard]] core::Result<std::shared_ptr<assets::AssetObject>> LoadAsset() override;
    [[nodiscard]] core::Result<std::vector<std::shared_ptr<assets::AssetObject>>>
    LoadAllAssets() override;
    [[nodiscard]] core::Result<std::vector<std::shared_ptr<assets::AssetObject>>>
    LoadSubAssets() override;
    [[nodiscard]] core::Result<std::vector<std::byte>> LoadRawFile(std::string_view name) override;
    void Unload() noexcept override;

    [[nodiscard]] std::shared_ptr<BundleLoader> Loader() const noexcept;

  private:
    std::shared_ptr<BundleLoader> loader_;
    const assets::TypeRegistry* registry_ = nullptr;
    assets::IObjectResolver* resolver_ = nullptr;
    std::uint32_t payload_id_ = 0;
    std::uint32_t local_object_id_ = 0;
};

} // namespace parcel::runtime
