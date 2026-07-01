# --- Dear ImGui: core + Win32/DX11 backends (static lib, warnings off) ---
add_library(imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_win32.cpp
    imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
)
target_compile_options(imgui PRIVATE /W0)

# --- ImPlot (depends on ImGui) ---
add_library(implot STATIC
    implot/implot.cpp
    implot/implot_items.cpp
)
target_link_libraries(implot PUBLIC imgui)
target_include_directories(implot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/implot)
target_compile_options(implot PRIVATE /W0)

# --- GoogleTest (vendored full tree); match our dynamic CRT, skip gmock/install ---
set(BUILD_GMOCK OFF)
set(INSTALL_GTEST OFF)
set(gtest_force_shared_crt ON)
# Real layout: third_party/googletest has no root CMakeLists.txt;
# the actual one lives at googletest/googletest/CMakeLists.txt.
# GOOGLETEST_VERSION must be set here because the root-level CMakeLists.txt
# (which normally defines it) is absent from this vendored copy.
set(GOOGLETEST_VERSION "1.14.0")
add_subdirectory(googletest/googletest)
