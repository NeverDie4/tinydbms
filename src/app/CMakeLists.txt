add_executable(tinydbms main.cpp)

target_compile_definitions(tinydbms PRIVATE
    TINYDBMS_VERSION="${PROJECT_VERSION}"
)
