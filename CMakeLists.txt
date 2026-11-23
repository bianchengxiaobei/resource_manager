cmake_minimum_required(VERSION 3.24)

project(Parcel VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)

option(PARCEL_BUILD_TESTS "Build Parcel unit tests" ${BUILD_TESTING})
option(PARCEL_CORE_NO_EXCEPTIONS "Build parcel_core without C++ exceptions" ON)
option(PARCEL_CORE_NO_RTTI "Build parcel_core without RTTI" ON)
option(PARCEL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(PARCEL_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer (Clang/ClangCL)" OFF)
option(PARCEL_ENABLE_TSAN "Enable ThreadSanitizer (non-MSVC)" OFF)
option(PARCEL_BUILD_FUZZER "Build the format fuzz targets" OFF)
option(PARCEL_FUZZ_LIBFUZZER "Build format fuzz targets with libFuzzer (ClangCL)" OFF)
option(PARCEL_BUILD_BENCHMARKS "Build performance benchmark targets" OFF)

set(PARCEL_CLANG_CL FALSE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(PARCEL_CLANG_CL TRUE)
endif()

if(PARCEL_CLANG_CL AND
   (PARCEL_ENABLE_UBSAN OR (PARCEL_BUILD_FUZZER AND PARCEL_FUZZ_LIBFUZZER)))
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING
        "Static CRT matches the Clang sanitizer/fuzzer runtime" FORCE)
    add_compile_definitions(_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
endif()

if(PARCEL_BUILD_TESTS)
    enable_testing()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/${config}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}")
endforeach()

add_subdirectory(third_party)
add_subdirectory(cpp)





