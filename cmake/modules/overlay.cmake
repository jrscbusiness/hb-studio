# Dynamic overlays (timer, scoreboard, QR, lower third, captions, pinned chat) composited over the
# Program output, plus the portable QR encoder they use. See docs/OVERLAYS.md.
add_library(hb_overlay STATIC
  src/qr/qr.cpp
  src/overlay/text.cpp
  src/overlay/layer.cpp
  src/overlay/element.cpp
  src/overlay/manager.cpp)
if(WIN32)
  target_sources(hb_overlay PRIVATE src/overlay/windows/gdi_glyphs.cpp)
  target_compile_definitions(hb_overlay PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(hb_overlay PUBLIC gdi32 user32)
endif()
target_include_directories(hb_overlay PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_overlay PUBLIC hb_core Threads::Threads)
if(MSVC)
  target_compile_options(hb_overlay PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_overlay PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_overlay_tests
    tests/qr_test.cpp
    tests/overlay_blend_text_test.cpp
    tests/overlay_timing_test.cpp
    tests/overlay_elements_test.cpp
    tests/template_overlays_test.cpp)
  target_link_libraries(hb_overlay_tests PRIVATE hb_overlay GTest::gtest_main)
  gtest_discover_tests(hb_overlay_tests)
endif()
