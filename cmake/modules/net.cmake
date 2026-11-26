# Shared network layer: IHttpClient (WinHTTP + fake), ISecretStore (DPAPI + fake), URL/host helpers.
# Library: hb_net. Tests: hb_net_tests. See docs/REDE.md.
add_library(hb_net STATIC
  src/net/url.cpp
  src/net/http.cpp
  src/net/http_fake.cpp
  src/net/secret_store.cpp)
if(WIN32)
  target_sources(hb_net PRIVATE src/net/http_win.cpp src/net/secret_store_win.cpp)
  target_link_libraries(hb_net PUBLIC winhttp crypt32)
  target_compile_definitions(hb_net PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
target_include_directories(hb_net PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_net PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(hb_net PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_net PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  file(GLOB HB_NET_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/net_*.cpp)
  add_executable(hb_net_tests ${HB_NET_TESTS})
  target_compile_definitions(hb_net_tests PRIVATE HB_NET_SRC_DIR="${CMAKE_SOURCE_DIR}/src/net")
  target_link_libraries(hb_net_tests PRIVATE hb_net GTest::gtest_main)
  gtest_discover_tests(hb_net_tests)
endif()
