#pragma once

#include "parcel/import/importer.hpp"

namespace parcel::import {

[[nodiscard]] core::Result<ImporterRegistry> CreateStandardImporterRegistry();

} // namespace parcel::import
