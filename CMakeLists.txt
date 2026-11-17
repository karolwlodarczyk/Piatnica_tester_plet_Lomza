cmake_minimum_required(VERSION 3.20)
project(WenglorScanner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

set(SDK_DIR "${CMAKE_SOURCE_DIR}/vendor/sdk/Software_ShapeDriveG4_SDK_Windows_x86_64_1.3.0/Sensor3D")

add_executable(WenglorScanner WIN32
    src/main.cpp
    src/Sensor.cpp
    src/Sensor.h
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(WenglorScanner PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    "${SDK_DIR}"
)

target_compile_definitions(WenglorScanner PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(WenglorScanner PRIVATE /utf-8 /W3 /MP)

target_link_libraries(WenglorScanner PRIVATE
    "${SDK_DIR}/Sensor3d.lib"
    d3d11 dxgi d3dcompiler comdlg32 dwmapi
)

add_custom_command(TARGET WenglorScanner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDK_DIR}/Sensor3d.dll" "$<TARGET_FILE_DIR:WenglorScanner>"
)
