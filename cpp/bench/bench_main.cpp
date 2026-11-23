#include "parcel/core/guid.hpp"
#include "parcel/core/hash.hpp"
#include "parcel/serialize/asset_file.hpp"
#include "parcel/serialize/bundle.hpp"
#include "parcel/runtime/file_system.hpp"
#include "parcel/runtime/asset_system.hpp"
#include "parcel/runtime/resource_package.hpp"
#include "parcel/serialize/manifest.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

using parcel::core::AssetGuid;
using parcel::core::Sha256;
using parcel::serialize::AssetFileDesc;
using parcel::serialize::AssetFileReader;
using parcel::serialize::AssetObjectDesc;
using parcel::serialize::AssetTypeDesc;
using parcel::serialize::BundlePayloadDesc;
using parcel::serialize::BundleReader;
using parcel::serialize::BundleToc;
using parcel::serialize::BundleWriteOptions;
using parcel::serialize::CompressionCodec;
using parcel::serialize::ManifestAssetDesc;
using parcel::serialize::ManifestBundleDesc;
using parcel::serialize::ManifestDesc;
using parcel::serialize::ManifestView;
using parcel::serialize::WriteAssetFile;
using parcel::serialize::WriteBundle;
using parcel::serialize::WriteManifest;

namespace {

using Clock = std::chrono::steady_clock;

AssetGuid Guid(std::uint32_t index) {
    AssetGuid guid;
    guid.bytes[0] = 0x50;
    guid.bytes[1] = 0x41;
    guid.bytes[2] = 0x52;
    guid.bytes[3] = 0x43;
    guid.bytes[8] = static_cast<std::uint8_t>(index >> 24U);
    guid.bytes[9] = static_cast<std::uint8_t>(index >> 16U);
    guid.bytes[10] = static_cast<std::uint8_t>(index >> 8U);
    guid.bytes[11] = static_cast<std::uint8_t>(index);
    guid.bytes[15] = 0x01;
    return guid;
}

template <typename Function> [[nodiscard]] double MedianMs(Function&& function, int iterations) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(iterations));
    for (int i = 0; i < iterations; ++i) {
        const auto start = Clock::now();
        function();
        const auto elapsed = Clock::now() - start;
        samples.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2U];
}

ManifestDesc MakeManifest(std::size_t assets, std::size_t bundles) {
    ManifestDesc desc;
    desc.package_name = "benchmark";
    desc.package_version = "1.0.0";
    desc.bundles.reserve(bundles);
    for (std::size_t i = 0; i < bundles; ++i) {
        ManifestBundleDesc bundle;
        bundle.bundle_guid = Guid(0x01000000U + static_cast<std::uint32_t>(i));
        bundle.bundle_name = "bundle_" + std::to_string(i);
        bundle.file_name = bundle.bundle_name + ".bundle";
        bundle.file_size = 4096U + i;
        bundle.file_sha256 = Sha256(bundle.file_name);
        desc.bundles.push_back(std::move(bundle));
    }
    desc.assets.reserve(assets);
    for (std::size_t i = 0; i < assets; ++i) {
        ManifestAssetDesc asset;
        asset.asset_guid = Guid(0x02000000U + static_cast<std::uint32_t>(i));
        asset.address = "address/" + std::to_string(i);
        asset.asset_path = "Assets/asset_" + std::to_string(i) + ".prefab";
        asset.type_name = "parcel.BenchmarkAsset";
        asset.bundle_id = static_cast<std::uint32_t>(i % bundles);
        asset.payload_id = static_cast<std::uint32_t>(i);
        desc.assets.push_back(std::move(asset));
    }
    return desc;
}

bool RunManifest(std::size_t assets, std::size_t bundles, int iterations) {
    const ManifestDesc desc = MakeManifest(assets, bundles);
    auto bytes = WriteManifest(desc);
    if (!bytes) {
        std::fprintf(stderr, "manifest write failed: %s\n", bytes.GetError().ToString().c_str());
        return false;
    }
    const auto& data = bytes.Value();
    auto view = ManifestView::Open(data, false);
    if (!view) {
        std::fprintf(stderr, "manifest parse failed: %s\n", view.GetError().ToString().c_str());
        return false;
    }

    bool parse_failed = false;
    std::size_t found = 0;
    const double write_ms = MedianMs(
        [&] {
            auto result = WriteManifest(desc);
            if (!result) {
                parse_failed = true;
            }
        },
        iterations);
    const double parse_ms = MedianMs(
        [&] {
            auto result = ManifestView::Open(data, false);
            if (!result) {
                parse_failed = true;
            }
        },
        iterations);
    const double parse_hash_ms = MedianMs(
        [&] {
            auto result = ManifestView::Open(data, true);
            if (!result) {
                parse_failed = true;
            }
        },
        std::max(1, iterations / 2));
    const double lookup_ms = MedianMs(
        [&] {
            for (std::size_t i = 0; i < assets; i += 17U) {
                if (view.Value().FindAssetByAddress("address/" + std::to_string(i)) != nullptr) {
                    ++found;
                }
            }
        },
        iterations);
    if (parse_failed || found == 0) {
        return false;
    }
    std::printf("manifest assets=%zu bundles=%zu bytes=%zu write_ms=%.3f parse_ms=%.3f "
                "parse_hash_ms=%.3f lookup_ns=%.1f\n",
                assets, bundles, data.size(), write_ms, parse_ms, parse_hash_ms,
                (lookup_ms * 1000000.0) / static_cast<double>((assets + 16U) / 17U));
    return true;
}

