cmake_minimum_required(VERSION 3.24)
project(HBStudio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HB_BUILD_TESTS "Build unit tests" ON)
option(HB_BUILD_TOOLS "Build command line tools" ON)

# Static MSVC runtime everywhere: the virtual camera DLL is loaded by a system
# service and must not depend on a VC++ redistributable being installed.
if(MSVC)
  add_compile_options(/utf-8)   # sources contain UTF-8 (pt-BR) literals
  # MSVC does not update the __cplusplus macro to reflect /std:c++20 unless told to (a long-standing
  # MSVC-only quirk, kept for backward compat): without this, any code - ours or a fetched
  # dependency's, e.g. the VST3 SDK's own `#if __cplusplus >= 202002L` guards - that checks
  # __cplusplus to detect C++20 sees the pre-C++11 default value (199711L) and silently takes the
  # wrong branch, which is exactly what broke the VST3 SDK's module_win32.cpp on this project.
  add_compile_options(/Zc:__cplusplus)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Threads REQUIRED)

add_library(hb_core STATIC
  src/core/event_bus.cpp
  src/core/redact.cpp
  src/core/logger.cpp
  src/core/clock.cpp
  src/core/json.cpp
  src/core/sha256.cpp
  src/audio/effect.cpp
  src/recording/writer_registry.cpp
  src/core/hardware_profile.cpp
  src/video/program_output.cpp
  src/video/synthetic_source.cpp
  src/video/pixel_convert.cpp
  src/switcher/switcher.cpp
  src/app/engine.cpp
  src/app/ptz_settings.cpp
  src/app/source_assignments.cpp
  src/app/stream_destinations.cpp
)
target_include_directories(hb_core PUBLIC src)
target_link_libraries(hb_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(hb_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TOOLS)
  add_executable(hb_probe tools/hb_probe.cpp)
  target_link_libraries(hb_probe PRIVATE hb_core)
endif()

# Windows 11 virtual camera (Media Foundation Frame Server media source + PoC app).
if(WIN32)
  add_library(hb_vcam_source SHARED
    src/virtual-camera/windows/vcam_source.cpp
    src/virtual-camera/windows/vcam_source.def)
  target_include_directories(hb_vcam_source PRIVATE src/virtual-camera/windows)
  target_link_libraries(hb_vcam_source PRIVATE
    mfplat mf mfuuid mfsensorgroup propsys ole32 advapi32 strmiids)
  target_compile_definitions(hb_vcam_source PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
  target_compile_options(hb_vcam_source PRIVATE /W4 /EHsc)

  add_library(hb_vcam_sender STATIC src/virtual-camera/windows/vcam_sender.cpp)
  target_link_libraries(hb_vcam_sender PUBLIC hb_core)
  target_compile_definitions(hb_vcam_sender PUBLIC UNICODE _UNICODE NOMINMAX)

  if(HB_BUILD_TOOLS)
    add_executable(hb_vcam_poc tools/hb_vcam_poc.cpp)
    target_link_libraries(hb_vcam_poc PRIVATE hb_vcam_sender mfplat mf mfuuid mfsensorgroup ole32)

    add_executable(hb_vcam_selftest tools/hb_vcam_selftest.cpp)
    target_link_libraries(hb_vcam_selftest PRIVATE hb_vcam_sender mfplat mf mfuuid mfsensorgroup ole32 propsys)
    add_dependencies(hb_vcam_selftest hb_vcam_source)
  endif()

  # ---- Graphical application + installer (needs the WebView2 SDK, see CI / BUILD.md) ----
  set(HB_VERSION "0.3.0")
  string(REPLACE "." "," HB_VER_COMMA "${HB_VERSION},0")
  # A plain "0.3.0" never changes between builds, so there was no way for anyone - including the
  # person who just downloaded a fresh installer - to tell a brand new build apart from an old one
  # just by looking at the version shown in the app or the installer. Append the short commit hash
  # this build was made from (falls back to the bare version if git isn't available, e.g. a source
  # tarball with no .git directory) to HB_VERSION_DISPLAY, used everywhere the version is shown to
  # a person; HB_VER_COMMA (the binary FILEVERSION/PRODUCTVERSION resource fields, which must stay
  # strictly numeric) is still derived from the clean, unsuffixed HB_VERSION above.
  find_package(Git QUIET)
  set(HB_GIT_HASH "")
  if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE HB_GIT_HASH
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE HB_GIT_RESULT)
    if(NOT HB_GIT_RESULT EQUAL 0)
      set(HB_GIT_HASH "")
    endif()
  endif()
  if(HB_GIT_HASH)
    set(HB_VERSION_DISPLAY "${HB_VERSION}+${HB_GIT_HASH}")
  else()
    set(HB_VERSION_DISPLAY "${HB_VERSION}")
  endif()
  set(HB_WEBVIEW2_DIR "${CMAKE_SOURCE_DIR}/third_party/webview2" CACHE PATH "Microsoft.Web.WebView2 NuGet package, extracted")
  if(EXISTS "${HB_WEBVIEW2_DIR}/build/native/include/WebView2.h")
    set(HB_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
    set(HB_EMBED_DIR "${CMAKE_BINARY_DIR}/embed")
    file(MAKE_DIRECTORY "${HB_EMBED_DIR}" "${CMAKE_BINARY_DIR}/rc")
    configure_file(ui/index.html "${HB_EMBED_DIR}/index.html" COPYONLY)
    configure_file(ui/setup.html "${HB_EMBED_DIR}/setup.html" COPYONLY)
    configure_file(src/app/windows/rc/app.rc.in "${CMAKE_BINARY_DIR}/rc/app.rc" @ONLY)
    configure_file(src/app/windows/rc/setup.rc.in "${CMAKE_BINARY_DIR}/rc/setup.rc" @ONLY)

    set(HB_WIN_COMMON src/app/windows/webview_host.cpp src/app/windows/win_util.cpp)
    set(HB_WIN_LIBS "${HB_WEBVIEW2_DIR}/build/native/x64/WebView2LoaderStatic.lib"
        strmiids windowscodecs shlwapi dwmapi shell32 ole32 oleaut32 advapi32 user32 version)

    add_executable(hb_studio WIN32 ${HB_WIN_COMMON}
      src/app/windows/app_main.cpp src/app/windows/wic_jpeg.cpp src/app/windows/vcam_controller.cpp
      src/app/windows/dshow_capture.cpp src/app/windows/output_window.cpp src/app/windows/stream_sender.cpp
      "${CMAKE_BINARY_DIR}/rc/app.rc" src/app/windows/rc/app.manifest)
    set_target_properties(hb_studio PROPERTIES OUTPUT_NAME HBStudio)
    target_include_directories(hb_studio PRIVATE src "${HB_WEBVIEW2_DIR}/build/native/include")
    target_compile_definitions(hb_studio PRIVATE UNICODE _UNICODE NOMINMAX HB_VERSION="${HB_VERSION_DISPLAY}")
    target_link_libraries(hb_studio PRIVATE hb_core hb_plugins hb_media hb_ndi hb_overlay hb_vcam_sender hb_streaming hb_streaming_win mfplat mf mfuuid comdlg32 ${HB_WIN_LIBS}
      hb_ai)   # FEATURE: ai
    target_link_libraries(hb_studio PRIVATE hb_remote)   # FEATURE: remote (defined in cmake/modules/remote.cmake)
    target_link_libraries(hb_studio PRIVATE hb_marketplace)   # FEATURE: marketplace (cmake/modules/marketplace.cmake)
    target_link_libraries(hb_studio PRIVATE hb_cloud)   # FEATURE: cloudbackup (cmake/modules/cloud.cmake)
    target_link_libraries(hb_studio PRIVATE hb_chat)   # FEATURE: chat (defined in cmake/modules/chat.cmake)
    target_link_options(hb_studio PRIVATE /MANIFESTUAC:NO)
    add_custom_command(TARGET hb_studio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:hb_studio> "${HB_EMBED_DIR}/HBStudio.exe")
    add_custom_command(TARGET hb_vcam_source POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:hb_vcam_source> "${HB_EMBED_DIR}/hb_vcam_source.dll")

    add_executable(hb_setup WIN32 ${HB_WIN_COMMON} src/app/windows/setup_main.cpp
      "${CMAKE_BINARY_DIR}/rc/setup.rc" src/app/windows/rc/setup.manifest)
    set_target_properties(hb_setup PROPERTIES OUTPUT_NAME HBStudio-Setup)
    target_include_directories(hb_setup PRIVATE src "${HB_WEBVIEW2_DIR}/build/native/include")
    target_compile_definitions(hb_setup PRIVATE UNICODE _UNICODE NOMINMAX HB_VERSION="${HB_VERSION_DISPLAY}")
    target_link_libraries(hb_setup PRIVATE ${HB_WIN_LIBS})
    target_link_options(hb_setup PRIVATE /MANIFESTUAC:NO)
    add_dependencies(hb_setup hb_studio hb_vcam_source)
  else()
    message(STATUS "WebView2 SDK not found in ${HB_WEBVIEW2_DIR}: skipping HBStudio.exe / HBStudio-Setup.exe")
  endif()
endif()

if(HB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0)
  set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_executable(hb_tests
    tests/event_bus_test.cpp
    tests/redact_test.cpp
    tests/logger_test.cpp
    tests/clock_test.cpp
    tests/json_sha_test.cpp
    tests/hardware_profile_test.cpp
    tests/program_output_test.cpp
    tests/pixel_convert_test.cpp
    tests/switcher_test.cpp
    tests/engine_test.cpp
    tests/ptz_settings_test.cpp
    tests/slots_test.cpp
    tests/stream_destinations_test.cpp)
  target_link_libraries(hb_tests PRIVATE hb_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(hb_tests)
endif()

# Feature modules: every cmake/modules/<name>.cmake registers its own library and
# (inside `if(HB_BUILD_TESTS)`) its own gtest executable. Loaded after GoogleTest.
file(GLOB HB_MODULES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/cmake/modules/*.cmake)
foreach(m ${HB_MODULES})
  include(${m})
endforeach()
