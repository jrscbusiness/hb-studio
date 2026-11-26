# App-shell pieces that are portable (no Windows headers) so they can be unit-tested on Linux:
# text-command router/splitter (command_router.hpp), JSON escaping, and the "s" state-message builder.
# Compiled into hb_core (which HBStudio.exe already links); the other headers are header-only.
target_sources(hb_core PRIVATE ${CMAKE_SOURCE_DIR}/src/app/state_json.cpp)

if(HB_BUILD_TESTS)
  add_executable(hb_app_cmd_tests
    tests/app_command_router_test.cpp
    tests/app_state_json_test.cpp)
  target_link_libraries(hb_app_cmd_tests PRIVATE hb_core GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_app_cmd_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_app_cmd_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  gtest_discover_tests(hb_app_cmd_tests)
endif()
