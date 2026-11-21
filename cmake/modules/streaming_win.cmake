# Windows-only raw H.264/AAC encoders for TRANSMISSÃO (RTMP streaming), via Media Foundation
# IMFTransform directly - see src/streaming/windows/mf_encoder.hpp for why this is separate from
# src/recording/windows/mf_writers.cpp's IMFSinkWriter-based encoder.
if(WIN32)
  add_library(hb_streaming_win STATIC
    src/streaming/windows/mf_video_encoder.cpp
    src/streaming/windows/mf_audio_encoder.cpp
    src/streaming/windows/nal_utils.cpp
    src/streaming/windows/aac_asc.cpp)
  target_include_directories(hb_streaming_win PUBLIC ${CMAKE_SOURCE_DIR}/src)
  # ole32: CoInitializeEx/CoUninitialize/CoTaskMemFree, used directly by the IMFTransform-driven
  # encoders (unlike mf_writers.cpp's IMFSinkWriter path, there's no higher-level MF helper that
  # wraps these here). wmcodecdspuuid: storage for the CODECAPI_* GUIDs (e.g.
  # CODECAPI_AVEncMPVGOPSize, used for best-effort GOP/keyframe-interval configuration via
  # ICodecAPI) - these are declared `extern const GUID` in <codecapi.h> without INITGUID defined,
  # so a lib providing their actual storage is needed; mfuuid.lib does not provide it.
  target_link_libraries(hb_streaming_win PUBLIC hb_core mfplat mf mfreadwrite mfuuid ole32 wmcodecdspuuid)
  target_compile_definitions(hb_streaming_win PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(hb_streaming_win PRIVATE /W4 /permissive-)
  endif()

  if(HB_BUILD_TESTS)
    # Only the genuinely host-independent pure logic (Annex-B NAL splitting, AAC ASC-from-blob
    # extraction - neither touches COM/MFT) is exercised here, same spirit as
    # tests/rec_win_test.cpp's registration in cmake/modules/recording_win.cmake; this whole test
    # binary is still Windows-only because it links the Windows-only hb_streaming_win library.
    add_executable(hb_streaming_win_tests tests/streaming_win_test.cpp)
    target_link_libraries(hb_streaming_win_tests PRIVATE hb_streaming_win GTest::gtest_main)
    target_compile_definitions(hb_streaming_win_tests PRIVATE UNICODE _UNICODE NOMINMAX)
    gtest_discover_tests(hb_streaming_win_tests)
  endif()
endif()
