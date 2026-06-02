cmake_minimum_required(VERSION 3.11)

add_library(engine STATIC
    aligned_buf.cpp
    device_probe.cpp
    hdr.cpp
    log_summary.cpp
)

target_include_directories(engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(engine
    PUBLIC
        workload
        stats
        # sender_engine.hpp + sender_workload_ctx.hpp are header-only and
        # surface stdexec / liburing / sisl::async to consumers. Public
        # so the CLI module sees them transitively when it includes the
        # sender engine header.
        liburing::liburing
        stdexec::stdexec
        sisl::sisl
        hdr_histogram::hdr_histogram_static
    PRIVATE
        sisl::logging
)
