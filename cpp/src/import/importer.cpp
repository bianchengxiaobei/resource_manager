#include "parcel/import/importer.hpp"

#include "parcel/core/error.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace parcel::import {
namespace {

std::string NormalizeExtension(std::string_view extension) {
    if (extension.empty()) {
        return {};
    }
    std::string result(extension);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (result.front() != '.') {
        result.insert(result.begin(), '.');
    }
    return result;
}

} // namespace

core::Result<void> ImporterRegistry::Register(std::shared_ptr<const IAssetImporter> importer) {
    if (!importer) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "importer is null");
    }
    if (importer->Name().empty()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "importer name is empty");
    }
    if (importer->Version() == 0) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "importer version is zero");
    }
    const auto duplicate =
        std::find_if(importers_.begin(), importers_.end(),
                     [&](const auto& existing) { return existing->Name() == importer->Name(); });
    if (duplicate != importers_.end()) {
        return core::MakeError(core::ErrorCode::InvalidArgument, "duplicate importer name");
    }
    importers_.push_back(std::move(importer));
    return {};
}

const IAssetImporter* ImporterRegistry::FindByName(std::string_view name) const noexcept {
    const auto found = std::find_if(importers_.begin(), importers_.end(),
                                    [&](const auto& importer) { return importer->Name() == name; });
    return found == importers_.end() ? nullptr : found->get();
}

const IAssetImporter*
ImporterRegistry::FindForExtension(std::string_view extension) const noexcept {
    const std::string normalized = NormalizeExtension(extension);
    if (normalized.empty()) {
        return nullptr;
    }
    for (const auto& importer : importers_) {
        if (importer->Supports(normalized)) {
            return importer.get();
        }
    }
    return nullptr;
}

} // namespace parcel::import
