add_library(tinydbms_storage STATIC
    buffer_pool.cpp
    cursor_state.cpp
    file_manager.cpp
    heap_table.cpp
    page_file.cpp
    record_codec.cpp
    record_page.cpp
    slotted_page.cpp
    storage.cpp
)

target_include_directories(tinydbms_storage PUBLIC
    ${PROJECT_SOURCE_DIR}/src/include
)
