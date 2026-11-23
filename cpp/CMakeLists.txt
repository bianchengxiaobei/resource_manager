function(parcel_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:preprocessor)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

function(parcel_enable_asan target)
    if(PARCEL_ENABLE_TSAN AND (PARCEL_ENABLE_ASAN OR PARCEL_ENABLE_UBSAN))
        message(FATAL_ERROR "ThreadSanitizer cannot be combined with ASan/UBSan")
    endif()
    if(PARCEL_CLANG_CL)
        set(parcel_sanitizers)
        if(PARCEL_ENABLE_ASAN)
            list(APPEND parcel_sanitizers address)
        endif()
        if(PARCEL_ENABLE_UBSAN)
            list(APPEND parcel_sanitizers undefined)
        endif()
        if(PARCEL_ENABLE_TSAN)
            message(FATAL_ERROR "ThreadSanitizer is not available for clang-cl on Windows")
        endif()
        if(parcel_sanitizers)
            list(JOIN parcel_sanitizers "," parcel_sanitizer_flag)
            target_compile_options(${target} PRIVATE
                -fsanitize=${parcel_sanitizer_flag}
                -fno-sanitize=alignment
                -g
            )
            target_link_options(${target} PRIVATE -fsanitize=${parcel_sanitizer_flag})
        endif()
        return()
    endif()
    if(MSVC)
        if(PARCEL_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address)
        endif()
        if(PARCEL_ENABLE_UBSAN OR PARCEL_ENABLE_TSAN)
            message(WARNING "UBSan/TSan are not supported by the MSVC preset; use Clang on Linux/macOS")
        endif()
        return()
    endif()
    set(parcel_sanitizers)
    if(PARCEL_ENABLE_ASAN)
        list(APPEND parcel_sanitizers address)
    endif()
    if(PARCEL_ENABLE_UBSAN)
        list(APPEND parcel_sanitizers undefined)
    endif()
    if(PARCEL_ENABLE_TSAN)
        list(APPEND parcel_sanitizers thread)
    endif()
    if(parcel_sanitizers)
        list(JOIN parcel_sanitizers "," parcel_sanitizer_flag)
        target_compile_options(${target} PRIVATE
            -fsanitize=${parcel_sanitizer_flag}
            -fno-omit-frame-pointer
            -g
        )
        target_link_options(${target} PRIVATE -fsanitize=${parcel_sanitizer_flag})
    endif()
endfunction()

add_library(parcel_core STATIC
    src/core/guid.cpp
    src/core/hash.cpp
    src/core/log.cpp
    src/core/operation.cpp
    src/core/operation_scheduler.cpp
    src/core/path.cpp
    src/core/platform_file.cpp
    src/core/thread_pool.cpp
    src/core/time.cpp
)

add_library(parcel::core ALIAS parcel_core)