bool RunBundle(std::size_t payload_size, int iterations) {
    std::vector<std::byte> payload(payload_size);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>((i * 73U + (i >> 8U)) & 0xFFU);
    }
    BundleToc toc;
    toc.bundle_guid = Guid(0x03000000U);
    toc.bundle_name = "benchmark.bundle";
    toc.payloads.push_back(BundlePayloadDesc{0U, 0U, payload.size(), Guid(0x03000001U), 0U});

    BundleWriteOptions options;
    options.block_size = 64U * 1024U;
    options.file_codec = CompressionCodec::Lz4;
    options.toc_codec = CompressionCodec::Lz4;
    auto bytes = WriteBundle(toc, payload, options);
    if (!bytes) {
        std::fprintf(stderr, "bundle write failed: %s\n", bytes.GetError().ToString().c_str());
        return false;
    }
    auto reader = BundleReader::Open(bytes.Value());
    if (!reader) {
        std::fprintf(stderr, "bundle open failed: %s\n", reader.GetError().ToString().c_str());
        return false;
    }
    bool failed = false;
    const double write_ms = MedianMs(
        [&] {
            auto result = WriteBundle(toc, payload, options);
            if (!result) {
                failed = true;
            }
        },
        std::max(1, iterations / 2));
    const double open_ms = MedianMs(
        [&] {
            auto result = BundleReader::Open(bytes.Value());
            if (!result) {
                failed = true;
            }
        },
        iterations);
    const double read_ms = MedianMs(
        [&] {
            auto result = reader.Value().ReadAllPayload();
            if (!result || result.Value().size() != payload.size()) {
                failed = true;
            }
        },
        iterations);
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    const double range_ms = MedianMs(
        [&] {
            for (int i = 0; i < 512; ++i) {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                const std::uint64_t offset = payload.empty() ? 0U : (state % (payload.size() - 1U));
                auto result = reader.Value().ReadLogical(offset, 4096U);
                if (!result) {
                    failed = true;
                }
            }
        },
        iterations);
    if (failed) {
        return false;
    }
    std::printf("bundle payload_bytes=%zu compressed_bytes=%zu write_ms=%.3f open_ms=%.3f "
                "read_all_ms=%.3f read_4k_us=%.1f\n",
                payload.size(), bytes.Value().size(), write_ms, open_ms, read_ms,
                (range_ms * 1000.0) / 512.0);
    return true;
}

bool RunAsset(std::size_t objects, int iterations) {
    AssetFileDesc desc;
    desc.types.push_back(AssetTypeDesc{1U, "parcel.BenchmarkAsset", 1U, 0U});
    desc.objects.reserve(objects);
    for (std::size_t i = 0; i < objects; ++i) {
        AssetObjectDesc object;
        object.local_id = static_cast<std::uint32_t>(i);
        object.type_id = 1U;
        object.metadata.resize(64U, static_cast<std::byte>(i & 0xFFU));
        desc.objects.push_back(std::move(object));
    }
    desc.blobs.push_back(parcel::serialize::AssetBlobDesc{
        std::vector<std::byte>(1024U * 1024U, std::byte{0x5A}), 0U});

    auto bytes = WriteAssetFile(desc);
    if (!bytes) {
        std::fprintf(stderr, "asset write failed: %s\n", bytes.GetError().ToString().c_str());
        return false;
    }
    bool failed = false;
    const double write_ms = MedianMs(
        [&] {
            auto result = WriteAssetFile(desc);
            if (!result) {
                failed = true;
            }
        },
        std::max(1, iterations / 2));
    const double open_ms = MedianMs(
        [&] {
            auto result = AssetFileReader::Open(bytes.Value(), false);
            if (!result || result.Value().Objects().size() != objects) {
                failed = true;
            }
        },
        iterations);
    if (failed) {
        return false;
    }
    std::printf("asset objects=%zu bytes=%zu write_ms=%.3f open_ms=%.3f\n", objects,
                bytes.Value().size(), write_ms, open_ms);
    return true;
}

