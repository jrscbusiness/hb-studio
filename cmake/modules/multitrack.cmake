# Multitrack (ISO) recording: portable timeline/sync logic, resource estimator, tracks, session, manifest, EDL.
add_library(hb_multitrack STATIC
  src/multitrack/track_timeline.cpp
  src/multitrack/resource_estimator.cpp
  src/multitrack/session_manifest.cpp
  src/multitrack/edl_export.cpp
  src/multitrack/iso_track.cpp
  src/multitrack/frame_sampler.cpp
  src/multitrack/iso_session.cpp)
target_include_directories(hb_multitrack PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_multitrack PUBLIC hb_core hb_audio hb_recording)
if(MSVC)
  target_compile_options(hb_multitrack PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_multitrack PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  file(GLOB HB_MULTITRACK_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/multitrack_*.cpp)
  add_executable(hb_multitrack_tests ${HB_MULTITRACK_TEST_SRCS})
  target_link_libraries(hb_multitrack_tests PRIVATE hb_multitrack GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_multitrack_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_multitrack_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  gtest_discover_tests(hb_multitrack_tests)
endif()
