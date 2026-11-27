#define NOMINMAX

#include "parcel/assets/material_asset.hpp"
#include "parcel/assets/mesh_asset.hpp"
#include "parcel/assets/raw_file_asset.hpp"
#include "parcel/assets/shader_library_asset.hpp"
#include "parcel/assets/texture_asset.hpp"
#include "parcel/core/error.hpp"
#include "parcel/core/guid.hpp"
#include "parcel/entt/scene_loader.hpp"
#include "parcel/hbg/asset_materializer.hpp"
#include "parcel/hbg/parcel_material_system.hpp"
#include "parcel/hbg/river.hpp"
#include "parcel/hbg/scene_render_bridge.hpp"
#include "parcel/hbg/shader_library_bootstrap.hpp"
#include "parcel/hbg/terrain_lod.hpp"
#include "parcel/hbg/vegetation.hpp"
#include "parcel/hbg/water_surface.hpp"
#include "parcel/runtime/resource_package.hpp"
#include "parcel/scene/component_codec.hpp"
#include "parcel/scene/components.hpp"
#include "parcel/scene/scene_data.hpp"

#include "app/backend.h"
#include "core/log.h"
#include "gfx/device.h"
#include "render/camera/view_state.h"
#include "render/passes/forward_renderer.h"
#include "render/pipeline/frame_settings.h"
#include "render/pipeline/pipeline_settings.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using parcel::core::AssetGuid;
using parcel::core::ErrorCode;
using parcel::core::MakeError;
using parcel::core::Result;

struct Options {
    std::string backend = "auto";
    std::uint32_t width = 320;
    std::uint32_t height = 240;
    std::uint32_t frames = 3;
    std::uint32_t warmup_frames = 0;
    std::uint32_t objects = 1;
    std::string screenshot;
    std::string package_root;
    std::string scene;
    std::string shader_cache;
    bool shadows = false;
    bool gpu_driven = false;
    bool gpu_light_list = false;
    bool motion_vectors = false;
    bool help = false;
};

class GlfwState {
  public:
    ~GlfwState() {
        if (window != nullptr) {
            glfwDestroyWindow(window);
        }
        if (initialized) {
            glfwTerminate();
        }
    }

    bool init(std::uint32_t width, std::uint32_t height) {
        glfwSetErrorCallback([](int code, const char* description) {
            std::fprintf(stderr, "GLFW error %d: %s\n", code,
                         description != nullptr ? description : "unknown");
        });
        if (glfwInit() != GLFW_TRUE) {
            return false;
        }
        initialized = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height),
                                  "parcel-hbg-gpu-smoke", nullptr, nullptr);
        return window != nullptr;
    }

    void* native_handle() const {
        return window != nullptr ? static_cast<void*>(glfwGetWin32Window(window)) : nullptr;
    }

    GLFWwindow* window = nullptr;
    bool initialized = false;
};

AssetGuid MakeGuid(char suffix) {
    std::string text = "00112233445566778899aabbccddee0";
    text.push_back(suffix);
    return AssetGuid::FromHex(text).Value();
}

parcel::assets::TextureAsset MakeMippedTexture(std::uint32_t width, std::uint32_t height,
                                               std::uint32_t mip_count) {
    parcel::assets::TextureAsset texture;
    texture.format = parcel::assets::TextureFormat::RGBA8;
    texture.sRGB = true;
    texture.mip_count = mip_count;
    for (std::uint32_t level = 0; level < mip_count; ++level) {
        const std::uint32_t shift = std::min(level, 31U);
        const std::uint32_t mip_width = std::max(1U, width >> shift);
        const std::uint32_t mip_height = std::max(1U, height >> shift);
        parcel::assets::TextureMip mip;
        mip.width = mip_width;
        mip.height = mip_height;
        mip.row_pitch = mip_width * 4;
        mip.data.resize(static_cast<std::size_t>(mip_width) * mip_height * 4);
        for (std::uint32_t y = 0; y < mip_height; ++y) {
            for (std::uint32_t x = 0; x < mip_width; ++x) {
                const bool light = ((x / 4U) + (y / 4U) + level) % 2U == 0U;
                const std::size_t offset = (static_cast<std::size_t>(y) * mip_width + x) * 4;
                const std::uint8_t value = light ? 230 : 25;
                mip.data[offset] = static_cast<std::byte>(value);
                mip.data[offset + 1] = static_cast<std::byte>(value);
                mip.data[offset + 2] = static_cast<std::byte>(value);
                mip.data[offset + 3] = std::byte{0xff};
            }
        }
        texture.mips.push_back(std::move(mip));
    }
    return texture;
}

parcel::assets::TextureAsset MakeCompressedTexture() {
    parcel::assets::TextureAsset texture;
    texture.format = parcel::assets::TextureFormat::BC7;
    texture.sRGB = true;
    texture.mip_count = 1;
    parcel::assets::TextureMip mip;
    mip.width = 4;
    mip.height = 4;
    mip.row_pitch = 16;
    mip.data.assign(16, std::byte{0x40});
    texture.mips.push_back(std::move(mip));
    return texture;
}

parcel::assets::TextureAsset MakeFloatTexture() {
    parcel::assets::TextureAsset texture;
    texture.format = parcel::assets::TextureFormat::RGBA16F;
    texture.sRGB = false;
    texture.mip_count = 1;
    parcel::assets::TextureMip mip;
    mip.width = 4;
    mip.height = 4;
    mip.row_pitch = mip.width * 8;
    mip.data.assign(static_cast<std::size_t>(mip.row_pitch) * mip.height, std::byte{0});
    texture.mips.push_back(std::move(mip));
    return texture;
}

