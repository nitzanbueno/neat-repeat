cmake_minimum_required(VERSION 3.20)

project(NeatRepeat VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Link the MSVC runtime statically so users don't need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
  clap
  GIT_REPOSITORY https://github.com/free-audio/clap.git
  GIT_TAG 1.2.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(clap)

# Plugin code as an object library so the test executable can drive the exact
# same CLAP entry point the host sees.
add_library(neat_repeat_obj OBJECT src/plugin.cpp)
target_include_directories(neat_repeat_obj PUBLIC src)
target_link_libraries(neat_repeat_obj PUBLIC clap)
set_target_properties(neat_repeat_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(neat_repeat MODULE $<TARGET_OBJECTS:neat_repeat_obj>)
target_link_libraries(neat_repeat PRIVATE clap)
set_target_properties(neat_repeat PROPERTIES OUTPUT_NAME "NeatRepeat")

if(APPLE)
  set_target_properties(neat_repeat PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION clap
    MACOSX_BUNDLE_GUI_IDENTIFIER com.neat-repeat.neat-repeat
    MACOSX_BUNDLE_BUNDLE_NAME "Neat Repeat"
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in)
else()
  set_target_properties(neat_repeat PROPERTIES PREFIX "" SUFFIX ".clap")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(neat_repeat_obj PRIVATE -Wall -Wextra)
  set_target_properties(neat_repeat_obj PROPERTIES CXX_VISIBILITY_PRESET hidden)
elseif(MSVC)
  target_compile_options(neat_repeat_obj PRIVATE /W4)
endif()

option(NEAT_REPEAT_BUILD_TESTS "Build the offline behaviour tests" ON)
if(NEAT_REPEAT_BUILD_TESTS)
  enable_testing()
  add_executable(neat_repeat_tests tests/plugin_test.cpp $<TARGET_OBJECTS:neat_repeat_obj>)
  target_link_libraries(neat_repeat_tests PRIVATE clap)
  target_include_directories(neat_repeat_tests PRIVATE src)
  add_test(NAME neat_repeat_tests COMMAND neat_repeat_tests)
endif()
