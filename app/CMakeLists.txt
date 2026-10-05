# nfsmw - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nfsmw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NFSMW_SOURCES
    src/main.cpp
    # Phase A of the native renderer: wraps the game's Direct3D to confirm that each
    # address is the function we think it is. It does not change drawing; it only writes rex_d3d.log.
    src/nfsmw_d3d_trace.cpp
    # The scene is drawn a single time: without the Xbox 360's 3-strip tiling or MSAA
    # (cvar nfsmw_render_sin_mosaico, on by default on Switch).
    src/nfsmw_render_targets.cpp
    # Race-time reductions: road reflection and faces of the car's cubemap
    # (cvars nfsmw_reflejo_carretera and nfsmw_cubemap_caras_max).
    src/nfsmw_recortes_carrera.cpp
    # Rescues the game's audio server thread if the XAudio voice stops
    # returning end-of-packet notifications (cvar nfsmw_audio_rescate).
    src/nfsmw_audio_servidor.cpp
    # Priority of the current thread on the Switch (nfsmw_audio_servidor_prioridad).
    src/nfsmw_hilos_switch.cpp
    # Diagnostic: timing of the sound engine functions on the audio server thread
    # (cvar nfsmw_audio_diag_funciones).
    src/nfsmw_audio_perfil_funciones.cpp
    # Measurement probe for the car engine sound (Ginsu): buffers, volumes and voices; it only
    # measures (cvar nfsmw_audio_sonda_motor).
    src/nfsmw_audio_motor_sonda.cpp
    # Linear resamplers of the sound engine in native code, bit-identical
    # (cvar nfsmw_audio_remuestreo_nativo).
    src/nfsmw_audio_remuestreo.cpp
    # Recursive filter of the sound engine (sub_825CD088) in native code, bit-identical
    # (cvar nfsmw_audio_filtro_nativo).
    src/nfsmw_audio_filtro.cpp
    # Gain-scaled sum of the sound engine (sub_825FDFB0) in native code, bit-identical
    # (cvar nfsmw_audio_suma_nativa).
    src/nfsmw_audio_suma.cpp
    # Environment variables for Mesa/NVK before creating Vulkan (cvar nfsmw_mesa_entorno) and, on the
    # Switch, the size of Mesa's on-disk shader cache.
    src/nfsmw_entorno_mesa.cpp
    # Native memset of the game's CRT (sub_826BE610), like the memcpy of [rexcrt] (cvar nfsmw_crt_memset_nativo):
    # src/nfsmw_crt_nativo.cpp is left out of the build. The recompiler's [rexcrt] (overrides.toml) now does that
    # memset: it emits rexcrt_memset at all 1,109 call sites, so the original function no longer exists and the
    # hook has nothing to hook (on PC it broke linking). Both did the same: std::memset with the same address
    # translation.
    # Cutscene diagnostic: frame presentation (cvar nfsmw_video_diag).
    src/nfsmw_video_diag.cpp
    # WMV3 cutscenes decoded with the SDK's FFmpeg instead of the recompiled
    # XDK decoder (cvar nfsmw_video_wmv3_nativo).
    src/nfsmw_video_wmv3.cpp
    src/nfsmw_video_nativo.cpp
    # Shadows updated once every N frames (cvar nfsmw_sombras_cada,
    # 1 = as the game does; it can be changed at run time from the settings menu).
    src/nfsmw_recorte_sombras.cpp
    src/nfsmw_sombras_lod.cpp
    # Native renderer, part C1: the app's own graphics system (cvar
    # nfsmw_renderizador, "xenos" by default). See docs/native-renderer.md.
    src/nfsmw_nativo_sistema.cpp
    # The game's D3D waits without spinning: sleeps until the PM4 ring thread advances
    # (cvar nfsmw_espera_anillo_bloqueante).
    src/nfsmw_espera_anillo.cpp
    # Frame handoff between the game's two threads without a Sleep(0) loop
    # (cvar nfsmw_espera_fotograma_bloqueante).
    src/nfsmw_espera_fotograma.cpp
    # Dump of the changed registers of the game's D3D (sub_825A2AA0) in native code: one copy instead of
    # ~15-20 instructions per word, bit-identical (cvar nfsmw_d3d_registros_nativo).
    src/nfsmw_d3d_registros_nativo.cpp
    # Per-draw material parameters (sub_824511E8 and its writers 8244F2E0, 82449360, 82449618,
    # 82449988 and 82449C00) in native code, bit-identical and checked at run time against the original
    # (cvar nfsmw_material_nativo).
    src/nfsmw_material_nativo.cpp
    # eViewPlatInterface::Render (sub_8243E358, the submesh loop of each model) in native code, bit-identical
    # and checked at run time against the original (cvar nfsmw_eview_nativo).
    src/nfsmw_eview_nativo.cpp
    # Per-draw matrices (sub_824538D0: A x P, A x Q, A x C and the rigid inverse of A) in native code,
    # bit-identical and checked at run time against the original (cvar nfsmw_matrices_nativo).
    src/nfsmw_matrices_nativo.cpp
    # ScenerySectionHeader::DrawAScenery (sub_824C2850: culling, size in pixels, LOD and SceneryDrawInfo of
    # each scenery object) in native code, bit-identical and checked at run time against the original
    # (cvar nfsmw_escenario_nativo).
    src/nfsmw_escenario_nativo.cpp
    # Start of an effect pass (sub_82448E80: dirty masks, shaders, render states and sampler states) in
    # native code, identical and checked at run time against a literal copy of the original
    # (cvar nfsmw_efecto_pasada_nativo).
    src/nfsmw_efecto_pasada_nativo.cpp
    # The draw glue (sub_82452730 with the streams, the indices and the draw of each object) and the loop
    # over the list that calls it (sub_82454B50) in native code, identical, keeping the game's cache hints
    # (dcbt), and checked at run time against a literal copy of the original (cvar nfsmw_pegamento_nativo).
    src/nfsmw_pegamento_nativo.cpp
    # PNG captures of the game image for testing (cvar
    # nfsmw_captura_cada_s, 0 = never).
    src/nfsmw_nativo_captura.cpp
    # Native renderer, part C2: render targets, copies (resolve and
    # clear) and presentation of the game image.
    src/nfsmw_nativo_destinos.cpp
    # Native renderer, part C5a: identifies the shaders that reach the PM4 ring
    # in the NFSSPV library (nfsmw_shaders.nfsp next to the executable).
    src/nfsmw_nativo_shaders.cpp
    src/nfsmw_shader_library.cpp
    # Native renderer, part C5b: VS and PS of each draw from the game
    # thread (shader constructors and the D3D Draw* functions).
    src/nfsmw_nativo_ganchos.cpp
    # Native renderer, parts C3-C6: textures, vertices, constants,
    # pipelines and render passes with the shaders from the library.
    src/nfsmw_nativo_dibujos.cpp
    src/nfsmw_nativo_texturas_pool.cpp
    src/nfsmw_escenario_lod.cpp
    # The streamer requests the zone too late: facade popping and a hitch when entering a new
    # zone (hook on TrackStreamer::GetPredictedZone).
    src/nfsmw_streaming_anticipacion.cpp
    # Language of the game text on multi-language discs (cvar nfsmw_idioma).
    src/nfsmw_idioma.cpp
    # Automated tests on PC: virtual gamepad driven by a script (nfsmw_prueba_botones).
    src/nfsmw_prueba_entrada.cpp
    # Internal resolution and FPS limit in the menu (F4) that work with the native renderer, with the
    # settings that have no effect taken out of the menu.
    src/nfsmw_ajustes_graficos.cpp
    # Profile dump of the instrumented build for PGO (NFSMW_PGO=generar; empty otherwise).
    src/nfsmw_pgo.cpp
)

