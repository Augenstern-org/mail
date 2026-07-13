cmake_minimum_required(VERSION 3.20)
project(mail LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options.
option(MAIL_BUILD_TESTS "Build unit / smoke tests (registered with CTest)" ON)
option(MAIL_BUILD_BENCHMARKS "Build micro-benchmarks (deferred; see benchmarks/)" OFF)

if(MAIL_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)

if(MAIL_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(MAIL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
