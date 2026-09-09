add_test(
    NAME tinydbms.version
    COMMAND $<TARGET_FILE:tinydbms> --version
)

set_tests_properties(tinydbms.version PROPERTIES
    PASS_REGULAR_EXPRESSION "tinydbms 0\\.1\\.0"
)

add_executable(tinydbms_contracts_compile_test contracts_compile_test.cpp)

target_link_libraries(tinydbms_contracts_compile_test PRIVATE
    tinydbms_core
    tinydbms_compiler
    tinydbms_storage
)

add_test(
    NAME tinydbms.contracts_compile
    COMMAND $<TARGET_FILE:tinydbms_contracts_compile_test>
)

add_executable(tinydbms_core_lifecycle_test
    core_lifecycle_test.cpp
    fakes/compiler_fake.cpp
    fakes/storage_fake.cpp
)

target_include_directories(tinydbms_core_lifecycle_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)

target_link_libraries(tinydbms_core_lifecycle_test PRIVATE
    tinydbms_core
)

add_test(
    NAME tinydbms.core_lifecycle
    COMMAND $<TARGET_FILE:tinydbms_core_lifecycle_test>
)
