# Teleprompter: portable script/scroll/command logic (tested on Linux and MSVC) + the Windows-only
# presenter pop-out window glue (second WebView2 host), linked into HBStudio.exe when it is built.
add_library(hb_teleprompter STATIC
  src/teleprompter/script.cpp
  src/teleprompter/scroll.cpp
  src/teleprompter/store.cpp
  src/teleprompter/controller.cpp
  src/teleprompter/presenter_page.cpp)
target_include_directories(hb_teleprompter PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_teleprompter PUBLIC hb_core)
if(MSVC)
  target_compile_options(hb_teleprompter PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_teleprompter PRIVATE -Wall -Wextra -Wpedantic)
endif()

# This file is included after the root CMakeLists defined hb_studio (when the WebView2 SDK exists).
if(WIN32 AND TARGET hb_studio)
  target_sources(hb_studio PRIVATE src/app/windows/presenter_window.cpp)
  target_link_libraries(hb_studio PRIVATE hb_teleprompter)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_teleprompter_tests
    tests/teleprompter_script_test.cpp
    tests/teleprompter_scroll_test.cpp
    tests/teleprompter_store_test.cpp
    tests/teleprompter_controller_test.cpp
    tests/teleprompter_page_test.cpp)
  target_link_libraries(hb_teleprompter_tests PRIVATE hb_teleprompter GTest::gtest_main)
  target_compile_definitions(hb_teleprompter_tests PRIVATE HB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  gtest_discover_tests(hb_teleprompter_tests)
endif()
