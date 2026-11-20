# Recording module: portable writers (WAV, FLAC, AVI Motion-JPEG, JPEG sequence) + Recorder.
add_library(hb_recording STATIC
  src/recording/md5.cpp
  src/recording/wav_writer.cpp
  src/recording/flac_writer.cpp
  src/recording/jpeg_encoder.cpp
  src/recording/avi_writer.cpp
  src/recording/jpeg_seq_writer.cpp
  src/recording/portable_writers.cpp
  src/recording/file_naming.cpp
  src/recording/recorder.cpp)
target_include_directories(hb_recording PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_recording PUBLIC hb_core hb_audio)
if(MSVC)
  target_compile_options(hb_recording PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_recording PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  file(GLOB HB_RECORDING_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/recording_*.cpp)
  add_executable(hb_recording_tests ${HB_RECORDING_TEST_SRCS})
  target_link_libraries(hb_recording_tests PRIVATE hb_recording GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_recording_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_recording_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  gtest_discover_tests(hb_recording_tests)
endif()
