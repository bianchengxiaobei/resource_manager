#include "parcel/core/path.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace parcel::core::path {
namespace {

bool IsDriveLetter(std::string_view value) noexcept {
    return value.size() >= 2 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
           value[1] == ':';
}

void AppendSegment(std::string& output, std::string_view segment) {
    if (segment.empty() || segment == ".") {
        return;
    }
    if (segment == "..") {
        const auto slash = output.find_last_of('/');
        if (slash == std::string::npos) {
            if (!output.empty() && output != "..") {
                output.clear();
            } else if (output.empty()) {
                output = "..";
            } else {
                output += "/..";
            }
        } else if (output == "..") {
            output += "/..";
        } else {
            output.erase(slash);
        }
        return;
    }

    if (!output.empty() && output.back() != '/') {
        output.push_back('/');
    }
    output.append(segment);
}

std::string ToLowerAscii(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

} // namespace

std::string Normalize(std::string_view value) {
    if (value.empty()) {
        return {};
    }

    std::string input(value);
    std::replace(input.begin(), input.end(), '\\', '/');

    std::string prefix;
    std::size_t offset = 0;
    if (IsDriveLetter(input)) {
        prefix.assign(input, 0, 2);
        offset = 2;
    } else if (input.rfind("//", 0) == 0) {
        prefix = "//";
        offset = 2;
    } else if (input.front() == '/') {
        prefix = "/";
        offset = 1;
    }

    std::string output;
    std::size_t start = offset;
    while (start <= input.size()) {
        const std::size_t slash = input.find('/', start);
        const std::size_t end = slash == std::string::npos ? input.size() : slash;
        AppendSegment(output, std::string_view(input).substr(start, end - start));
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }

    if (prefix.empty()) {
        return output;
    }
    if (output.empty()) {
        return prefix;
    }
    if (prefix == "//") {
        return "//" + output;
    }
    if (prefix == "/") {
        return "/" + output;
    }
    return prefix + "/" + output;
}

bool IsAbsolute(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    if (value.front() == '/' || value.front() == '\\') {
        return true;
    }
    return IsDriveLetter(value);
}

bool IsSafeRelative(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    if (value.find('\0') != std::string_view::npos) {
        return false;
    }
    if (IsAbsolute(value)) {
        return false;
    }

    const std::string normalized = Normalize(value);
    if (normalized.empty() || normalized == ".." || normalized.rfind("../", 0) == 0) {
        return false;
    }
    if (normalized.find(":/") != std::string::npos || normalized.find(':') != std::string::npos) {
        return false;
    }
    return true;
}

Result<std::string> Join(std::string_view base, std::string_view child) {
    if (IsAbsolute(child)) {
        return MakeError(ErrorCode::InvalidArgument, "child path must be relative");
    }
    if (!IsSafeRelative(child)) {
        return MakeError(ErrorCode::InvalidArgument, "child path escapes its base");
    }

    const std::string normalized_base = Normalize(base);
    const std::string normalized_child = Normalize(child);
    if (normalized_base.empty()) {
        return normalized_child;
    }
    if (normalized_base == "/" || normalized_base == "//") {
        return normalized_base + normalized_child;
    }
    if (normalized_base.back() == '/') {
        return normalized_base + normalized_child;
    }
    return normalized_base + "/" + normalized_child;
}

Result<std::string> ResolveWithin(std::string_view root, std::string_view relative) {
    if (!IsSafeRelative(relative)) {
        return MakeError(ErrorCode::InvalidArgument, "relative path is not safe")
            .WithPath(std::string(relative));
    }

    const std::string normalized_root = Normalize(root);
    auto joined = Join(normalized_root, relative);
    if (!joined) {
        return std::move(joined).TakeError();
    }

    const std::string& resolved = joined.Value();
    if (!normalized_root.empty() && !IsWithin(normalized_root, resolved)) {
        return MakeError(ErrorCode::InvalidArgument, "relative path escapes the root")
            .WithPath(std::string(relative));
    }
    return resolved;
}

bool IsWithin(std::string_view root, std::string_view child, bool case_insensitive) {
    std::string normalized_root = Normalize(root);
    std::string normalized_child = Normalize(child);
    if (case_insensitive) {
        normalized_root = ToLowerAscii(normalized_root);
        normalized_child = ToLowerAscii(normalized_child);
    }

    if (normalized_root.empty()) {
        return !IsAbsolute(normalized_child) && normalized_child != ".." &&
               normalized_child.rfind("../", 0) != 0;
    }
    if (normalized_root == "/" || normalized_root == "//") {
        return normalized_child.rfind(normalized_root, 0) == 0;
    }
    if (normalized_child == normalized_root) {
        return true;
    }
    if (normalized_child.size() <= normalized_root.size()) {
        return false;
    }
    if (normalized_child.compare(0, normalized_root.size(), normalized_root) != 0) {
        return false;
    }
    return normalized_child[normalized_root.size()] == '/';
}

std::string_view Extension(std::string_view value) noexcept {
    const std::size_t slash = value.find_last_of("/\\");
    const std::size_t dot = value.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0 ||
        (slash != std::string_view::npos && dot < slash) || dot + 1 == value.size() ||
        (slash != std::string_view::npos && dot == slash + 1)) {
        return {};
    }
    return value.substr(dot);
}

std::string_view FileName(std::string_view value) noexcept {
    const std::size_t slash = value.find_last_of("/\\");
    if (slash == std::string_view::npos) {
        return value;
    }
    return value.substr(slash + 1);
}

std::string_view Stem(std::string_view value) noexcept {
    const std::string_view name = FileName(value);
    const std::string_view extension = Extension(name);
    if (extension.empty()) {
        return name;
    }
    return name.substr(0, name.size() - extension.size());
}

} // namespace parcel::core::path
