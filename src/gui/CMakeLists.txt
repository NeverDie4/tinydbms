# GUI 是可选入口：只有 TINYDBMS_BUILD_GUI=ON 时本文件才会被加入构建。
# AUTOMOC 只在本目录作用域打开，避免根级设置让其他模块也参与 moc 扫描。
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.3 REQUIRED COMPONENTS Widgets)

add_library(tinydbms_gui_objects OBJECT
    chart_data.cpp
    chart_widget.cpp
    diagnostic_list.cpp
    main_window.cpp
    result_model.cpp
    result_panel.cpp
    schema_browser.cpp
    script_editor.cpp
    session_state.cpp
    source_mapping.cpp
    worker.cpp
)

target_include_directories(tinydbms_gui_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(tinydbms_gui_objects PUBLIC
    Qt6::Widgets
)

add_executable(tinydbms-gui
    main.cpp
    $<TARGET_OBJECTS:tinydbms_gui_objects>
)

target_include_directories(tinydbms-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_definitions(tinydbms-gui PRIVATE
    TINYDBMS_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(tinydbms-gui PRIVATE
    Qt6::Widgets
)

# 后端选择与 app 的 session_core / session_unavailable 保持同一模式：
# compiler/storage 未接入时使用不可用后端，窗口仍然可用，但不伪造任何数据。
if(TINYDBMS_ENABLE_REAL_MODULES)
    target_sources(tinydbms-gui PRIVATE core_backend.cpp)
    target_link_libraries(tinydbms-gui PRIVATE tinydbms_core)
else()
    target_sources(tinydbms-gui PRIVATE unavailable_backend.cpp)
endif()
