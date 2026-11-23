#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace parcel::runtime {

struct RuntimeStats {
    std::size_t packages = 0;
    std::size_t providers = 0;
    std::size_t handles = 0;
    std::size_t bundle_loaders = 0;
    std::size_t loaded_bundles = 0;
    std::size_t pending_operations = 0;
    std::uint64_t bundle_bytes = 0;
    std::uint64_t cache_bytes = 0;
    std::uint64_t download_bytes = 0;
    std::uint64_t downloaded_bytes = 0;
};

struct LeakReport {
    std::vector<std::string> handles;
    std::vector<std::string> providers;
    std::vector<std::string> bundle_loaders;

    [[nodiscard]] bool Empty() const noexcept {
        return handles.empty() && providers.empty() && bundle_loaders.empty();
    }

    [[nodiscard]] std::string ToString() const {
        std::string result;
        for (const auto& item : handles) {
            result += "handle: ";
            result += item;
            result += '\n';
        }
        for (const auto& item : providers) {
            result += "provider: ";
            result += item;
            result += '\n';
        }
        for (const auto& item : bundle_loaders) {
            result += "bundle_loader: ";
            result += item;
            result += '\n';
        }
        return result;
    }
};

} // namespace parcel::runtime
