add_library(parcel_flatbuffers INTERFACE)
add_library(parcel::flatbuffers ALIAS parcel_flatbuffers)
target_include_directories(parcel_flatbuffers
    INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/flatbuffers/include"
)

set(PARCEL_ZSTD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/zstd")

add_library(parcel_zstd STATIC
    ${PARCEL_ZSTD_DIR}/common/debug.c
    ${PARCEL_ZSTD_DIR}/common/entropy_common.c
    ${PARCEL_ZSTD_DIR}/common/error_private.c
    ${PARCEL_ZSTD_DIR}/common/fse_decompress.c
    ${PARCEL_ZSTD_DIR}/common/pool.c
    ${PARCEL_ZSTD_DIR}/common/threading.c
    ${PARCEL_ZSTD_DIR}/common/xxhash.c
    ${PARCEL_ZSTD_DIR}/common/zstd_common.c
    ${PARCEL_ZSTD_DIR}/compress/fse_compress.c
    ${PARCEL_ZSTD_DIR}/compress/hist.c
    ${PARCEL_ZSTD_DIR}/compress/huf_compress.c
    ${PARCEL_ZSTD_DIR}/compress/zstd_compress.c
    ${PARCEL_ZSTD_DIR}/compress/zstd_compress_literals.c
    ${PARCEL_ZSTD_DIR}/compress/zstd_compress_sequences.c
    ${PARCEL_ZSTD_DIR}/compress/zstd_compress_superblock.c
    ${PARCEL_ZSTD_DIR}/compress/zstd_double_fast.c
    ${PARCEL_ZSTD_DIR}/compress/zstd_fast.c
    ${PARCEL_ZSTD_DIR}/compress/zstd_lazy.c
    ${PARCEL_ZSTD_DIR}/compress/zstd_ldm.c
    ${PARCEL_ZSTD_DIR}/compress/zstd_opt.c
    ${PARCEL_ZSTD_DIR}/compress/zstd_preSplit.c
    ${PARCEL_ZSTD_DIR}/decompress/huf_decompress.c
    ${PARCEL_ZSTD_DIR}/decompress/zstd_ddict.c
    ${PARCEL_ZSTD_DIR}/decompress/zstd_decompress.c
    ${PARCEL_ZSTD_DIR}/decompress/zstd_decompress_block.c
)
add_library(parcel::zstd ALIAS parcel_zstd)
target_include_directories(parcel_zstd
    PUBLIC
        "${PARCEL_ZSTD_DIR}"
)
target_compile_features(parcel_zstd PRIVATE c_std_11)
target_compile_definitions(parcel_zstd PRIVATE
    ZSTD_DISABLE_ASM
    ZSTD_LEGACY_SUPPORT=0
)
if(MSVC)
    target_compile_options(parcel_zstd PRIVATE /W3)
else()
    target_compile_options(parcel_zstd PRIVATE -w)
endif()

add_library(parcel_lz4 STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/lz4/lz4.cpp"
)
add_library(parcel::lz4 ALIAS parcel_lz4)
target_include_directories(parcel_lz4
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/lz4"
)
target_compile_features(parcel_lz4 PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(parcel_lz4 PRIVATE /W3)
else()
    target_compile_options(parcel_lz4 PRIVATE -w)
endif()

add_library(parcel_stb STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/stb/stb_dxt.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/stb/stb_image.cpp"
)
add_library(parcel::stb ALIAS parcel_stb)
target_include_directories(parcel_stb
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/stb"
)
target_compile_features(parcel_stb PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(parcel_stb PRIVATE /W0)
else()
    target_compile_options(parcel_stb PRIVATE -w)
endif()

add_library(parcel_bc7enc STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/bc7enc/bc7enc.c"
)
add_library(parcel::bc7enc ALIAS parcel_bc7enc)
target_include_directories(parcel_bc7enc
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/bc7enc"
)
target_compile_features(parcel_bc7enc PRIVATE c_std_11)
if(MSVC)
    target_compile_options(parcel_bc7enc PRIVATE /W0 /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(parcel_bc7enc PRIVATE -w)
endif()

add_library(parcel_meshoptimizer STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer/src/indexgenerator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer/src/overdrawoptimizer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer/src/vcacheoptimizer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer/src/vfetchoptimizer.cpp"
)
add_library(parcel::meshoptimizer ALIAS parcel_meshoptimizer)
target_include_directories(parcel_meshoptimizer
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer/src"
)
target_compile_features(parcel_meshoptimizer PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(parcel_meshoptimizer PRIVATE /W0)
else()
    target_compile_options(parcel_meshoptimizer PRIVATE -w)
endif()
