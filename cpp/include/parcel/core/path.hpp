#pragma once

#include "parcel/core/result.hpp"

#include <string>
#include <string_view>

namespace parcel::core::path {

[[nodiscard]] std::string Normalize(std::string_view value);
[[nodiscard]] bool IsAbsolute(std::string_view value);
[[nodiscard]] bool IsSafeRelative(std::string_view value);

[[nodiscard]] Result<std::string> Join(std::string_view base, std::string_view child);
[[nodiscard]] Result<std::string> ResolveWithin(std::string_view root, std::string_view relative);
[[nodiscard]] bool IsWithin(std::string_view root, std::string_view child,
                            bool case_insensitive = true);

[[nodiscard]] std::string_view Extension(std::string_view value) noexcept;
[[nodiscard]] std::string_view FileName(std::string_view value) noexcept;
[[nodiscard]] std::string_view Stem(std::string_view value) noexcept;

} // namespace parcel::core::path
