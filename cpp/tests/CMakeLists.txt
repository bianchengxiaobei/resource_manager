add_library(parcel_catch2 STATIC
    "${CMAKE_SOURCE_DIR}/third_party/catch2/catch_amalgamated.cpp"
)

target_include_directories(parcel_catch2
    PUBLIC
        "${CMAKE_SOURCE_DIR}/third_party/catch2"
)

target_compile_features(parcel_catch2 PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(parcel_catch2 PRIVATE /W3 /EHsc /GR)
else()
    target_compile_options(parcel_catch2 PRIVATE -Wall -Wextra)
endif()
parcel_enable_asan(parcel_catch2)

add_executable(parcel_fake_shader_build
    fake_shader_build.cpp
)
target_compile_features(parcel_fake_shader_build PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(parcel_fake_shader_build PRIVATE /EHsc /GR)
endif()
parcel_set_warnings(parcel_fake_shader_build)

set(PARCEL_TEST_SOURCES
    unit/test_assets.cpp
    unit/test_asset_file.cpp
    unit/test_builtin_catalog.cpp
    unit/test_bundle.cpp
    unit/test_compression.cpp
    unit/test_fuzz.cpp
    unit/test_guid.cpp
    unit/test_gltf.cpp
    unit/test_golden.cpp
    unit/test_hash.cpp
    unit/test_import.cpp
    unit/test_log.cpp
    unit/test_manifest.cpp
    unit/test_mesh_optimizer.cpp
    unit/test_operation.cpp
    unit/test_pack.cpp
    unit/test_remote.cpp
    unit/test_runtime.cpp
    unit/test_path.cpp
    unit/test_platform_file.cpp
    unit/test_result.cpp
    unit/test_scene.cpp
    unit/test_security.cpp
    unit/test_serialize_binary.cpp
    unit/test_sidecar.cpp
    unit/test_thread_pool.cpp
    unit/test_time.cpp
)

set(PARCEL_TEST_LIBS
    parcel_core
    parcel_serialize
    parcel_assets
    parcel_import
    parcel_pack
    parcel_runtime
    parcel_remote
    parcel_scene
    parcel_catch2
)

if(TARGET parcel_entt)
    list(APPEND PARCEL_TEST_SOURCES unit/test_entt.cpp)
    list(APPEND PARCEL_TEST_LIBS parcel_entt)
endif()

if(TARGET parcel_hbg)
    list(APPEND PARCEL_TEST_SOURCES unit/test_hbg.cpp)
    list(APPEND PARCEL_TEST_LIBS parcel_hbg)
endif()

add_executable(parcel_tests ${PARCEL_TEST_SOURCES})
add_dependencies(parcel_tests parcel_fake_shader_build)

target_link_libraries(parcel_tests
    PRIVATE
        ${PARCEL_TEST_LIBS}
)


target_include_directories(parcel_tests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated"
)

target_compile_features(parcel_tests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(parcel_tests PRIVATE /EHsc /GR)
endif()

target_compile_definitions(parcel_tests
    PRIVATE
        PARCEL_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
        PARCEL_GLTF_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
        PARCEL_FAKE_SHADER_BUILD="$<TARGET_FILE:parcel_fake_shader_build>"
)

if(PARCEL_ASSIMP_ROOT)
    file(TO_CMAKE_PATH "${PARCEL_ASSIMP_ROOT}/test/models/FBX/animation_with_skeleton.fbx"
         PARCEL_ASSIMP_ANIMATION_MODEL)
    if(EXISTS "${PARCEL_ASSIMP_ANIMATION_MODEL}")
        target_compile_definitions(parcel_tests
            PRIVATE PARCEL_ASSIMP_ANIMATION_MODEL="${PARCEL_ASSIMP_ANIMATION_MODEL}")
    endif()
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/data/morph_animation.fbx"
         PARCEL_ASSIMP_MORPH_MODEL)
    if(EXISTS "${PARCEL_ASSIMP_MORPH_MODEL}")
        target_compile_definitions(parcel_tests
            PRIVATE PARCEL_ASSIMP_MORPH_MODEL="${PARCEL_ASSIMP_MORPH_MODEL}")
    endif()
endif()

add_executable(parcel_golden_generate EXCLUDE_FROM_ALL
    golden/generate_golden.cpp
)
target_link_libraries(parcel_golden_generate PRIVATE parcel_serialize)
target_compile_features(parcel_golden_generate PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(parcel_golden_generate PRIVATE /EHsc /GR)
endif()

parcel_set_warnings(parcel_tests)
parcel_enable_asan(parcel_tests)

if(PARCEL_ASSIMP_RUNTIME_DLL)
    add_custom_command(TARGET parcel_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${PARCEL_ASSIMP_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:parcel_tests>"
    )
endif()

if(PARCEL_ENABLE_ASAN OR PARCEL_ENABLE_UBSAN)
    set(PARCEL_RUN_PERFORMANCE_TESTS FALSE)
else()
    set(PARCEL_RUN_PERFORMANCE_TESTS TRUE)
endif()

add_test(NAME parcel_tests COMMAND parcel_tests "~[performance]")
set_tests_properties(parcel_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT 300
)

if(PARCEL_RUN_PERFORMANCE_TESTS)
    add_test(NAME parcel_performance COMMAND parcel_tests "[performance]")
    set_tests_properties(parcel_performance PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        TIMEOUT 300
        RUN_SERIAL TRUE
    )
endif()

if(PARCEL_ENABLE_ASAN AND MSVC)
    get_filename_component(PARCEL_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(parcel_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${PARCEL_COMPILER_DIR}"
    )
endif()










