add_executable(tinydbms.lifecycle_test lifecycle_test.cpp)

foreach(audit_test storage_v1_stress storage_v1_metadata)
    add_executable(tinydbms.${audit_test}_test ${audit_test}_test.cpp)
    target_include_directories(tinydbms.${audit_test}_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
    target_link_libraries(tinydbms.${audit_test}_test PRIVATE tinydbms_storage)
    add_test(NAME tinydbms.${audit_test} COMMAND $<TARGET_FILE:tinydbms.${audit_test}_test>)
endforeach()
set_tests_properties(tinydbms.storage_v1_stress PROPERTIES TIMEOUT 180)

add_executable(tinydbms.storage_crud_test storage_crud_test.cpp)
target_include_directories(tinydbms.storage_crud_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.storage_crud_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.storage_crud COMMAND $<TARGET_FILE:tinydbms.storage_crud_test>)

add_executable(tinydbms.heap_delete_test heap_delete_test.cpp)
target_include_directories(tinydbms.heap_delete_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.heap_delete_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.heap_delete COMMAND $<TARGET_FILE:tinydbms.heap_delete_test>)

add_executable(tinydbms.heap_scan_test heap_scan_test.cpp)
target_include_directories(tinydbms.heap_scan_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.heap_scan_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.heap_scan COMMAND $<TARGET_FILE:tinydbms.heap_scan_test>)

add_executable(tinydbms.scan_page_test scan_page_test.cpp)
target_include_directories(tinydbms.scan_page_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.scan_page_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.scan_page COMMAND $<TARGET_FILE:tinydbms.scan_page_test>)

add_executable(tinydbms.heap_table_test heap_table_test.cpp)
target_include_directories(tinydbms.heap_table_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.heap_table_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.heap_table COMMAND $<TARGET_FILE:tinydbms.heap_table_test>)

add_executable(tinydbms.page_allocation_state_test page_allocation_state_test.cpp)
target_include_directories(tinydbms.page_allocation_state_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.page_allocation_state_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.page_allocation_state COMMAND $<TARGET_FILE:tinydbms.page_allocation_state_test>)

add_executable(tinydbms.buffer_pool_test buffer_pool_test.cpp)
target_include_directories(tinydbms.buffer_pool_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.buffer_pool_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.buffer_pool COMMAND $<TARGET_FILE:tinydbms.buffer_pool_test>)

add_executable(tinydbms.buffer_pool_flush_test buffer_pool_flush_test.cpp)
target_include_directories(tinydbms.buffer_pool_flush_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.buffer_pool_flush_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.buffer_pool_flush COMMAND $<TARGET_FILE:tinydbms.buffer_pool_flush_test>)

add_executable(tinydbms.buffer_pool_fifo_test buffer_pool_fifo_test.cpp)
target_include_directories(tinydbms.buffer_pool_fifo_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.buffer_pool_fifo_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.buffer_pool_fifo COMMAND $<TARGET_FILE:tinydbms.buffer_pool_fifo_test>)

add_executable(tinydbms.storage_buffer_pool_test storage_buffer_pool_test.cpp)
target_include_directories(tinydbms.storage_buffer_pool_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.storage_buffer_pool_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.storage_buffer_pool COMMAND $<TARGET_FILE:tinydbms.storage_buffer_pool_test>)

target_include_directories(tinydbms.lifecycle_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
)

target_link_libraries(tinydbms.lifecycle_test PRIVATE
    tinydbms_core
    tinydbms_compiler
    tinydbms_storage
)

add_test(
    NAME tinydbms.lifecycle
    COMMAND $<TARGET_FILE:tinydbms.lifecycle_test>
)

add_executable(tinydbms.storage_contract_test storage_contract_test.cpp)

target_include_directories(tinydbms.storage_contract_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/storage
)

target_link_libraries(tinydbms.storage_contract_test PRIVATE
    tinydbms_storage
)

add_test(
    NAME tinydbms.storage_contract
    COMMAND $<TARGET_FILE:tinydbms.storage_contract_test>
)

add_executable(tinydbms.page_file_test page_file_test.cpp)

target_include_directories(tinydbms.page_file_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/storage
)

target_link_libraries(tinydbms.page_file_test PRIVATE
    tinydbms_storage
)

add_test(
    NAME tinydbms.page_file
    COMMAND $<TARGET_FILE:tinydbms.page_file_test>
)

add_executable(tinydbms.file_manager_test file_manager_test.cpp)

target_include_directories(tinydbms.file_manager_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/storage
)

target_link_libraries(tinydbms.file_manager_test PRIVATE
    tinydbms_storage
)

add_test(
    NAME tinydbms.file_manager
    COMMAND $<TARGET_FILE:tinydbms.file_manager_test>
)

add_executable(tinydbms.storage_page_file_integration_test
    storage_page_file_integration_test.cpp
)

target_include_directories(tinydbms.storage_page_file_integration_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/storage
)

target_link_libraries(tinydbms.storage_page_file_integration_test PRIVATE
    tinydbms_storage
)

add_test(
    NAME tinydbms.storage_page_file_integration
    COMMAND $<TARGET_FILE:tinydbms.storage_page_file_integration_test>
)

add_executable(tinydbms.record_codec_test record_codec_test.cpp)

target_include_directories(tinydbms.record_codec_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/storage
)

target_link_libraries(tinydbms.record_codec_test PRIVATE
    tinydbms_storage
)

add_test(
    NAME tinydbms.record_codec
    COMMAND $<TARGET_FILE:tinydbms.record_codec_test>
)

add_executable(tinydbms.slotted_page_test slotted_page_test.cpp)

target_include_directories(tinydbms.slotted_page_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/storage
)

target_link_libraries(tinydbms.slotted_page_test PRIVATE
    tinydbms_storage
)

add_test(
    NAME tinydbms.slotted_page
    COMMAND $<TARGET_FILE:tinydbms.slotted_page_test>
)

add_executable(tinydbms.record_page_test record_page_test.cpp)
target_include_directories(tinydbms.record_page_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.record_page_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.record_page COMMAND $<TARGET_FILE:tinydbms.record_page_test>)

add_executable(tinydbms.record_page_file_integration_test record_page_file_integration_test.cpp)
target_include_directories(tinydbms.record_page_file_integration_test PRIVATE ${PROJECT_SOURCE_DIR}/src/storage)
target_link_libraries(tinydbms.record_page_file_integration_test PRIVATE tinydbms_storage)
add_test(NAME tinydbms.record_page_file_integration COMMAND $<TARGET_FILE:tinydbms.record_page_file_integration_test>)

add_test(
    NAME tinydbms.version
    COMMAND $<TARGET_FILE:tinydbms> --version
)

set_tests_properties(tinydbms.version PROPERTIES
    PASS_REGULAR_EXPRESSION "tinydbms 0\\.1\\.0"
)
