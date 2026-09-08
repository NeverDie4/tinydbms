add_library(tinydbms_core STATIC core.cpp)

target_include_directories(tinydbms_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
