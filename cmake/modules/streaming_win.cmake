# Windows-only raw H.264/AAC encoders for TRANSMISSÃO (RTMP streaming), via Media Foundation
# IMFTransform directly - see src/streaming/windows/mf_encoder.hpp for why this is separate from
# src/recording/windows/mf_writers.cpp's IMFSinkWriter-based encoder.
if(WIN32)
  add_library(hb_streaming_win STATIC
    src/streaming/windows/mf_video_encoder.cpp
    src/streaming/windows/mf_audio_encoder.cpp)
  target_include_directories(hb_streaming_win PUBLIC ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(hb_streaming_win PUBLIC hb_core mfplat mf mfreadwrite mfuuid)
  target_compile_definitions(hb_streaming_win PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(hb_streaming_win PRIVATE /W4 /permissive-)
  endif()
endif()
