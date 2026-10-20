# Audio module (Phase 12): mixer, meters, resampler, ring buffer, delay line.
add_library(hb_audio STATIC
  src/audio/meters.cpp
  src/audio/resampler.cpp
  src/audio/drift_compensator.cpp
  src/audio/delay_line.cpp
  src/audio/sources.cpp
  src/audio/limiter.cpp
  src/audio/mixer.cpp
  src/audio/level_publisher.cpp)
target_include_directories(hb_audio PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_audio PUBLIC hb_core)
if(MSVC)
  target_compile_options(hb_audio PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_audio PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_audio_tests
    tests/audio_mixer_test.cpp
    tests/audio_meters_test.cpp
    tests/audio_dsp_test.cpp)
  target_link_libraries(hb_audio_tests PRIVATE hb_audio GTest::gtest_main)
  gtest_discover_tests(hb_audio_tests)
endif()