bool RunCache(std::size_t files, int iterations) {
    std::error_code error;
    auto root = std::filesystem::temp_directory_path(error);
    if (error) {
        std::fprintf(stderr, "temp directory lookup failed: %s\n", error.message().c_str());
        return false;
    }
    root /= "parcel-cache-bench-" +
            std::to_string(Clock::now().time_since_epoch().count());
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root / "bundles", error);
    if (error) {
        std::fprintf(stderr, "cache fixture directory failed: %s\n", error.message().c_str());
        return false;
    }

    const char payload[] = "parcel-cache-bench";
    for (std::size_t i = 0; i < files; ++i) {
        const auto path = root / "bundles" /
                          ("bundle_" + std::to_string(i) + ".bundle");
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "cache fixture write failed\n");
            std::filesystem::remove_all(root, error);
            return false;
        }
        out.write(payload, sizeof(payload) - 1U);
        if (!out) {
            std::fprintf(stderr, "cache fixture write failed\n");
            std::filesystem::remove_all(root, error);
            return false;
        }
    }

    parcel::runtime::FileSystemOptions options;
    options.package_name = "benchmark";
    options.root = root.string();
    parcel::runtime::SandboxFileSystem file_system(options);
    auto initialized = file_system.Initialize(options);
    if (!initialized) {
        std::fprintf(stderr, "cache fixture initialize failed: %s\n",
                     initialized.GetError().ToString().c_str());
        std::filesystem::remove_all(root, error);
        return false;
    }

    bool failed = false;
    parcel::runtime::CacheStats stats;
    const double scan_ms = MedianMs(
        [&] {
            auto result = file_system.QueryCache();
            if (!result || result.Value().files != files) {
                failed = true;
                return;
            }
            stats = result.Value();
        },
        iterations);
    file_system.Shutdown();
    std::filesystem::remove_all(root, error);
    if (failed) {
        std::fprintf(stderr, "cache scan returned an invalid result\n");
        return false;
    }
    std::printf("cache files=%zu bytes=%llu scan_ms=%.3f\n", stats.files,
                static_cast<unsigned long long>(stats.bytes), scan_ms);
    return true;
}

bool RunCacheVerify(std::size_t files, int iterations) {
    std::error_code error;
    auto base = std::filesystem::temp_directory_path(error);
    if (error) {
        std::fprintf(stderr, "temp directory lookup failed: %s\n", error.message().c_str());
        return false;
    }
    const auto root =
        base / ("parcel-cache-verify-bench-" +
                std::to_string(Clock::now().time_since_epoch().count()));
    const auto builtin = root / "builtin";
    const auto sandbox = root / "sandbox";
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(builtin, error);
    std::filesystem::create_directories(sandbox / "bundles", error);
    if (error) {
        std::fprintf(stderr, "cache verify fixture directory failed: %s\n",
                     error.message().c_str());
        return false;
    }

    ManifestDesc desc;
    desc.package_name = "benchmark";
    desc.package_version = "1.0.0";
    std::vector<std::byte> payload(1024);
    desc.bundles.reserve(files);
    for (std::size_t i = 0; i < files; ++i) {
        for (std::size_t j = 0; j < payload.size(); ++j) {
            payload[j] = static_cast<std::byte>((i * 31U + j * 17U) & 0xFFU);
        }
        ManifestBundleDesc bundle;
        bundle.bundle_guid = Guid(0x30000000U + static_cast<std::uint32_t>(i));
        bundle.bundle_name = "bundle_" + std::to_string(i);
        bundle.file_name = bundle.bundle_name + ".bundle";
        bundle.file_size = payload.size();
        bundle.file_sha256 = Sha256(payload);
        bundle.file_crc = parcel::core::Crc32(payload);
        desc.bundles.push_back(std::move(bundle));

        const auto path = sandbox / "bundles" / desc.bundles.back().file_name;
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "cache verify fixture write failed\n");
            std::filesystem::remove_all(root, error);
            return false;
        }
        out.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
        if (!out) {
            std::fprintf(stderr, "cache verify fixture write failed\n");
            std::filesystem::remove_all(root, error);
            return false;
        }
    }

    auto manifest_bytes = WriteManifest(desc);
    if (!manifest_bytes) {
        std::fprintf(stderr, "cache verify manifest write failed: %s\n",
                     manifest_bytes.GetError().ToString().c_str());
        std::filesystem::remove_all(root, error);
        return false;
    }
    {
        std::ofstream manifest_file(builtin / "manifest.bytes", std::ios::binary);
        manifest_file.write(reinterpret_cast<const char*>(manifest_bytes.Value().data()),
                            static_cast<std::streamsize>(manifest_bytes.Value().size()));
        std::ofstream version_file(builtin / "benchmark.version", std::ios::binary);
        version_file << "1.0.0";
        if (!manifest_file || !version_file) {
            std::fprintf(stderr, "cache verify manifest fixture write failed\n");
            std::filesystem::remove_all(root, error);
            return false;
        }
    }

    parcel::runtime::AssetSystem system;
    auto initialized = system.Initialize({});
    if (!initialized) {
        std::fprintf(stderr, "cache verify system initialize failed: %s\n",
                     initialized.GetError().ToString().c_str());
        std::filesystem::remove_all(root, error);
        return false;
    }
    parcel::runtime::ResourcePackageOptions package_options;
    package_options.package_name = "benchmark";
    package_options.package_version = "1.0.0";
    package_options.builtin_root = builtin.string();
    package_options.sandbox_root = sandbox.string();
    auto package = system.CreatePackage(package_options);
    if (!package) {
        std::fprintf(stderr, "cache verify package create failed: %s\n",
                     package.GetError().ToString().c_str());
        system.Shutdown();
        std::filesystem::remove_all(root, error);
        return false;
    }

    bool failed = false;
    auto verify = [&](std::size_t workers) {
        parcel::runtime::CacheVerifyOptions options;
        options.level = parcel::runtime::CacheVerifyLevel::Content;
        options.max_concurrency = workers;
        auto report = package.Value()->VerifyCacheFiles(options);
        if (!report || report.Value().total != files || report.Value().valid != files) {
            failed = true;
        }
    };
    const double serial_ms = MedianMs([&] { verify(1); }, iterations);
    const double parallel_ms = MedianMs([&] { verify(8); }, iterations);

    (void)system.DestroyPackage("benchmark");
    system.Shutdown();
    std::filesystem::remove_all(root, error);
    if (failed) {
        std::fprintf(stderr, "cache verify returned an invalid report\n");
        return false;
    }
    std::printf("cache_verify files=%zu bytes_per_file=%zu serial_ms=%.3f parallel_ms=%.3f\n",
                files, payload.size(), serial_ms, parallel_ms);
    return true;
}