if(ANDROID)
    # SDLActivity resolves SDL_main from libmain.so.
    add_library(nfsmw SHARED ${NFSMW_SOURCES})
    set_target_properties(nfsmw PROPERTIES OUTPUT_NAME main)
elseif(WIN32)
    add_executable(nfsmw WIN32 ${NFSMW_SOURCES})
    # PC build: masks again the floating-point exceptions that
    # rex::ppc::FPSCR can leave unmasked on x86 (see the file).
    target_sources(nfsmw PRIVATE src/nfsmw_pc_coma_flotante.cpp)
    # Sampling CPU profiler for the PC tests (cvar nfsmw_perfil_pc_desde_s, 0 = never).
    target_sources(nfsmw PRIVATE src/nfsmw_perfil_pc.cpp)
    # Full symbols (CodeView) of the native renderer in the PDB: without them the profiler only sees the public
    # symbols, and all the code in anonymous namespaces shows up under the name of the preceding public symbol.
    # Debug info only; it does not change the generated code.
    set_source_files_properties(
        src/nfsmw_nativo_sistema.cpp src/nfsmw_nativo_destinos.cpp src/nfsmw_nativo_dibujos.cpp
        src/nfsmw_nativo_texturas_pool.cpp
        src/nfsmw_nativo_shaders.cpp src/nfsmw_nativo_ganchos.cpp src/nfsmw_espera_anillo.cpp
        src/nfsmw_espera_fotograma.cpp
        PROPERTIES COMPILE_OPTIONS "-g;-gcodeview")