target_include_directories(parcel_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(parcel_core PUBLIC cxx_std_20)
set_target_properties(parcel_core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

parcel_set_warnings(parcel_core)
parcel_enable_asan(parcel_core)

if(MSVC)
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_core PRIVATE /GR-)
        target_compile_definitions(parcel_core PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_core PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        # MSVC's STL headers still contain exception blocks; C4530 is expected
        # when the library intentionally builds without exception handling.
        target_compile_options(parcel_core PRIVATE /EHs-c- /wd4530)
        target_compile_definitions(parcel_core PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_options(parcel_core PRIVATE /EHsc)
        target_compile_definitions(parcel_core PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
else()
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_core PRIVATE -fno-rtti)
        target_compile_definitions(parcel_core PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_core PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_core PRIVATE -fno-exceptions)
        target_compile_definitions(parcel_core PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_definitions(parcel_core PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
endif()

add_library(parcel_serialize STATIC
    src/serialize/asset_file.cpp
    src/serialize/binary.cpp
    src/serialize/builtin_catalog.cpp
    src/serialize/bundle.cpp
    src/serialize/compression.cpp
    src/serialize/format.cpp
    src/serialize/manifest.cpp
    src/serialize/math_io.cpp
)

add_library(parcel::serialize ALIAS parcel_serialize)

target_include_directories(parcel_serialize
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(parcel_serialize
    PUBLIC
        parcel_core
        parcel_flatbuffers
    PRIVATE
        parcel_lz4
        parcel_zstd
)

target_compile_features(parcel_serialize PUBLIC cxx_std_20)
set_target_properties(parcel_serialize PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

parcel_set_warnings(parcel_serialize)
parcel_enable_asan(parcel_serialize)

if(MSVC)
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_serialize PRIVATE /GR-)
        target_compile_definitions(parcel_serialize PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_serialize PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_serialize PRIVATE /EHs-c- /wd4530)
        target_compile_definitions(parcel_serialize PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_options(parcel_serialize PRIVATE /EHsc)
        target_compile_definitions(parcel_serialize PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
else()
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_serialize PRIVATE -fno-rtti)
        target_compile_definitions(parcel_serialize PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_serialize PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_serialize PRIVATE -fno-exceptions)
        target_compile_definitions(parcel_serialize PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_definitions(parcel_serialize PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
endif()

add_library(parcel_scene STATIC
    src/scene/component_codec.cpp
    src/scene/scene_data.cpp
)

add_library(parcel::scene ALIAS parcel_scene)

target_include_directories(parcel_scene
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(parcel_scene
    PUBLIC
        parcel_core
        parcel_serialize
)

target_compile_features(parcel_scene PUBLIC cxx_std_20)
set_target_properties(parcel_scene PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

parcel_set_warnings(parcel_scene)
parcel_enable_asan(parcel_scene)

if(MSVC)
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_scene PRIVATE /GR-)
        target_compile_definitions(parcel_scene PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_scene PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_scene PRIVATE /EHs-c- /wd4530)
        target_compile_definitions(parcel_scene PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_options(parcel_scene PRIVATE /EHsc)
        target_compile_definitions(parcel_scene PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
else()
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_scene PRIVATE -fno-rtti)
        target_compile_definitions(parcel_scene PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_scene PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_scene PRIVATE -fno-exceptions)
        target_compile_definitions(parcel_scene PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_definitions(parcel_scene PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
endif()

add_library(parcel_assets STATIC
    src/assets/animation_clip_asset.cpp
    src/assets/animation_clip_serializer.cpp
    src/assets/asset.cpp
    src/assets/asset_codec.cpp
    src/assets/asset_serializers.cpp
    src/assets/ies_profile_asset.cpp
    src/assets/ies_profile_serializer.cpp
    src/assets/json.cpp
    src/assets/material_shader_serializers.cpp
    src/assets/scene_asset_serializer.cpp
    src/assets/settings_serializers.cpp
    src/assets/sidecar.cpp
    src/assets/texture_mesh_serializers.cpp
)

add_library(parcel::assets ALIAS parcel_assets)

target_include_directories(parcel_assets
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(parcel_assets
    PUBLIC
        parcel_core
        parcel_serialize
        parcel_scene
)

target_compile_features(parcel_assets PUBLIC cxx_std_20)
set_target_properties(parcel_assets PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

parcel_set_warnings(parcel_assets)
parcel_enable_asan(parcel_assets)

if(MSVC)
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_assets PRIVATE /GR-)
        target_compile_definitions(parcel_assets PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_assets PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_assets PRIVATE /EHs-c- /wd4530)
        target_compile_definitions(parcel_assets PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_options(parcel_assets PRIVATE /EHsc)
        target_compile_definitions(parcel_assets PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
else()
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_assets PRIVATE -fno-rtti)
        target_compile_definitions(parcel_assets PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_assets PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_assets PRIVATE -fno-exceptions)
        target_compile_definitions(parcel_assets PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_definitions(parcel_assets PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
endif()

set(PARCEL_ENTT_ROOT "" CACHE PATH "Optional EnTT source root containing src/entt")
if(NOT PARCEL_ENTT_ROOT)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/entt/src/entt/entt.hpp")
        set(PARCEL_ENTT_ROOT "${CMAKE_SOURCE_DIR}/third_party/entt" CACHE PATH
            "Optional EnTT source root containing src/entt" FORCE)
    elseif(EXISTS "E:/Learn/entt/src/entt/entt.hpp")
        set(PARCEL_ENTT_ROOT "E:/Learn/entt" CACHE PATH
            "Optional EnTT source root containing src/entt" FORCE)
    endif()
endif()

if(PARCEL_ENTT_ROOT AND EXISTS "${PARCEL_ENTT_ROOT}/src/entt/entt.hpp")
    add_library(parcel_entt STATIC
        src/entt/entt_scene_loader.cpp
    )
    add_library(parcel::entt ALIAS parcel_entt)

    target_include_directories(parcel_entt
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${PARCEL_ENTT_ROOT}/src"
    )

    target_link_libraries(parcel_entt
        PUBLIC
            parcel_scene
            parcel_assets
    )

    target_compile_features(parcel_entt PUBLIC cxx_std_20)
    set_target_properties(parcel_entt PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    parcel_set_warnings(parcel_entt)
    parcel_enable_asan(parcel_entt)

    if(MSVC)
        if(PARCEL_CORE_NO_RTTI)
            target_compile_options(parcel_entt PRIVATE /GR-)
        else()
            target_compile_options(parcel_entt PRIVATE /GR)
        endif()
        if(PARCEL_CORE_NO_EXCEPTIONS)
            target_compile_options(parcel_entt PRIVATE /EHs-c- /wd4530)
            target_compile_definitions(parcel_entt PUBLIC ENTT_NOEXCEPTION)
        else()
            target_compile_options(parcel_entt PRIVATE /EHsc)
        endif()
    else()
        if(PARCEL_CORE_NO_RTTI)
            target_compile_options(parcel_entt PRIVATE -fno-rtti)
        endif()
        if(PARCEL_CORE_NO_EXCEPTIONS)
            target_compile_options(parcel_entt PRIVATE -fno-exceptions)
            target_compile_definitions(parcel_entt PUBLIC ENTT_NOEXCEPTION)
        endif()
    endif()
    message(STATUS "Parcel: EnTT enabled at ${PARCEL_ENTT_ROOT}")
else()
    message(STATUS "Parcel: EnTT not found; parcel_entt disabled")
endif()

set(PARCEL_HBG_ROOT "" CACHE PATH "Optional hdrp-bgfx source root")
if(NOT PARCEL_HBG_ROOT)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/hdrp-bgfx/src/render/scene/render_scene.h")
        set(PARCEL_HBG_ROOT "${CMAKE_SOURCE_DIR}/third_party/hdrp-bgfx" CACHE PATH
            "Optional hdrp-bgfx source root" FORCE)
    elseif(EXISTS "E:/Learn/hdrp-bgfx/src/render/scene/render_scene.h")
        set(PARCEL_HBG_ROOT "E:/Learn/hdrp-bgfx" CACHE PATH
            "Optional hdrp-bgfx source root" FORCE)
    endif()
endif()

set(PARCEL_HBG_BGFX_INCLUDE_DIR "" CACHE PATH "bgfx include directory used by hdrp-bgfx headers")
if(PARCEL_HBG_ROOT AND NOT PARCEL_HBG_BGFX_INCLUDE_DIR)
    foreach(parcel_bgfx_candidate
            "${PARCEL_HBG_ROOT}/build/vs2022/_deps/bgfx-src/include"
            "${PARCEL_HBG_ROOT}/build/_deps/bgfx-src/include")
        if(EXISTS "${parcel_bgfx_candidate}/bgfx/bgfx.h")
            set(PARCEL_HBG_BGFX_INCLUDE_DIR "${parcel_bgfx_candidate}" CACHE PATH
                "bgfx include directory used by hdrp-bgfx headers" FORCE)
            break()
        endif()
    endforeach()
endif()

if(PARCEL_HBG_ROOT AND PARCEL_HBG_BGFX_INCLUDE_DIR AND TARGET parcel_entt AND
   EXISTS "${PARCEL_HBG_ROOT}/src/render/scene/render_scene.h")
    add_library(parcel_hbg STATIC
        src/hbg/asset_materializer.cpp
        src/hbg/material_mapping.cpp
        src/hbg/parcel_material_system.cpp
        src/hbg/scene_render_bridge.cpp
        src/hbg/settings_bridge.cpp
        src/hbg/shader_library_bootstrap.cpp
    )
    add_library(parcel::hbg ALIAS parcel_hbg)

    target_include_directories(parcel_hbg
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${PARCEL_HBG_ROOT}/src"
            "${PARCEL_HBG_BGFX_INCLUDE_DIR}"
    )

    target_link_libraries(parcel_hbg
        PUBLIC
            parcel_core
            parcel_assets
            parcel_entt
    )

    if(TARGET hbg_app)
        target_link_libraries(parcel_hbg PRIVATE hbg_app)
    elseif(TARGET hbg_gfx)
        target_link_libraries(parcel_hbg PRIVATE hbg_gfx)
    endif()

    target_compile_features(parcel_hbg PUBLIC cxx_std_20)
    set_target_properties(parcel_hbg PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    parcel_set_warnings(parcel_hbg)
    parcel_enable_asan(parcel_hbg)
    message(STATUS "Parcel: hdrp-bgfx adapter enabled at ${PARCEL_HBG_ROOT}")
else()
    message(STATUS "Parcel: hdrp-bgfx adapter disabled (set PARCEL_HBG_ROOT and PARCEL_HBG_BGFX_INCLUDE_DIR)")
endif()

find_package(Threads REQUIRED)

add_library(parcel_import STATIC
    src/import/content_hash.cpp
    src/import/fbx_importer.cpp
    src/import/gltf_importer.cpp
    src/import/ies_importer.cpp
    src/import/import_db.cpp
    src/import/importer.cpp
    src/import/mesh_optimizer.cpp
    src/import/meta.cpp
    src/import/pipeline.cpp
    src/import/shader_build.cpp
    src/import/shader_importer.cpp
    src/import/sidecar_merge.cpp
    src/import/standard_importers.cpp
    src/import/text_importer.cpp
    src/import/texture_canonical.cpp
    src/import/texture_decode.cpp
    src/import/texture_dedup.cpp
    src/import/texture_encoder.cpp
    src/import/texture_importer.cpp
)

add_library(parcel::import ALIAS parcel_import)

target_include_directories(parcel_import
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party/cgltf"
)

target_link_libraries(parcel_import
    PUBLIC
        parcel_core
        parcel_serialize
        parcel_assets
        parcel_stb
        Threads::Threads
    PRIVATE
        parcel_bc7enc
        parcel_meshoptimizer
)

target_compile_features(parcel_import PUBLIC cxx_std_20)
set_target_properties(parcel_import PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

parcel_set_warnings(parcel_import)
parcel_enable_asan(parcel_import)

if(MSVC)
    target_compile_definitions(parcel_import PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_import PRIVATE /GR-)
        target_compile_definitions(parcel_import PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_import PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_import PRIVATE /EHs-c- /wd4530)
        target_compile_definitions(parcel_import PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_options(parcel_import PRIVATE /EHsc)
        target_compile_definitions(parcel_import PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
else()
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_import PRIVATE -fno-rtti)
        target_compile_definitions(parcel_import PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_import PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_import PRIVATE -fno-exceptions)
        target_compile_definitions(parcel_import PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_definitions(parcel_import PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
endif()

add_executable(parcel_import_cli
    src/import/cli_main.cpp
)
set_target_properties(parcel_import_cli PROPERTIES
    OUTPUT_NAME parcel_import
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(parcel_import_cli PRIVATE parcel_import)
parcel_set_warnings(parcel_import_cli)
if(MSVC)
    target_compile_options(parcel_import_cli PRIVATE /EHsc /GR)
else()
    target_compile_options(parcel_import_cli PRIVATE -fexceptions -frtti)
endif()

add_library(parcel_pack STATIC
    src/pack/pack.cpp
)

add_library(parcel::pack ALIAS parcel_pack)

target_include_directories(parcel_pack
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(parcel_pack
    PUBLIC
        parcel_core
        parcel_serialize
        parcel_assets
    PRIVATE
        Threads::Threads
)

target_compile_features(parcel_pack PUBLIC cxx_std_20)
set_target_properties(parcel_pack PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

parcel_set_warnings(parcel_pack)
parcel_enable_asan(parcel_pack)

if(MSVC)
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_pack PRIVATE /GR-)
        target_compile_definitions(parcel_pack PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_pack PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_pack PRIVATE /EHs-c- /wd4530)
        target_compile_definitions(parcel_pack PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_options(parcel_pack PRIVATE /EHsc)
        target_compile_definitions(parcel_pack PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
else()
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_pack PRIVATE -fno-rtti)
        target_compile_definitions(parcel_pack PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_pack PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_pack PRIVATE -fno-exceptions)
        target_compile_definitions(parcel_pack PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_definitions(parcel_pack PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
endif()

add_executable(parcel_pack_cli
    src/pack/cli_main.cpp
)
set_target_properties(parcel_pack_cli PROPERTIES
    OUTPUT_NAME parcel_pack
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(parcel_pack_cli PRIVATE parcel_pack)
parcel_set_warnings(parcel_pack_cli)
if(MSVC)
    target_compile_options(parcel_pack_cli PRIVATE /EHsc /GR)
else()
    target_compile_options(parcel_pack_cli PRIVATE -fexceptions -frtti)
endif()

add_library(parcel_runtime STATIC
    src/runtime/asset_system.cpp
    src/runtime/bundle_loader.cpp
    src/runtime/downloader.cpp
    src/runtime/file_system.cpp
    src/runtime/handle.cpp
    src/runtime/resource_package.cpp
)

add_library(parcel::runtime ALIAS parcel_runtime)

target_include_directories(parcel_runtime
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(parcel_runtime
    PUBLIC
        parcel_core
        parcel_serialize
        parcel_assets
    PRIVATE
        Threads::Threads
)

target_compile_features(parcel_runtime PUBLIC cxx_std_20)
set_target_properties(parcel_runtime PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

parcel_set_warnings(parcel_runtime)
parcel_enable_asan(parcel_runtime)

if(MSVC)
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_runtime PRIVATE /GR-)
        target_compile_definitions(parcel_runtime PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_runtime PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_runtime PRIVATE /EHs-c- /wd4530)
        target_compile_definitions(parcel_runtime PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_options(parcel_runtime PRIVATE /EHsc)
        target_compile_definitions(parcel_runtime PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
else()
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_runtime PRIVATE -fno-rtti)
        target_compile_definitions(parcel_runtime PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_runtime PUBLIC PARCEL_HAS_RTTI=1)
    endif()

    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_runtime PRIVATE -fno-exceptions)
        target_compile_definitions(parcel_runtime PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_definitions(parcel_runtime PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
endif()

add_library(parcel_remote STATIC
    src/remote/remote.cpp
    src/remote/manifest_service.cpp
    src/remote/win_http.cpp
)

add_library(parcel::remote ALIAS parcel_remote)

target_include_directories(parcel_remote
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(parcel_remote
    PUBLIC
        parcel_core
        parcel_serialize
        parcel_assets
        parcel_runtime
    PRIVATE
        Threads::Threads
)

target_compile_features(parcel_remote PUBLIC cxx_std_20)
set_target_properties(parcel_remote PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

parcel_set_warnings(parcel_remote)
parcel_enable_asan(parcel_remote)

if(WIN32)
    target_link_libraries(parcel_remote PRIVATE winhttp)
endif()

if(MSVC)
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_remote PRIVATE /GR-)
        target_compile_definitions(parcel_remote PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_remote PUBLIC PARCEL_HAS_RTTI=1)
    endif()
    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_remote PRIVATE /EHs-c- /wd4530)
        target_compile_definitions(parcel_remote PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_options(parcel_remote PRIVATE /EHsc)
        target_compile_definitions(parcel_remote PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
else()
    if(PARCEL_CORE_NO_RTTI)
        target_compile_options(parcel_remote PRIVATE -fno-rtti)
        target_compile_definitions(parcel_remote PUBLIC PARCEL_HAS_RTTI=0)
    else()
        target_compile_definitions(parcel_remote PUBLIC PARCEL_HAS_RTTI=1)
    endif()
    if(PARCEL_CORE_NO_EXCEPTIONS)
        target_compile_options(parcel_remote PRIVATE -fno-exceptions)
        target_compile_definitions(parcel_remote PUBLIC PARCEL_HAS_EXCEPTIONS=0)
    else()
        target_compile_definitions(parcel_remote PUBLIC PARCEL_HAS_EXCEPTIONS=1)
    endif()
endif()

if(TARGET parcel_hbg)
    target_link_libraries(parcel_hbg PUBLIC parcel_runtime)
endif()

set(PARCEL_ASSIMP_ROOT "" CACHE PATH "Optional assimp root containing include/, lib/ and bin/")
if(PARCEL_ASSIMP_ROOT)
    find_path(PARCEL_ASSIMP_INCLUDE_DIR
        NAMES assimp/scene.h
        PATHS "${PARCEL_ASSIMP_ROOT}/include"
        NO_DEFAULT_PATH
        REQUIRED
    )
    find_library(PARCEL_ASSIMP_LIBRARY
        NAMES assimp-vc143-mt assimp
        PATHS
            "${PARCEL_ASSIMP_ROOT}/lib/MinSizeRel"
            "${PARCEL_ASSIMP_ROOT}/lib/Release"
            "${PARCEL_ASSIMP_ROOT}/lib/Debug"
        NO_DEFAULT_PATH
        REQUIRED
    )
    find_file(PARCEL_ASSIMP_RUNTIME_DLL
        NAMES assimp-vc143-mt.dll assimp.dll
        PATHS
            "${PARCEL_ASSIMP_ROOT}/bin/MinSizeRel"
            "${PARCEL_ASSIMP_ROOT}/bin/Release"
            "${PARCEL_ASSIMP_ROOT}/bin/Debug"
        NO_DEFAULT_PATH
    )
    add_library(parcel_assimp SHARED IMPORTED)
    set_target_properties(parcel_assimp PROPERTIES
        IMPORTED_IMPLIB "${PARCEL_ASSIMP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PARCEL_ASSIMP_INCLUDE_DIR}"
    )
    if(PARCEL_ASSIMP_RUNTIME_DLL)
        set_target_properties(parcel_assimp PROPERTIES
            IMPORTED_LOCATION "${PARCEL_ASSIMP_RUNTIME_DLL}"
        )
        add_custom_command(TARGET parcel_import_cli POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${PARCEL_ASSIMP_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:parcel_import_cli>"
        )
        set(PARCEL_ASSIMP_RUNTIME_DLL "${PARCEL_ASSIMP_RUNTIME_DLL}" CACHE INTERNAL "")
    endif()
    target_link_libraries(parcel_import PRIVATE parcel_assimp)
    target_compile_definitions(parcel_import PRIVATE PARCEL_IMPORT_ASSIMP=1)
    message(STATUS "Parcel: FBX importer enabled with assimp at ${PARCEL_ASSIMP_ROOT}")
endif()

if(PARCEL_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(PARCEL_BUILD_FUZZER)
    if(PARCEL_CLANG_CL AND PARCEL_FUZZ_LIBFUZZER)
        get_filename_component(_parcel_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB _parcel_fuzzer_runtimes
            "${_parcel_llvm_bin}/../lib/clang/*/lib/windows/clang_rt.fuzzer-x86_64.lib")
        list(LENGTH _parcel_fuzzer_runtimes _parcel_fuzzer_runtime_count)
        if(_parcel_fuzzer_runtime_count EQUAL 0)
            message(FATAL_ERROR "clang_rt.fuzzer-x86_64.lib was not found next to clang-cl")
        endif()
        math(EXPR _parcel_fuzzer_runtime_index "${_parcel_fuzzer_runtime_count} - 1")
        list(GET _parcel_fuzzer_runtimes ${_parcel_fuzzer_runtime_index} _parcel_fuzzer_runtime)
        message(STATUS "Parcel: libFuzzer runtime ${_parcel_fuzzer_runtime}")
    endif()

    function(parcel_add_fuzzer target format)
        string(TOUPPER "${format}" format_upper)
        add_executable(${target} fuzz/fuzz_formats.cpp)
        if(format STREQUAL "sidecar")
            target_link_libraries(${target} PRIVATE parcel_assets parcel_import parcel_remote)
        else()
            target_link_libraries(${target} PRIVATE parcel_serialize)
        endif()
        target_compile_features(${target} PRIVATE cxx_std_20)
        parcel_set_warnings(${target})
        if(NOT format STREQUAL "combined")
            target_compile_definitions(${target} PRIVATE PARCEL_FUZZ_FORMAT_${format_upper}=1)
        endif()
        if(PARCEL_CLANG_CL AND PARCEL_FUZZ_LIBFUZZER)
            target_compile_options(${target} PRIVATE /EHsc /GR "-fsanitize=fuzzer")
            target_link_libraries(${target} PRIVATE "${_parcel_fuzzer_runtime}")
        elseif(MSVC)
            target_compile_options(${target} PRIVATE /EHsc /GR)
            target_compile_definitions(${target} PRIVATE PARCEL_FUZZ_STANDALONE)
        else()
            target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address)
            target_link_options(${target} PRIVATE -fsanitize=fuzzer,address)
        endif()
    endfunction()

    parcel_add_fuzzer(parcel_fuzz combined)
    parcel_add_fuzzer(parcel_fuzz_asset asset)
    parcel_add_fuzzer(parcel_fuzz_bundle bundle)
    parcel_add_fuzzer(parcel_fuzz_manifest manifest)
    parcel_add_fuzzer(parcel_fuzz_sidecar sidecar)

    if(PARCEL_BUILD_TESTS AND MSVC AND NOT PARCEL_FUZZ_LIBFUZZER)
        foreach(fuzz_pair "asset;prca" "bundle;prcl" "manifest;prcm")
            list(GET fuzz_pair 0 fuzz_target)
            list(GET fuzz_pair 1 fuzz_extension)
            add_test(NAME parcel_fuzz_${fuzz_target}_smoke
                COMMAND parcel_fuzz_${fuzz_target}
                    "${CMAKE_SOURCE_DIR}/cpp/tests/golden/minimal.${fuzz_extension}")
            set_tests_properties(parcel_fuzz_${fuzz_target}_smoke PROPERTIES TIMEOUT 10)
        endforeach()
        add_test(NAME parcel_fuzz_sidecar_smoke
            COMMAND parcel_fuzz_sidecar
                "${CMAKE_SOURCE_DIR}/cpp/fuzz/corpus/sidecar/minimal.json")
        set_tests_properties(parcel_fuzz_sidecar_smoke PROPERTIES TIMEOUT 10)
    endif()
endif()

if(PARCEL_BUILD_BENCHMARKS)
    add_executable(parcel_bench bench/bench_main.cpp)
    target_link_libraries(parcel_bench PRIVATE parcel_runtime parcel_serialize)
    target_compile_features(parcel_bench PRIVATE cxx_std_20)
    parcel_set_warnings(parcel_bench)
    parcel_enable_asan(parcel_bench)
    if(MSVC)
        target_compile_options(parcel_bench PRIVATE /EHsc /GR)
    else()
        target_compile_options(parcel_bench PRIVATE -fexceptions -frtti)
    endif()
endif()























