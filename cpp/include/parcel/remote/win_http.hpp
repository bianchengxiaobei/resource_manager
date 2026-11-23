#pragma once

#include "parcel/runtime/download.hpp"

#include <cstdint>
#include <memory>

namespace parcel::remote {

class WinHttpTransport final : public runtime::IHttpTransport {
  public:
    struct Stats {
        std::uint64_t requests = 0;
        std::uint64_t connections_created = 0;
        std::uint64_t connections_reused = 0;
    };

    explicit WinHttpTransport(bool use_system_proxy = false);
    ~WinHttpTransport() override;

    [[nodiscard]] core::Result<runtime::HttpResponse>
    Send(const runtime::HttpRequest& request) override;

    [[nodiscard]] Stats GetStats() const noexcept;

  private:
    struct Impl;

    bool use_system_proxy_ = false;
    std::shared_ptr<Impl> impl_;
};

} // namespace parcel::remote