parcel::assets::MeshAsset MakeQuad() {
    parcel::assets::MeshAsset mesh;
    mesh.name = "SmokeQuad";
    mesh.has_normals = true;
    mesh.has_tangents = true;
    mesh.has_uv0 = true;
    mesh.vertices.resize(4);
    mesh.vertices[0].position = {-1.0f, -1.0f, 0.0f};
    mesh.vertices[0].texcoord0 = {0.0f, 1.0f};
    mesh.vertices[1].position = {1.0f, -1.0f, 0.0f};
    mesh.vertices[1].texcoord0 = {1.0f, 1.0f};
    mesh.vertices[2].position = {1.0f, 1.0f, 0.0f};
    mesh.vertices[2].texcoord0 = {1.0f, 0.0f};
    mesh.vertices[3].position = {-1.0f, 1.0f, 0.0f};
    mesh.vertices[3].texcoord0 = {0.0f, 0.0f};
    for (parcel::assets::MeshVertex& vertex : mesh.vertices) {
        vertex.normal = {0.0f, 0.0f, 1.0f};
        vertex.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    parcel::assets::SubMesh submesh;
    submesh.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    submesh.material_index = 0;
    submesh.bounds.center = {0.0f, 0.0f, 0.0f};
    submesh.bounds.extents = {1.0f, 1.0f, 0.0f};
    mesh.sub_meshes.push_back(submesh);
    mesh.bounds = submesh.bounds;
    return mesh;
}

void AddEntity(parcel::scene::SceneData& scene, std::uint64_t id, std::uint64_t parent,
               std::string name) {
    parcel::scene::SceneEntityRecord record;
    record.id = {id};
    record.parent = {parent};
    record.name = std::move(name);
    scene.entities.push_back(std::move(record));
}

template <typename T>
bool AddComponent(parcel::scene::SceneData& scene, parcel::scene::SceneEntityId owner,
                  const T& component) {
    auto payload = parcel::scene::ComponentCodec<T>::Encode(component);
    if (!payload) {
        std::fprintf(stderr, "component encode failed: %s\n",
                     payload.GetError().ToString().c_str());
        return false;
    }
    auto* chunk = scene.FindComponent(parcel::scene::ComponentCodec<T>::type_id);
    if (chunk == nullptr) {
        parcel::scene::ComponentChunk created;
        created.type = parcel::scene::ComponentCodec<T>::type_id;
        created.schema_version = parcel::scene::ComponentCodec<T>::schema_version;
        scene.components.push_back(std::move(created));
        chunk = &scene.components.back();
    }
    chunk->owners.push_back(owner);
    chunk->payloads.push_back(payload.Value());
    return true;
}

void DestroyScene(parcel_entt::EnttSceneLoader& loader, parcel::scene::SceneInstance& instance) {
    const parcel::core::Result<void> destroyed = loader.Destroy(instance);
    if (!destroyed) {
        std::fprintf(stderr, "scene destroy failed: %s\n", destroyed.GetError().ToString().c_str());
    }
}

bool ParseUint(std::string_view text, std::uint32_t& value) {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size();
}

std::optional<std::string_view> OptionValue(std::string_view argument, std::string_view name) {
    if (argument.size() <= name.size() || argument.substr(0, name.size()) != name ||
        argument[name.size()] != '=') {
        return std::nullopt;
    }
    return argument.substr(name.size() + 1);
}

void PrintUsage() {
    std::printf(
        "parcel_hbg_gpu_smoke [--backend=auto|d3d11|d3d12|vulkan] "
        "[--width=N] [--height=N] [--frames=N] [--warmup=N] [--objects=N]\n"
        "                       [--screenshot=path] [--shadows] [--gpu-driven] "
        "[--gpu-light-list] [--motion-vectors]\n"
        "                       [--package=builtin_root] [--scene=address] [--shader-cache=dir]\n");
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            return true;
        }
        if (const auto value = OptionValue(argument, "--backend")) {
            options.backend = std::string(*value);
            continue;
        }
        if (const auto value = OptionValue(argument, "--width")) {
            if (!ParseUint(*value, options.width) || options.width == 0 || options.width > 4096) {
                return false;
            }
            continue;
        }
        if (const auto value = OptionValue(argument, "--height")) {
            if (!ParseUint(*value, options.height) || options.height == 0 ||
                options.height > 4096) {
                return false;
            }
            continue;
        }
        if (const auto value = OptionValue(argument, "--frames")) {
            if (!ParseUint(*value, options.frames) || options.frames == 0 ||
                options.frames > 1000) {
                return false;
            }
            continue;
        }
        if (const auto value = OptionValue(argument, "--warmup")) {
            if (!ParseUint(*value, options.warmup_frames) || options.warmup_frames > 1000) {
                return false;
            }
            continue;
        }
        if (argument == "--shadows") {
            options.shadows = true;
            continue;
        }
        if (argument == "--gpu-driven") {
            options.gpu_driven = true;
            continue;
        }
        if (argument == "--gpu-light-list") {
            options.gpu_light_list = true;
            continue;
        }
        if (argument == "--motion-vectors") {
            options.motion_vectors = true;
            continue;
        }
        if (const auto value = OptionValue(argument, "--objects")) {
            if (!ParseUint(*value, options.objects) || options.objects == 0 ||
                options.objects > 100000) {
                return false;
            }
            continue;
        }
        if (const auto value = OptionValue(argument, "--screenshot")) {
            options.screenshot = std::string(*value);
            continue;
        }
        if (const auto value = OptionValue(argument, "--package")) {
            options.package_root = std::string(*value);
            continue;
        }
        if (const auto value = OptionValue(argument, "--scene")) {
            options.scene = std::string(*value);
            continue;
        }
        if (const auto value = OptionValue(argument, "--shader-cache")) {
            options.shader_cache = std::string(*value);
            continue;
        }
        std::fprintf(stderr, "unknown option: %s\n", argv[index]);
        return false;
    }
    return true;
}

bool AddSmokeEntities(parcel::scene::SceneData& scene_data, std::uint32_t width,
                      std::uint32_t height, std::uint32_t object_count, AssetGuid mesh,
                      AssetGuid material) {
    const std::uint32_t count = std::max(1U, object_count);
    const std::uint32_t columns =
        static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    const std::uint32_t rows = (count + columns - 1) / columns;
    constexpr float spacing = 2.5f;
    const float width_extent = static_cast<float>(columns - 1) * spacing + 2.0f;
    const float height_extent = static_cast<float>(rows - 1) * spacing + 2.0f;
    constexpr float yfov = 1.0f;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float horizontal_fov = 2.0f * std::atan(std::tan(yfov * 0.5f) * aspect);
    const float distance =
        std::max({4.0f, width_extent * 0.5f / std::tan(horizontal_fov * 0.5f) * 1.2f,
                  height_extent * 0.5f / std::tan(yfov * 0.5f) * 1.2f});

    AddEntity(scene_data, 1, 0, "Camera");
    AddEntity(scene_data, 2, 0, "Light");

    parcel::scene::TransformComponent camera_transform;
    camera_transform.position = {0.0f, 0.0f, distance};
    if (!AddComponent(scene_data, {1}, camera_transform)) {
        return false;
    }
    parcel::scene::CameraComponent camera;
    camera.aspect = aspect;
    camera.yfov = yfov;
    camera.near_clip = 0.1f;
    camera.far_clip = distance + std::max(width_extent, height_extent) + 16.0f;
    camera.clear_color = {0.02f, 0.03f, 0.05f, 1.0f};
    if (!AddComponent(scene_data, {1}, camera)) {
        return false;
    }

    parcel::scene::TransformComponent light_transform;
    light_transform.position = {2.0f, 3.0f, 4.0f};
    if (!AddComponent(scene_data, {2}, light_transform)) {
        return false;
    }
    parcel::scene::LightComponent light;
    light.type = parcel::scene::LightType::Point;
    light.color = {1.0f, 0.9f, 0.8f};
    light.intensity = 80.0f;
    light.range = std::max(20.0f, distance * 2.0f);
    if (!AddComponent(scene_data, {2}, light)) {
        return false;
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t column = index % columns;
        const std::uint32_t row = index / columns;
        const std::uint64_t entity_id = 3 + static_cast<std::uint64_t>(index);
        AddEntity(scene_data, entity_id, 0, "Quad" + std::to_string(index));

        parcel::scene::TransformComponent transform;
        transform.position = {
            (static_cast<float>(column) - static_cast<float>(columns - 1) * 0.5f) * spacing,
            (static_cast<float>(rows - 1) * 0.5f - static_cast<float>(row)) * spacing, 0.0f};
        if (!AddComponent(scene_data, {entity_id}, transform)) {
            return false;
        }
        parcel::scene::MeshRendererComponent renderer;
        renderer.mesh = mesh;
        renderer.materials = {material};
        if (!AddComponent(scene_data, {entity_id}, renderer)) {
            return false;
        }
    }
    return true;
}

