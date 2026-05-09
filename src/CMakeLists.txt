cmake_minimum_required(VERSION 3.11)

add_compile_options(-Wall -Wextra -Werror)

include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR})

# Per-module static libs land here as they come online (engine, workload, stats, report).
# The cli module is the binary target and consumes whichever modules exist.
add_subdirectory(workload)
add_subdirectory(engine)
add_subdirectory(report)
add_subdirectory(stats)
add_subdirectory(cli)
