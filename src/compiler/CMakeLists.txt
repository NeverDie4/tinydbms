add_library(tinydbms_compiler STATIC compiler.cpp)

target_include_directories(tinydbms_compiler PUBLIC
    ${PROJECT_SOURCE_DIR}/src/include
)
