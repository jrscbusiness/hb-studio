# PTZ control module (VISCA / Pelco / ONVIF-PTZ provider, safety layer, presets).
add_library(hb_ptz STATIC
  src/ptz/types.cpp
  src/ptz/visca.cpp
  src/ptz/visca_provider.cpp
  src/ptz/pelco.cpp
  src/ptz/pelco_provider.cpp
  src/ptz/onvif_provider.cpp
  src/ptz/guarded_controller.cpp
  src/ptz/provider_selection.cpp
  src/ptz/camera_config.cpp
  src/ptz/preset_store.cpp
  src/ptz/preset_manager.cpp
  src/ptz/provider_factory.cpp)
if(WIN32)
  target_sources(hb_ptz PRIVATE src/ptz/udp_transport_win.cpp)
  target_link_libraries(hb_ptz PUBLIC ws2_32)
else()
  target_sources(hb_ptz PRIVATE src/ptz/udp_transport_posix.cpp)
endif()
target_include_directories(hb_ptz PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_ptz PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(hb_ptz PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_ptz PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_ptz_tests
    tests/ptz_visca_test.cpp
    tests/ptz_pelco_test.cpp
    tests/ptz_providers_test.cpp
    tests/ptz_guard_test.cpp
    tests/ptz_presets_test.cpp
    tests/ptz_udp_test.cpp)
  target_link_libraries(hb_ptz_tests PRIVATE hb_ptz GTest::gtest_main)
  gtest_discover_tests(hb_ptz_tests)
endif()
