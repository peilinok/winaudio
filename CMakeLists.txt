cmake_minimum_required(VERSION 3.24)
project(WinAudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/FS)
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " /Zi")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /DEBUG")
endif()

enable_testing()

option(
  WINAUDIO_ENABLE_HARDWARE_VALIDATION_TESTS
  "Register the real-hardware validation script as an opt-in CTest entry."
  OFF
)
option(
  WINAUDIO_ENABLE_GUI_SMOKE_TESTS
  "Register the GUI smoke validation entrypoint as an opt-in CTest item."
  OFF
)
option(
  WINAUDIO_ENABLE_AGORA_SDK
  "Enable real Agora Windows SDK integration."
  OFF
)

set(WINAUDIO_AGORA_SDK_ROOT_DEFAULT
    "${CMAKE_SOURCE_DIR}/third_party/agora-sdk/4.2.6.142_oopz/Agora_Native_SDK_for_Windows_FULL/sdk")
set(WINAUDIO_AGORA_SDK_ROOT
    "$ENV{WINAUDIO_AGORA_SDK_ROOT}"
    CACHE PATH
    "Path to the Agora Native SDK for Windows sdk directory.")

if(NOT WINAUDIO_AGORA_SDK_ROOT)
  set(WINAUDIO_AGORA_SDK_ROOT "${WINAUDIO_AGORA_SDK_ROOT_DEFAULT}")
endif()

set(WINAUDIO_AGORA_HIGH_LEVEL_INCLUDE_DIR
    "${WINAUDIO_AGORA_SDK_ROOT}/high_level_api/include")
set(WINAUDIO_AGORA_LOW_LEVEL_INCLUDE_DIR
    "${WINAUDIO_AGORA_SDK_ROOT}/low_level_api/include")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(WINAUDIO_AGORA_BIN_DIR "${WINAUDIO_AGORA_SDK_ROOT}/x86_64")
else()
  set(WINAUDIO_AGORA_BIN_DIR "${WINAUDIO_AGORA_SDK_ROOT}/x86")
endif()

if(WINAUDIO_ENABLE_AGORA_SDK)
  if(NOT EXISTS "${WINAUDIO_AGORA_SDK_ROOT}/high_level_api/include/IAgoraRtcEngine.h")
    message(FATAL_ERROR
      "WINAUDIO_ENABLE_AGORA_SDK=ON but WINAUDIO_AGORA_SDK_ROOT is invalid: ${WINAUDIO_AGORA_SDK_ROOT}")
  endif()
  file(GLOB WINAUDIO_AGORA_RUNTIME_DLLS "${WINAUDIO_AGORA_BIN_DIR}/*.dll")
endif()

add_library(winaudio_core
  src/audio/audio_types.cpp
  src/audio/audio_session_types.cpp
  src/audio/com_support.cpp
  src/audio/audio_session_controller.cpp
  src/audio/backends/real_backends.cpp
  src/audio/backends/stub_backends.cpp
  src/audio/backends/wave_format_utils.cpp
  src/audio/pipeline/audio_ring_buffer.cpp
  src/audio/pipeline/signal_analyzer.cpp
  src/audio/pipeline/wav_dump_writer.cpp
  src/audio/resample/audio_resampler.cpp
  src/app/app_model.cpp
  src/app/device_notification_client.cpp
  src/app/probe_cli_options.cpp
  src/app/probe_ui_text.cpp
  src/rtc/agora_rtc_types.cpp
  src/rtc/agora_rtc_publisher.cpp
)

target_include_directories(winaudio_core PUBLIC src)

if(WINAUDIO_ENABLE_AGORA_SDK)
  target_include_directories(winaudio_core PRIVATE
    "${WINAUDIO_AGORA_HIGH_LEVEL_INCLUDE_DIR}"
    "${WINAUDIO_AGORA_LOW_LEVEL_INCLUDE_DIR}")
  target_compile_definitions(winaudio_core PRIVATE WINAUDIO_ENABLE_AGORA_SDK=1)
  target_link_libraries(winaudio_core PUBLIC
    "${WINAUDIO_AGORA_BIN_DIR}/agora_rtc_sdk.dll.lib"
  )
endif()

target_link_libraries(winaudio_core PUBLIC
  ole32
  mmdevapi
  winmm
  avrt
  Mfplat
  Mf
  Mfcore
  mfuuid
  wmcodecdspuuid
)

