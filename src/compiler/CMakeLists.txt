add_library(tinydbms_compiler STATIC
    compiler.cpp
    lexer.cpp
    optimizer.cpp
    parser.cpp
    plan_formatter.cpp
    planner.cpp
    semantic.cpp
)

target_include_directories(tinydbms_compiler PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)
