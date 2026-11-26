# Diagnostics: "Exportar diagnóstico" report builder + redactor, and the pre-live checklist logic
# (pure, probes injectable). Windows glue lives in diagnostics_win.cmake.
add_library(hb_diagnostics STATIC
  src/diagnostics/redactor.cpp
  src/diagnostics/report.cpp
  src/diagnostics/checklist.cpp
  src/diagnostics/settings.cpp)
target_include_directories(hb_diagnostics PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_diagnostics PUBLIC hb_core)
if(MSVC)
  target_compile_options(hb_diagnostics PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_diagnostics PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  file(GLOB HB_DIAG_TEST_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/diagnostics_*.cpp)
  add_executable(hb_diagnostics_tests ${HB_DIAG_TEST_SRCS})
  target_link_libraries(hb_diagnostics_tests PRIVATE hb_diagnostics GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_diagnostics_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_diagnostics_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  gtest_discover_tests(hb_diagnostics_tests)
endif()
