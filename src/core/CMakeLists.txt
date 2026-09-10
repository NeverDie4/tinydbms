add_library(tinydbms_core_objects OBJECT
    database.cpp
    executor.cpp
    expression.cpp
    script.cpp
)

target_include_directories(tinydbms_core_objects PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

add_library(tinydbms_core STATIC
    $<TARGET_OBJECTS:tinydbms_core_objects>
)

target_include_directories(tinydbms_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(tinydbms_core PRIVATE
    tinydbms_compiler
    tinydbms_storage
)