bool RunGpu(const Options& options, hbg::render::RenderScene scene,
            std::vector<parcel_hbg::MaterializedTexture> material_images,
            std::optional<parcel_hbg::ShaderMaterialization> shader_materialization,
            std::optional<parcel_hbg::VegetationSettings> vegetation_settings,
            std::optional<parcel_hbg::WaterSurfaceSettings> water_settings,
            parcel_hbg::RiverScene* river_scene = nullptr) {
    GlfwState glfw;
    if (!glfw.init(options.width, options.height)) {
        std::fprintf(stderr, "GLFW initialization failed\n");
        return false;
    }

    const auto backend = hbg::parseBackend(options.backend);
    if (!backend) {
        std::fprintf(stderr, "unknown backend: %s\n", options.backend.c_str());
        return false;
    }

    hbg::gfx::Device device;
    hbg::gfx::DeviceDesc device_desc;
    device_desc.backend = *backend;
    device_desc.nativeWindow = glfw.native_handle();
    device_desc.width = static_cast<std::uint16_t>(options.width);
    device_desc.height = static_cast<std::uint16_t>(options.height);
    device_desc.vsync = false;
    device_desc.profile = false;
    if (!device.init(device_desc)) {
        std::fprintf(stderr, "bgfx device initialization failed\n");
        return false;
    }

    parcel_hbg::WaterSurface water_surface;
    bool water_surface_active = false;
    if (water_settings) {
        std::string water_error;
        if (!water_surface.Bind(scene, *water_settings, &water_error)) {
            std::fprintf(stderr, "water surface disabled: %s\n", water_error.c_str());
        } else {
            water_surface_active = true;
        }
    }

    {
        if (material_images.size() < scene.images.size()) {
            std::fprintf(stderr, "material image count is smaller than the render scene\n");
            return false;
        }
        hbg::render::RenderScene material_scene = scene;
        material_scene.images.resize(material_images.size());

        parcel_hbg::ParcelMaterialSystem material_system;
        auto initialized =
            material_system.Initialize(device.resources(), material_scene, material_images);
        if (!initialized) {
            std::fprintf(stderr, "ParcelMaterialSystem initialization failed: %s\n",
                         initialized.GetError().ToString().c_str());
            return false;
        }
        if (material_system.meshes().empty() ||
            !material_system.meshes().front().vertexBuffer.valid() ||
            (!material_system.materials().empty() &&
             !material_system.materials().front().baseColorTexture.valid())) {
            std::fprintf(stderr, "ParcelMaterialSystem produced incomplete GPU resources\n");
            material_system.Shutdown(device.resources());
            return false;
        }
        material_system.Shutdown(device.resources());
        device.resources().flushPending(true);
        if (device.resources().liveResourceCount() != 0) {
            std::fprintf(stderr, "ParcelMaterialSystem leaked %zu GPU resources\n",
                         device.resources().liveResourceCount());
            return false;
        }
    }

    hbg::render::PipelineSettings pipeline = hbg::render::PipelineSettings::defaults();
    pipeline.supportShadows = options.shadows;
    pipeline.supportGpuLightList = options.gpu_light_list;
    pipeline.supportGpuLightListBuffers = options.gpu_light_list;
    pipeline.supportGpuSkinning = false;
    pipeline.supportDepthPyramidCompute = options.gpu_driven || options.motion_vectors;
    pipeline.supportGpuDrivenGeometry = options.gpu_driven;
    pipeline.supportCookieAtlas = false;
    pipeline.supportMotionVectors = options.motion_vectors;
    std::printf("parcel_hbg_gpu_smoke: budget_mode shadows=%d gpu_driven=%d "
                "gpu_light_list=%d motion_vectors=%d warmup=%u frames=%u\n",
                options.shadows ? 1 : 0, options.gpu_driven ? 1 : 0, options.gpu_light_list ? 1 : 0,
                options.motion_vectors ? 1 : 0, options.warmup_frames, options.frames);

    hbg::render::FrameSettings frame =
        hbg::render::FrameSettings::defaults(hbg::render::FrameSettingsRenderType::Camera);

    hbg::render::ForwardRenderer renderer;
    if (!renderer.init(device.resources(), scene, static_cast<std::uint16_t>(options.width),
                       static_cast<std::uint16_t>(options.height))) {
        std::fprintf(stderr, "ForwardRenderer initialization failed\n");
        renderer.shutdown(device.resources());
        return false;
    }
    if (shader_materialization) {
        auto shader_initialized = parcel_hbg::ShaderLibraryBootstrap::Initialize(
            renderer.shaderLibrary(), device.resources(), *shader_materialization);
        if (!shader_initialized) {
            std::fprintf(stderr, "ShaderLibraryBootstrap initialization failed: %s\n",
                         shader_initialized.GetError().ToString().c_str());
            renderer.shutdown(device.resources());
            return false;
        }
        const auto forward_program = renderer.shaderLibrary().program(
            hbg::shader::ProgramKey{hbg::shader::PassId::Forward, 0, 0});
        if (!forward_program.valid()) {
            std::fprintf(stderr,
                         "materialized shader library did not create the forward program\n");
            renderer.shutdown(device.resources());
            return false;
        }
        std::printf("parcel_hbg_gpu_smoke: shader_bootstrap='%s' hash='%s' backend='%s' "
                    "files=%zu\n",
                    shader_materialization->root.string().c_str(),
                    shader_materialization->content_hash.c_str(),
                    std::string(renderer.shaderLibrary().backendName()).c_str(),
                    shader_materialization->files.size());
    }
    renderer.setSettings(pipeline, frame);
    renderer.setShadowsEnabled(options.shadows);
    renderer.setDebugDisplayMode(hbg::render::DebugDisplayMode::None);
    renderer.setRenderScale(1.0f);

    parcel_hbg::VegetationController vegetation_controller;
    hbg::render::ViewState vegetation_view;
    bool vegetation_visibility_active = false;
    if (vegetation_settings) {
        vegetation_visibility_active = vegetation_controller.Bind(
            scene, vegetation_settings->tile_size, vegetation_settings->lod_distance);
        vegetation_view.update(scene.cameras.front(), static_cast<std::uint16_t>(options.width),
                               static_cast<std::uint16_t>(options.height), 1.0f, false, false);
    }
    const auto update_vegetation_visibility = [&]() {
        if (!vegetation_visibility_active) {
            return;
        }
        vegetation_controller.Update(scene, vegetation_view.cameraPosition(),
                                     &vegetation_view.constants().frustumPlanes,
                                     vegetation_settings->max_distance);
    };

    hbg::render::ViewState water_view;
    if (water_surface_active && !scene.cameras.empty()) {
        water_view.update(scene.cameras.front(), static_cast<std::uint16_t>(options.width),
                          static_cast<std::uint16_t>(options.height), 1.0f, false, false);
    }
    const auto update_water_surface = [&](float time_seconds) {
        if (water_surface_active && !scene.cameras.empty()) {
            water_view.update(scene.cameras.front(), static_cast<std::uint16_t>(options.width),
                              static_cast<std::uint16_t>(options.height), 1.0f, false, false);
            const hbg::Vec3 water_camera = water_view.cameraPosition();
            (void)water_surface.Update(scene, time_seconds, &water_view.constants().invViewProj,
                                       &water_camera);
        }
    };

    hbg::render::ViewState river_view_state;
    parcel_hbg::RiverView river_view;
    const auto update_river_scene = [&]() {
        if (river_scene == nullptr || scene.cameras.empty()) {
            return;
        }
        river_view_state.update(scene.cameras.front(), static_cast<std::uint16_t>(options.width),
                                static_cast<std::uint16_t>(options.height), 1.0f, false, false);
        river_view.view_proj = river_view_state.constants().viewProj;
        river_view.inv_view_proj = river_view_state.constants().invViewProj;
        river_view.camera_position = river_view_state.cameraPosition();
        river_view.viewport_width = options.width;
        river_view.viewport_height = options.height;
        (void)river_scene->Update(scene, 1.0f / 60.0f, &river_view);
    };

    for (std::uint32_t warmup_index = 0; warmup_index < options.warmup_frames; ++warmup_index) {
        glfwPollEvents();
        update_vegetation_visibility();
        update_water_surface(static_cast<float>(warmup_index) / 60.0f);
        update_river_scene();
        renderer.render(scene, static_cast<std::uint16_t>(options.width),
                        static_cast<std::uint16_t>(options.height),
                        static_cast<float>(warmup_index) / 60.0f);
        bgfx::frame();
        device.resources().endFrame();
    }

    std::vector<double> frame_times;
    frame_times.reserve(options.frames);
    const auto render_start = std::chrono::steady_clock::now();
    for (std::uint32_t frame_index = 0; frame_index < options.frames; ++frame_index) {
        const auto frame_start = std::chrono::steady_clock::now();
        glfwPollEvents();
        update_vegetation_visibility();
        update_water_surface(static_cast<float>(options.warmup_frames + frame_index) / 60.0f);
        update_river_scene();
        renderer.render(scene, static_cast<std::uint16_t>(options.width),
                        static_cast<std::uint16_t>(options.height),
                        static_cast<float>(options.warmup_frames + frame_index) / 60.0f);
        if (frame_index + 1 == options.frames && !options.screenshot.empty()) {
            bgfx::requestScreenShot(BGFX_INVALID_HANDLE, options.screenshot.c_str());
        }
        bgfx::frame();
        device.resources().endFrame();
        const auto frame_end = std::chrono::steady_clock::now();
        frame_times.push_back(
            std::chrono::duration<double, std::milli>(frame_end - frame_start).count());
    }
    const auto render_end = std::chrono::steady_clock::now();
    const double average_frame_ms =
        std::chrono::duration<double, std::milli>(render_end - render_start).count() /
        static_cast<double>(options.frames);
    std::sort(frame_times.begin(), frame_times.end());
    const double median_frame_ms = frame_times[frame_times.size() / 2];
    const std::size_t p95_index =
        std::min(frame_times.size() - 1, (frame_times.size() * 95 + 99) / 100 - 1);
    const double p95_frame_ms = frame_times[p95_index];
    const double average_fps = average_frame_ms > 0.0 ? 1000.0 / average_frame_ms : 0.0;

    if (vegetation_settings) {
        const parcel_hbg::VegetationCullingStats& vegetation_stats = vegetation_controller.stats();
        std::printf("parcel_hbg_gpu_smoke: vegetation_visibility=objects=%u tiles=%u "
                    "submeshes=%u visible=%u visible_tiles=%u visible_submeshes=%u "
                    "frustum_culled=%u frustum_culled_submeshes=%u distance_culled=%u "
                    "distance_culled_submeshes=%u\n",
                    vegetation_stats.object_count, vegetation_stats.tile_count,
                    vegetation_stats.submesh_count, vegetation_stats.visible_object_count,
                    vegetation_stats.visible_tile_count, vegetation_stats.visible_submesh_count,
                    vegetation_stats.frustum_culled_object_count,
                    vegetation_stats.frustum_culled_submesh_count,
                    vegetation_stats.distance_culled_object_count,
                    vegetation_stats.distance_culled_submesh_count);
    }
    if (water_surface_active) {
        const parcel_hbg::WaterSurfaceStats& water_stats = water_surface.stats();
        std::printf("parcel_hbg_gpu_smoke: water_fft=active mesh=%u vertices=%u updates=%u "
                    "morph_updates=%u base=%.3f height=[%.3f,%.3f] displacement=%.3f "
                    "screen_grid=%d grid=%ux%u\n",
                    water_stats.mesh_index, water_stats.vertex_count, water_stats.fft_updates,
                    water_stats.morph_updates, water_stats.base_height, water_stats.min_height,
                    water_stats.max_height, water_stats.max_displacement,
                    water_stats.screen_grid ? 1 : 0, water_stats.grid_width,
                    water_stats.grid_height);
    }

    const bgfx::Stats* stats = bgfx::getStats();
    const hbg::render::ForwardRenderer::FrameDiagnostics diagnostics = renderer.frameDiagnostics();
    const bool drew = stats != nullptr && stats->numDraw > 0;
    if (!drew || diagnostics.passCount == 0) {
        std::fprintf(stderr, "ForwardRenderer produced no draw calls\n");
        renderer.shutdown(device.resources());
        return false;
    }

    std::printf("parcel_hbg_gpu_smoke: objects=%zu draws=%u passes=%u triangles=%u textures=%u "
                "avg_frame_ms=%.3f median_frame_ms=%.3f p95_frame_ms=%.3f avg_fps=%.3f\n",
                scene.objects.size(), stats->numDraw, diagnostics.passCount,
                stats->numPrims[bgfx::Topology::TriList], stats->numTextures, average_frame_ms,
                median_frame_ms, p95_frame_ms, average_fps);

    renderer.shutdown(device.resources());
    device.resources().flushPending(true);
    if (device.resources().liveResourceCount() != 0) {
        std::fprintf(stderr, "ForwardRenderer leaked %zu GPU resources\n",
                     device.resources().liveResourceCount());
        return false;
    }

    device.shutdown();
    return true;
}