add_executable(core_pipeline_test
  tests/core_pipeline_test.cpp
)

target_link_libraries(core_pipeline_test PRIVATE winaudio_core)

add_executable(session_controller_test
  tests/session_controller_test.cpp
)

target_link_libraries(session_controller_test PRIVATE winaudio_core)

add_executable(wave_format_utils_test
  tests/wave_format_utils_test.cpp
)

target_link_libraries(wave_format_utils_test PRIVATE winaudio_core)

add_executable(app_model_text_test
  tests/app_model_text_test.cpp
)

target_link_libraries(app_model_text_test PRIVATE winaudio_core)

add_executable(probe_cli_test
  tests/probe_cli_test.cpp
)

target_link_libraries(probe_cli_test PRIVATE winaudio_core)

add_executable(probe_ui_text_test
  tests/probe_ui_text_test.cpp
)

target_link_libraries(probe_ui_text_test PRIVATE winaudio_core)

add_executable(winaudio WIN32
  src/app/win_audio_app.cpp
  src/ui/waveform_renderer.cpp
)

target_link_libraries(winaudio PRIVATE winaudio_core user32 gdi32 comctl32)

add_executable(winaudio_probe
  src/app/probe_cli_main.cpp
)

target_link_libraries(winaudio_probe PRIVATE winaudio_core)

if(WINAUDIO_ENABLE_AGORA_SDK)
  set(WINAUDIO_AGORA_RUNTIME_TARGETS
    core_pipeline_test
    session_controller_test
    wave_format_utils_test
    app_model_text_test
    probe_cli_test
    probe_ui_text_test
    winaudio
    winaudio_probe)
  foreach(target_name IN LISTS WINAUDIO_AGORA_RUNTIME_TARGETS)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target_name}>
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${WINAUDIO_AGORA_RUNTIME_DLLS}
              $<TARGET_FILE_DIR:${target_name}>)
  endforeach()
endif()

add_test(NAME core_pipeline_test COMMAND core_pipeline_test)
add_test(NAME session_controller_test COMMAND session_controller_test)
add_test(NAME wave_format_utils_test COMMAND wave_format_utils_test)
add_test(NAME app_model_text_test COMMAND app_model_text_test)
add_test(NAME probe_cli_test COMMAND probe_cli_test)
add_test(NAME probe_ui_text_test COMMAND probe_ui_text_test)
set_tests_properties(
  core_pipeline_test
  session_controller_test
  wave_format_utils_test
  app_model_text_test
  probe_cli_test
  probe_ui_text_test
  PROPERTIES LABELS "unit;hosted-stable"
)
add_test(
  NAME build_environment_tools_test
  COMMAND powershell -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/test_build_environment_tools.ps1 -BuildDir ${CMAKE_BINARY_DIR}
)
add_test(
  NAME convergence_helpers_test
  COMMAND powershell -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/test_convergence_helpers.ps1 -BuildDir ${CMAKE_BINARY_DIR}
)
add_test(
  NAME cli_integration_test
  COMMAND powershell -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/test_cli_integration.ps1 -Config $<CONFIG> -BuildDir ${CMAKE_BINARY_DIR} -SkipBuild
)
set_tests_properties(
  build_environment_tools_test
  convergence_helpers_test
  PROPERTIES LABELS "script;hosted-stable"
)
set_tests_properties(cli_integration_test PROPERTIES LABELS "integration;device")

if (WINAUDIO_ENABLE_GUI_SMOKE_TESTS)
  add_test(
    NAME gui_smoke_test
    COMMAND powershell -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/run_gui_smoke.ps1 -Config $<CONFIG> -BuildDir ${CMAKE_BINARY_DIR} -SkipBuild
  )
  set_tests_properties(gui_smoke_test PROPERTIES LABELS "gui;interaction;integration")
endif()

if (WINAUDIO_ENABLE_HARDWARE_VALIDATION_TESTS)
  add_test(
    NAME hardware_validation_test
    COMMAND powershell -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/tools/run_hardware_validation.ps1 -Config $<CONFIG> -BuildDir ${CMAKE_BINARY_DIR} -SkipBuild
  )
  set_tests_properties(hardware_validation_test PROPERTIES LABELS "hardware;integration")
endif()
