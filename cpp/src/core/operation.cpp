#include "parcel/core/operation.hpp"

#include "parcel/core/log.hpp"

#include <algorithm>
#include <utility>

namespace parcel::core {

OperationBase::~OperationBase() {
    for (auto& child : children_) {
        if (child && child->parent_ == this) {
            child->parent_ = nullptr;
        }
    }
}

std::chrono::milliseconds OperationBase::Elapsed() const noexcept {
    if (started_at_ == std::chrono::steady_clock::time_point{}) {
        return std::chrono::milliseconds(0);
    }
    const auto end = IsDone() ? finished_at_ : std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - started_at_);
}

float OperationBase::Progress() const noexcept {
    if (status_ == OperationStatus::Succeeded) {
        return 1.0f;
    }
    if (progress_explicit_) {
        return progress_;
    }
    if (children_.empty()) {
        return progress_;
    }

    float total = 0.0f;
    for (const auto& child : children_) {
        if (child) {
            total += child->Progress();
        }
    }
    return total / static_cast<float>(children_.size());
}

void OperationBase::AddChild(OperationPtr child) {
    if (!child) {
        Log::Write(LogLevel::Error, "operation", "AddChild received a null operation");
        return;
    }
    if (IsDone()) {
        Log::Write(LogLevel::Warn, "operation", "AddChild called on a completed operation");
        return;
    }
    if (child.get() == this) {
        Log::Write(LogLevel::Error, "operation", "operation cannot be its own child");
        return;
    }
    if (child->parent_ != nullptr) {
        Log::Write(LogLevel::Error, "operation", "operation already has a parent");
        return;
    }

    for (OperationBase* ancestor = this; ancestor != nullptr; ancestor = ancestor->parent_) {
        if (ancestor == child.get()) {
            Log::Write(LogLevel::Error, "operation", "operation child would create a cycle");
            return;
        }
    }

    child->parent_ = this;
    children_.push_back(child);
    if (child_observer_) {
        child_observer_(child);
    }
    if (status_ == OperationStatus::Running) {
        child->Start();
    }
}

void OperationBase::OnComplete(OperationCompletion callback) {
    if (!callback) {
        return;
    }
    if (IsDone() && callbacks_invoked_) {
        callback(*this);
        return;
    }
    callbacks_.push_back(std::move(callback));
}

void OperationBase::Start() {
    if (status_ != OperationStatus::None) {
        return;
    }

    started_at_ = std::chrono::steady_clock::now();
    status_ = OperationStatus::Running;
    OnStart();
    if (status_ != OperationStatus::Running) {
        return;
    }

    for (const auto& child : children_) {
        if (child) {
            child->Start();
        }
    }
    TryComplete();
}

void OperationBase::Tick() {
    if (status_ != OperationStatus::Running) {
        return;
    }

    if (cancellation_.IsCancellationRequested()) {
        Cancel();
        return;
    }

    OnTick();
    TryComplete();
}

void OperationBase::Cancel() {
    if (IsDone()) {
        return;
    }
    if (cancellation_.IsCancellationRequested()) {
        for (const auto& child : children_) {
            if (child && !child->IsDone()) {
                child->Cancel();
            }
        }
        return;
    }

    cancellation_.Cancel();
    if (!completion_requested_ || requested_status_ == OperationStatus::Succeeded) {
        completion_requested_ = true;
        requested_status_ = OperationStatus::Canceled;
        requested_error_ = Error(ErrorCode::Canceled, "operation canceled");
    }

    OnCancel();
    for (const auto& child : children_) {
        if (child && !child->IsDone()) {
            child->Cancel();
        }
    }
    TryComplete();
}

void OperationBase::InvokeCompletionCallbacks() {
    if (!IsDone() || callbacks_invoked_) {
        return;
    }

    callbacks_invoked_ = true;
    auto callbacks = std::move(callbacks_);
    callbacks_.clear();
    for (auto& callback : callbacks) {
        if (callback) {
            callback(*this);
        }
    }
}

void OperationBase::SetProgress(float value) noexcept {
    progress_ = std::clamp(value, 0.0f, 1.0f);
    progress_explicit_ = true;
}

void OperationBase::Succeed() {
    if (IsDone() || completion_requested_) {
        return;
    }
    completion_requested_ = true;
    requested_status_ = OperationStatus::Succeeded;
    requested_error_ = Error(ErrorCode::Ok);
    TryComplete();
}

void OperationBase::Fail(Error error) {
    if (IsDone() || completion_requested_) {
        return;
    }

    completion_requested_ = true;
    requested_status_ = OperationStatus::Failed;
    requested_error_ = std::move(error);
    if (requested_error_.code == ErrorCode::Ok) {
        requested_error_.code = ErrorCode::Internal;
    }

    for (const auto& child : children_) {
        if (child && !child->IsDone()) {
            child->Cancel();
        }
    }
    TryComplete();
}

void OperationBase::TryComplete() {
    if (status_ != OperationStatus::Running || !completion_requested_ || !AllChildrenDone()) {
        return;
    }
    Finalize(requested_status_, requested_error_);
}

void OperationBase::Finalize(OperationStatus status, Error error) {
    if (IsDone()) {
        return;
    }

    status_ = status;
    finished_at_ = std::chrono::steady_clock::now();
    if (status_ == OperationStatus::Succeeded) {
        last_error_ = Error(ErrorCode::Ok);
    } else {
        last_error_ = std::move(error);
        if (last_error_.code == ErrorCode::Ok) {
            last_error_.code =
                status_ == OperationStatus::Canceled ? ErrorCode::Canceled : ErrorCode::Internal;
        }
    }

    if (parent_ != nullptr) {
        parent_->OnChildCompleted(*this);
    }
}

void OperationBase::OnChildCompleted(OperationBase& child) {
    if (IsDone()) {
        return;
    }

    if (child.Status() == OperationStatus::Failed) {
        if (!completion_requested_ || requested_status_ == OperationStatus::Succeeded) {
            completion_requested_ = true;
            requested_status_ = OperationStatus::Failed;
            requested_error_ = child.LastError();
            for (const auto& sibling : children_) {
                if (sibling && sibling.get() != &child && !sibling->IsDone()) {
                    sibling->Cancel();
                }
            }
        }
    } else if (child.Status() == OperationStatus::Canceled && !completion_requested_) {
        completion_requested_ = true;
        requested_status_ = OperationStatus::Canceled;
        requested_error_ = Error(ErrorCode::Canceled, "child operation canceled");
        for (const auto& sibling : children_) {
            if (sibling && sibling.get() != &child && !sibling->IsDone()) {
                sibling->Cancel();
            }
        }
    }

    TryComplete();
}

bool OperationBase::AllChildrenDone() const noexcept {
    for (const auto& child : children_) {
        if (child && !child->IsDone()) {
            return false;
        }
    }
    return true;
}

} // namespace parcel::core

