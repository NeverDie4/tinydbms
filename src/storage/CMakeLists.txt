add_library(tinydbms_storage STATIC storage.cpp)

target_include_directories(tinydbms_storage PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
