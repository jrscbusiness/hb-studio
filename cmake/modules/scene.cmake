# Scene compositor + template library ("Templates prontos"). The sources are compiled into hb_core
# because Engine owns the scene slots on the switcher bus.
set(HB_SCENE_JSON "${CMAKE_SOURCE_DIR}/resources/templates/builtin.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${HB_SCENE_JSON}")
file(READ "${HB_SCENE_JSON}" HB_SCENE_HEX HEX)
string(LENGTH "${HB_SCENE_HEX}" HB_SCENE_HEXLEN)
math(EXPR HB_SCENE_BYTES "${HB_SCENE_HEXLEN} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HB_SCENE_ARRAY "${HB_SCENE_HEX}")
set(HB_SCENE_GEN "${CMAKE_BINARY_DIR}/generated/scene_builtin_data.cpp")
file(WRITE "${HB_SCENE_GEN}"
  "// Generated from resources/templates/builtin.json - do not edit.\n#include <cstddef>\nnamespace hb::scene {\nextern const unsigned char kBuiltinTemplatesData[];\nextern const std::size_t kBuiltinTemplatesSize;\nconst unsigned char kBuiltinTemplatesData[] = {${HB_SCENE_ARRAY}0x00};\nconst std::size_t kBuiltinTemplatesSize = ${HB_SCENE_BYTES};\n}\n")

target_sources(hb_core PRIVATE
  ${CMAKE_SOURCE_DIR}/src/scene/scene.cpp
  ${CMAKE_SOURCE_DIR}/src/scene/compositor.cpp
  ${CMAKE_SOURCE_DIR}/src/scene/scene_source.cpp
  ${CMAKE_SOURCE_DIR}/src/scene/template_library.cpp
  ${CMAKE_SOURCE_DIR}/src/scene/scene_service.cpp
  ${HB_SCENE_GEN})

if(HB_BUILD_TESTS)
  add_executable(hb_scene_tests
    tests/scene_model_test.cpp
    tests/scene_compositor_test.cpp
    tests/scene_templates_test.cpp
    tests/scene_service_test.cpp)
  target_link_libraries(hb_scene_tests PRIVATE hb_core GTest::gtest_main)
  if(MSVC)
    target_compile_options(hb_scene_tests PRIVATE /W4 /permissive-)
  else()
    target_compile_options(hb_scene_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  gtest_discover_tests(hb_scene_tests)
endif()
