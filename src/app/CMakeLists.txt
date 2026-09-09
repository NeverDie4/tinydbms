add_library(tinydbms_cli_logic STATIC
    arguments.cpp
    input.cpp
    output.cpp
    runner.cpp
    terminal.cpp
)

target_include_directories(tinydbms_cli_logic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(tinydbms_cli_logic PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

add_executable(tinydbms main.cpp)

if(TINYDBMS_ENABLE_REAL_MODULES)
    target_sources(tinydbms PRIVATE session_core.cpp)
    target_compile_definitions(tinydbms PRIVATE TINYDBMS_ENABLE_REAL_MODULES=1)
else()
    target_sources(tinydbms PRIVATE session_unavailable.cpp)
endif()

target_include_directories(tinydbms PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_definitions(tinydbms PRIVATE
    TINYDBMS_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(tinydbms PRIVATE
    tinydbms_cli_logic
    tinydbms_core
    tinydbms_compiler
    tinydbms_storage
)
