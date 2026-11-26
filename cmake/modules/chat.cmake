# Unified live chat (YouTube / Facebook / Instagram(limited) / Twitch): parsers, OAuth+PKCE, sources, hub, service.
add_library(hb_chat STATIC
  src/chat/types.cpp
  src/chat/net.cpp
  src/chat/irc.cpp
  src/chat/parsers.cpp
  src/chat/oauth.cpp
  src/chat/loopback.cpp
  src/chat/sources.cpp
  src/chat/hub.cpp
  src/chat/settings.cpp
  src/chat/service.cpp
  src/chat/net_adapter.cpp)
if(WIN32)
  target_sources(hb_chat PRIVATE src/chat/net_win.cpp)
  target_link_libraries(hb_chat PUBLIC winhttp ws2_32)
  target_compile_definitions(hb_chat PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
target_include_directories(hb_chat PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_chat PUBLIC hb_core hb_net Threads::Threads)
if(MSVC)
  target_compile_options(hb_chat PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_chat PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  file(GLOB HB_CHAT_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/chat_*.cpp)
  add_executable(hb_chat_tests ${HB_CHAT_TESTS})
  target_link_libraries(hb_chat_tests PRIVATE hb_chat GTest::gtest_main)
  gtest_discover_tests(hb_chat_tests)
endif()
