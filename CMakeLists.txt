cmake_minimum_required(VERSION 3.24)
project(HBStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HB_BUILD_TESTS "Build unit tests" ON)
option(HB_BUILD_TOOLS "Build command line tools" ON)

# Static MSVC runtime everywhere: the virtual camera DLL is loaded by a system
# service and must not depend on a VC++ redistributable being installed.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Threads REQUIRED)

add_library(hb_core STATIC
  src/core/event_bus.cpp
  src/core/redact.cpp
  src/core/logger.cpp
  src/core/clock.cpp
  src/core/hardware_profile.cpp
  src/video/program_output.cpp
  src/video/synthetic_source.cpp
  src/switcher/switcher.cpp
  src/app/engine.cpp
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
    tests/hardware_profile_test.cpp
    tests/program_output_test.cpp
    tests/switcher_test.cpp
    tests/engine_test.cpp)
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
