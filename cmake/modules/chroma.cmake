# Chroma key / AI background removal (src/chroma). Portable core + Windows-only HTTP/WIC/ONNX glue.
add_library(hb_chroma STATIC
  src/chroma/types.cpp
  src/chroma/mask_ops.cpp
  src/chroma/chroma_key.cpp
  src/chroma/compositor.cpp
  src/chroma/ai_worker.cpp
  src/chroma/model_store.cpp
  src/chroma/chroma_filter.cpp
  src/chroma/service.cpp)
target_include_directories(hb_chroma PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_chroma PUBLIC hb_core Threads::Threads)
if(MSVC)
  target_compile_options(hb_chroma PRIVATE /W4 /permissive-)
else()
  target_compile_options(hb_chroma PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(HB_BUILD_TESTS)
  file(GLOB HB_CHROMA_TESTS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/chroma_*.cpp)
  add_executable(hb_chroma_tests ${HB_CHROMA_TESTS})
  target_link_libraries(hb_chroma_tests PRIVATE hb_chroma GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_chroma_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_chroma_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  gtest_discover_tests(hb_chroma_tests)
endif()

if(WIN32)
  add_library(hb_chroma_win STATIC
    src/chroma/windows/winhttp_client.cpp
    src/chroma/windows/wic_image.cpp)
  target_link_libraries(hb_chroma_win PUBLIC hb_chroma winhttp windowscodecs ole32)
  target_compile_definitions(hb_chroma_win PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(hb_chroma_win PRIVATE /W4 /permissive-)
  endif()

  # Optional ONNX Runtime (MIT). Point HB_ORT_DIR at an extracted official onnxruntime-win-x64-<ver>.zip
  # (include/ + lib/onnxruntime.lib). Without it the app builds and runs with classic chroma only.
  # onnxruntime.dll must then be shipped next to HBStudio.exe (installer packaging: see docs/CHROMA.md).
  set(HB_ORT_DIR "${CMAKE_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH "Extracted onnxruntime-win-x64 release (optional)")
  if(EXISTS "${HB_ORT_DIR}/include/onnxruntime_cxx_api.h" AND EXISTS "${HB_ORT_DIR}/lib/onnxruntime.lib")
    message(STATUS "Chroma: ONNX Runtime found in ${HB_ORT_DIR} (AI background removal enabled)")
    add_library(hb_chroma_onnx STATIC src/chroma/onnx/onnx_segmenter.cpp)
    target_include_directories(hb_chroma_onnx PUBLIC "${HB_ORT_DIR}/include")
    target_link_libraries(hb_chroma_onnx PUBLIC hb_chroma "${HB_ORT_DIR}/lib/onnxruntime.lib" delayimp)
    target_link_options(hb_chroma_onnx PUBLIC /DELAYLOAD:onnxruntime.dll)
    target_compile_definitions(hb_chroma_onnx PUBLIC HB_HAVE_ONNX)
    target_compile_definitions(hb_chroma_onnx PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    message(STATUS "Chroma: ONNX Runtime not found (HB_ORT_DIR) - AI background removal disabled, classic chroma only")
  endif()

  # Modules load after the main CMakeLists, so hb_studio already exists here.
  if(TARGET hb_studio)
    target_link_libraries(hb_studio PRIVATE hb_chroma_win)
    if(TARGET hb_chroma_onnx)
      target_link_libraries(hb_studio PRIVATE hb_chroma_onnx)
    endif()
  endif()
endif()
