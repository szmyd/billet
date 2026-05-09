cmake_minimum_required(VERSION 3.11)

find_package(GTest REQUIRED)

# Tests provide their own main() (see test_main.cpp). Linking GTest::gmock
# (not gtest_main) avoids dragging Boost.Test's test_exec_monitor in via
# gtest_main's transitive deps.
add_executable(billet_tests
    test_async_scheduler.cpp
    test_engine.cpp
    test_main.cpp
    test_pg_checkpointer.cpp
    test_pg_reader.cpp
    test_pg_wal.cpp
    test_profiles.cpp
    test_random_read_4k.cpp
    test_report.cpp
    test_workload.cpp
)

target_include_directories(billet_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(billet_tests PRIVATE
    engine
    workload
    report
    hdr_histogram::hdr_histogram_static
    nlohmann_json::nlohmann_json
    liburing::liburing
    sisl::sisl
    stdexec
    GTest::gmock
)

add_test(NAME billet_tests COMMAND billet_tests)
