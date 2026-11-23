#include "parcel/runtime/handle.hpp"

#include "runtime_internal.hpp"

#include <algorithm>
#include <utility>

namespace parcel::runtime {
namespace {

const AssetInfo& InvalidInfo() {
    static const AssetInfo info;
    return info;
}

const core::Error& InvalidHandleError() {
    static const core::Error error(core::ErrorCode::InvalidArgument,
                                   "asset handle is invalid or has been released");
    return error;
}

} // namespace

HandleState::HandleState(std::shared_ptr<ResourcePackageImpl> package,
                         std::shared_ptr<ProviderBase> provider, AssetInfo info, LoadMethod method)
    : package_(std::move(package)), provider_(std::move(provider)), info_(std::move(info)),
      method_(method) {}

HandleState::HandleState(std::shared_ptr<ResourcePackageImpl> package, AssetInfo info,
                         core::OperationStatus status, core::Error error,
                         std::shared_ptr<std::vector<std::byte>> standalone_data, LoadMethod method)
    : package_(std::move(package)), info_(std::move(info)), standalone_status_(status),
      standalone_error_(std::move(error)), standalone_data_(std::move(standalone_data)),
      method_(method) {}

HandleState::~HandleState() { Release(); }

bool HandleState::IsValid() const noexcept { return !released_.load(); }

bool HandleState::IsReleased() const noexcept { return released_.load(); }

core::OperationStatus HandleState::Status() const noexcept {
    if (released_.load()) {
        return core::OperationStatus::None;
    }
    if (provider_) {
        return provider_->Status();
    }
    return standalone_status_;
}

bool HandleState::IsDone() const noexcept {
    const core::OperationStatus status = Status();
    return status == core::OperationStatus::Succeeded || status == core::OperationStatus::Failed ||
           status == core::OperationStatus::Canceled;
}

float HandleState::Progress() const noexcept {
    if (released_.load()) {
        return 0.0f;
    }
    if (provider_) {
        return provider_->Progress();
    }
    return standalone_status_ == core::OperationStatus::Succeeded ? 1.0f : 0.0f;
}

const core::Error& HandleState::LastError() const noexcept {
    if (released_.load()) {
        return InvalidHandleError();
    }
    if (provider_) {
        return provider_->LastError();
    }
    return standalone_error_;
}

assets::AssetObject* HandleState::Object() const noexcept {
    if (released_.load() || !provider_ || provider_->IsInvalidated()) {
        return nullptr;
    }
    return provider_->Object();
}

std::span<const std::shared_ptr<assets::AssetObject>> HandleState::Objects() const noexcept {
    static const std::vector<std::shared_ptr<assets::AssetObject>> empty;
    if (released_.load() || !provider_ || provider_->IsInvalidated()) {
        return empty;
    }
    return provider_->Objects();
}

std::span<const std::byte> HandleState::Data() const noexcept {
    static const std::vector<std::byte> empty;
    if (released_.load()) {
        return empty;
    }
    if (provider_) {
        return provider_->Data();
    }
    if (standalone_data_) {
        return *standalone_data_;
    }
    return empty;
}

core::OperationPtr HandleState::Operation() const noexcept {
    if (released_.load() || !provider_) {
        return nullptr;
    }
    return provider_;
}

core::Result<void> HandleState::Wait(std::chrono::milliseconds timeout) const {
    if (released_.load()) {
        return InvalidHandleError();
    }
    if (provider_) {
        if (!package_) {
            return core::MakeError(core::ErrorCode::Internal, "asset handle has no package");
        }
        return package_->WaitProvider(provider_, timeout);
    }
    if (standalone_status_ == core::OperationStatus::Failed) {
        return standalone_error_;
    }
    if (standalone_status_ == core::OperationStatus::Canceled) {
        return core::MakeError(core::ErrorCode::Canceled, "asset operation was canceled");
    }
    return {};
}

void HandleState::OnComplete(std::function<void(const AssetHandle&)> callback) {
    if (!callback) {
        return;
    }
    if (IsDone()) {
        callback(AssetHandle(shared_from_this()));
        return;
    }
    std::lock_guard lock(mutex_);
    handle_callbacks_.push_back(std::move(callback));
}

void HandleState::OnComplete(std::function<void()> callback) {
    if (!callback) {
        return;
    }
    if (IsDone()) {
        callback();
        return;
    }
    std::lock_guard lock(mutex_);
    callbacks_.push_back(std::move(callback));
}

void HandleState::Notify() {
    std::vector<std::function<void(const AssetHandle&)>> handle_callbacks;
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard lock(mutex_);
        if (notified_ || released_.load()) {
            return;
        }
        notified_ = true;
        handle_callbacks = std::move(handle_callbacks_);
        callbacks = std::move(callbacks_);
        handle_callbacks_.clear();
        callbacks_.clear();
    }

    AssetHandle handle(shared_from_this());
    for (auto& callback : handle_callbacks) {
        if (callback) {
            callback(handle);
        }
    }
    for (auto& callback : callbacks) {
        if (callback) {
            callback();
        }
    }
}

void HandleState::Release() noexcept {
    if (released_.exchange(true)) {
        return;
    }

    std::shared_ptr<ProviderBase> provider;
    {
        std::lock_guard lock(mutex_);
        provider = std::move(provider_);
        package_.reset();
        standalone_data_.reset();
        handle_callbacks_.clear();
        callbacks_.clear();
    }
    if (provider) {
        provider->ReleaseHandle();
    }
}

AssetHandle::AssetHandle(std::shared_ptr<HandleState> state) noexcept : state_(std::move(state)) {}

bool AssetHandle::IsValid() const noexcept { return state_ != nullptr && state_->IsValid(); }

core::OperationStatus AssetHandle::Status() const noexcept {
    return state_ ? state_->Status() : core::OperationStatus::None;
}

bool AssetHandle::IsDone() const noexcept { return state_ ? state_->IsDone() : false; }

float AssetHandle::Progress() const noexcept { return state_ ? state_->Progress() : 0.0f; }

const core::Error& AssetHandle::LastError() const noexcept {
    return state_ ? state_->LastError() : InvalidHandleError();
}

LoadMethod AssetHandle::Method() const noexcept {
    return state_ ? state_->Method() : LoadMethod::Asset;
}

const AssetInfo& AssetHandle::Info() const noexcept {
    return state_ ? state_->Info() : InvalidInfo();
}

assets::AssetObject* AssetHandle::GetObject() const noexcept {
    return state_ ? state_->Object() : nullptr;
}

std::span<const std::shared_ptr<assets::AssetObject>> AssetHandle::Objects() const noexcept {
    return state_ ? state_->Objects() : std::span<const std::shared_ptr<assets::AssetObject>>{};
}

std::span<const std::byte> AssetHandle::Data() const noexcept {
    return state_ ? state_->Data() : std::span<const std::byte>{};
}

core::OperationPtr AssetHandle::Operation() const noexcept {
    return state_ ? state_->Operation() : nullptr;
}

core::Result<void> AssetHandle::WaitForCompletion(std::chrono::milliseconds timeout) const {
    if (!state_) {
        return InvalidHandleError();
    }
    return state_->Wait(timeout);
}

void AssetHandle::OnComplete(std::function<void(const AssetHandle&)> callback) {
    if (state_) {
        state_->OnComplete(std::move(callback));
    }
}

void AssetHandle::OnComplete(std::function<void()> callback) {
    if (state_) {
        state_->OnComplete(std::move(callback));
    }
}

void AssetHandle::Release() noexcept {
    if (state_) {
        state_->Release();
    }
}

} // namespace parcel::runtime
