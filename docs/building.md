# Building the port yourself

## In short

Players do not need this page: the [installer page](https://stevensnd.github.io/nfsmw-nx-installer/) makes the package
from their own copy of the game, with the released NROs. This page is for building the NRO (the Switch program) and
the shader library yourself, from the source code in this repository.

The steps, in order: get the missing libraries, extract the game, build the graphics driver, build the code
generator, translate the game to C++, build the NRO, build the shader library, and copy everything to the SD card.
Each step says what to do and why. If a word is new to you, it is in the [glossary](glossary.md).

Everything was done on Windows 10/11 (64-bit). The scripts are PowerShell, bash (Git Bash or MSYS2) and Python. Only
the driver build is tied to Windows; other systems should work, but nobody has tried them.

## What you need

- [devkitPro](https://devkitpro.org/wiki/Getting_Started), with devkitA64 (the compiler for the Switch) and libnx (the
  Switch library). It installs to `C:/devkitPro`; if you put it somewhere else, set the `DEVKITPRO` environment
  variable to that folder.
- CMake 3.25 or newer, Ninja, Git, and Python 3.10 or newer.
- A C++ compiler for your PC, to build the code generator. Clang 20 or newer was used.
- Your own copy of Need for Speed: Most Wanted for Xbox 360 (an ISO or the extracted files). Nothing from the game is
  in this repository, and nothing made from it may be added to it.
- About 16 GB of RAM. The translated game code is big (about 144 MB of C++), and the final optimization step
  ([LTO](glossary.md#lto)) alone takes about ten minutes.

Only if you need them:

- MSYS2 and Rust, to build the graphics driver yourself (see [mesa/README.md](../mesa/README.md)).
- The Vulkan SDK (for its DXC and spirv-val tools) and MinGW g++, to build the shader library on your PC instead of in
  the installer page.
- Emscripten, to rebuild the tools that the installer page runs in the browser (`shaders/wasm/`).

## 1. Get the missing libraries

**Why:** to keep the repository small, `sdk/thirdparty` only has the files this port changed. The rest comes from the
[ReXGlue](glossary.md#rexglue) version this port is based on (v0.10.0).

```sh
python tools/fetch_thirdparty.py
```

It downloads the rest, and it never overwrites a file that is already there, so the port's changes stay.

## 2. Extract the game

**Why:** the code generator needs the game's program, `default.xex`, and the port needs the game files.

```sh
python tools/fase1_extraer.py path/to/NFSMW.iso -o assets/game_root
```

This extracts the disc into `assets/game_root` ("fase1_extraer" means "phase 1: extract"). Git ignores the `assets/`
folder, so the game never ends up in the repository.

Each [edition](glossary.md#edition) of the game has a different `default.xex` and needs its own build. The main
folder, `app/`, is set up for the PAL Spanish edition; [editions.md](editions.md) explains the others.

## 3. Build the graphics driver

**Why:** the NRO carries its own Vulkan driver, [NVK](glossary.md#nvk), with this port's changes.

Follow [mesa/README.md](../mesa/README.md): build [mesa-switch](https://github.com/danfromtico/mesa-switch) at commit
`1a8c1a66d6f` with `mesa/mesa-switch-nfsmw.patch` applied. The result is an SDK folder. The app needs its subfolder
`opt/devkitpro/portlibs/switch`, which contains `lib/libvulkan.a`.

## 4. Build the code generator

**Why:** the code generator (`rexglue`) is the program that translates the game to C++. It runs on your PC, so you
build it with your PC's compiler, not with the Switch one.

```sh
cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2
cmake --build out/host --target rexglue
```

On an x86-64 PC the SDK needs at least `-march=x86-64-v2` (the SDK presets pass it too): without it
`src/core/memory.cpp` fails with `_mm_shuffle_epi8 requires target feature 'ssse3'`. On an ARM64 PC leave both flags
out.

The SDK writes its programs to `sdk/out/<platform>/`, not to the build folder: `rexglue` ends up in
`sdk/out/linux-amd64/rexglue` on Linux and `sdk/out/win-amd64/rexglue.exe` on Windows.

This version has a change that makes the translated code faster: the Xbox 360 processor's registers live in normal C++
variables, shared by the pieces of a split function (`share_registers`, see [toolchain.md](toolchain.md)).

## 5. Translate the game to C++

```sh
REXGLUE=$PWD/sdk/out/linux-amd64/rexglue tools/codegen.sh app
```

This does three things:

1. It runs the code generator with the settings in `app/nfsmw_manifest.toml`. The C++ goes to `app/generated/default`,
   which git ignores because it is made from the game.
2. It runs `tools/llamadas_directas.py` ("direct calls"). It turns calls between translated functions into direct C++
   calls, so that LTO can optimize across files.
3. It runs `tools/copia_literal.py` ("literal copy"). It writes `app/src/copias_literales/`: exact copies of five game
   functions. The [guards](glossary.md#guard) of their native replacements compare against them (see
   [native-renderer.md](native-renderer.md)).

## 6. Build the NRO

From PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -MesaSdk C:\path\to\mesa-sdk\opt\devkitpro\portlibs\switch
```

Or by hand:

```sh
cmake -S app -B app/out/sw8 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake \
  -DREXSDK_DIR=$PWD/sdk \
  -DREXGLUE_SWITCH_NVK_SDK=/path/to/mesa-sdk/opt/devkitpro/portlibs/switch \
  -DNFSMW_PGO=usar -DNFSMW_BUILD_LAUNCHER=OFF
cmake --build app/out/sw8 -j 4
```

The result is `app/out/sw8/nfsmw.nro`. Rename it to `nfsmw-nx.nro` when you copy it to the SD card (step 8).

Build options, in `app/CMakeLists.txt`:

| Option | Normally | What it does |
|---|---|---|
| `NFSMW_LTO` | ON | Optimizes the translated code and the app as a whole at the end of the build ([LTO](glossary.md#lto)). It uses 2 jobs (`-flto=2`), because more do not fit in 16 GB of RAM. |
| `NFSMW_PGO` | empty | `usar` ("use") builds with the recorded profile in `pgo/pal_es`. `generar` ("generate") builds a special NRO that records a new profile. |
| `NFSMW_ORDEN_FUNCIONES` | ON | Puts the functions that run most at the start of the program (list in `app/orden_funciones.ld`), which makes them faster to fetch. |

The icon for the Homebrew Menu is not included. Put a 256x256 JPEG at `tools/switch/icono/nfsmw_icono.jpg`, or the
NRO gets libnx's default icon.

### The PGO profile

[PGO](glossary.md#pgo) uses a record of which code runs most. `pgo/<edition>/` has the profile the released NROs were
built with. It was recorded on the console while playing (a small test, not a full playthrough), and then translated to
each edition with `tools/editions/pgo/traducir_perfil.py`.

**Why the folder paths matter:** GCC finds each function in the profile by a fingerprint of its name. For functions
that only exist inside one file (`static` functions, or those in anonymous namespaces), the fingerprint also includes
the path of that file. So those functions only match if you build from the same folder paths the profile was recorded
with. Otherwise GCC builds them without the profile: a small loss, not an error.

To record a new profile:

1. Configure with `-DNFSMW_PGO=generar` and build.
2. Play. The NRO writes its counters to `sdmc:/switch/nfsmw/pgo/` every three minutes.
3. Copy the `.gcda` files into `pgo/pal_es/`.
4. Build again with `-DNFSMW_PGO=usar`.

[toolchain.md](toolchain.md) has the details and the traps.

## 7. Build the shader library

The game's [shaders](glossary.md#shader) are Xbox 360 [microcode](glossary.md#microcode) inside the disc files.
`nfsmw_shaders.nfsp` has them translated to [SPIR-V](glossary.md#spir-v) (see [shaders.md](shaders.md)). The
installer page builds it in the browser from the disc; you only need this step to build it on your PC.

```sh
MESA=/path/to/mesa-switch shaders/nfsmw_regenerar_biblioteca_pcf.sh out/library /path/to/extracted/containers
```

The containers (the files that hold the shaders) are taken out of the disc files by
`shaders/nfsmw_buscar_contenedores.cpp` ("find containers").

## 8. Copy it to the SD card

The NRO expects this layout, the same one the installer page makes:

```
sdmc:/switch/nfsmw-nx/
    nfsmw-nx.nro
    nfsmw.toml
    nfsmw_shaders.nfsp
    game_root/          the files of the disc
```

`nfsmw.toml` has the settings read at startup ([cvars](glossary.md#cvar)). The one the installer page gives players is
`release/nfsmw.toml` in the [installer's repository](https://github.com/StevensND/nfsmw-nx-installer).

Start the NRO from the Homebrew Menu in [title takeover](glossary.md#title-takeover) mode (hold R while you start a
game), or with a [forwarder](glossary.md#forwarder). Started from the album, it does not get enough memory.

## Other editions

Every `default.xex` is a different program, so every edition needs its own build:

- `tools/editions/crear_arbol.py` ("create tree") makes a copy of the app, `app_<edition>`, with all of our addresses
  translated to that edition.
- `tools/editions/build_edition.ps1` builds it from the same folder paths as the main one, so the PGO profile matches.

[editions.md](editions.md) explains it step by step.
