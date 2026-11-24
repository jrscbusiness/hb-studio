# Cloud backup: SigV4/S3 + Google Drive providers, resumable upload engine, platform glue.
# Library: hb_cloud. Tests: hb_cloud_tests.
set(HB_CLOUD_SRCS
  src/cloud/cloud_types.cpp
  src/cloud/hash_util.cpp
  src/cloud/sigv4.cpp
  src/cloud/cloud_util.cpp
  src/cloud/throttle.cpp
  src/cloud/file_system.cpp
  src/cloud/s3_provider.cpp
  src/cloud/gdrive_oauth.cpp
  src/cloud/gdrive_provider.cpp
  src/cloud/upload_engine.cpp
  src/cloud/file_feed.cpp
  src/cloud/cloud_settings.cpp
  src/cloud/loopback_listener.cpp
  src/cloud/cloud_service.cpp)
if(WIN32)
  list(APPEND HB_CLOUD_SRCS src/cloud/cloud_platform_win.cpp)
else()
  list(APPEND HB_CLOUD_SRCS src/cloud/cloud_platform_posix.cpp)
endif()
add_library(hb_cloud STATIC ${HB_CLOUD_SRCS})
target_include_directories(hb_cloud PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_cloud PUBLIC hb_core)
if(WIN32)
  target_link_libraries(hb_cloud PUBLIC winhttp crypt32 ws2_32)
endif()
if(MSVC)
  target_compile_options(hb_cloud PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_cloud PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  file(GLOB HB_CLOUD_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/cloud_*.cpp)
  add_executable(hb_cloud_tests ${HB_CLOUD_TEST_SRCS})
  target_link_libraries(hb_cloud_tests PRIVATE hb_cloud GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_cloud_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_cloud_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  gtest_discover_tests(hb_cloud_tests)
endif()
