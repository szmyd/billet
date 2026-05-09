cmake_minimum_required(VERSION 3.11)

add_executable(billet
    main.cpp
    progress.cpp
)

target_compile_definitions(billet PRIVATE
    PACKAGE_NAME=${PROJECT_NAME}
    PACKAGE_VERSION=${PACKAGE_VERSION}
)

target_include_directories(billet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(billet PRIVATE
    engine
    report
    stats
    workload
    sisl::options
    sisl::logging
    indicators::indicators
)
