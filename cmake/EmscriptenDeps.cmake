# The libraries reVC and librw look for, as Emscripten provides them (the browser build, Orthros):
# SDL2 and mpg123 are Emscripten ports (from the pinned cache, no network), OpenAL its WebAudio implementation, OpenGL
# its WebGL 2 one (linked by default). Included after each project(): the targets are defined once.
if(NOT TARGET SDL2::SDL2)
  add_library(SDL2::SDL2 INTERFACE IMPORTED)
  set_target_properties(SDL2::SDL2 PROPERTIES INTERFACE_COMPILE_OPTIONS "-sUSE_SDL=2" INTERFACE_LINK_OPTIONS "-sUSE_SDL=2")
endif()
if(NOT TARGET OpenGL::GL)
  add_library(OpenGL::GL INTERFACE IMPORTED)
endif()
if(NOT TARGET OpenAL::OpenAL)
  add_library(OpenAL::OpenAL INTERFACE IMPORTED)
  # (OpenAL Soft's headers from reVC's vendor folder: Emscripten's have no efx.h; the game finds no EFX at run time)
  set_target_properties(OpenAL::OpenAL PROPERTIES INTERFACE_LINK_OPTIONS "-lopenal"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/vendor/openal-soft/include")
endif()
if(NOT TARGET MPG123::libmpg123)
  add_library(MPG123::libmpg123 INTERFACE IMPORTED)
  set_target_properties(MPG123::libmpg123 PROPERTIES INTERFACE_COMPILE_OPTIONS "-sUSE_MPG123=1" INTERFACE_LINK_OPTIONS "-sUSE_MPG123=1")
endif()