std::size_t ParseSize(const char* value, std::size_t fallback) {
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end != value ? static_cast<std::size_t>(parsed) : fallback;
}

} // namespace

int main(int argc, char** argv) {
    std::size_t assets = 10000;
    std::size_t large_assets = 100000;
    std::size_t bundle_mb = 8;
    std::size_t cache_files = 10000;
    int iterations = 3;
    int cache_iterations = 3;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quick") {
            assets = 2000;
            large_assets = 10000;
            bundle_mb = 2;
            cache_files = 2000;
            iterations = 2;
            cache_iterations = 2;
        } else if (arg.rfind("--assets=", 0) == 0) {
            assets = ParseSize(arg.c_str() + 9, assets);
        } else if (arg.rfind("--large-assets=", 0) == 0) {
            large_assets = ParseSize(arg.c_str() + 15, large_assets);
        } else if (arg.rfind("--bundle-mb=", 0) == 0) {
            bundle_mb = ParseSize(arg.c_str() + 12, bundle_mb);
        } else if (arg.rfind("--cache-files=", 0) == 0) {
            cache_files = ParseSize(arg.c_str() + 14, cache_files);
        } else if (arg.rfind("--cache-iterations=", 0) == 0) {
            cache_iterations = static_cast<int>(
                ParseSize(arg.c_str() + 19, static_cast<std::size_t>(cache_iterations)));
        } else if (arg.rfind("--iterations=", 0) == 0) {
            iterations =
                static_cast<int>(ParseSize(arg.c_str() + 13, static_cast<std::size_t>(iterations)));
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }
    iterations = std::max(1, iterations);
    cache_iterations = std::max(1, cache_iterations);
    if (assets == 0 || large_assets == 0 || bundle_mb == 0 || cache_files == 0) {
        std::fprintf(stderr, "benchmark sizes must be positive\n");
        return 2;
    }

    if (!RunManifest(assets, std::max<std::size_t>(1U, assets / 10U), iterations) ||
        !RunManifest(large_assets, std::max<std::size_t>(1U, large_assets / 10U), iterations) ||
        !RunBundle(bundle_mb * 1024U * 1024U, iterations) || !RunAsset(assets, iterations) ||
        !RunCache(cache_files, cache_iterations) ||
        !RunCacheVerify(cache_files, cache_iterations)) {
        return 1;
    }
    return 0;
}






