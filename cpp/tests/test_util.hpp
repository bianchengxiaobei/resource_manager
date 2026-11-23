#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace parcel::test {

inline std::string ToUtf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

inline std::string TempRootName(std::string_view tag) {
    static std::uint64_t counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return "parcel_test_" + std::string(tag) + "_" + std::to_string(now) + "_" +
           std::to_string(counter++);
}

class TempDir {
  public:
    explicit TempDir(std::string_view tag = "tmp")
        : path_(std::filesystem::temp_directory_path() / TempRootName(tag)) {
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

} // namespace parcel::test
