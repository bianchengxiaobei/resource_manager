#include <catch_amalgamated.hpp>

#include "parcel/core/path.hpp"

using namespace parcel::core::path;

TEST_CASE("Path normalization is lexical and slash-normalized", "[core][path]") {
    REQUIRE(Normalize("a\\b/./c") == "a/b/c");
    REQUIRE(Normalize("/a/../b") == "/b");
    REQUIRE(Normalize("a/../../b") == "../b");
    REQUIRE(Normalize("C:\\Games\\Parcel") == "C:/Games/Parcel");
    REQUIRE(Normalize("a//b/") == "a/b");
}

TEST_CASE("Absolute and relative path detection", "[core][path]") {
    REQUIRE(IsAbsolute("/a"));
    REQUIRE(IsAbsolute("\\a"));
    REQUIRE(IsAbsolute("C:/a"));
    REQUIRE_FALSE(IsAbsolute("a/b"));

    REQUIRE(IsSafeRelative("textures/a.png"));
    REQUIRE(IsSafeRelative("a/../b"));
    REQUIRE_FALSE(IsSafeRelative("../a"));
    REQUIRE_FALSE(IsSafeRelative("a/../../b"));
    REQUIRE_FALSE(IsSafeRelative("C:/a"));
    REQUIRE_FALSE(IsSafeRelative("/a"));
    REQUIRE_FALSE(IsSafeRelative("foo:bar"));
}

TEST_CASE("Join and ResolveWithin reject escapes", "[core][path]") {
    auto joined = Join("root", "a/b");
    REQUIRE(joined.HasValue());
    REQUIRE(joined.Value() == "root/a/b");

    auto joined_root = Join("/", "a");
    REQUIRE(joined_root.HasValue());
    REQUIRE(joined_root.Value() == "/a");

    REQUIRE_FALSE(Join("root", "../a").HasValue());

    auto resolved = ResolveWithin("C:/root", "a/../b");
    REQUIRE(resolved.HasValue());
    REQUIRE(resolved.Value() == "C:/root/b");

    REQUIRE_FALSE(ResolveWithin("C:/root", "../b").HasValue());
    REQUIRE_FALSE(ResolveWithin("C:/root", "C:/other").HasValue());
}

TEST_CASE("IsWithin uses path component boundaries", "[core][path]") {
    REQUIRE(IsWithin("C:/root", "C:/root"));
    REQUIRE(IsWithin("C:/root", "C:/root/a"));
    REQUIRE(IsWithin("C:/root", "c:/ROOT/a"));
    REQUIRE_FALSE(IsWithin("C:/root", "C:/root2"));
    REQUIRE(IsWithin("/", "/a"));
    REQUIRE_FALSE(IsWithin("C:/root", "C:/other/a"));
}

TEST_CASE("File name and extension helpers", "[core][path]") {
    REQUIRE(FileName("a/b/c.txt") == "c.txt");
    REQUIRE(Extension("a/b/c.txt") == ".txt");
    REQUIRE(Stem("a/b/c.txt") == "c");
    REQUIRE(Extension(".gitignore").empty());
    REQUIRE(Extension("a/b/c").empty());
    REQUIRE(Stem("a/b/c") == "c");
}
