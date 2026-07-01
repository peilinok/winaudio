cmake_minimum_required(VERSION 3.21)   # CMP0092 NEW => no default /W3
project(WinAudio LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "WinAudio requires the MSVC toolchain (cl.exe).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WinAudio requires a 64-bit (x64) build.")
endif()

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
add_subdirectory(src/cli)
add_subdirectory(src/gui)
# Tasks 3-4 each append one add_subdirectory line below (src/gui, src/tests).
