# Plugin marketplace: catalog parser/validator, semver, HTTPS downloader (WinHTTP on Windows) and
# the download/verify/install-review service. Library: hb_marketplace. Tests: hb_marketplace_tests.
add_library(hb_marketplace STATIC
  src/marketplace/semver.cpp
  src/marketplace/catalog.cpp
  src/marketplace/service.cpp
  src/marketplace/downloader_stub.cpp
  src/marketplace/downloader_win.cpp)
target_include_directories(hb_marketplace PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_marketplace PUBLIC hb_core)
if(WIN32)
  target_link_libraries(hb_marketplace PUBLIC winhttp)
endif()
if(MSVC)
  target_compile_options(hb_marketplace PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_marketplace PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_marketplace_tests
    tests/marketplace_semver_test.cpp
    tests/marketplace_catalog_test.cpp
    tests/marketplace_service_test.cpp)
  target_link_libraries(hb_marketplace_tests PRIVATE hb_marketplace GTest::gtest_main)
  gtest_discover_tests(hb_marketplace_tests)
endif()
