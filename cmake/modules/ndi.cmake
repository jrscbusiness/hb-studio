# NDI discovery/ingestion (src/ndi/). Portable: hb_ndi builds and is fully testable on Linux with
# the FakeNdiBackend (tests/ndi_test_util.hpp); the real dynamic-loading backend
# (ndi_backend_win.cpp) only compiles on Windows and is never built or run in this CI. The NDI
# SDK/Runtime itself is never bundled or linked against -- see LICENSES.md.
add_library(hb_ndi STATIC
  src/ndi/discovery.cpp
  src/ndi/source.cpp
  src/ndi/sender.cpp)
target_include_directories(hb_ndi PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_ndi PUBLIC hb_core)
if(WIN32)
  target_sources(hb_ndi PRIVATE src/ndi/ndi_backend_win.cpp)
else()
  target_sources(hb_ndi PRIVATE src/ndi/ndi_backend_stub.cpp)
endif()
if(MSVC)
  target_compile_options(hb_ndi PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_ndi PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_ndi_tests
    tests/ndi_discovery_test.cpp
    tests/ndi_source_test.cpp
    tests/ndi_sender_test.cpp)
  target_link_libraries(hb_ndi_tests PRIVATE hb_ndi GTest::gtest_main)
  gtest_discover_tests(hb_ndi_tests)
endif()
