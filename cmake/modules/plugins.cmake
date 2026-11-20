# Plugin system: manifest, safe .hbplugin packages (zip), manager, native source plugins, themes.
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.0.2)
FetchContent_MakeAvailable(miniz)

add_library(hb_plugins STATIC
  src/plugins/manifest.cpp
  src/plugins/package.cpp
  src/plugins/manager.cpp
  src/plugins/shared_library.cpp
  src/plugins/native_source.cpp
  src/plugins/runtime.cpp
  src/plugins/service.cpp)
target_include_directories(hb_plugins PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_plugins PUBLIC hb_core PRIVATE miniz)
if(NOT WIN32)
  target_link_libraries(hb_plugins PUBLIC ${CMAKE_DL_LIBS})
endif()
if(MSVC)
  target_compile_options(hb_plugins PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_plugins PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Example native plugin (C, no dependencies): two test-pattern sources. Used by the tests and shipped
# as a downloadable sample so the install flow can be tried end to end.
add_library(hb_plugin_example SHARED plugins/examples/colorbars/plugin.c)
target_include_directories(hb_plugin_example PRIVATE ${CMAKE_SOURCE_DIR}/src/plugins)
set_target_properties(hb_plugin_example PROPERTIES C_VISIBILITY_PRESET hidden)
if(NOT WIN32)
  set_target_properties(hb_plugin_example PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Developer tool: builds a .hbplugin from a folder (fills in the sha256 table).
add_executable(hb_pack tools/hb_pack.cpp)
target_link_libraries(hb_pack PRIVATE hb_plugins miniz)

if(HB_BUILD_TESTS)
  add_executable(hb_plugins_tests
    tests/plugins_manifest_test.cpp
    tests/plugins_package_test.cpp
    tests/plugins_manager_test.cpp
    tests/plugins_native_test.cpp
    tests/plugins_service_test.cpp)
  target_link_libraries(hb_plugins_tests PRIVATE hb_plugins miniz GTest::gtest_main)
  target_compile_definitions(hb_plugins_tests PRIVATE
    HB_EXAMPLE_PLUGIN_PATH="$<TARGET_FILE:hb_plugin_example>")
  add_dependencies(hb_plugins_tests hb_plugin_example)
  gtest_discover_tests(hb_plugins_tests)
endif()
