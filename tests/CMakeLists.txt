add_test(
    NAME tinydbms.version
    COMMAND $<TARGET_FILE:tinydbms> --version
)

set_tests_properties(tinydbms.version PROPERTIES
    PASS_REGULAR_EXPRESSION "tinydbms 0\\.1\\.0"
)

add_executable(tinydbms_contracts_compile_test contracts_compile_test.cpp)

target_include_directories(tinydbms_contracts_compile_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

add_test(
    NAME tinydbms.contracts_compile
    COMMAND $<TARGET_FILE:tinydbms_contracts_compile_test>
)

add_executable(tinydbms_core_lifecycle_test
    core_lifecycle_test.cpp
    fakes/compiler_fake.cpp
    fakes/storage_fake.cpp
    $<TARGET_OBJECTS:tinydbms_core_objects>
)

target_include_directories(tinydbms_core_lifecycle_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)

add_test(
    NAME tinydbms.core_lifecycle
    COMMAND $<TARGET_FILE:tinydbms_core_lifecycle_test>
)

add_executable(tinydbms_core_executor_test
    core_executor_test.cpp
    fakes/compiler_fake.cpp
    fakes/storage_fake.cpp
    $<TARGET_OBJECTS:tinydbms_core_objects>
)

target_include_directories(tinydbms_core_executor_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
)

add_test(
    NAME tinydbms.core_executor
    COMMAND $<TARGET_FILE:tinydbms_core_executor_test>
)

add_executable(tinydbms_app_cli_test app_cli_test.cpp)

target_include_directories(tinydbms_app_cli_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/app
)

target_link_libraries(tinydbms_app_cli_test PRIVATE
    tinydbms_cli_logic
)

add_test(
    NAME tinydbms.app_cli
    COMMAND $<TARGET_FILE:tinydbms_app_cli_test>
)
