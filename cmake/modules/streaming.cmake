# TRANSMISSÃO (live streaming) module: AMF0, FLV tag bodies and an RTMP(S) client - pure protocol
# logic over hb_rtsp's existing, already cross-platform ITcpStream (POSIX/Winsock), reused rather
# than duplicated. No FFmpeg/GStreamer dependency: encoding is Windows Media Foundation, wired in
# separately under src/streaming/windows (see cmake/modules/streaming_win.cmake).
add_library(hb_streaming STATIC
  src/streaming/amf0.cpp
  src/streaming/flv.cpp
  src/streaming/rtmp_client.cpp
  src/streaming/transport.cpp
  src/streaming/tls_stream.cpp
  src/streaming/tls_engine_stub.cpp        # whole file is #ifndef _WIN32
  src/streaming/tls_schannel.cpp           # whole file is #ifdef _WIN32 (Schannel; not validated against a live server)
  src/streaming/stream_stats.cpp
  src/streaming/adaptive_bitrate.cpp
  src/streaming/reconnecting_publisher.cpp)
target_include_directories(hb_streaming PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_streaming PUBLIC hb_core hb_rtsp)
if(WIN32)
  target_link_libraries(hb_streaming PUBLIC secur32 crypt32)   # Schannel (SSPI) + certificate chain policy
endif()
if(MSVC)
  target_compile_options(hb_streaming PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_streaming PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_streaming_tests
    tests/amf0_test.cpp
    tests/flv_test.cpp
    tests/rtmp_client_test.cpp
    tests/tls_stream_test.cpp
    tests/rtmps_test.cpp
    tests/stream_stats_test.cpp
    tests/adaptive_bitrate_test.cpp
    tests/reconnecting_publisher_test.cpp)
  target_include_directories(hb_streaming_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(hb_streaming_tests PRIVATE hb_streaming GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_streaming_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_streaming_tests PRIVATE -Wall -Wextra)
  endif()
  gtest_discover_tests(hb_streaming_tests)
endif()
