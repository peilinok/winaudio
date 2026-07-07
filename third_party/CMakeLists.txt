# gtest, imgui, implot are vendored as git submodules under third_party/.
# Static CRT (/MT) is inherited from the top-level CMAKE_MSVC_RUNTIME_LIBRARY (CMP0091 NEW).
# All third_party targets are excluded from our /W4 + Werror policy (they set their own warning levels).

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(googletest)

# Dear ImGui (docking branch) — built as a small static lib we control.
add_library(imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_win32.cpp
    imgui/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC imgui imgui/backends)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# ImPlot — plotting on top of ImGui.
add_library(implot STATIC
    implot/implot.cpp
    implot/implot_items.cpp
)
target_include_directories(implot PUBLIC implot imgui)
target_link_libraries(implot PUBLIC imgui)
wa_set_project_warnings(implot)
# NOTE: implot uses our warnings but we allow it to build; core stays the strict one.

# spdlog (v1.15.3) — header-only, vendored as a submodule. Marked SYSTEM so its
# headers stay out of our /W4 + Werror policy; only Log.cpp compiles spdlog.
add_library(spdlog INTERFACE)
target_include_directories(spdlog SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/spdlog/include)
