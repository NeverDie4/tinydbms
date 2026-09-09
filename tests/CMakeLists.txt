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

add_executable(tinydbms_compiler_splitter_test compiler_splitter_test.cpp)

target_link_libraries(tinydbms_compiler_splitter_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_splitter
    COMMAND $<TARGET_FILE:tinydbms_compiler_splitter_test>
)

add_executable(tinydbms_compiler_lexer_test compiler_lexer_test.cpp)

target_include_directories(tinydbms_compiler_lexer_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_lexer_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_lexer
    COMMAND $<TARGET_FILE:tinydbms_compiler_lexer_test>
)

add_executable(tinydbms_compiler_parser_create_test compiler_parser_create_test.cpp)

target_include_directories(tinydbms_compiler_parser_create_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_parser_create_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_parser_create
    COMMAND $<TARGET_FILE:tinydbms_compiler_parser_create_test>
)

add_executable(tinydbms_compiler_parser_insert_test compiler_parser_insert_test.cpp)

target_include_directories(tinydbms_compiler_parser_insert_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_parser_insert_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_parser_insert
    COMMAND $<TARGET_FILE:tinydbms_compiler_parser_insert_test>
)

add_executable(tinydbms_compiler_parser_expression_test compiler_parser_expression_test.cpp)

target_include_directories(tinydbms_compiler_parser_expression_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_parser_expression_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_parser_expression
    COMMAND $<TARGET_FILE:tinydbms_compiler_parser_expression_test>
)

add_executable(tinydbms_compiler_parser_select_test compiler_parser_select_test.cpp)

target_include_directories(tinydbms_compiler_parser_select_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_parser_select_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_parser_select
    COMMAND $<TARGET_FILE:tinydbms_compiler_parser_select_test>
)

add_executable(tinydbms_compiler_parser_delete_test compiler_parser_delete_test.cpp)

target_include_directories(tinydbms_compiler_parser_delete_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_parser_delete_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_parser_delete
    COMMAND $<TARGET_FILE:tinydbms_compiler_parser_delete_test>
)

add_executable(tinydbms_compiler_semantic_create_test compiler_semantic_create_test.cpp)

target_include_directories(tinydbms_compiler_semantic_create_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_semantic_create_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_semantic_create
    COMMAND $<TARGET_FILE:tinydbms_compiler_semantic_create_test>
)

add_executable(tinydbms_compiler_semantic_insert_test compiler_semantic_insert_test.cpp)

target_include_directories(tinydbms_compiler_semantic_insert_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_semantic_insert_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_semantic_insert
    COMMAND $<TARGET_FILE:tinydbms_compiler_semantic_insert_test>
)

add_executable(tinydbms_compiler_semantic_expression_test compiler_semantic_expression_test.cpp)

target_include_directories(tinydbms_compiler_semantic_expression_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_semantic_expression_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_semantic_expression
    COMMAND $<TARGET_FILE:tinydbms_compiler_semantic_expression_test>
)

add_executable(tinydbms_compiler_semantic_select_test compiler_semantic_select_test.cpp)

target_include_directories(tinydbms_compiler_semantic_select_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_semantic_select_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_semantic_select
    COMMAND $<TARGET_FILE:tinydbms_compiler_semantic_select_test>
)

add_executable(tinydbms_compiler_semantic_delete_test compiler_semantic_delete_test.cpp)

target_include_directories(tinydbms_compiler_semantic_delete_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_semantic_delete_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_semantic_delete
    COMMAND $<TARGET_FILE:tinydbms_compiler_semantic_delete_test>
)

add_executable(tinydbms_compiler_planner_create_test compiler_planner_create_test.cpp)

target_include_directories(tinydbms_compiler_planner_create_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_planner_create_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_planner_create
    COMMAND $<TARGET_FILE:tinydbms_compiler_planner_create_test>
)

add_executable(tinydbms_compiler_planner_insert_test compiler_planner_insert_test.cpp)

target_include_directories(tinydbms_compiler_planner_insert_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_planner_insert_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_planner_insert
    COMMAND $<TARGET_FILE:tinydbms_compiler_planner_insert_test>
)

add_executable(tinydbms_compiler_planner_select_test compiler_planner_select_test.cpp)

target_include_directories(tinydbms_compiler_planner_select_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_planner_select_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_planner_select
    COMMAND $<TARGET_FILE:tinydbms_compiler_planner_select_test>
)

add_executable(tinydbms_compiler_planner_delete_test compiler_planner_delete_test.cpp)

target_include_directories(tinydbms_compiler_planner_delete_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_planner_delete_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_planner_delete
    COMMAND $<TARGET_FILE:tinydbms_compiler_planner_delete_test>
)

add_executable(tinydbms_compiler_compile_test compiler_compile_test.cpp)

target_link_libraries(tinydbms_compiler_compile_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_compile
    COMMAND $<TARGET_FILE:tinydbms_compiler_compile_test>
)

add_executable(tinydbms_compiler_hardening_test compiler_hardening_test.cpp)

target_link_libraries(tinydbms_compiler_hardening_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_hardening
    COMMAND $<TARGET_FILE:tinydbms_compiler_hardening_test>
)

add_executable(tinydbms_compiler_optimizer_test compiler_optimizer_test.cpp)

target_include_directories(tinydbms_compiler_optimizer_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_optimizer_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_optimizer
    COMMAND $<TARGET_FILE:tinydbms_compiler_optimizer_test>
)

add_executable(tinydbms_compiler_plan_formatter_test compiler_plan_formatter_test.cpp)

target_include_directories(tinydbms_compiler_plan_formatter_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_plan_formatter_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_plan_formatter
    COMMAND $<TARGET_FILE:tinydbms_compiler_plan_formatter_test>
)

add_executable(tinydbms_compiler_fuzz_valid_test compiler_fuzz_valid_test.cpp)

target_include_directories(tinydbms_compiler_fuzz_valid_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_fuzz_valid_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_fuzz_valid
    COMMAND $<TARGET_FILE:tinydbms_compiler_fuzz_valid_test>
)

add_executable(tinydbms_compiler_fuzz_invalid_test compiler_fuzz_invalid_test.cpp)

target_include_directories(tinydbms_compiler_fuzz_invalid_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler
)

target_link_libraries(tinydbms_compiler_fuzz_invalid_test PRIVATE
    tinydbms_compiler
)

add_test(
    NAME tinydbms.compiler_fuzz_invalid
    COMMAND $<TARGET_FILE:tinydbms_compiler_fuzz_invalid_test>
)
