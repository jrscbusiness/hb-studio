# Self-update check/download (src/updater): release JSON parser, version comparison, policy and the
# download + SHA-256 verification service. Portable; reuses the hardened marketplace downloader.
# The Windows glue (native confirmation dialog, running the installer) is in app_main.cpp.
add_library(hb_updater STATIC
  src/updater/update_check.cpp
  src/updater/update_service.cpp)
target_include_directories(hb_updater PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_updater PUBLIC hb_core hb_marketplace)
if(MSVC)
  target_compile_options(hb_updater PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_updater PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(TARGET hb_studio)
  target_link_libraries(hb_studio PRIVATE hb_updater)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_updater_tests
    tests/updater_check_test.cpp
    tests/updater_service_test.cpp)
  target_link_libraries(hb_updater_tests PRIVATE hb_updater GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_updater_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_updater_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  gtest_discover_tests(hb_updater_tests)
endif()
