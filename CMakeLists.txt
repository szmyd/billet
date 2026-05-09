cmake_minimum_required(VERSION 3.11)
project(billet CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED PACKAGE_VERSION OR PACKAGE_VERSION STREQUAL "")
    set(PACKAGE_VERSION "0.1.0")
endif()

if ((DEFINED BUILD_COVERAGE) AND (${BUILD_COVERAGE}))
    include (cmake/coverage.cmake)
    APPEND_COVERAGE_COMPILER_FLAGS()
    SETUP_TARGET_FOR_COVERAGE_GCOVR_XML(NAME coverage EXECUTABLE ctest DEPENDENCIES )
elseif ((DEFINED THREAD_SANITIZER_ON) AND (${THREAD_SANITIZER_ON}))
    set(SANITIZER_TYPE "thread")
    include (cmake/sanitize.cmake)
elseif ((DEFINED ADDRESS_SANITIZER_ON) AND (${ADDRESS_SANITIZER_ON}))
    set(SANITIZER_TYPE "address")
    include (cmake/sanitize.cmake)
endif()

find_program(CCACHE_FOUND ccache)
if (CCACHE_FOUND)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
endif ()

# stdexec is not yet in Conan; pull it via FetchContent until a real
# recipe exists. sisl's async headers (sisl/async/...) are header-only
# and rely on the consumer to provide stdexec + liburing at compile/link time.
if (POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif ()

include(FetchContent)
FetchContent_Declare(
    stdexec
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG        ea0d8788da51a1671daa00b88bf238725f0844a8 # nvhpc-25.09
    GIT_SHALLOW    TRUE
)
set(STDEXEC_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(STDEXEC_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(STDEXEC_ENABLE_CUDA      OFF CACHE BOOL "" FORCE)
set(STDEXEC_ENABLE_TBB       OFF CACHE BOOL "" FORCE)
set(STDEXEC_ENABLE_IO_URING  OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(stdexec)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # stdexec's coroutine machinery materializes anonymous-namespace lambdas
    # inside its internal coroutine-frame types, which on GCC trips
    # -Wsubobject-linkage; with billet's -Werror that's a hard fail. The
    # warning is internal to stdexec's templates (not under our control)
    # and benign here -- suppress it on every stdexec consumer.
    target_compile_options(stdexec INTERFACE -Wno-empty-body -Wno-subobject-linkage)
endif ()

# stdexec's <execution> include drags in libstdc++'s PSTL, which on gcc-15
# defaults to the TBB backend and emits unresolved tbb::detail::r1::* symbols.
# We never use std::execution::par, so force the serial backend on every
# consumer to keep the link clean.
target_compile_options(stdexec INTERFACE -U_PSTL_PAR_BACKEND_TBB)
target_compile_definitions(stdexec INTERFACE _PSTL_PAR_BACKEND_SERIAL)

include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Toolchain validation: every dep declared in conanfile.py must resolve here.
# Linking is per-module; this block only proves the conan profile is wired up.
find_package(sisl REQUIRED)
find_package(liburing REQUIRED)
find_package(hdr_histogram REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(indicators REQUIRED)

add_subdirectory(src)

if ((DEFINED ENABLE_TESTS) AND (${ENABLE_TESTS}))
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
        add_subdirectory(test)
    endif()
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/example/CMakeLists.txt)
        add_subdirectory(example)
    endif()
endif()
