# =============================================================================
# Jurassic Park: Operation Genesis - static recompilation
#
#   python3 -I tools/gen/analyse.py GAME work/analysis    # the lift's inputs
#   python3 run_lift.py                                    # -> work/gen/
#   cmake -S . -B build-macos -G Ninja && cmake --build build-macos
#
# For now this compiles the lifted C of the Toshi DLLs, the shader DLLs and
# libmmd (jpog_gen, an object library): the host (src/host), pcrecomp's
# runtime/win32hle and the program join it in Phase 4 (docs/PLAN.md), as
# stl-recomp's did.
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(jpog C)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
if(NOT (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
  message(FATAL_ERROR "macOS first, Linux next")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "a 64-bit host: the guest's 4 GB is one block of its memory (based)")
endif()

set(PCRECOMP "${CMAKE_SOURCE_DIR}/../pcrecomp" CACHE PATH "pcrecomp checkout")
set(GEN_DIR "${CMAKE_SOURCE_DIR}/work/gen" CACHE PATH "the lifted C (run_lift.py --out)")
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GEN_DIR}/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "${GEN_DIR} is empty -- run python3 run_lift.py first")
endif()
if(NOT EXISTS "${PCRECOMP}/runtime/recomp32/recomp_types.h")
  message(FATAL_ERROR "no pcrecomp at ${PCRECOMP}: pass -DPCRECOMP=DIR")
endif()

# A fixed address for the guest's 4 GB lets the lifted code add a constant,
# not a global it reloads after every store (tpw-recomp's and stl-recomp's
# choice: 16 TB up is free on a Mac, arm64 and x86-64).
if(APPLE)
  set(JPOG_MEM_BASE "0x100000000000ull" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
else()
  set(JPOG_MEM_BASE "" CACHE STRING "host address of the guest's 4 GB (empty: anywhere)")
endif()

# The lifted code and the runtime agree on these (pcrecomp's recomp32), as
# stl-recomp builds them: the x87 stack as a ring with a function's own
# pushes in locals, and indirect calls cached per call site and in one table
# (one thread at a time runs lifted code, under win32hle's machine lock).
set(JPOG_RECOMP_DEFS RECOMP_RETADDR=0u RECOMP_LOCAL_REGS RECOMP_X87_RING RECOMP_X87_LOCAL
    RECOMP_RESOLVE_CACHE=16384 RECOMP_ICALL_SITE_CACHE)

add_library(jpog_gen OBJECT ${GEN_SOURCES})
set_target_properties(jpog_gen PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(jpog_gen PRIVATE ${GEN_DIR} ${PCRECOMP}/runtime/win32hle ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(jpog_gen PRIVATE ${JPOG_RECOMP_DEFS})
if(JPOG_MEM_BASE)
  target_compile_definitions(jpog_gen PRIVATE RECOMP_MEM_BASE=${JPOG_MEM_BASE})
endif()
set(GEN_OPT "-O2" CACHE STRING "optimisation for the generated C")
target_compile_options(jpog_gen PRIVATE ${GEN_OPT} -w -fno-strict-aliasing -fwrapv)

# ---- the host ---------------------------------------------------------------
# SDL2 and SDL2_ttf from pkg-config (Homebrew): win32hle's window, sound and
# text. The movies' ffmpeg, the app's frameworks and the library the app loads
# join in later phases, as stl-recomp's.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
# The music is Ogg Vorbis (src/host/fsound.c): without libvorbisfile there is none.
pkg_check_modules(VORBISFILE IMPORTED_TARGET vorbisfile)
set(HLE ${PCRECOMP}/runtime/win32hle)
file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$")
file(GLOB HOST_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/host/*.c")
list(FILTER HOST_SOURCES EXCLUDE REGEX "/exe\\.c$")
add_library(jpog_obj OBJECT ${HOST_SOURCES} ${HLE_SOURCES})
set_target_properties(jpog_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(jpog_obj PRIVATE src/host ${GEN_DIR} ${HLE} ${PCRECOMP}/runtime/recomp32)
target_compile_definitions(jpog_obj PRIVATE ${JPOG_RECOMP_DEFS})
if(JPOG_MEM_BASE)
  target_compile_definitions(jpog_obj PRIVATE RECOMP_MEM_BASE=${JPOG_MEM_BASE})
endif()
target_compile_options(jpog_obj PRIVATE -fno-strict-aliasing -fwrapv)
target_link_libraries(jpog_obj PUBLIC PkgConfig::SDL2 PkgConfig::SDL2_TTF pthread m)
if(VORBISFILE_FOUND)
  target_compile_definitions(jpog_obj PRIVATE JPOG_VORBIS)
  target_link_libraries(jpog_obj PUBLIC PkgConfig::VORBISFILE)
else()
  message(STATUS "music: no libvorbisfile, so none")
endif()
if(APPLE)
  enable_language(OBJC)
  target_sources(jpog_obj PRIVATE ${HLE}/d3d_metal.m src/host/d3d_metal.m)
  set_source_files_properties(${HLE}/d3d_metal.m PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
  set_source_files_properties(src/host/d3d_metal.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(jpog_obj PUBLIC "-framework Metal" "-framework Foundation")
else()
  target_link_libraries(jpog_obj PUBLIC dl)
endif()
add_executable(jpog src/host/exe.c)
target_link_libraries(jpog PRIVATE jpog_obj jpog_gen)
