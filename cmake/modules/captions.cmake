# LEGENDAS AO VIVO (live captions): portable pipeline (resampler tap, VAD segmenter, committer,
# normaliser, SRT, model manager, cloud client logic) + optional whisper.cpp engine + Windows glue.
# See docs/LEGENDAS.md.

add_library(hb_captions STATIC
  src/captions/text_normalizer.cpp
  src/captions/segmenter.cpp
  src/captions/committer.cpp
  src/captions/srt.cpp
  src/captions/audio_tap.cpp
  src/captions/session.cpp
  src/captions/service.cpp
  src/captions/model_manager.cpp
  src/captions/cloud_recognizer.cpp
  src/captions/settings.cpp
  src/captions/controller.cpp)
target_include_directories(hb_captions PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_captions PUBLIC hb_core hb_audio Threads::Threads)
if(MSVC)
  target_compile_options(hb_captions PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_captions PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Local speech recognition with whisper.cpp (MIT). Fetched + compiled from source, CPU only.
# Default ON for the Windows app (the offline engine is the primary one), OFF elsewhere to keep
# Linux CI small; the portable tests never need it.
if(WIN32)
  set(_hb_whisper_default ON)
else()
  set(_hb_whisper_default OFF)
endif()
option(HB_CAPTIONS_WHISPER "Build the whisper.cpp speech recognizer for live captions" ${_hb_whisper_default})

if(HB_CAPTIONS_WHISPER)
  include(FetchContent)
  set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(WHISPER_CCACHE OFF CACHE BOOL "" FORCE)
  # No OpenMP (vcomp140.dll would need the VC++ redistributable) and no -march=native (the
  # installer is distributed to other machines). CPU needs AVX2 on x86-64 (ggml default).
  set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
  set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(whisper_cpp
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG v1.7.6)   # a8d002cfd879315632a579e73f0148d06959de36
  # Static libraries only: the installer ships a single HBStudio.exe (no side-by-side DLLs).
  set(_hb_prev_shared "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(whisper_cpp)
  set(BUILD_SHARED_LIBS "${_hb_prev_shared}" CACHE BOOL "" FORCE)
  add_library(hb_captions_whisper STATIC src/captions/whisper_recognizer.cpp)
  target_include_directories(hb_captions_whisper PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(hb_captions_whisper PUBLIC hb_captions whisper)
  target_compile_definitions(hb_captions_whisper PUBLIC HB_HAVE_WHISPER=1)
  if(MSVC)
    target_compile_options(hb_captions_whisper PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_captions_whisper PRIVATE -Wall -Wextra)
  endif()
endif()

# Windows glue: WinHTTP downloader / HTTP poster (the controller itself is portable, see controller.hpp).
if(WIN32)
  add_library(hb_captions_win STATIC
    src/captions/windows/winhttp_client.cpp)
  target_include_directories(hb_captions_win PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(hb_captions_win PUBLIC hb_captions winhttp)
  if(HB_CAPTIONS_WHISPER)
    target_link_libraries(hb_captions_win PUBLIC hb_captions_whisper)
  endif()
  target_compile_definitions(hb_captions_win PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(hb_captions_win PRIVATE /W4 /permissive-)
  endif()
  # hb_studio is defined in the root CMakeLists (only when the WebView2 SDK is present).
  if(TARGET hb_studio)
    target_link_libraries(hb_studio PRIVATE hb_captions_win)
  endif()
endif()

if(HB_BUILD_TESTS)
  add_executable(hb_captions_tests
    tests/captions_text_test.cpp
    tests/captions_segmenter_test.cpp
    tests/captions_committer_test.cpp
    tests/captions_srt_test.cpp
    tests/captions_tap_test.cpp
    tests/captions_session_test.cpp
    tests/captions_models_test.cpp
    tests/captions_cloud_test.cpp
    tests/captions_settings_test.cpp
    tests/captions_controller_test.cpp)
  target_include_directories(hb_captions_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(hb_captions_tests PRIVATE hb_captions GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_captions_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_captions_tests PRIVATE -Wall -Wextra)
  endif()
  gtest_discover_tests(hb_captions_tests)
endif()
