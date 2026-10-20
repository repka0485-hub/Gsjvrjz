cmake_minimum_required(VERSION 3.18)
project(cf_esp CXX)
set(CMAKE_CXX_STANDARD 17)
set(IMGUI ${CMAKE_SOURCE_DIR}/imgui)          # imgui >= 1.87 checkout (AddMousePosEvent)
set(DOBBY ${CMAKE_SOURCE_DIR}/dobby)          # dobby.h + libdobby.a (arm64-v8a)
add_library(esp_core SHARED main.cpp
  ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
  ${IMGUI}/backends/imgui_impl_opengl3.cpp)
target_include_directories(esp_core PRIVATE ${IMGUI} ${DOBBY})
target_compile_definitions(esp_core PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_compile_options(esp_core PRIVATE -O2 -fvisibility=hidden)
target_link_libraries(esp_core ${DOBBY}/libdobby.a EGL GLESv3 log dl)
