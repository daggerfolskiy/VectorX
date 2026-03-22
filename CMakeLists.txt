cmake_minimum_required(VERSION 3.20)
project(timmy_black_menu_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

if (NOT WIN32)
    message(FATAL_ERROR "timmy_black_menu_cpp currently supports Windows only.")
endif()

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
)
FetchContent_MakeAvailable(imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

add_executable(timmy_menu WIN32
    src/main.cpp
    src/debug/debug_log.cpp
    src/window/window_utils.cpp
    src/ui/menu.cpp
    src/features/esp.cpp
    src/features/aim.cpp
)
target_include_directories(timmy_menu PRIVATE src)
target_link_libraries(timmy_menu PRIVATE imgui_lib d3d11 dxgi dwmapi)

if (WIN32)
    target_compile_definitions(timmy_menu PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(timmy_menu PRIVATE wininet)
endif()
