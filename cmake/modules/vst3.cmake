# VST3 plugin hosting (src/vst3/): audio processing is portable (built and tested on Linux CI
# against a project-authored test-fixture plugin, see plugins/examples/vst3-test-fixture/); the
# plugin's own native GUI editor window (IVst3Editor) is Windows-only, isolated in
# src/vst3/windows/vst3_editor.cpp and only added to the target under WIN32.
#
# The Steinberg VST3 SDK (MIT-licensed since v3.8, see src/vst3/vst3.hpp) is fetched via
# FetchContent. We build only the pieces a HOST needs — base, pluginterfaces, and the SDK's own
# "common"/"hosting" helper static libs, via the SDK's own cmake/modules/SMTG_VST3_SDK.cmake
# functions — instead of add_subdirectory'ing the SDK's top-level CMakeLists.txt, which would also
# build its plugin/hosting *examples*, its validator tool, and (were vstgui4 fetched) a GUI toolkit
# none of which a plugin *host* needs. Everything the SDK's own CMake does is wrapped in a function
# so its generator/toolset setup (which changes global output directories, build types, etc. —
# meant for being the top-level project) cannot leak into the rest of HB Studio's build: a
# function() gets its own variable scope, so a plain set() inside it is undone when it returns,
# while the targets it creates (add_library) remain, same as any other CMake function.
include(FetchContent)
FetchContent_Declare(vst3sdk
  GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
  GIT_TAG v3.8.1_build_84
  GIT_SUBMODULES base cmake pluginterfaces public.sdk   # not vstgui4/doc/tutorials: a host doesn't need them
  GIT_SUBMODULES_RECURSE OFF)
FetchContent_GetProperties(vst3sdk)
if(NOT vst3sdk_POPULATED)
  FetchContent_Populate(vst3sdk)
endif()

# These three are genuine VST3-distribution features (moduleinfo.json generation, running the
# SDK's own validator tool, symlinking the built plugin into the system VST3 folder) that the test
# fixture doesn't need and that would otherwise require building the SDK's "vst-utilities"/
# "vst-hosting" sample trees (moduleinfotool, validator) or touching the machine's real VST3 folder
# — none of which belongs in this project's build. Forced into the cache before the SDK's own
# cmake/modules/SMTG_AddVST3Library.cmake declares their option() defaults.
set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)

function(hb_vst3_build_sdk)
  set(SDK_ROOT "${vst3sdk_SOURCE_DIR}")
  list(APPEND CMAKE_MODULE_PATH "${SDK_ROOT}/cmake/modules")
  include(SMTG_VST3_SDK)
  smtg_create_lib_base_target()
  smtg_create_pluginterfaces_target()
  smtg_create_public_sdk_common_target()
  smtg_create_public_sdk_target()
  smtg_create_public_sdk_hosting_target()
  # smtg_setup_platform_toolset() (skipped — see the big comment above) is what normally turns this
  # on for UNIX; without it these static libs default to non-PIC, which fails to link once
  # hb_vst3_test_fixture (a MODULE/shared library) pulls "sdk" in below.
  if(NOT WIN32)
    set_target_properties(base pluginterfaces sdk_common sdk sdk_hosting PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()
endfunction()
hb_vst3_build_sdk()

# plugprovider.{h,cpp} do the documented, Steinberg-tested component+controller creation/connection
# (both the "single component" and the "separate processor/controller classes" cases) but aren't
# part of smtg_create_public_sdk_hosting_target()'s own source list (only its consumers, like
# Steinberg's own validator tool, add them) — so we add them to our own host sources below, along
# with the one per-platform module-loading file every hosting SAMPLE in the SDK also adds itself
# (module.cpp's Module::create() is implemented in a separate per-OS file).
set(HB_VST3_HOST_SOURCES
  ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
  ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.h
  src/vst3/detail.hpp
  src/vst3/detail.cpp
  src/vst3/vst3_effect.hpp
  src/vst3/vst3_effect.cpp
  src/vst3/vst3.cpp)

if(WIN32)
  list(APPEND HB_VST3_HOST_SOURCES
    ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
    src/vst3/windows/vst3_editor.cpp)
else()
  list(APPEND HB_VST3_HOST_SOURCES ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()

add_library(hb_vst3 STATIC ${HB_VST3_HOST_SOURCES})
target_include_directories(hb_vst3 PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_vst3 PUBLIC hb_core PRIVATE sdk_hosting)
if(WIN32)
  target_compile_definitions(hb_vst3 PRIVATE UNICODE _UNICODE NOMINMAX)
elseif(NOT APPLE)
  target_link_libraries(hb_vst3 PRIVATE ${CMAKE_DL_LIBS})
endif()
if(MSVC)
  target_compile_options(hb_vst3 PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_vst3 PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  # Minimal VST3 effect built as a real .vst3 module (via the SDK's own packaging helper, which
  # lays out the "Contents/<arch>/<name>.so|dll|dylib" bundle structure for us) purely to give the
  # tests below a real module to scan/load/process audio through end to end — see
  # plugins/examples/vst3-test-fixture/README.md: this is NOT a real/commercial VST3 plugin.
  set(HB_VST3_FIXTURE_SOURCES
    plugins/examples/vst3-test-fixture/ids.h
    plugins/examples/vst3-test-fixture/gain_processor.h
    plugins/examples/vst3-test-fixture/gain_processor.cpp
    plugins/examples/vst3-test-fixture/factory_entry.cpp)
  # linuxmain.cpp/dllmain.cpp are what actually export ModuleEntry()/ModuleExit() (calling our
  # InitModule()/DeinitModule() in factory_entry.cpp) — every VST3 module needs one, per-OS; see
  # public.sdk/source/main/{linux,dll}main.cpp.
  if(WIN32)
    list(APPEND HB_VST3_FIXTURE_SOURCES ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/dllmain.cpp)
  else()
    list(APPEND HB_VST3_FIXTURE_SOURCES ${vst3sdk_SOURCE_DIR}/public.sdk/source/main/linuxmain.cpp)
  endif()
  smtg_add_vst3plugin(hb_vst3_test_fixture
    PACKAGE_NAME "HB VST3 Test Fixture"
    SOURCES_LIST ${HB_VST3_FIXTURE_SOURCES})
  target_include_directories(hb_vst3_test_fixture PRIVATE ${vst3sdk_SOURCE_DIR})
  target_link_libraries(hb_vst3_test_fixture PRIVATE sdk)
  get_target_property(HB_VST3_FIXTURE_PACKAGE_PATH hb_vst3_test_fixture SMTG_PLUGIN_PACKAGE_PATH)

  add_executable(hb_vst3_tests
    tests/vst3_scan_test.cpp
    tests/vst3_effect_test.cpp)
  target_link_libraries(hb_vst3_tests PRIVATE hb_vst3 GTest::gtest_main)
  target_compile_definitions(hb_vst3_tests PRIVATE
    HB_VST3_FIXTURE_PACKAGE_PATH="${HB_VST3_FIXTURE_PACKAGE_PATH}")
  add_dependencies(hb_vst3_tests hb_vst3_test_fixture)
  if(MSVC)
    target_compile_options(hb_vst3_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_vst3_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  gtest_discover_tests(hb_vst3_tests)
endif()
