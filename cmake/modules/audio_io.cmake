# Audio engine (devices via miniaudio, channel strips, Program bus, persistence).
include(FetchContent)
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.22
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR hb_no_cmake)   # single-header library: we compile the implementation ourselves
FetchContent_MakeAvailable(miniaudio)

add_library(hb_audio_io STATIC
  src/audio_io/ma_impl.cpp
  src/audio_io/raw_device_ma.cpp
  src/audio_io/device_enumerator.cpp
  src/audio_io/device_stream.cpp
  src/audio_io/streams.cpp
  src/audio_io/audio_engine.cpp
  src/audio_io/engine_json.cpp
  src/audio_io/engine_settings.cpp)
target_include_directories(hb_audio_io PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(hb_audio_io SYSTEM PRIVATE ${miniaudio_SOURCE_DIR})
target_link_libraries(hb_audio_io PUBLIC hb_audio hb_core Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(hb_audio_io PUBLIC winmm)
  target_compile_definitions(hb_audio_io PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
# Third-party implementation: no warnings from it (our own sources keep the strict flags).
if(MSVC)
  set_source_files_properties(src/audio_io/ma_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
  target_compile_options(hb_audio_io PRIVATE /W4 /permissive-)
else()
  set_source_files_properties(src/audio_io/ma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
  target_compile_options(hb_audio_io PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  file(GLOB HB_AUDIO_IO_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/audio_io_*.cpp)
  add_executable(hb_audio_io_tests ${HB_AUDIO_IO_TESTS})
  target_link_libraries(hb_audio_io_tests PRIVATE hb_audio_io GTest::gtest_main)
  gtest_discover_tests(hb_audio_io_tests)
endif()
