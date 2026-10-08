cmake_minimum_required(VERSION 3.22.1)
project(nfsmw_android LANGUAGES C CXX)

# Small launcher probe: it must also load on ARMv8.0 phones without loading the game.
add_library(nfsmw_diagnostics SHARED android_diagnostico.cpp)
target_include_directories(nfsmw_diagnostics PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../sdk/thirdparty/vulkan-headers/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../sdk/include")
target_compile_options(nfsmw_diagnostics PRIVATE -march=armv8-a)
target_link_libraries(nfsmw_diagnostics PRIVATE rex_android_gpu_driver dl)
target_link_options(nfsmw_diagnostics PRIVATE "-Wl,-z,max-page-size=16384")

# SDL's Android Activity loads libSDL3.so and then libmain.so. Keep both native
# libraries and the ReXGlue app in this Gradle externalNativeBuild graph.
get_filename_component(NFSMW_REXSDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../sdk" ABSOLUTE)
set(REXSDK_DIR "${NFSMW_REXSDK_DIR}" CACHE PATH "ReXGlue SDK source tree")
set(REXGLUE_USE_VULKAN ON CACHE BOOL "Build ReXGlue's Vulkan backend" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "Android uses Vulkan" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Disable Tracy on Android" FORCE)
set(REXGLUE_VERSION_OVERRIDE "0.10.0-dev.android" CACHE STRING "ReXGlue SDK version for Android")
set(REXGLUE_RECOMP_DEBUG_INFO "none" CACHE STRING "Omit debug tables for the large generated guest code")
set(SDL_SHARED ON CACHE BOOL "SDL shared library loaded by SDLActivity" FORCE)
set(SDL_STATIC OFF CACHE BOOL "Use SDL's shared Android library" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../../../../app" "nfsmw-app")
target_sources(nfsmw PRIVATE "${CMAKE_CURRENT_LIST_DIR}/android_touch.cpp"
                             "${CMAKE_CURRENT_LIST_DIR}/android_rendimiento.cpp")

if(TARGET SDL3-shared)
    # SDLActivity calls System.loadLibrary("SDL3") in every build variant.
    # CMake otherwise gives RelWithDebInfo libraries an "rd" suffix, which
    # SDLActivity cannot resolve from Java.
    set_target_properties(SDL3-shared PROPERTIES
        OUTPUT_NAME "SDL3"
        DEBUG_POSTFIX ""
        RELEASE_POSTFIX ""
        RELWITHDEBINFO_POSTFIX ""
        MINSIZEREL_POSTFIX ""
    )
endif()

# Package the namespace hooks adrenotools opens from nativeLibraryDir.
add_dependencies(nfsmw main_hook file_redirect_hook gsl_alloc_hook hook_impl)

# The game never needs to access the extracted disc through a SAF URI. Java
# imports it into app-private POSIX storage before GameActivity starts.
target_link_options(nfsmw PRIVATE "-Wl,-z,max-page-size=16384")

# Both libmain and rexruntime contain FFmpeg. Its C symbols are hidden, but
# assembly DSP symbols are not covered by C_VISIBILITY_PRESET. Bind those too:
# otherwise libmain initializes its FFT tables and calls rexruntime's NEON FFT,
# whose larger tables are still empty during the startup movies.
foreach(objetivo nfsmw rexruntime)
    target_link_options(${objetivo} PRIVATE
        "LINKER:--exclude-libs,liblibavcodec.a:liblibavutil.a")
endforeach()

# ThinLTO over the translated game and the app, as the Switch build does with GCC's LTO. The generated code
# already calls __imp__sub_X directly (tools/llamadas_directas.py), so clang can inline game functions across
# the 265 files. Release only: in Debug it would just slow the link down.
option(NFSMW_ANDROID_LTO "ThinLTO en el codigo del juego y de la app (Android, Release)" ON)
if(NFSMW_ANDROID_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    foreach(objetivo nfsmw_recomp nfsmw)
        if(TARGET ${objetivo})
            target_compile_options(${objetivo} PRIVATE -flto=thin)
        endif()
    endforeach()
    cmake_host_system_information(RESULT NFSMW_NUCLEOS QUERY NUMBER_OF_LOGICAL_CORES)
    target_link_options(nfsmw PRIVATE -flto=thin "-Wl,--thinlto-jobs=${NFSMW_NUCLEOS}"
                        "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
    # Hot code together, as orden_funciones.ld does on the Switch; lld has no section ordering file for GCC,
    # but it can keep the text of each function in its own section and drop the unused ones.
    target_link_options(nfsmw PRIVATE -Wl,--gc-sections -Wl,--icf=safe)
endif()
