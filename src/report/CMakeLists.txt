cmake_minimum_required(VERSION 3.11)

add_library(report STATIC
    json_writer.cpp
)

target_include_directories(report PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(report
    PRIVATE
        nlohmann_json::nlohmann_json
        hdr_histogram::hdr_histogram_static
)
