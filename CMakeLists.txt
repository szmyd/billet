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

# stdexec (NVIDIA std::execution) -- provided by the conan package now (sisl ships it transitively;
# see sisl/3rd_party/stdexec). The stdexec::stdexec target carries the include dirs, the
# _PSTL_PAR_BACKEND_SERIAL define, and (on gcc) the -Wno-empty-body/-Wno-subobject-linkage flags.
find_package(stdexec REQUIRED)

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