bool Run(const Options& options) {
    const AssetGuid mesh_guid = MakeGuid('a');
    const AssetGuid material_guid = MakeGuid('b');
    const AssetGuid texture_guid = MakeGuid('c');

    parcel::scene::SceneData scene_data;
    if (!AddSmokeEntities(scene_data, options.width, options.height, options.objects, mesh_guid,
                          material_guid)) {
        return false;
    }

    auto texture = std::make_shared<parcel::assets::TextureAsset>(MakeMippedTexture(64, 64, 7));
    auto material = std::make_shared<parcel::assets::MaterialAsset>();
    material->name = "SmokeMaterial";
    material->double_sided = true;
    material->metallic = 0.0f;
    material->roughness = 0.55f;
    material->base_color_texture.texture = texture_guid;
    material->base_color_texture.sampler.wrap_s = parcel::assets::TextureWrap::Repeat;
    material->base_color_texture.sampler.wrap_t = parcel::assets::TextureWrap::Repeat;

    std::unordered_map<AssetGuid, std::shared_ptr<parcel::assets::AssetObject>,
                       parcel::core::Guid128Hash>
        assets;
    assets.emplace(mesh_guid, std::make_shared<parcel::assets::MeshAsset>(MakeQuad()));
    assets.emplace(material_guid, material);
    assets.emplace(texture_guid, texture);

    auto resolve = [&](AssetGuid guid) -> Result<std::shared_ptr<parcel::assets::AssetObject>> {
        const auto found = assets.find(guid);
        if (found == assets.end()) {
            return MakeError(ErrorCode::NotFound, "missing smoke asset");
        }
        return found->second;
    };

    ::entt::registry registry;
    parcel_entt::EnttSceneLoader loader;
    auto instance = loader.Instantiate(scene_data, &registry);
    if (!instance) {
        std::fprintf(stderr, "scene instantiate failed: %s\n",
                     instance.GetError().ToString().c_str());
        return false;
    }

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene scene;
    auto updated = bridge.Update(registry, scene);
    if (!updated) {
        std::fprintf(stderr, "render scene build failed: %s\n",
                     updated.GetError().ToString().c_str());
        DestroyScene(loader, instance.Value());
        return false;
    }
    if (scene.cameras.empty() || scene.objects.empty() || scene.meshes.empty() ||
        scene.images.empty() || scene.objects.size() != options.objects) {
        std::fprintf(stderr, "render scene is incomplete or object count does not match\n");
        DestroyScene(loader, instance.Value());
        return false;
    }
    if (scene.images.front().mipCount != static_cast<std::uint8_t>(texture->mips.size())) {
        std::fprintf(stderr, "render scene image did not preserve the mip chain\n");
        DestroyScene(loader, instance.Value());
        return false;
    }

    auto base_image = parcel_hbg::MaterializeTexture(*texture, "smoke_base");
    auto compressed_image = parcel_hbg::MaterializeTexture(MakeCompressedTexture(), "smoke_bc7");
    auto float_image = parcel_hbg::MaterializeTexture(MakeFloatTexture(), "smoke_rgba16f");
    if (!base_image || !compressed_image || !float_image) {
        std::fprintf(stderr, "texture materialization failed\n");
        DestroyScene(loader, instance.Value());
        return false;
    }

    std::vector<parcel_hbg::MaterializedTexture> material_images;
    material_images.push_back(std::move(base_image).Value());
    material_images.push_back(std::move(compressed_image).Value());
    material_images.push_back(std::move(float_image).Value());
    const bool rendered = RunGpu(options, std::move(scene), std::move(material_images),
                                 std::nullopt, std::nullopt, std::nullopt, nullptr);
    DestroyScene(loader, instance.Value());
    return rendered;
}

