# Remote control from a phone/tablet browser (src/remote): HTTP + WebSocket server over the
# portable ITcpStream abstraction, PIN pairing, command allowlist. Needs hb_rtsp (socket layer).

# Embed the mobile page as a byte array (a plain string literal would hit MSVC's 16 KB literal limit).
set(HB_REMOTE_PAGE_HTML "${CMAKE_SOURCE_DIR}/src/remote/mobile_page.html")
set(HB_REMOTE_PAGE_CPP "${CMAKE_BINARY_DIR}/generated/remote_page_data.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HB_REMOTE_PAGE_HTML}")
file(READ "${HB_REMOTE_PAGE_HTML}" HB_REMOTE_PAGE_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HB_REMOTE_PAGE_HEX "${HB_REMOTE_PAGE_HEX}")
file(WRITE "${HB_REMOTE_PAGE_CPP}.tmp"
  "// Generated from src/remote/mobile_page.html by cmake/modules/remote.cmake. Do not edit.\n"
  "#include <cstddef>\nnamespace hb::remote {\nextern const unsigned char kMobilePageData[];\nextern const std::size_t kMobilePageSize;\n"
  "const unsigned char kMobilePageData[] = {${HB_REMOTE_PAGE_HEX}0x00};\n"
  "const std::size_t kMobilePageSize = sizeof(kMobilePageData) - 1;\n}\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${HB_REMOTE_PAGE_CPP}.tmp" "${HB_REMOTE_PAGE_CPP}")

add_library(hb_remote STATIC
  src/remote/sha1.cpp
  src/remote/random_posix.cpp
  src/remote/random_win.cpp
  src/remote/websocket.cpp
  src/remote/http.cpp
  src/remote/auth.cpp
  src/remote/commands.cpp
  src/remote/state.cpp
  src/remote/lan.cpp
  src/remote/lan_win.cpp
  src/remote/server.cpp
  src/remote/service.cpp
  "${HB_REMOTE_PAGE_CPP}")
target_include_directories(hb_remote PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_remote PUBLIC hb_rtsp hb_core)
if(WIN32)
  target_link_libraries(hb_remote PUBLIC bcrypt iphlpapi ws2_32)
endif()
if(MSVC)
  target_compile_options(hb_remote PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_remote PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_remote_tests
    tests/remote_basics_test.cpp
    tests/remote_auth_test.cpp
    tests/remote_server_test.cpp)
  target_include_directories(hb_remote_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(hb_remote_tests PRIVATE hb_remote GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_remote_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_remote_tests PRIVATE -Wall -Wextra)
  endif()
  gtest_discover_tests(hb_remote_tests)
endif()

# Developer tool: runs the real server with a fake host, for browser-driven UI tests.
if(HB_BUILD_TOOLS)
  add_executable(hb_remote_sim tools/hb_remote_sim.cpp)
  target_link_libraries(hb_remote_sim PRIVATE hb_remote)
endif()
