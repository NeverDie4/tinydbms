cmake_minimum_required(VERSION 3.20)

project(tinydbms
    VERSION 0.1.0
    DESCRIPTION "A lightweight educational DBMS"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TINYDBMS_BUILD_TESTS "Build tinydbms tests" ON)

add_compile_options(
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
    $<$<COMPILE_LANGUAGE:CXX>:-Wconversion>
    $<$<COMPILE_LANGUAGE:CXX>:-Wsign-conversion>
)

add_subdirectory(src/compiler)
add_subdirectory(src/storage)
add_subdirectory(src/core)
add_subdirectory(src/app)

if(TINYDBMS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