bool RunPackage(const Options& options) {
    parcel::runtime::ResourcePackageOptions package_options;
    package_options.builtin_root = options.package_root;
    package_options.verify_bundle_on_load = true;
    auto package = parcel::runtime::ResourcePackage::Create(package_options);
    if (!package) {
        std::fprintf(stderr, "package initialization failed: %s\n",
                     package.GetError().ToString().c_str());
        return false;
    }

    std::string scene_location = options.scene;
    if (scene_location.empty()) {
        for (const parcel::runtime::AssetInfo& info : package.Value()->GetAllAssetInfos()) {
            if (info.type_name == "SceneAsset") {
                scene_location = info.address;
                break;
            }
        }
    }
    if (scene_location.empty()) {
        std::fprintf(stderr, "package has no scene; pass --scene=<address>\n");
        return false;
    }

    ::entt::registry registry;
    auto loader = std::make_shared<parcel_entt::EnttSceneLoader>();
    package.Value()->SetSceneLoader(loader, &registry);
    auto scene_handle = package.Value()->LoadScene(scene_location);
    if (!scene_handle.IsDone()) {
        (void)scene_handle.WaitForCompletion();
    }
    if (scene_handle.Status() != parcel::core::OperationStatus::Succeeded) {
        std::fprintf(stderr, "package scene load failed: %s\n",
                     scene_handle.LastError().ToString().c_str());
        return false;
    }

    std::unordered_map<AssetGuid, parcel::runtime::AssetHandle, parcel::core::Guid128Hash>
        asset_handles;
    auto resolve = [&](AssetGuid guid) -> Result<std::shared_ptr<parcel::assets::AssetObject>> {
        if (guid.IsZero()) {
            return MakeError(ErrorCode::InvalidArgument, "package asset guid is zero");
        }
        const auto cached = asset_handles.find(guid);
        if (cached != asset_handles.end()) {
            const auto objects = cached->second.Objects();
            if (objects.empty()) {
                return MakeError(ErrorCode::Internal, "cached package asset has no object");
            }
            return objects.front();
        }

        const auto info = package.Value()->GetAssetInfoByGuid(guid);
        if (!info) {
            return MakeError(ErrorCode::NotFound,
                             "package asset guid was not found: " + guid.ToHex());
        }
        auto handle = package.Value()->LoadAsset(info->address);
        if (!handle.IsDone()) {
            (void)handle.WaitForCompletion();
        }
        if (handle.Status() != parcel::core::OperationStatus::Succeeded) {
            return MakeError(ErrorCode::Corrupt,
                             "package asset load failed: " + handle.LastError().ToString());
        }
        const auto objects = handle.Objects();
        if (objects.empty()) {
            return MakeError(ErrorCode::Internal, "package asset has no object");
        }
        auto object = objects.front();
        asset_handles.emplace(guid, handle);
        return object;
    };

    parcel_hbg::SceneRenderBridge bridge(resolve);
    hbg::render::RenderScene scene;
    auto updated = bridge.Update(registry, scene);
    if (!updated) {
        std::fprintf(stderr, "render scene build failed: %s\n",
                     updated.GetError().ToString().c_str());
        return false;
    }
    if (scene.cameras.empty() || scene.objects.empty() || scene.meshes.empty()) {
        std::fprintf(stderr, "package render scene is incomplete\n");
        return false;
    }

    std::string terrain_json;
    std::string terrain_json_fallback;
    std::string river_json;
    std::shared_ptr<parcel::assets::RawFileAsset> heightmap_asset;
    const auto scene_objects = scene_handle.Objects();
    const parcel::assets::AssetObject* scene_object =
        scene_objects.empty() ? nullptr : scene_objects.front().get();
    if (scene_object != nullptr && scene_object->Type() == parcel::assets::kSceneAssetType) {
        const auto* scene_asset = static_cast<const parcel::assets::SceneAsset*>(scene_object);
        const std::string terrain_name = scene_asset->name + ".terrain.json";
        for (const parcel::runtime::AssetInfo& info : package.Value()->GetAllAssetInfos()) {
            if (info.type_name != "TextAsset") {
                continue;
            }
            auto resolved = resolve(info.asset_guid);
            if (!resolved) {
                continue;
            }
            if (resolved.Value()->Type() != parcel::assets::kTextAssetType) {
                continue;
            }
            const auto text = std::static_pointer_cast<parcel::assets::TextAsset>(resolved.Value());
            if (text->name.ends_with(".hdrp.json") ||
                text->text.find("\"river\"") != std::string::npos) {
                river_json = text->text;
            }
            if (text->name == terrain_name) {
                terrain_json = text->text;
            }
            if (terrain_json_fallback.empty() && text->name.ends_with(".terrain.json")) {
                terrain_json_fallback = text->text;
            }
        }
        if (terrain_json.empty()) {
            terrain_json = std::move(terrain_json_fallback);
        }
    }
    if (!terrain_json.empty()) {
        for (const parcel::runtime::AssetInfo& info : package.Value()->GetAllAssetInfos()) {
            if (info.type_name != "RawFileAsset") {
                continue;
            }
            auto resolved = resolve(info.asset_guid);
            if (!resolved || resolved.Value()->Type() != parcel::assets::kRawFileAssetType) {
                continue;
            }
            auto raw = std::static_pointer_cast<parcel::assets::RawFileAsset>(resolved.Value());
            if (raw->data.size() < 8 || std::memcmp(raw->data.data(), "HBGHMAP1", 8) != 0) {
                continue;
            }
            heightmap_asset = std::move(raw);
            break;
        }
    }
    parcel_hbg::TerrainLodController terrain_lod;
    if (!terrain_json.empty()) {
        const std::span<const std::byte> heightmap_bytes =
            heightmap_asset ? std::span<const std::byte>(heightmap_asset->data)
                            : std::span<const std::byte>{};
        std::string terrain_error;
        if (!terrain_lod.Load(terrain_json, heightmap_bytes, scene, &terrain_error)) {
            std::fprintf(stderr, "terrain lod disabled: %s\n", terrain_error.c_str());
        } else if (terrain_lod.active()) {
            hbg::render::ViewState terrain_view;
            terrain_view.update(scene.cameras.front(), static_cast<std::uint16_t>(options.width),
                                static_cast<std::uint16_t>(options.height), 1.0f, false, false);
            const hbg::Vec3 camera_forward = terrain_view.cameraForward();
            terrain_lod.Update(scene, terrain_view.cameraPosition(),
                               &terrain_view.constants().frustumPlanes, &camera_forward);
            const parcel_hbg::TerrainLodStats& terrain_stats = terrain_lod.stats();
            std::printf("parcel_hbg_gpu_smoke: terrain_lod=active leaves=%u visible=%u "
                        "objects=%u bound=%u\n",
                        terrain_stats.leaf_count, terrain_stats.visible_leaf_count,
                        terrain_stats.visible_object_count, terrain_stats.bound_object_count);
            if (terrain_stats.shared_geometry) {
                std::printf("parcel_hbg_gpu_smoke: terrain_shared=meshes=%u vertices=%u "
                            "triangles=%u coarse=%u fine=%u fine_from=%u\n",
                            terrain_stats.shared_mesh_count, terrain_stats.shared_vertex_count,
                            terrain_stats.shared_triangle_count,
                            terrain_lod.settings().terrain_coarse_grid,
                            terrain_lod.settings().terrain_fine_grid,
                            terrain_lod.settings().terrain_fine_from_level);
            }
            const parcel_hbg::TerrainHeightmapStats& heightmap_stats =
                terrain_lod.heightmap_stats();
            if (heightmap_stats.loaded) {
                std::printf("parcel_hbg_gpu_smoke: terrain_heightmap=%ux%u scale=%.6f "
                            "ground=%.3f hits=%u misses=%u tiles=%u evictions=%u\n",
                            heightmap_stats.width, heightmap_stats.height,
                            heightmap_stats.height_scale, heightmap_stats.ground_height,
                            heightmap_stats.hits, heightmap_stats.misses,
                            heightmap_stats.loaded_tiles, heightmap_stats.evictions);
            } else {
                std::printf("parcel_hbg_gpu_smoke: terrain_heightmap=disabled error=%s\n",
                            terrain_lod.heightmap_error().c_str());
            }
        } else if (terrain_lod.water_only()) {
            std::printf("parcel_hbg_gpu_smoke: terrain_lod=water_only\n");
        }
    }

    std::vector<parcel_hbg::MaterializedTexture> material_images;
    material_images.reserve(scene.images.size());
    const auto& image_guids = bridge.image_guids();
    if (image_guids.size() != scene.images.size()) {
        std::fprintf(stderr, "package image guid count does not match the render scene\n");
        return false;
    }
    for (const AssetGuid& guid : image_guids) {
        auto asset = resolve(guid);
        if (!asset) {
            std::fprintf(stderr, "package texture resolve failed: %s\n",
                         asset.GetError().ToString().c_str());
            return false;
        }
        if (asset.Value()->Type() != parcel::assets::kTextureAssetType) {
            std::fprintf(stderr, "package image guid is not a texture\n");
            return false;
        }
        const auto texture = std::static_pointer_cast<parcel::assets::TextureAsset>(asset.Value());
        auto materialized = parcel_hbg::MaterializeTexture(*texture, guid.ToHex());
        if (!materialized) {
            std::fprintf(stderr, "package texture materialization failed: %s\n",
                         materialized.GetError().ToString().c_str());
            return false;
        }
        const parcel_hbg::MaterializedTexture& image = materialized.Value();
        const hbg::asset::ImageAsset& bridge_image = scene.images[material_images.size()];
        if (image.desc.mipCount != bridge_image.mipCount ||
            image.desc.format != bridge_image.format || image.desc.width != bridge_image.width ||
            image.desc.height != bridge_image.height) {
            std::fprintf(stderr, "package texture did not preserve the bridge image\n");
            return false;
        }
        material_images.push_back(std::move(materialized).Value());
    }

    parcel_hbg::RiverScene river_scene;
    if (!river_json.empty()) {
        std::string river_error;
        if (!river_scene.Apply(scene, river_json, &river_error, &material_images)) {
            std::fprintf(stderr, "river build failed: %s\n", river_error.c_str());
            return false;
        }
        const parcel_hbg::RiverStats& river_stats = river_scene.stats();
        std::printf("parcel_hbg_gpu_smoke: river=active mesh=%u object=%u material=%u "
                    "vertices=%u indices=%u flow_samples=%u speed=[%.4f,%.4f]\n",
                    river_stats.mesh_index, river_stats.object_index, river_stats.material_index,
                    river_stats.vertex_count, river_stats.index_count, river_stats.flow_samples,
                    river_stats.min_speed, river_stats.max_speed);
        if (river_stats.terrain_deformed) {
            std::printf("parcel_hbg_gpu_smoke: river_terrain=mesh='%s' vertices=%u changed=%u "
                        "delta=[%.3f,%.3f]\n",
                        river_stats.terrain_mesh_name.c_str(), river_stats.terrain_vertex_count,
                        river_stats.terrain_changed_vertex_count, river_stats.terrain_min_delta,
                        river_stats.terrain_max_delta);
        } else {
            std::printf("parcel_hbg_gpu_smoke: river_terrain=disabled mesh='%s' vertices=%u\n",
                        river_stats.terrain_mesh_name.c_str(), river_stats.terrain_vertex_count);
        }
        if (river_stats.textures_generated) {
            std::printf("parcel_hbg_gpu_smoke: river_texture=size=%u base=%u normal=%u frames=%u\n",
                        river_stats.texture_size, river_stats.base_color_image_index,
                        river_stats.normal_image_index, river_stats.normal_frame_count);
        }
        if (river_stats.foam_generated) {
            std::printf("parcel_hbg_gpu_smoke: river_foam=particles=%u vertices=%u indices=%u "
                        "texture=%u image=%u fade=[%.3f,%.3f]\n",
                        river_stats.foam_particle_count, river_stats.foam_vertex_count,
                        river_stats.foam_index_count, river_stats.foam_texture_size,
                        river_stats.foam_texture_image_index, river_stats.foam_min_fade,
                        river_stats.foam_max_fade);
        } else {
            std::printf("parcel_hbg_gpu_smoke: river_foam=disabled\n");
        }
    }

    std::string vegetation_json;
    std::array<std::shared_ptr<parcel::assets::TextureAsset>, 2> vegetation_textures;
    std::vector<std::shared_ptr<parcel::assets::TextureAsset>> tree_texture_candidates;
    for (const parcel::runtime::AssetInfo& info : package.Value()->GetAllAssetInfos()) {
        if (info.type_name == "TextAsset" && vegetation_json.empty()) {
            auto resolved = resolve(info.asset_guid);
            if (!resolved || resolved.Value()->Type() != parcel::assets::kTextAssetType) {
                continue;
            }
            const auto text = std::static_pointer_cast<parcel::assets::TextAsset>(resolved.Value());
            if (text->name == "trees.json" || text->name == "trees") {
                vegetation_json = text->text;
            }
            continue;
        }
        if (info.type_name != "TextureAsset" || tree_texture_candidates.size() >= 2) {
            continue;
        }
        auto resolved = resolve(info.asset_guid);
        if (!resolved || resolved.Value()->Type() != parcel::assets::kTextureAssetType) {
            continue;
        }
        auto texture = std::static_pointer_cast<parcel::assets::TextureAsset>(resolved.Value());
        if (!texture->mips.empty() && texture->mips.front().width == 512 &&
            texture->mips.front().height == 800) {
            tree_texture_candidates.push_back(std::move(texture));
        }
    }
    if (!tree_texture_candidates.empty()) {
        const auto green = [](const parcel::assets::TextureAsset& texture) {
            if (texture.format != parcel::assets::TextureFormat::RGBA8 || texture.mips.empty() ||
                texture.mips.front().width == 0 || texture.mips.front().height == 0) {
                return 0.0f;
            }
            const auto& mip = texture.mips.front();
            const std::size_t pixels = static_cast<std::size_t>(mip.width) * mip.height;
            const std::size_t available = mip.data.size() / 4;
            const std::size_t count = std::min(pixels, available);
            float sum = 0.0f;
            for (std::size_t index = 0; index < count; ++index) {
                sum += static_cast<float>(std::to_integer<unsigned>(mip.data[index * 4 + 1]));
            }
            return count == 0 ? 0.0f : sum / static_cast<float>(count);
        };
        if (tree_texture_candidates.size() == 1) {
            vegetation_textures[0] = tree_texture_candidates.front();
        } else {
            const std::size_t deciduous =
                green(*tree_texture_candidates[0]) >= green(*tree_texture_candidates[1]) ? 0 : 1;
            vegetation_textures[0] = tree_texture_candidates[deciduous];
            vegetation_textures[1] = tree_texture_candidates[1 - deciduous];
        }
    }

    parcel_hbg::VegetationSettings vegetation_settings;
    vegetation_settings.tile_size =
        std::max(terrain_lod.settings().vegetation_tile_size, parcel_hbg::kVegetationMeshTileSize);
    vegetation_settings.max_distance = terrain_lod.settings().vegetation_max_distance;
    std::optional<parcel_hbg::VegetationSettings> vegetation_visibility_settings;
    std::optional<parcel_hbg::WaterSurfaceSettings> water_settings;
    const bool has_water_object =
        std::any_of(scene.objects.begin(), scene.objects.end(),
                    [](const hbg::render::RenderObject& object) { return object.name == "Water"; });
    if (terrain_lod.settings().water_fft && has_water_object) {
        parcel_hbg::WaterSurfaceSettings settings;
        settings.tile_size = terrain_lod.settings().water_fft_tile_size;
        settings.amplitude = terrain_lod.settings().water_fft_amplitude;
        settings.update_hz = terrain_lod.settings().water_fft_update_hz;
        settings.sphere_radius = terrain_lod.settings().water_sphere_radius;
        settings.viewport_width = static_cast<std::uint32_t>(options.width);
        settings.viewport_height = static_cast<std::uint32_t>(options.height);
        water_settings = settings;
    }
    if (!vegetation_json.empty()) {
        if (vegetation_textures[0] == nullptr || vegetation_textures[1] == nullptr) {
            std::fprintf(stderr, "vegetation textures were not found in the package\n");
            return false;
        }
        const std::array<const parcel::assets::TextureAsset*, 2> textures{
            {vegetation_textures[0].get(), vegetation_textures[1].get()}};
        auto vegetation = parcel_hbg::BuildVegetation(vegetation_json, textures, scene,
                                                      material_images, vegetation_settings);
        if (!vegetation) {
            std::fprintf(stderr, "vegetation build failed: %s\n",
                         vegetation.GetError().ToString().c_str());
            return false;
        }
        const parcel_hbg::VegetationStats& stats = vegetation.Value();
        std::printf("parcel_hbg_gpu_smoke: vegetation=%u deciduous=%u conifer=%u meshes=%u "
                    "triangles=%u textures=%u objects=%u tiles=%u submeshes=%u\n",
                    stats.instance_count, stats.deciduous_count, stats.conifer_count,
                    stats.mesh_count, stats.triangle_count, stats.texture_count, stats.object_count,
                    stats.tile_count, stats.submesh_count);
        vegetation_visibility_settings = vegetation_settings;
    } else {
        std::printf("parcel_hbg_gpu_smoke: vegetation=disabled\n");
    }

    std::optional<parcel::runtime::AssetInfo> shader_info;
    for (const parcel::runtime::AssetInfo& info : package.Value()->GetAllAssetInfos()) {
        if (info.type_name == "ShaderLibraryAsset") {
            shader_info = info;
            break;
        }
    }

    std::optional<parcel_hbg::ShaderMaterialization> shader_materialization;
    if (shader_info) {
        auto shader_asset = resolve(shader_info->asset_guid);
        if (!shader_asset) {
            std::fprintf(stderr, "package shader resolve failed: %s\n",
                         shader_asset.GetError().ToString().c_str());
            return false;
        }
        if (shader_asset.Value()->Type() != parcel::assets::kShaderLibraryAssetType) {
            std::fprintf(stderr, "package shader asset has an unexpected type\n");
            return false;
        }
        const auto shader =
            std::static_pointer_cast<parcel::assets::ShaderLibraryAsset>(shader_asset.Value());

        parcel_hbg::ShaderMaterializeOptions shader_options;
        shader_options.cache_root =
            options.shader_cache.empty()
                ? std::filesystem::temp_directory_path() / "parcel_hbg_shader_cache"
                : std::filesystem::path(options.shader_cache);
        shader_options.package = "hbg_gpu_smoke";
        auto materialized =
            parcel_hbg::ShaderLibraryBootstrap::Materialize(*shader, shader_options);
        if (!materialized) {
            std::fprintf(stderr, "package shader materialization failed: %s\n",
                         materialized.GetError().ToString().c_str());
            return false;
        }
        shader_materialization = std::move(materialized).Value();
        std::printf("parcel_hbg_gpu_smoke: shader_library='%s' shader_cache='%s' "
                    "shader_hash='%s' shader_files=%zu\n",
                    shader_info->address.c_str(), shader_options.cache_root.string().c_str(),
                    shader_materialization->content_hash.c_str(),
                    shader_materialization->files.size());
    } else {
        std::printf(
            "parcel_hbg_gpu_smoke: package has no ShaderLibraryAsset; using hdrp shader root\n");
    }

    std::printf("parcel_hbg_gpu_smoke: package='%s' version='%s' scene='%s' "
                "assets=%zu images=%zu\n",
                std::string(package.Value()->Name()).c_str(),
                std::string(package.Value()->Version()).c_str(), scene_location.c_str(),
                package.Value()->GetAllAssetInfos().size(), material_images.size());

    const bool rendered =
        RunGpu(options, std::move(scene), std::move(material_images),
               std::move(shader_materialization), std::move(vegetation_visibility_settings),
               std::move(water_settings), river_scene.active() ? &river_scene : nullptr);
    if (rendered && river_scene.active()) {
        const parcel_hbg::RiverStats& river_stats = river_scene.stats();
        std::printf("parcel_hbg_gpu_smoke: river_normal_frame=frame=%u/%u time=%.3f "
                    "step=%.3f\n",
                    river_stats.normal_frame_index, river_stats.normal_frame_count,
                    river_stats.normal_frame_time, river_stats.normal_frame_step);
    }
    if (rendered && river_scene.active() && river_scene.stats().foam_dynamic &&
        river_scene.stats().foam_updates > 0) {
        const parcel_hbg::RiverStats& river_stats = river_scene.stats();
        std::printf("parcel_hbg_gpu_smoke: river_foam_update=updates=%u time=%.3f "
                    "max_delta=%.3f fade=[%.3f,%.3f]\n",
                    river_stats.foam_updates, river_stats.foam_time,
                    river_stats.foam_max_displacement, river_stats.foam_min_fade,
                    river_stats.foam_max_fade);
        if (river_stats.foam_screen_space) {
            std::printf("parcel_hbg_gpu_smoke: river_foam_screen=particles=%u\n",
                        river_stats.foam_screen_particle_count);
        }
    }
    asset_handles.clear();
    if (scene_handle.IsValid()) {
        const auto unloaded = package.Value()->UnloadScene(scene_handle);
        if (!unloaded) {
            std::fprintf(stderr, "package scene unload failed: %s\n",
                         unloaded.GetError().ToString().c_str());
        }
    }
    (void)package.Value()->UnloadAllAssets();
    return rendered;
}

} // namespace

int main(int argc, char** argv) {
    hbg::log::init();
    hbg::log::setLevel(hbg::log::Level::Info);
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        hbg::log::shutdown();
        return 2;
    }
    if (options.help) {
        PrintUsage();
        hbg::log::shutdown();
        return 0;
    }
    if (options.package_root.empty() && !options.scene.empty()) {
        std::fprintf(stderr, "--scene requires --package\n");
        hbg::log::shutdown();
        return 2;
    }
    if (options.package_root.empty() && !options.shader_cache.empty()) {
        std::fprintf(stderr, "--shader-cache requires --package\n");
        hbg::log::shutdown();
        return 2;
    }
    if (!options.package_root.empty() && options.objects != 1) {
        std::fprintf(stderr, "--objects is only supported in synthetic mode\n");
        hbg::log::shutdown();
        return 2;
    }
    const bool ok = options.package_root.empty() ? Run(options) : RunPackage(options);
    hbg::log::shutdown();
    return ok ? 0 : 1;
}
