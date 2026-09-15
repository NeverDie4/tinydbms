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

if(TINYDBMS_BUILD_STORAGE_BENCHMARKS)
    add_executable(tinydbms_io_benchmark
        io_benchmark.cpp
    )
    target_include_directories(tinydbms_io_benchmark PRIVATE
        ${PROJECT_SOURCE_DIR}/src/storage
    )
    target_link_libraries(tinydbms_io_benchmark PRIVATE tinydbms_storage)

    add_executable(tinydbms_prefetch_benchmark prefetch_benchmark.cpp)
    target_link_libraries(tinydbms_prefetch_benchmark PRIVATE tinydbms_storage)
endif()

target_include_directories(tinydbms_storage PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
