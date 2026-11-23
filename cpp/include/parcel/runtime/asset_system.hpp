#pragma once

#include "parcel/core/operation_scheduler.hpp"
#include "parcel/core/result.hpp"
#include "parcel/runtime/diagnostics.hpp"
#include "parcel/runtime/resource_package.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace parcel::runtime {

struct AssetSystemImpl;

struct AssetSystemOptions {
    std::size_t worker_threads = 0;
    std::chrono::nanoseconds frame_budget = std::chrono::milliseconds(4);
    int max_concurrent_bundle_loads = 4;
};

class AssetSystem final {
  public:
    AssetSystem();
    ~AssetSystem();

    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;

    [[nodiscard]] core::Result<void> Initialize(const AssetSystemOptions& options = {});
    void Shutdown() noexcept;
    [[nodiscard]] bool IsInitialized() const noexcept;

    void Update();
    void Update(std::chrono::nanoseconds budget);
    void SetFrameBudget(std::chrono::nanoseconds budget) noexcept;
    [[nodiscard]] std::chrono::nanoseconds FrameBudget() const noexcept;

    [[nodiscard]] core::Result<ResourcePackage*>
    CreatePackage(const ResourcePackageOptions& options);
    [[nodiscard]] core::Result<ResourcePackage*>
    CreatePackage(std::string_view name, const ResourcePackageOptions& options);
    [[nodiscard]] ResourcePackage* GetPackage(std::string_view name) const noexcept;
    [[nodiscard]] std::shared_ptr<ResourcePackage>
    FindPackage(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<std::string> PackageNames() const;
    [[nodiscard]] core::Result<void> DestroyPackage(std::string_view name);

    [[nodiscard]] core::OperationScheduler& Scheduler() noexcept;
    [[nodiscard]] const core::OperationScheduler& Scheduler() const noexcept;
    [[nodiscard]] core::Result<void> StartOperation(core::OperationPtr operation);

    [[nodiscard]] RuntimeStats Stats() const;
    [[nodiscard]] LeakReport Leaks() const;
    [[nodiscard]] std::string LeakReportText() const;

  private:
    std::shared_ptr<AssetSystemImpl> impl_;
};

} // namespace parcel::runtime
