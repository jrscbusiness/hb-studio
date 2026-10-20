# ONVIF client (SOAP/WS-Security/parsers) + WS-Discovery engine.
# Libraries: hb_onvif, hb_discovery. Tests: hb_onvif_tests.
include(FetchContent)
FetchContent_Declare(pugixml
  GIT_REPOSITORY https://github.com/zeux/pugixml.git
  GIT_TAG v1.14
  GIT_SHALLOW TRUE)
set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PUGIXML_INSTALL OFF CACHE BOOL "" FORCE)
set(PUGIXML_BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pugixml)

add_library(hb_onvif STATIC
  src/onvif/result.cpp
  src/onvif/crypto.cpp
  src/onvif/time_util.cpp
  src/onvif/url.cpp
  src/onvif/soap.cpp
  src/onvif/messages.cpp
  src/onvif/xml_util.cpp
  src/onvif/parsers.cpp
  src/onvif/onvif_client.cpp
  src/onvif/http_client_posix.cpp
  src/onvif/http_client_win.cpp)
target_include_directories(hb_onvif PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_onvif PUBLIC hb_core pugixml::pugixml)
if(WIN32)
  target_link_libraries(hb_onvif PUBLIC winhttp)
endif()

add_library(hb_discovery STATIC
  src/discovery/ws_discovery.cpp
  src/discovery/onvif_source.cpp
  src/discovery/engine.cpp
  src/discovery/udp_socket_posix.cpp
  src/discovery/udp_socket_win.cpp)
target_link_libraries(hb_discovery PUBLIC hb_onvif)
if(WIN32)
  target_link_libraries(hb_discovery PUBLIC ws2_32 iphlpapi)
endif()

foreach(t hb_onvif hb_discovery)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()

if(HB_BUILD_TESTS)
  add_executable(hb_onvif_tests
    tests/onvif_crypto_test.cpp
    tests/onvif_soap_test.cpp
    tests/onvif_parsers_test.cpp
    tests/onvif_client_test.cpp
    tests/discovery_ws_test.cpp
    tests/discovery_engine_test.cpp)
  target_link_libraries(hb_onvif_tests PRIVATE hb_discovery GTest::gtest_main)
  gtest_discover_tests(hb_onvif_tests)
endif()
