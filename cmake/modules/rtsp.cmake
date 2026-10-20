# RTSP/RTP ingest module (src/rtsp). Pure logic + a thin socket layer; no FFmpeg dependency.
set(HB_RTSP_SOURCES
  src/rtsp/types.cpp
  src/rtsp/md5.cpp
  src/rtsp/base64.cpp
  src/rtsp/url.cpp
  src/rtsp/message.cpp
  src/rtsp/auth.cpp
  src/rtsp/sdp.cpp
  src/rtsp/rtp.cpp
  src/rtsp/depacketizer.cpp
  src/rtsp/depacketizer_h264.cpp
  src/rtsp/depacketizer_h265.cpp
  src/rtsp/jitter_buffer.cpp
  src/rtsp/stream_receiver.cpp
  src/rtsp/session.cpp
  src/rtsp/rtsp_input.cpp
  src/rtsp/tcp_stream_posix.cpp   # whole file is #ifndef _WIN32
  src/rtsp/tcp_stream_win.cpp)    # whole file is #ifdef _WIN32 (not validated)

add_library(hb_rtsp STATIC ${HB_RTSP_SOURCES})
target_include_directories(hb_rtsp PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_rtsp PUBLIC hb_core)
if(WIN32)
  target_link_libraries(hb_rtsp PUBLIC ws2_32)
endif()
if(MSVC)
  target_compile_options(hb_rtsp PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_rtsp PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_rtsp_tests
    tests/rtsp_basics_test.cpp
    tests/rtsp_auth_test.cpp
    tests/rtsp_sdp_test.cpp
    tests/rtsp_depacketizer_test.cpp
    tests/rtsp_jitter_test.cpp
    tests/rtsp_session_test.cpp
    tests/rtsp_input_test.cpp
    tests/rtsp_loopback_test.cpp)
  target_include_directories(hb_rtsp_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(hb_rtsp_tests PRIVATE hb_rtsp GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_rtsp_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_rtsp_tests PRIVATE -Wall -Wextra)
  endif()
  gtest_discover_tests(hb_rtsp_tests)
endif()