else()
    add_executable(nfsmw ${NFSMW_SOURCES})
endif()

# GPU_PLUGINS leaves the graphics backend DLL next to the .exe
# (rexgpu-xenosd.dll in Debug, rexgpu-xenos.dll in Release).
#
# Both things are needed to get an image:
#   1. this line, which puts the DLL where the loader looks for it
#   2. gpu_plugin set to "xenos" at run time
#
# The gpu_plugin CVar is empty by default, and empty means "no GPU
# emulation": the game initializes its ring buffer and its graphics callbacks, the
# runtime discards them with "no GPU emulation loaded", and the screen stays
# black even though the game is running perfectly.
#
# Since src/nfsmw_app.h sets gpu_plugin=xenos by default, the --gpu_plugin
# argument is no longer needed; it can still be passed to change it.
rexglue_setup_target(nfsmw GPU_PLUGINS xenos)

# The SDK's FFmpeg (WMV3 decoder) for the cutscenes: nfsmw_video_wmv3.cpp.
target_link_libraries(nfsmw PRIVATE libavcodec)

# PC build with the SDK as a subdirectory (tools/pc/construir_pc.bat):
# rex_app.cpp includes imgui.h and rexruntime links imgui privately, so its
# include path does not reach the executable. On Switch rexglue_switch.cmake handles it, and with
# the SDK installed the headers are already in its include directory (the target does not exist there).
# The same goes for renderdoc_app.h, which rex/ui/vulkan/instance.h includes outside
# Switch (nfsmw_nativo_sistema.cpp uses the SDK's Vulkan device).
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(nfsmw PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(nfsmw PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Precompiled SPIR-V of the SDK presenter: nfsmw_nativo_destinos.cpp uses it to draw
# the game image into the presenter output.
target_include_directories(nfsmw PRIVATE "${REXSDK_DIR}/src/ui/shaders")

# The SDK's xxHash: fingerprints for the shader library and the native renderer.
find_path(NFSMW_XXHASH_INCLUDE xxhash.h HINTS "${REXSDK_DIR}/thirdparty/xxHash" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
target_include_directories(nfsmw PRIVATE "${NFSMW_XXHASH_INCLUDE}")

# Experimental library: the synthetic tests do not validate the full game.
# Regression confirmed on Switch: the release build excludes this path.
option(NFSMW_NATIVE_SHADER_LIBRARY "Integrar la biblioteca experimental de shaders nativos" OFF)
if(NFSMW_NATIVE_SHADER_LIBRARY)
    target_sources(nfsmw PRIVATE
        src/nfsmw_shader_vulkan.cpp
        src/nfsmw_video_vulkan.cpp
        src/nfsmw_video_session.cpp
        src/nfsmw_video_bridge.cpp
        src/nfsmw_video_present.cpp
        src/nfsmw_shader_hooks.cpp)
    target_compile_definitions(nfsmw PRIVATE NFSMW_NATIVE_SHADER_LIBRARY=1)
endif()

# Nintendo Switch: nfsmw.nro next to the ELF. On the SD card it goes in sdmc:/switch/nfsmw/
# with the game_root/ folder extracted beside it (src/nfsmw_app.h looks for it there).
if(REXGLUE_PLATFORM_SWITCH)
    # Hot functions grouped at the start of .text. There are 53.5 MB of code and the A57 has 48 KB of
    # instruction cache and a small TLB: hot code scattered across the whole executable forces a page
    # table walk on every far jump. This only changes where each function is placed (-ffunction-sections
    # already gives one section per function). The list comes from stack sampling on the console.
    # LTO on the game code and ours (not on the SDK libraries). Together with tools/llamadas_directas.py
    # (calls to functions without a hook go to __imp__, which is not weak) the compiler can inline small
    # game functions across files. -flto=2 and partitions: the whole-program phase handles 144 MB of
    # generated source; with more processes it does not fit in 16 GB.
    option(NFSMW_LTO "LTO en el codigo del juego y de la app (Switch)" ON)
    if(NFSMW_LTO)
        foreach(objetivo nfsmw_recomp nfsmw)
            if(TARGET ${objetivo})
                target_compile_options(${objetivo} PRIVATE -flto -fno-fat-lto-objects)
            endif()
        endforeach()
        target_link_options(nfsmw PRIVATE -flto=2 -flto-partition=balanced)
    endif()
    # Profile-guided optimization (PGO) in two steps.
    #   generar: instrumented build; the game dumps the profile to sdmc:/switch/nfsmw/pgo every 3 minutes
    #            (src/nfsmw_pgo.cpp). GCC embeds paths "<profile folder>\<object with # instead of />.gcda"
    #            in the executable; a --wrap of fopen redirects them to the SD card.
    #   usar:    rebuilds with those .gcda files copied to pgo/<edition> on the PC.
    # -fprofile-prefix-path with the build folder in native format (backslashes), which is how GCC sees it:
    # that way each .gcda name is relative and the same on the console and on the PC.
    # Profile folder: one per edition under pgo/ at the root of the repository (tools/editions/crear_arbol.py
    # points each edition tree to its own).
    get_filename_component(NFSMW_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo/pal_es" ABSOLUTE)
    set(NFSMW_PGO "" CACHE STRING "Optimizacion por perfil: vacio, generar o usar")
    if(NFSMW_PGO)
        file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" NFSMW_PGO_PREFIJO)
        if(NFSMW_PGO STREQUAL "generar")
            # -fno-profile-values: libgcov's indirect-call profiling reads the TLS with
            # mrs tpidr_el0, which is 0 on Horizon (libnx uses -mtp=soft), so the instrumented
            # build crashed at startup. Without value profiling the branch and function counts
            # remain, and those are what PGO uses.
            set(NFSMW_PGO_OPCIONES -fprofile-generate=${NFSMW_PGO_DIR} "-fprofile-prefix-path=${NFSMW_PGO_PREFIJO}"
                                   -fprofile-update=single -fno-profile-values)
            target_link_options(nfsmw PRIVATE -fprofile-generate -Wl,--wrap=fopen)
            target_compile_definitions(nfsmw PRIVATE NFSMW_PGO_GENERAR=1 "NFSMW_PGO_DIR=\"${NFSMW_PGO_DIR}\"")
        elseif(NFSMW_PGO STREQUAL "usar")
            set(NFSMW_PGO_OPCIONES -fprofile-use=${NFSMW_PGO_DIR} "-fprofile-prefix-path=${NFSMW_PGO_PREFIJO}"
                                   -fprofile-partial-training -fno-profile-values -fprofile-correction
                                   -Wno-missing-profile
                                   -Wno-error=coverage-mismatch)
            # -fprofile-correction: the instrumentation counts without synchronization (-fprofile-update=single)
            # and files used by several threads at once (the renderer) ended up with negative counts: GCC
            # treated that as a corrupt profile and stopped. This option corrects them.
            target_link_options(nfsmw PRIVATE -fprofile-use -fprofile-partial-training -fprofile-correction)
        else()
            message(FATAL_ERROR "NFSMW_PGO tiene que ser vacio, generar o usar (es '${NFSMW_PGO}')")
        endif()
        foreach(objetivo nfsmw_recomp nfsmw)
            if(TARGET ${objetivo})
                target_compile_options(${objetivo} PRIVATE ${NFSMW_PGO_OPCIONES})
            endif()
        endforeach()
    endif()
    # NFSMW_RECOMP_O2: A/B test of the optimization level of the recompiled code (nfsmw_recomp). -O3 bloats
    # the code (53 MB) and the A57 has 48 KB of instruction cache: -O2 gives smaller code. It comes after the
    # -O3 of CMAKE_CXX_FLAGS_RELEASE, so it takes precedence. With LTO, GCC records each function's level at
    # compile time.
    option(NFSMW_RECOMP_O2 "Compilar el codigo traducido con -O2 en vez de -O3" OFF)
    if(NFSMW_RECOMP_O2 AND TARGET nfsmw_recomp)
        target_compile_options(nfsmw_recomp PRIVATE -O2)
    endif()
    option(NFSMW_ORDEN_FUNCIONES "Juntar las funciones calientes al principio de .text" ON)
    if(NFSMW_ORDEN_FUNCIONES)
        target_link_options(nfsmw PRIVATE
            "-Wl,--section-ordering-file,${CMAKE_CURRENT_SOURCE_DIR}/orden_funciones.ld")
        set_property(TARGET nfsmw APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/orden_funciones.ld")
    endif()
    # Homebrew menu icon: a 256x256 JPEG, as elf2nro expects. It is not distributed with the sources; without
    # tools/switch/icono/nfsmw_icono.jpg the NRO gets the default libnx icon.
    set(NFSMW_ICONO "${CMAKE_CURRENT_SOURCE_DIR}/../tools/switch/icono/nfsmw_icono.jpg")
    set(NFSMW_ICONO_ARG)
    if(EXISTS "${NFSMW_ICONO}")
        set(NFSMW_ICONO_ARG ICON "${NFSMW_ICONO}")
    endif()
    rexglue_switch_add_nro(nfsmw
        NAME "Need for Speed Most Wanted"
        AUTHOR "StevensND"
        VERSION "1.0.0"
        ${NFSMW_ICONO_ARG})
endif()

# Native Qt launcher (Linux). Used by packaging/appimage/.
option(NFSMW_BUILD_LAUNCHER "Build the native Qt launcher" ON)
option(NFSMW_LAUNCHER_BUILD_TESTS "Build Qt launcher tests" OFF)
if(NFSMW_BUILD_LAUNCHER AND UNIX AND NOT APPLE)
    find_package(Qt6 6.4 COMPONENTS Core Gui Widgets QUIET)
    if(Qt6_FOUND)
        if(NFSMW_LAUNCHER_BUILD_TESTS)
            enable_testing()
        endif()
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../launcher-linux"
                         "${CMAKE_CURRENT_BINARY_DIR}/launcher-linux")
    else()
        message(WARNING
            "Qt 6.4+ was not found; nfsmw_launcher will not be built")
    endif()
endif()


# =============================================================================
#  TARGET "dist" - the portable folder, which is named  build\
#
#  Gathers into a single folder the executable, the project DLLs and the
#  Visual C++ runtime DLLs, so it can be taken to a machine without
#  development tools.
#
#      cmake --build --preset win-amd64-release --target dist
#
#  The target is named "dist" and the folder "build". This is not an oversight:
#  build/ was already in .gitignore and dist/ was not, so putting the folder
#  there (it contains the executable with the game code) keeps it out of git
#  automatically. To put it somewhere else:  -D NFSMW_DIST_DIR=...
#
#  Do not confuse it with app/out/build/win-amd64-release, which is where
#  CMake builds. This one is the assembled folder, ready to be copied.
#
#  What it does not do, and this matters:
#  it does not pack the ISO into anything. The ISO stays a separate file that
#  the game reads on the fly, and src/nfsmw_app.h looks for it next to the .exe.
#  That way the folder works wherever it is, with no fixed path.
#
#  None of this changes how the project is built: it is an extra, separate step.
# =============================================================================
if(WIN32)
    # Resolves the VC runtime DLLs from the toolchain in use.
    # With _SKIP set to TRUE it installs nothing: it only fills in the list, which
    # is all we want. Without this, a path like
    # "C:\Program Files\Microsoft Visual Studio\...\Microsoft.VC143.CRT" would have
    # to be written by hand, which is exactly what must not be here.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
    include(InstallRequiredSystemLibraries)

    # Deliberately not cached. If these were cache variables, changing the default
    # would have no effect in an already configured tree: it would silently keep
    # the old value, the target would report "done" and the folder would
    # appear where it did before. This way the value here always applies, and a -D
    # on the command line still overrides it.
    if(NOT DEFINED NFSMW_DIST_NOMBRE)
        set(NFSMW_DIST_NOMBRE "NFS_Most_Wanted")
    endif()
    if(NOT DEFINED NFSMW_DIST_DIR)
        set(NFSMW_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build")
    endif()

    add_custom_target(dist
        DEPENDS nfsmw
        COMMAND ${CMAKE_COMMAND}
                -D "D_EXE=$<TARGET_FILE:nfsmw>"
                -D "D_BUILD=$<TARGET_FILE_DIR:nfsmw>"
                -D "D_DIST=${NFSMW_DIST_DIR}"
                -D "D_NOMBRE=${NFSMW_DIST_NOMBRE}"
                -D "D_VCRUNTIME=${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}"
                -D "D_REDIST=$ENV{VCToolsRedistDir}"
                -D "D_PLANTILLA=${CMAKE_CURRENT_SOURCE_DIR}/dist_leeme.txt"
                -D "D_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/nfsmw.toml"
                -D "D_EDRAM=${CMAKE_CURRENT_SOURCE_DIR}/dist_comparar_edram.bat"
                -D "D_LANZADOR=${CMAKE_CURRENT_SOURCE_DIR}/../tools/lanzador/lanzador.ps1"
                -D "D_LANZADOR_BAT=${CMAKE_CURRENT_SOURCE_DIR}/dist_lanzador.bat"
                -D "D_MATRIZ=${CMAKE_CURRENT_SOURCE_DIR}/../tools/matriz.ps1"
                -D "D_PROBAR=${CMAKE_CURRENT_SOURCE_DIR}/dist_probar.bat"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hacer_dist.cmake"
        COMMENT "Armando la carpeta portable en ${NFSMW_DIST_DIR}"
        VERBATIM
    )
endif()
