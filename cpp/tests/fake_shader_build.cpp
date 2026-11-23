#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool NextValue(int argc, char** argv, int& index, std::string& value) {
    if (index + 1 >= argc) {
        return false;
    }
    value = argv[++index];
    return true;
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

} // namespace

int main(int argc, char** argv) {
    std::string root;
    std::string shaderc;
    std::string bgfx_src;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--root") {
            if (!NextValue(argc, argv, i, root)) {
                return 2;
            }
        } else if (argument == "--shaderc") {
            if (!NextValue(argc, argv, i, shaderc)) {
                return 2;
            }
        } else if (argument == "--bgfx-src") {
            if (!NextValue(argc, argv, i, bgfx_src)) {
                return 2;
            }
        }
    }
    if (root.empty() || shaderc.empty() || bgfx_src.empty()) {
        std::cerr << "fake shader build: missing required arguments\n";
        return 2;
    }
    const std::filesystem::path root_path(root);
    const std::filesystem::path shader_dir = root_path / "shaders";
    std::error_code error;
    std::filesystem::create_directories(shader_dir / "build" / "dxbc", error);
    if (error) {
        std::cerr << "fake shader build: failed to create output directories\n";
        return 3;
    }

    const std::filesystem::path fixture_root = root_path.parent_path();
    std::ofstream arguments_file(fixture_root / "shader_build_args.txt", std::ios::binary);
    for (int i = 1; i < argc; ++i) {
        arguments_file << argv[i] << '\n';
    }

    if (std::filesystem::is_regular_file(fixture_root / "shader_build_fail.txt", error)) {
        std::cerr << "fake shader build failure\n";
        return 7;
    }

    WriteText(shader_dir / "build" / "dxbc" / "forward.vs.bin", "fake vertex");
    WriteText(shader_dir / "build" / "dxbc" / "forward.fs.bin", "fake fragment");
    WriteText(shader_dir / "manifest.json", R"json({
  "version": 1,
  "passes": {
    "dxbc": {
      "forward": {
        "vertex": "build/dxbc/forward.vs.bin",
        "fragments": ["build/dxbc/forward.fs.bin"]
      }
    }
  }
})json");
    return 0;
}
