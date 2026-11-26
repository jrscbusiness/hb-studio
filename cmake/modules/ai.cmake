# AI providers (OpenRouter / 9router / OpenAI-compatible): client, key store, service, UI commands.
add_library(hb_ai STATIC
  src/ai/protocol.cpp
  src/ai/ai_client.cpp
  src/ai/secret_store.cpp
  src/ai/provider_store.cpp
  src/ai/ai_service.cpp
  src/ai/assistant.cpp
  src/ai/ai_commands.cpp
  src/ai/net_adapter.cpp)
target_include_directories(hb_ai PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_ai PUBLIC hb_core hb_net Threads::Threads)
if(MSVC)
  target_compile_options(hb_ai PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_ai PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  file(GLOB HB_AI_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/ai_*.cpp)
  add_executable(hb_ai_tests ${HB_AI_TESTS})
  target_link_libraries(hb_ai_tests PRIVATE hb_ai GTest::gtest_main)
  gtest_discover_tests(hb_ai_tests)
endif()
