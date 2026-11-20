# Audio effects (per-channel DSP) + plugin effect adapter.
add_library(hb_effects STATIC
  src/audio/effects/utility_effects.cpp
  src/audio/effects/filter_effects.cpp
  src/audio/effects/dynamics_effects.cpp
  src/audio/effects/builtin_registry.cpp
  src/audio/plugin_effect.cpp)
target_include_directories(hb_effects PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_effects PUBLIC hb_core)
if(MSVC)
  target_compile_options(hb_effects PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_effects PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_effects_tests
    tests/effects_basic_test.cpp
    tests/effects_filters_test.cpp
    tests/effects_dynamics_test.cpp
    tests/effects_utility_test.cpp
    tests/effects_realtime_test.cpp
    tests/effects_thread_test.cpp
    tests/effects_plugin_test.cpp)
  target_link_libraries(hb_effects_tests PRIVATE hb_effects GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_effects_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_effects_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  gtest_discover_tests(hb_effects_tests)

  # Optional ThreadSanitizer build of the concurrency test: -DHB_EFFECTS_TSAN=ON (GCC/Clang, Linux).
  option(HB_EFFECTS_TSAN "Build hb_effects_tsan_tests (ThreadSanitizer)" OFF)
  if(HB_EFFECTS_TSAN AND NOT MSVC)
    add_executable(hb_effects_tsan_tests
      tests/effects_thread_test.cpp
      src/audio/effects/utility_effects.cpp
      src/audio/effects/filter_effects.cpp
      src/audio/effects/dynamics_effects.cpp
      src/audio/effects/builtin_registry.cpp
      src/audio/effect.cpp)
    target_include_directories(hb_effects_tsan_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(hb_effects_tsan_tests PRIVATE -fsanitize=thread -g -O1)
    target_link_options(hb_effects_tsan_tests PRIVATE -fsanitize=thread)
    target_link_libraries(hb_effects_tsan_tests PRIVATE GTest::gtest_main Threads::Threads)
    gtest_discover_tests(hb_effects_tsan_tests)
  endif()
endif()
