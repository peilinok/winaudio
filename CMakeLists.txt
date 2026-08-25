cmake_minimum_required(VERSION 3.21)   # CMP0092 NEW => no default /W3
project(WinAudio LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "WinAudio requires the MSVC toolchain (cl.exe).")
endif()

# Static CRT (/MT, /MTd) so distributed exes are self-contained (no VC++ Redistributable needed).
# CMP0091 is NEW (cmake_minimum_required 3.21), so this drives the runtime library for every
# target, including third_party (gtest/imgui/implot); see third_party/CMakeLists.txt.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(UNICODE _UNICODE)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(${CMAKE_SOURCE_DIR}/cmake/CompilerWarnings.cmake)

enable_testing()

add_subdirectory(third_party)
add_subdirectory(src/core)
add_subdirectory(src/hook)
add_subdirectory(src/cli)
add_subdirectory(src/gui)
add_subdirectory(src/tests)
