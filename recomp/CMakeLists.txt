# Builds the recompiled game code + runtime.
#   python3 recomp/arm2c.py <code.bin> <exheader.bin> <gen dir>
#   cmake -S recomp -B build -DGEN_DIR=<gen dir> && cmake --build build -j
# Compilers: GCC or Clang (Linux), clang-cl or MinGW-w64 (Windows). Plain MSVC is not supported: the generated
# code and cpu.h use GCC/Clang builtins (__atomic_*, __builtin_bswap*, __builtin_clz).
cmake_minimum_required(VERSION 3.16)
project(pwr_recomp C)
set(CMAKE_C_STANDARD 11)

# PWR_RUNTIME_ONLY: compile the runtime and shaders without a game (CI check); nothing is linked
option(PWR_RUNTIME_ONLY "compile check of the runtime without generated code" OFF)
if(NOT GEN_DIR AND NOT PWR_RUNTIME_ONLY)
  message(FATAL_ERROR "set -DGEN_DIR=<directory produced by arm2c.py>")
endif()
file(GLOB GEN_SOURCES ${GEN_DIR}/gen_*.c)

option(PWR_WATCH "guest store watch for debugging (PWR_WATCH=<va>,<len> at run time); slower" OFF)
if(PWR_WATCH)
  add_compile_definitions(PWR_WATCH)
endif()

if(NOT PWR_RUNTIME_ONLY)
  # generated code and runtime reference each other (indirect-call table <-> rt_* helpers): one library
  add_library(pwr_game STATIC ${GEN_SOURCES} ${GEN_DIR}/table.c runtime/runtime.c)
  target_include_directories(pwr_game PUBLIC runtime ${GEN_DIR})
  set_source_files_properties(${GEN_SOURCES} ${GEN_DIR}/table.c PROPERTIES COMPILE_OPTIONS "-O2;-w")

  add_executable(test_known tests/test_known.c)
  target_link_libraries(test_known pwr_game m)
endif()

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
find_package(SDL2 REQUIRED)
find_program(GLSLC glslc REQUIRED)
# PICA renderer shaders -> SPIR-V as C initializer lists (#included by runtime/pica_vk.c)
foreach(sh pica.vert pica_vs.vert pica.frag present.vert present.frag fsr_easu.frag fsr_rcas.frag)
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/shaders/${sh}.inc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c -o ${CMAKE_BINARY_DIR}/shaders/${sh}.inc ${CMAKE_SOURCE_DIR}/runtime/shaders/${sh}
    DEPENDS ${CMAKE_SOURCE_DIR}/runtime/shaders/${sh} ${CMAKE_SOURCE_DIR}/runtime/shaders/fsr/ffx_a.h ${CMAKE_SOURCE_DIR}/runtime/shaders/fsr/ffx_fsr1.h)
endforeach()
# the fragment shader without its depth write (pica_vk.c uses it when the rasterizer's depth already matches): early depth tests
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/shaders/pica_nd.frag.inc
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
  COMMAND ${GLSLC} --target-env=vulkan1.3 -O -DNO_FRAG_DEPTH -mfmt=c -o ${CMAKE_BINARY_DIR}/shaders/pica_nd.frag.inc ${CMAKE_SOURCE_DIR}/runtime/shaders/pica.frag
  DEPENDS ${CMAKE_SOURCE_DIR}/runtime/shaders/pica.frag)
set(RUNTIME_SOURCES runtime/main.c runtime/kernel.c runtime/services.c runtime/fs.c runtime/gsp.c runtime/pica.c runtime/video.c runtime/dsp.c runtime/pica_sw.c runtime/input.c runtime/pica_vk.c runtime/pica_vsgen.c runtime/mii.c runtime/freelook.c runtime/drawdist.c
    ${CMAKE_BINARY_DIR}/shaders/pica.vert.inc ${CMAKE_BINARY_DIR}/shaders/pica_vs.vert.inc ${CMAKE_BINARY_DIR}/shaders/pica.frag.inc
    ${CMAKE_BINARY_DIR}/shaders/present.vert.inc ${CMAKE_BINARY_DIR}/shaders/present.frag.inc
    ${CMAKE_BINARY_DIR}/shaders/fsr_easu.frag.inc ${CMAKE_BINARY_DIR}/shaders/fsr_rcas.frag.inc ${CMAKE_BINARY_DIR}/shaders/pica_nd.frag.inc)
if(PWR_RUNTIME_ONLY)
  add_library(pwr_runtime_check OBJECT ${RUNTIME_SOURCES} runtime/runtime.c)
  target_include_directories(pwr_runtime_check PRIVATE runtime ${CMAKE_BINARY_DIR}/shaders)
  target_link_libraries(pwr_runtime_check PRIVATE Vulkan::Vulkan SDL2::SDL2)
  return()
endif()
add_executable(pwr ${RUNTIME_SOURCES})
target_include_directories(pwr PRIVATE ${CMAKE_BINARY_DIR}/shaders)
target_link_libraries(pwr pwr_game Threads::Threads Vulkan::Vulkan SDL2::SDL2 m ${CMAKE_DL_LIBS})
if(WIN32)
  # manifest (UTF-8 code page, DPI awareness); winmm for timeBeginPeriod
  enable_language(RC)
  if(MINGW AND CMAKE_HOST_WIN32)
    # windres runs its preprocessor through cmd.exe, which mangles the command when it starts with a quoted path (a compiler
    # in a folder with spaces, e.g. the build kit under C:\Users\First Last); gcc by name, found on PATH, avoids that
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} --preprocessor=gcc --preprocessor-arg=-E --preprocessor-arg=-xc --preprocessor-arg=-DRC_INVOKED")
  endif()
  target_sources(pwr PRIVATE runtime/win/pwr.rc)
  set_source_files_properties(runtime/win/pwr.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/win/pwr.manifest)
  target_link_libraries(pwr winmm)
endif()
