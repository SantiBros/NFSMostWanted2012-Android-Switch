cmake_minimum_required(VERSION 3.16)
project(nfsmw_loader CXX C)

# The port's version: shown in the Homebrew Menu / Sphaira, in the .nro and zip
# file names (NFSMostWanted2012-<version>.nro) and at the top of log.txt.
set(NFSMW_VERSION "0.3.1")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(NINTENDO_SWITCH)
  # Dynarmic needs the (header-only) Boost headers it uses. devkitPro has no
  # Boost package, so the host's copy is used (Linux: libboost-dev; macOS:
  # brew install boost; MSYS2: mingw-w64-x86_64-boost), or pass
  # -DNFSMW_BOOST_INCLUDE=<dir containing boost/>. Only the boost/ folder is
  # exposed to the cross compiler, never the host's other system headers.
  find_path(NFSMW_BOOST_INCLUDE boost/icl/interval_set.hpp
    PATHS /usr/include /usr/local/include /opt/homebrew/include /mingw64/include /ucrt64/include
    NO_CMAKE_FIND_ROOT_PATH NO_DEFAULT_PATH)
  if(NOT NFSMW_BOOST_INCLUDE)
    message(FATAL_ERROR "Boost headers not found: install them (e.g. libboost-dev) or pass -DNFSMW_BOOST_INCLUDE=<dir>")
  endif()
  set(_boost_fwd ${CMAKE_BINARY_DIR}/boost-headers)
  file(MAKE_DIRECTORY ${_boost_fwd})
  if(NOT EXISTS ${_boost_fwd}/boost)
    file(CREATE_LINK ${NFSMW_BOOST_INCLUDE}/boost ${_boost_fwd}/boost SYMBOLIC COPY_ON_ERROR)
  endif()
  set(Boost_INCLUDE_DIR ${_boost_fwd} CACHE PATH "" FORCE)
  set(Boost_NO_BOOST_CMAKE ON)
  # newlib hides POSIX functions (putc_unlocked, used by fmt) in strict C++ mode.
  add_compile_definitions(_GNU_SOURCE)
endif()

# Dynarmic (ARM -> host JIT). Only the 32-bit ARM frontend is needed.
set(FMT_OS OFF CACHE BOOL "" FORCE)  # fmt's OS layer is not needed (and not portable to Horizon)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

set(CORE_SOURCES
  src/core/log.cpp
  src/core/memory.cpp
  src/core/cpu.cpp
  src/core/elf.cpp
  src/core/crc32.cpp
  src/core/crash.cpp
  src/core/thunks.cpp
  src/core/hook.cpp
  src/hle/hle.cpp
  src/hle/libc_string.cpp
  src/hle/libc_stdio.cpp
  src/hle/libc_io.cpp
  src/hle/file_cache.cpp
  src/core/jit_cache.cpp
  src/hle/libc_misc.cpp
  src/hle/net_block.cpp
  src/hle/libc_math.cpp
  src/hle/libc_pthread.cpp
  src/hle/android.cpp
  src/hle/gles.cpp
  src/hle/gl_queue.cpp
  src/hle/jni/jni.cpp
  src/hle/jni/java_classes.cpp
  src/hle/jni/text.cpp
  src/game/input.cpp
  src/game/motion.cpp
  src/game/prefs.cpp
  src/game/paths.cpp
  src/game/setup.cpp
  src/game/perf.cpp
  src/game/debug_menu.cpp
  src/game/ui_nav.cpp
  src/game/overlay.cpp
  src/game/fixes.cpp
  src/game/native.cpp
  src/game/obb_patch.cpp
  src/game/controller_art.cpp
  src/game/rtti.cpp
  src/game/audio.cpp
  src/platform/thread.cpp
  src/main.cpp
)

if(NINTENDO_SWITCH)
  # ---- Nintendo Switch (devkitPro / libnx) ----------------------------------
  # cmake -B build-switch -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake
  add_executable(nfsmw ${CORE_SOURCES} src/platform/platform_switch.cpp src/platform/audio_switch.cpp)
  target_compile_definitions(nfsmw PRIVATE NFSMW_VERSION="${NFSMW_VERSION}")
  target_include_directories(nfsmw PRIVATE src . external/dynarmic/src)
  target_compile_options(nfsmw PRIVATE -Wall -Wno-unused-function -ffunction-sections)
  # switch-mesa: EGL + the GL dispatch (glapi) + the nouveau driver interface.
  # The game's GL calls go straight to the GLESv2 entry points.
  find_package(OpenGL REQUIRED)
  find_library(NFSMW_GLESV2 GLESv2 REQUIRED)
  target_link_libraries(nfsmw PRIVATE dynarmic ${NFSMW_GLESV2} OpenGL::EGL nx)
  nx_generate_nacp(nfsmw.nacp
    NAME "NFS Most Wanted 2012 Loader"
    AUTHOR "SantiBros"
    VERSION "${NFSMW_VERSION}")
  # The port's file name on the SD card: sd:/switch/NFSMostWanted2012/NFSMostWanted2012-<version>.nro
  # (it finds its files in the folder it is started from, so both can be renamed).
  # res/romfs is built into the .nro: the /proc and /sys files the game reads.
  nx_create_nro(nfsmw
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/NFSMostWanted2012-${NFSMW_VERSION}.nro
    NACP nfsmw.nacp
    ICON ${CMAKE_CURRENT_SOURCE_DIR}/res/icon.jpg
    ROMFS ${CMAKE_CURRENT_SOURCE_DIR}/res/romfs)
  return()
endif()

# NFSMW_SWITCH_TEST=ON builds the desktop loader the way the Switch build
# works: no fastmem (page table), dual-mapped JIT code memory, and (for
# cross builds without GPU libraries) no-op GL.
option(NFSMW_SWITCH_TEST "Use the Switch memory/JIT model on Linux" OFF)
option(NFSMW_NO_GL "Build without OpenGL (smoke tests)" OFF)

add_executable(nfsmw ${CORE_SOURCES} src/platform/platform_linux.cpp)
target_compile_definitions(nfsmw PRIVATE NFSMW_VERSION="${NFSMW_VERSION}")
target_include_directories(nfsmw PRIVATE src . external/dynarmic/src)
target_link_libraries(nfsmw PRIVATE dynarmic pthread)
if(NOT NFSMW_NO_GL)
  target_link_libraries(nfsmw PRIVATE EGL GLESv2)
else()
  target_compile_definitions(nfsmw PRIVATE NFSMW_NO_GL)
endif()
if(NFSMW_SWITCH_TEST)
  target_compile_definitions(nfsmw PRIVATE NFSMW_SWITCH_MEMORY_MODEL)
  target_compile_definitions(dynarmic PRIVATE DYNARMIC_DUAL_MAPPED_CODE)
endif()
target_compile_options(nfsmw PRIVATE -Wall -Wno-unused-function)
# The /proc and /sys files the game reads (built into the .nro on the Switch).
target_compile_definitions(nfsmw PRIVATE NFSMW_ROMFS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/res/romfs")
