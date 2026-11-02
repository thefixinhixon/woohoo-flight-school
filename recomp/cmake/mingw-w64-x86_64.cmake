# Cross-compiling for Windows (x86-64) from Linux with MinGW-w64:
#   cmake -S recomp -B build-win -DCMAKE_TOOLCHAIN_FILE=recomp/cmake/mingw-w64-x86_64.cmake \
#         -DPWR_WIN_DEPS=<MSYS2 mingw64 prefix with SDL2, vulkan-headers, vulkan-loader> -DGEN_DIR=build/gen
# glslc runs on the build machine (host program); libraries and headers come from PWR_WIN_DEPS only.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 ${PWR_WIN_DEPS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# MSYS2 names the loader's import library libvulkan.dll.a; FindVulkan looks for vulkan-1 on Windows
if(PWR_WIN_DEPS AND EXISTS ${PWR_WIN_DEPS}/lib/libvulkan.dll.a)
  set(Vulkan_LIBRARY ${PWR_WIN_DEPS}/lib/libvulkan.dll.a CACHE FILEPATH "")
endif()
