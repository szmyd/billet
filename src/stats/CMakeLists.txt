cmake_minimum_required(VERSION 3.11)

add_library(stats STATIC
    stats.cpp
)

target_include_directories(stats PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(stats
    PUBLIC
        sisl::metrics
    PRIVATE
        sisl::http
)
