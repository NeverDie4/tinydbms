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
