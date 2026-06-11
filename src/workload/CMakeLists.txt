cmake_minimum_required(VERSION 3.11)

add_library(workload STATIC
    pg_wal_commit.cpp
    postgresql.cpp
    random_read_4k.cpp
)

target_include_directories(workload PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(workload
    PUBLIC
        # Profile headers transitively pull engine's sender_workload_ctx.hpp,
        # which surfaces stdexec + sisl::async + stats. Liburing comes in
        # via sisl::async. PUBLIC so consumers (engine, cli) see the
        # symbols without re-declaring them.
        stdexec::stdexec
        stats
        sisl::sisl
        liburing::liburing
        hdr_histogram::hdr_histogram_static
    PRIVATE
        sisl::logging
)
