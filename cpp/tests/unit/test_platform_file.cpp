#include <catch_amalgamated.hpp>

#include "parcel/core/platform_file.hpp"
#include "test_util.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

using parcel::core::FileStat;
using parcel::core::PlatformFile;
using parcel::core::ReadAll;
using parcel::core::WriteAll;

namespace {

std::span<const std::byte> Bytes(std::string_view value) {
    return std::as_bytes(std::span(value.data(), value.size()));
}

std::string Text(std::span<const std::byte> value) {
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

} // namespace

TEST_CASE("PlatformFile writes, reads, stats and renames files", "[core][platform_file]") {
    parcel::test::TempDir temp("file");
    PlatformFile filesystem;

    const auto source = temp.Path() / "source.txt";
    const auto target = temp.Path() / "renamed.txt";
    const std::string payload = "Parcel platform file test";

    auto write_result = filesystem.OpenWrite(parcel::test::ToUtf8(source));
    REQUIRE(write_result.HasValue());
    {
        auto& file = *write_result.Value();
        REQUIRE(WriteAll(file, Bytes(payload)).HasValue());
        REQUIRE(file.Flush().HasValue());
        file.Close();
        REQUIRE_FALSE(file.IsOpen());
    }

    REQUIRE(filesystem.Exists(parcel::test::ToUtf8(source)).Value());
    REQUIRE(filesystem.Size(parcel::test::ToUtf8(source)).Value() == payload.size());

    auto stat = filesystem.Stat(parcel::test::ToUtf8(source));
    REQUIRE(stat.HasValue());
    REQUIRE(stat.Value().exists);
    REQUIRE_FALSE(stat.Value().is_directory);
    REQUIRE(stat.Value().size == payload.size());

    auto read_result = filesystem.OpenRead(parcel::test::ToUtf8(source));
    REQUIRE(read_result.HasValue());
    auto& read_file = *read_result.Value();

    auto all = ReadAll(read_file);
    REQUIRE(all.HasValue());
    REQUIRE(Text(all.Value()) == payload);

    std::array<std::byte, 6> prefix{};
    auto prefix_result = read_file.ReadAt(0, prefix);
    REQUIRE(prefix_result.HasValue());
    REQUIRE(prefix_result.Value() == prefix.size());
    REQUIRE(Text(prefix) == "Parcel");

    REQUIRE(
        filesystem.Rename(parcel::test::ToUtf8(source), parcel::test::ToUtf8(target)).HasValue());
    REQUIRE_FALSE(filesystem.Exists(parcel::test::ToUtf8(source)).Value());
    REQUIRE(filesystem.Exists(parcel::test::ToUtf8(target)).Value());

    REQUIRE(filesystem.Delete(parcel::test::ToUtf8(target)).HasValue());
    REQUIRE_FALSE(filesystem.Exists(parcel::test::ToUtf8(target)).Value());
}

TEST_CASE("PlatformFile handles Unicode paths and append mode", "[core][platform_file]") {
    parcel::test::TempDir temp("unicode");
    PlatformFile filesystem;

    const auto path = temp.Path() / u8"测试-文件.bin";
    auto write_result = filesystem.OpenWrite(parcel::test::ToUtf8(path));
    REQUIRE(write_result.HasValue());
    {
        auto& file = *write_result.Value();
        REQUIRE(WriteAll(file, Bytes("first")).HasValue());
        file.Close();
    }

    auto append_result = filesystem.OpenWrite(parcel::test::ToUtf8(path), true);
    REQUIRE(append_result.HasValue());
    {
        auto& file = *append_result.Value();
        REQUIRE(WriteAll(file, Bytes("-second")).HasValue());
    }

    auto read_result = filesystem.OpenRead(parcel::test::ToUtf8(path));
    REQUIRE(read_result.HasValue());
    auto all = ReadAll(*read_result.Value());
    REQUIRE(all.HasValue());
    REQUIRE(Text(all.Value()) == "first-second");
}

TEST_CASE("PlatformFile reports missing paths and free space", "[core][platform_file]") {
    parcel::test::TempDir temp("errors");
    PlatformFile filesystem;

    const auto missing = temp.Path() / "missing.txt";
    const auto exists = filesystem.Exists(parcel::test::ToUtf8(missing));
    REQUIRE(exists.HasValue());
    REQUIRE_FALSE(exists.Value());

    REQUIRE_FALSE(filesystem.OpenRead(parcel::test::ToUtf8(missing)).HasValue());
    REQUIRE_FALSE(filesystem.Stat(parcel::test::ToUtf8(missing)).HasValue());
    REQUIRE_FALSE(filesystem.Delete(parcel::test::ToUtf8(missing)).HasValue());

    const auto free_space = filesystem.FreeSpace(parcel::test::ToUtf8(temp.Path()));
    REQUIRE(free_space.HasValue());
    REQUIRE(free_space.Value() > 0);
}

TEST_CASE("PlatformFile creates nested directories", "[core][platform_file]") {
    parcel::test::TempDir temp("directories");
    PlatformFile filesystem;

    const auto nested = temp.Path() / "a" / "b" / "c";
    REQUIRE(filesystem.CreateDirectories(parcel::test::ToUtf8(nested)).HasValue());

    const auto stat = filesystem.Stat(parcel::test::ToUtf8(nested));
    REQUIRE(stat.HasValue());
    REQUIRE(stat.Value().is_directory);
}

TEST_CASE("PlatformFile supports paths longer than MAX_PATH", "[core][platform_file]") {
    parcel::test::TempDir temp("long_path");
    PlatformFile filesystem;

    std::filesystem::path deep = temp.Path();
    for (int i = 0; i < 12; ++i) {
        deep /= std::string(24, static_cast<char>('a' + i));
    }
    REQUIRE(parcel::test::ToUtf8(deep).size() > 260);

    REQUIRE(filesystem.CreateDirectories(parcel::test::ToUtf8(deep)).HasValue());
    const auto file_path = deep / "payload.txt";

    auto write_result = filesystem.OpenWrite(parcel::test::ToUtf8(file_path));
    REQUIRE(write_result.HasValue());
    {
        auto& file = *write_result.Value();
        REQUIRE(WriteAll(file, Bytes("long path")).HasValue());
        file.Close();
    }

    const auto exists = filesystem.Exists(parcel::test::ToUtf8(file_path));
    REQUIRE(exists.HasValue());
    REQUIRE(exists.Value());
    REQUIRE(filesystem.Delete(parcel::test::ToUtf8(file_path)).HasValue());
}
