add_executable(tinydbms main.cpp)

target_include_directories(tinydbms PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_definitions(tinydbms PRIVATE
    TINYDBMS_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(tinydbms PRIVATE
    tinydbms_core
)
