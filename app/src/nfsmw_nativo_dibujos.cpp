// nfsmw - native renderer, steps C3, C4, C5c and C6 (see nfsmw_nativo_dibujos.h).
//
// What it covers
//   - Shaders: modules of the NFSSPV library with the XenosRecomp interface
//     (shader_common.h and shader_recompiler.cpp): push constants with three
//     buffer addresses (VS, PS and shared constants), 2D, 3D and cubemap
//     textures in sets 0-2 and samplers in set 3, with no size limit;
//     specialization constant 0 with R11G11B10 normals (bit 0) and alpha test (bit 1).
//   - Vertices: the input comes from the fetches of the VS patched by D3D, each
//     element found by its destination register (C5b). Guest data is converted
//     to host words with the fetch constant's byte order, like the emulation
//     (spirv_translator_fetch.cpp), and uploaded to the frame buffer.
//   - Indices: VGT_DMA_BASE/SIZE or automatic, with VGT_INDX_OFFSET. Quad lists
//     become triangles v0 v1 v2 / v0 v2 v3 (primitive_processor.cpp).
//   - 2D textures by fetch constant: linear or tiled, formats 8, 8_8, 8888,
//     2_10_10_10, 16, 16_16, 16F, 32F, DXT1/3/5, DXT5A and DXN, with the fetch
//     constant's swizzle in the view. C2's resolved textures are sampled as they
//     are. The content is compared once per frame. The game's mip levels are
//     included (nfsmw_nativo_mipmaps), packed as on the Xbox 360; without them,
//     foliage and asphalt looked grainy in the distance.
//   - State: blending, color mask, depth, stencil, culling, viewport (without the
//     half pixel: the shader adds it, g_HalfPixelOffset) and window scissor.
//   - Clipping disabled (draws in pixels, like the videos): viewport the size of
//     the render target and the transform in the VS with g_NdcScale/g_NdcOffset,
//     which only the library regenerated with the NFSMW XenosRecomp has.
//
// Not covered (rejected or substituted, with the cause logged once)
//   Mips of 3D textures, vertex textures, signed or gamma textures, points,
//   rectangle lists, line loops and vertex formats without a direct Vulkan
//   equivalent.

#include "nfsmw_nativo_dibujos.h"
#include "nfsmw_esperas_tiron.h"

#include "nfsmw_nativo_vertices_dedupe.h"
#include "nfsmw_nativo_texturas_pool.h"
#include "nfsmw_texturas_bc.h"
#include "nfsmw_nativo_sincronizacion.h"
#include "nfsmw_spirv_vulkan11.h"

#include "nfsmw_ajustes_graficos.h"
#if __has_include("nfsmw_nativo_resplandor_energia_spirv.h") && __has_include("nfsmw_nativo_resplandor_suave_spirv.h")
#include "nfsmw_nativo_resplandor_energia_spirv.h"
#include "nfsmw_nativo_resplandor_suave_spirv.h"
#else
// The two variants of the glow bright pass are derived from a game shader and are not distributed with the
// sources. Without them the glow setting keeps the game's own shader.
#define NFSMW_SIN_VARIANTES_RESPLANDOR 1
static const uint32_t kSpirvResplandorEnergia[1] = {0};
static const uint32_t kSpirvResplandorSuave[1] = {0};
#endif
#include "nfsmw_nativo_shaders.h"
#include "nfsmw_nativo_ganchos.h"  // nfsmw_d3d_vegetacion_juego
#include "nfsmw_reflejo_demanda.h"  // nfsmw_reflejo_visibilidad
#include "nfsmw_shader_library.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/ui/vulkan/util.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <fstream>
#include <set>
#include <string>
#include <system_error>  // the bind thread
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). The method it picks for GCC (1) reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the write of the data
 * being hashed (strict aliasing). The texture key read claves[4] before writing it, and the same texture
 * was created several times. Same hash values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h ya se ha incluido con su implementacion antes de este punto: XXH_FORCE_MEMORY_ACCESS 0 llegaria tarde"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

#if REX_PLATFORM_SWITCH
// Only for RexSwitchSetCurrentThreadPriorityOk (texture bind thread). That header deliberately does not
// include switch.h (same as in nfsmw_nativo_sistema.cpp).
#include "../../sdk/src/core/threading_switch.h"
#endif

/*
 * p_000139, the final composite, is a single full-screen quad with twelve texture samples: 2.8-3.2 real
 * ms, 72-80 % of all post-processing. Seven of those twelve are the radial speed blur. Turning it off
 * removes them and leaves the center tap.
 */
/*
 * Vegetation in the shadow map.
 *
 * The four alpha-tested pixel shaders of the shadow pass (n36, n68, n99, n103) are trees, bushes and wire
 * fences. Measured on PC with an A/B test: they are 55 % of the pass's draws but only 12 % of its
 * triangles.
 *
 * The pass cost follows the triangle count, and removing them is worth -1.2 ms of GPU but -3.1 ms of CPU,
 * which is also at 96 % of a core.
 *
 * The right constant is 0.556 real ms per 10,000 triangles, not the 0.80 first estimated (fit over 17
 * race intervals with a constant open area, r2 = 0.982; see nfsmw_recorte_sombras.cpp). With it, 12 % of
 * the triangles would be -0.44 ms, so about -0.76 ms of the -1.2 ms measured here is not geometry: it is
 * the vegetation's alpha pixel shader, which in the shadow map runs in full only to produce the cutout.
 *
 * What is lost: trees and fences stop casting shadows. Buildings, cars and the road keep theirs. It shows
 * on a forest track.
 */
/*
 * Enabled by default. It used to be enabled only by a line in a local toml, and the toml overrides the
 * default: shipping without that line would silently lose the improvement. Three cvars were found in
 * that state; one of them was worth 2 ms and had been inactive for six builds.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_mosaico_rapido, true, "NFSMW",
                    "Renderizador nativo (24/09, build 167): desenmosaicado de texturas de 16 en 16 bytes con el "
                    "calculo de la fila hecho una vez. Los primeros 2000 niveles se comprueban contra el de siempre "
                    "y, si uno difiere, se apaga solo. false = el de siempre, bloque a bloque");

/*
 * The FramebufferDe cache forgets destroyed views. It is keyed by handle values (render pass, 5 views
 * and size) and used to release nothing until the destructor. When C2 destroys an image (ObtenerResuelta
 * recreates it with another size or format at the same address, and those textures end up as render
 * targets through image swaps), its framebuffers stayed behind and, if Vulkan gave the same handle to a
 * new view, a framebuffer created on the dead view was returned. Destruir now notifies through
 * OlvidarVista and the framebuffers that use the view are destroyed. false = previous behaviour (nothing
 * is forgotten).
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_framebuffers_olvidan_vistas, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): al destruir una imagen de C2 se destruyen los "
                    "framebuffers que usan su vista, para que un handle reutilizado no devuelva uno viejo. false = "
                    "como antes");

REXCVAR_DEFINE_INT32(nfsmw_nativo_huellas_kb_fotograma, 6144, "NFSMW",
                     "Renderizador nativo (24/09, build 165): KB de texturas ESTABLES que el anillo vuelve a "
                     "comprobar como mucho en un fotograma; las que no caben esperan 1-2 fotogramas (8 veces "
                     "seguidas como mucho). Reparte las comprobaciones que coincidian en el mismo fotograma. "
                     "0 = sin limite, como antes");

/*
 * Sampled recheck of stable textures. See PrepararTextura and HuellaMuestra.
 * Enabled by default: its guard verifies itself and switches off at the first mismatch.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_huellas_muestreo, 8, "NFSMW",
                     "Renderizador nativo (25/09, build 170): las texturas ESTABLES se recomprueban con la huella de "
                     "una muestra (primer y ultimo bloque de 4 KB y 1 de cada 8, base y mips) en vez de con todos sus "
                     "bytes; si la muestra cambia se sigue por el camino completo. N = 1 de cada N recomprobaciones de "
                     "cada textura sigue siendo completa, y las 3000 primeras hacen las dos huellas: con un solo "
                     "desacuerdo se apaga solo. 0 = siempre la huella completa, como antes")
    .range(0, 64);

REXCVAR_DEFINE_BOOL(nfsmw_sombras_sin_vegetacion, true, "NFSMW",
                    "No dibujar en el mapa de sombras lo que lleva prueba de alfa: arboles, arbustos y "
                    "vallas de alambre. Son el 55 % de los dibujos del pase y el 12 % de sus triangulos, "
                    "asi que ahorra poca GPU (-1,2 ms) y bastante CPU (-3,1 ms). Los arboles dejan de "
                    "proyectar sombra");

/*
 * Radial blur of the final composite, removed by default (true).
 *
 * The Xbox 360 blurs the screen edges at speed (visible at 200 km/h in a capture of the 360 version),
 * and without it the port looks sharper. Restoring it was measured in a race: with the blur and the
 * 9-sample PCF the GPU went from 25.1 to 27.3 real ms per frame and became the bottleneck again, with
 * twice as many frames of 36 ms or more (4.9 % vs 9.6 %) and no streak above 40 FPS. The difference is
 * hard to notice next to the 360 footage, so it stays removed. false = keep the blur, as on the Xbox 360.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_sin_desenfoque, true, "NFSMW",
                    "Quitar el desenfoque radial de la composicion final. Son 7 de los 12 muestreos del "
                    "unico cuadrilatero a pantalla completa del posproceso (1,5-2,0 ms reales). Se pierde "
                    "el difuminado de los bordes al acelerar y con el NOS: la imagen queda mas nitida");

REXCVAR_DEFINE_BOOL(nfsmw_nativo_omitir_sombras, false, "NFSMW",
                    "Renderizador nativo (prueba de FPS): no graba los dibujos del mapa de sombras "
                    "(destino solo de profundidad de 1600 o mas de pitch); recorte visible");
// Enabled by default. On the console (A and B alternating every 30 s), the UBO intervals run 18-23 %
// faster than the neighbouring pointer intervals at the same draw load.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_simular_vulkan11, false, "NFSMW",
                    "Prueba: el renderizador nativo actua como en un controlador Vulkan 1.1 sin punteros de 64 bits "
                    "(SPIR-V de la biblioteca convertido a 1.3, sin buffer device address)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_constantes_ubo, true, "NFSMW",
                    "Renderizador nativo: los shaders leen sus constantes de UBO dinamicos (banco de constantes en "
                    "Maxwell) en vez de por puntero de 64 bits. Mismos bytes: no cambia la imagen. Necesita la "
                    "biblioteca de shaders con SPEC_CONSTANT_CONSTANTES_UBO; false vuelve al puntero");
REXCVAR_DEFINE_INT32(nfsmw_nativo_constantes_ubo_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba): con N > 0 alterna las constantes entre puntero (tramos pares) y "
                     "UBO (tramos impares) cada N segundos y anota cada cambio, para comparar capturas de una escena "
                     "quieta en la misma ejecucion");
/*
 * Descriptor set 4 by differences (the work is done in NVK, see mesa/parche_nvk_set4.py).
 *
 * vkCmdBindDescriptorSets for set 4 costs 1.3-1.4 us per call in a race and happens in 70-78 % of the
 * draws ("C6 subetapas" report), and it leaves four cbuf rebinds for the Draw. Almost all of it is NVK:
 * four writes to the root table (one per word of the dynamic descriptors, even when only the low part of
 * the address changes) and an 80-slot walk that dirties every cbuf of the set even when only one offset
 * changed. With the patch, NVK sends only the words that change and rebinds only the cbufs whose
 * descriptor changes. It is exact (the GPU ends up with the same bindings), and NVK checks it against
 * what it really bound in each slot: on a DIFERENCIA it fixes that draw, switches off for the session and
 * ControlSet4 reports it in the log as an error.
 * This cvar requests or withdraws it from NVK on each submission; with an unpatched Mesa (or on PC) it
 * does nothing. false = NVK binds the whole set, as usual.
 */
// Enabled by default. Measured: 1 root-table write per bind instead of 4, although the set 4 bind still
// costs 1.31-1.45 us (1.29-1.63 before). At one point the NVK guard seemed to check no cbuf at all; that
// was a bug in its own sampling ("1 in 4,096 draws of the buffer", and no buffer reaches 4,096 draws),
// not a sign that there was nothing to check. In its first ~40 s it checked 386,271 cbufs without a
// single difference, and the Draw rebound 38-45 % fewer cbufs. The patched Mesa (p03 in
// mesa/c186_dibujo) samples with the process-wide count.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_set4_diferencias, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el conjunto 4 (constantes por UBO) por diferencias en "
                    "NVK: solo se mandan y se reenlazan los descriptores que cambian. Mismo resultado (se comprueba "
                    "sola). Necesita el Mesa con parche_nvk_set4; false = como siempre");
REXCVAR_DEFINE_INT32(nfsmw_nativo_set4_diferencias_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 184): con N > 0 alterna el conjunto 4 por diferencias "
                     "(tramos impares) y como siempre (tramos pares) cada N segundos y anota cada cambio, para "
                     "comparar C6 subetapas en la misma ejecucion. 0 = no alterna");
/*
 * The draw path in NVK (Mesa with p01-p04 and p06 from mesa/c186_dibujo).
 *
 * The ring is the bottleneck, and ~3.4 us of each draw is spent inside NVK: vkCmdDrawIndexed 1.4-1.6 us
 * per call, vkCmdBindPipeline 3-3.7 and set 4 1.3 ("C6 subetapas" report). Four exact improvements (the
 * GPU receives the same commands with the same data), each with its own guard in NVK that compares
 * against the usual path for the first 20,000 uses and then 1 in 1,024, and switches off on a DIFERENCIA:
 *   - emission: each command written in one go (Draw, cbuf rebinds, root table, BIND_VB), same bytes;
 *   - cbufs: only the slots that can get dirty, and no cbuf flush when nothing is dirty;
 *   - dynamic: only the dynamic state groups with dirty bits;
 *   - prefetch: cache hints (PRFM) for the pipeline and shaders before they are used; the app also hands
 *     the pipeline to NVK as soon as it knows it (after PipelineDe), several us before vkCmdBindPipeline.
 * Estimated for the four together: 0.6-1.5 us per draw (~1.1-2.8 ms of ring time per frame). The
 * per-part measurement (CNTPCT clock, 1 in N calls) gives the real breakdown in "C6 NVK por partes".
 * With an unpatched Mesa (or on PC) none of this does anything.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_nvk_medir, 64, "NFSMW",
                     "Renderizador nativo (26/09, build 186): mide por partes dentro de NVK 1 de cada N llamadas "
                     "(potencia de 2; C6 NVK por partes cada 10 s). 0 = no mide")
    .range(0, 4096);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_nvk_medir_fallos, false, "NFSMW",
                    "Renderizador nativo (prueba, build 186): lee antes lo que va a usar el enlace de pipeline y el "
                    "flush de shaders y lo mide aparte (cuanto son fallos de cache). Hace trabajo de mas: solo para "
                    "medir");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_nvk_emision, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186): NVK escribe cada orden del dibujo de una vez, con los "
                    "mismos bytes (se comprueba sola). false = como siempre");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_nvk_cbufs, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186): NVK solo mira las casillas de cbufs que pueden ensuciarse "
                    "y se salta el flush de cbufs si no hay nada sucio (se comprueba sola). false = como siempre");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_nvk_dinamico, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186): NVK solo emite los grupos de estado dinamico con bits "
                    "sucios (se comprueba sola). false = como siempre");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_nvk_precarga, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186): pistas de cache (PRFM) del pipeline y de los shaders antes "
                    "de usarlos, y el pipeline pedido tras PipelineDe. No cambia nada. false = sin pistas");
REXCVAR_DEFINE_INT32(nfsmw_nativo_nvk_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 186): con N > 0 alterna las cuatro mejoras de NVK (tramos "
                     "impares encendidas, pares apagadas) cada N segundos, para comparar en la misma ejecucion. "
                     "0 = no alterna");

#if REX_PLATFORM_SWITCH
/*
 * The state of set 4 by differences lives in NVK (struct nvk_switch_set4 in nvk_cmd_buffer.h,
 * mesa/parche_nvk_set4.py). Same field order and types, with a version: it is a contract. Weak symbol:
 * with an unpatched Mesa the address is null and ControlSet4 reports it once.
 */
extern "C" {
struct NvkSwitchSet4Cuentas {
  uint64_t enlaces_diferencia;
  uint64_t enlaces_completos;
  uint64_t escrituras_raiz;
  uint64_t dwords_raiz;
  uint64_t cbufs_sucios;
  uint64_t cbufs_ahorrados;
  uint64_t dibujos;
  uint64_t comprobaciones;
  uint64_t diferencias;
};
struct NvkSwitchSet4 {
  int32_t version;
  int32_t pedido;
  int32_t entorno;
  int32_t apagado;
  NvkSwitchSet4Cuentas total;
};
extern NvkSwitchSet4 nvk_switch_set4 __attribute__((weak));
}
static_assert(sizeof(NvkSwitchSet4) == 16 + 9 * 8, "NvkSwitchSet4 tiene que medir lo mismo que en NVK");
#endif
#if REX_PLATFORM_SWITCH
/*
 * The draw path in NVK (struct nvk_switch_dibujo in nvk_cmd_buffer.h, p01 in mesa/c186_dibujo). Same
 * field order and types, with a version: it is a contract. Weak symbols: with an unpatched Mesa the
 * addresses are null and ControlDibujoNvk reports it once.
 */
extern "C" {
struct NvkSwParte {
  uint64_t veces;
  uint64_t ticks;  // CNTPCT_EL0, a ticks_por_segundo
};
struct NvkSwMejora {
  int32_t pedido;  // written by the app: 1 yes, 0 no
  int32_t apagado;  // 1: its guard saw a DIFERENCIA
  uint64_t usos;
  uint64_t validadas;
  uint64_t diferencias;
  uint64_t sin_comprobar;
};
struct NvkSwitchDibujo {
  int32_t version;
  int32_t medir;
  int32_t medir_fallos;
  int32_t entorno;
  uint64_t ticks_por_segundo;
  NvkSwParte partes[16];   // enum nvk_sw_parte
  uint64_t cuentas[13];    // enum nvk_sw_cuenta
  NvkSwMejora mejoras[5];  // emission, cbufs, dynamic, set4_rapido (not applied), prefetch
};
extern NvkSwitchDibujo nvk_switch_dibujo __attribute__((weak));
void vk_switch_precargar_pipeline(VkPipeline pipeline) __attribute__((weak));
}
static_assert(sizeof(NvkSwitchDibujo) == 24 + 16 * 16 + 13 * 8 + 5 * 40, "NvkSwitchDibujo: el mismo tamano que en NVK");
#endif
/*
 * Sampler caches valid across frames.
 *
 * With three work slots, this cache once turned the race leaderboard (names and distances) into a smear;
 * with two slots it did not show, and disabling the cache with three slots made the text perfect again.
 *
 * The cause: two paths set no validity horizon at all. Resolved textures (a render target read back as a
 * texture) set `valido_hasta = UINT64_MAX`, "valid forever while the generation does not change", and
 * the race leaderboard is exactly that: a render target the game rewrites every frame. With three work
 * slots the CPU runs two frames ahead and the descriptor slot of an old view was reused, hence the smear.
 * With two slots the distance was not enough for it to show.
 *
 * Fixed by setting `valido_hasta = fotograma_` on those two paths (see the comments in PrepararTextura).
 * The other paths were already tied to `textura.siguiente`, the same policy that decides when the content
 * is rechecked: nothing that was there before is relaxed.
 *
 * What it buys: the cold path of the `texturas` stage costs 13.5 us and runs 266 times per frame =
 * 3.6 ms. The hot path (2,893 hits) is indistinguishable from zero. With the cache valid across frames,
 * the cold ones drop to those expiring in that frame (~45-70), i.e. 2.6-3.0 ms.
 *
 * What to check, in motion (not paused): the race leaderboard with names and distances, the HUD and the
 * rear-view mirror. If anything looks blurry or delayed, disable this cvar in the toml.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_cache_texturas_entre_fotogramas, true, "NFSMW",
                    "Renderizador nativo: las caches de samplers valen entre fotogramas (build 128) mientras no toque "
                    "comprobar el contenido de la textura. false: caducan en cada fotograma, como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Uploads the game's mip levels. With only the base level of each texture, distant surfaces looked grainy
// compared with the Xbox 360. This also fixes the base level of small textures with packed mips, which
// does not start at the base address.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_texturas_bc_cpu, false, "NFSMW",
                    "Forzar conversion BC1-5 en CPU para probar la ruta de GPU sin texturas BC")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_nativo_mipmaps, true, "NFSMW",
                    "Renderizador nativo: sube los niveles de mip que trae el juego (como en la Xbox 360). false: solo "
                    "el nivel base, como antes de la build 136")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// A mip level read from the wrong place (packed tail offset, row or slice alignment) causes no Vulkan
// errors, only smudges in the distance. The game's mips are reductions of the base level, so their
// average color has to resemble the base level's.
// The texture cache used to release nothing. On the console (8 laps) it went from 62 MB at the start of
// the race to 392 MB after 12 minutes without levelling off (2,578 textures); with mipmaps it grows 25 %
// faster. Above the limit, the textures unused for the longest are evicted.
// 384 MB was once too much for the console: the cache reached 313 MB after 14 minutes and the GPU ran out
// of memory before getting near the limit: nvMapCreate failed even for 64 KB and the screen went black
// with the audio still playing. The limit was then lowered to 192 MB, to leave room for the render
// targets, guest memory and the rest.
/*
 * 192 -> 384, based on the GPU memory budget report.
 *
 * On the console: "monton 0 (GPU): 478 MB usados de 1375 MB presupuestados (tamano 2391 MB); la cache
 * de texturas lleva 150 MB de 192". So more does fit: almost 900 MB of the budget is untouched.
 *
 * And the 192 limit was doing harm: "cache de texturas cerca del limite: se sueltan frias a goteo (200
 * en total)" in every report, with 53 MB of texture uploads every 10 s. That is evicting textures only
 * to upload them again right away, and each re-upload is a spike on the ring thread. It matches the
 * stutters: 3.5-5 % of the frames exceed 50 ms.
 *
 * The reason for lowering it to 192 still stands (the GPU ran out of memory after 14 minutes with the
 * cache at 313 MB), but that was with 384 and without the emergency path that exists now (stop the GPU,
 * release half the cache and retry). With 478 of 1375 MB used, 384 leaves a 700 MB margin.
 */
/* 384 -> 512 on the Switch, with nfsmw_resolucion_interna = "automatico" (720p handheld and 1080p docked).
 * In races at 1024x576 the cache already reached 272-281 MB, and at 720p and 1080p the resolved render targets
 * are larger, so the resolution and the limit go up together: raising only the resolution once filled the cache,
 * and releasing textures to upload them again caused stutters. GPU memory on the console: 514 MB used of
 * 1,492 MB budgeted. nfsmw.toml has the same value. */
#if REX_PLATFORM_SWITCH
constexpr int32_t kTexturasMbMaxPorDefecto = 512;
#else
constexpr int32_t kTexturasMbMaxPorDefecto = 384;
#endif
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_mb_max, kTexturasMbMaxPorDefecto, "NFSMW",
                     "Renderizador nativo: MB de texturas a partir de los que se sueltan las que llevan mas tiempo sin "
                     "usarse (al menos 120 fotogramas), hasta bajar al 75 %. Si el juego las vuelve a pedir, se suben "
                     "otra vez. 0 = sin limite, como antes de la build 144")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Creating a new texture without stalling the ring thread.
 *
 * With the memory pool, creating a texture no longer asks the system for memory, but vkBindImageMemory
 * still makes two ioctls: the plane's VA with its pte_kind and the mapping of the pool chunk
 * (nvk_image.c:1696-1710; all our textures are tiled with pte_kind GENERIC_16BX2,
 * nil/image.rs:439-440). In a race that is ~0.6 ms of wall time per texture, and entering a new zone
 * brings 33-45 at once: 19-27 ms of stalled ring in that frame.
 * With this, the ring does the CPU part and the bind thread does vkBindImageMemory while the ring carries
 * on with the draws; before closing the upload buffer it waits for whatever is missing and records the
 * barrier and the copy in that same buffer. The GPU receives the same thing in the same submission: no
 * placeholder textures and no lower mips.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_texturas_enlace_hilo, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el vkBindImageMemory de las texturas nuevas (reserva de "
                    "direccion y mapeo en Horizon, ~0,6 ms cada una) va en un hilo aparte y el anillo sigue grabando; "
                    "antes de enviar se espera lo que falte. Mismo resultado (se comprueba sola). false = en el anillo")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_enlace_hilo_prioridad, 0x2D, "NFSMW",
                     "Renderizador nativo (build 184): prioridad del hilo de enlaces de texturas. 0x2D, la del anillo: "
                     "por encima del invitado (0x3B) para que un ioctl terminado no espere nucleo, y fuera de las "
                     "franjas del audio (0x2B) y de la presentacion (0x2C). Casi todo el tiempo duerme en el ioctl")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Hashing and untiling of new textures, off the ring thread.
 *
 * During zone-change stutters the ring spends 5.3-8.7 ms of the frame on new textures: the XXH3 of guest
 * memory (~37 %), untiling and byte swapping (~63 %, 1.8-3.1 ms per MB), plus the copy to the upload
 * buffer. With this, the ring only copies the bytes the hash covers (the same memory at the same point as
 * the inline path: a snapshot) and a thread does the hash, the untiling, the byte swap and the copy to the
 * upload buffer on that copy. Before submitting, the ring collects whatever is missing and does itself
 * whatever the thread has not started. The barrier and the copy are recorded where they always are: the
 * GPU receives the same bytes in the same submission, with no placeholder textures and no frame of delay.
 * It verifies itself (observing phase, then 1 in 128) and switches off with REXLOG_ERROR on any
 * DIFERENCIA. false = everything on the ring, as before.
 */
// Disabled by default. Its guard tripped at start-up (3 jobs not collected when switching upload buffers)
// and switched it off; also, analysis showed that texture creation causes no race stutter (56 us per
// texture on the ring) and that the zone-change stutter is limited by the game itself.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_texturas_huella_hilo, false, "NFSMW",
                    "Renderizador nativo (26/09, build 185): la huella (XXH3), el desenmosaicado y el orden de bytes de "
                    "las texturas nuevas los hace un hilo aparte sobre una copia de la memoria del invitado tomada en el "
                    "anillo; antes de enviar se recoge lo que falte. Mismo resultado (se comprueba sola). false = en el "
                    "anillo, como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_huella_hilo_prioridad, 0x2E, "NFSMW",
                     "Renderizador nativo (build 185): prioridad del hilo de huellas de texturas. 0x2E: por debajo del "
                     "anillo (0x2D), que lo desaloja en cuanto tiene trabajo, y por encima del invitado (0x3B), que asi "
                     "no lo deja sin nucleo; fuera del audio (0x2B) y de la presentacion (0x2C)")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_huella_hilo_nucleo, -2, "NFSMW",
                     "Renderizador nativo (build 185): nucleo preferido del hilo de huellas. -2 = automatico: el 2, o el "
                     "1 si el anillo corre en el 2 (en el nucleo del anillo, ocupado, casi no correria); -1 = el de por "
                     "defecto del proceso; 0-2 = uno concreto. No es exclusivo")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_huella_hilo_mb, 16, "NFSMW",
                     "Renderizador nativo (build 185): MB para las copias de la memoria del invitado de las texturas "
                     "nuevas de un envio. Lo que no cabe se prepara en el anillo, como antes")
    .range(4, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Measurement only: new textures whose content repeats a live one. See AnotarContenidoTextura. It changes
 * no decision of the ring: it only counts, and reports every 10 s in the "C3 reutilizar por contenido"
 * line.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_reutilizar, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186, solo medida): cuenta cuantas texturas nuevas tienen el "
                    "mismo contenido y forma que otra viva de la cache (el juego recarga los packs en otra direccion) "
                    "y cuantas de esas otras llevan mas de 120 fotogramas sin usarse. No cambia nada. false = sin "
                    "contar")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Pipeline prewarming. A pipeline that is not in the Vulkan cache is compiled on the fly on the ring: 68-159 ms
 * each on the console (56 of them in the first race, 5.2 s of stutter). It happens the first time after any
 * change to the shader library, the driver or the key, and on a fresh install. See BuclePrecalentado.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pipelines_precalentar, true, "NFSMW",
                    "Renderizador nativo (26/09, build 186): al arrancar, un hilo de la prioridad mas baja vuelve a "
                    "crear en la cache de Vulkan los pipelines que el anillo creo en partidas anteriores "
                    "(su lista va en cache/nfsmw_nativo_pipelines.bin), con la misma funcion que el anillo, y los destruye: cuando el "
                    "anillo los pide ya estan compilados (sin los tirones de 70-160 ms por pipeline de la primera "
                    "carrera tras un cambio). No cambia ningun pipeline ni ningun dibujo. false = sin precalentar (la "
                    "lista se sigue guardando)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Testing only. Pretends the GPU runs out of memory on one in N texture allocations, to check that the
// emergency path (stop the GPU, release half the cache and retry) really works. 0 does nothing, which is the
// normal setting.
REXCVAR_DEFINE_INT32(nfsmw_nativo_prueba_sin_memoria_cada, 0, "NFSMW",
                     "Renderizador nativo (solo pruebas): finge que falta memoria en la GPU una de cada N reservas "
                     "de textura, para ejercitar la recuperacion. 0 = apagado")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_mips, false, "NFSMW",
                    "Renderizador nativo (diagnostico): compara el color medio de cada nivel de mip con el de la base en "
                    "las texturas DXT1/3/5 y 8888 y anota las que no se parecen (un nivel leido de otro sitio). No "
                    "cambia la imagen")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_prueba_depth_clamp, false, "NFSMW",
                    "Renderizador nativo (solo pruebas, 17/09 build 149): depthClampEnable en todos los pipelines. Imita "
                    "lo que parece hacer NVK en la consola con lo que queda detras del plano lejano (el sol de las "
                    "consultas de oclusion de la escena)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_resplandor_suave_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 150): con N > 0 va rotando el resplandor del cielo original, "
                     "natural y suave (nfsmw_resplandor_cielo) cada N segundos y anota cada cambio, para comparar "
                     "capturas del mismo sitio");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_prueba_oclusion_siempre, false, "NFSMW",
                    "Renderizador nativo (solo pruebas, 17/09 build 150): los dibujos sin color de una consulta de "
                    "oclusion en un destino de 640 o mas pasan la prueba de profundidad siempre. Separa un sol que no "
                    "cubre pixeles de uno tapado por la profundidad. Cambia lo que ve el juego")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * This test has been answered: there is nothing to gain.
 *
 * A fit over 17 race intervals gives, for the shadow pass, raw ms = 0.0342 x thousands of triangles + 0.023
 * (r2 = 0.982). The intercept is 0.037 real ms: with both 1600x1600 maps open (5.12 Mtexels, i.e. 20.5 MB of
 * loadOp = LOAD per frame) and zero triangles, the pass costs nothing measurable. So on this GPU the
 * loadOp = LOAD of a depth attachment is not paid for: Maxwell does not load the tile up front like a tiled
 * GPU, it reads on demand. The theory of "130-165 MB per frame moved for nothing" further below does not
 * hold for the shadow map depth.
 *
 * On ZCULL: in NVK (nvk_cmd_draw.c, `use_zcull`) a pass with loadOp = CLEAR enables ZCULL even when the
 * image has no plane (ephemeral, without LOAD/STORE between passes, which a map drawn whole every time does
 * not need), while DONT_CARE, which this cvar uses, does not: the condition is
 * `zcull_plane || loadOp == CLEAR`, and DONT_CARE is neither. CLEAR has the same visual risk as DONT_CARE and
 * leaves the map at 1.0 (far) instead of garbage, so it is the better of the two. It still does not pay off:
 * the ZCULL ceiling here is those 0.037 ms, because the pass is pure geometry. For ZCULL to cover the map,
 * the renderArea has to span the full 1600x1600: the ZCULL region comes from render->area, not from the
 * image size.
 */
/*
 * Enabled by default. It used to be enabled only by a line in a local toml, and the toml overrides the
 * default: shipping without that line would silently lose the improvement. Three cvars were found in
 * that state; one of them was worth 2 ms and had been inactive for six builds.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pase_sombras_sin_load, true, "NFSMW",
                    "Renderizador nativo (prueba de FPS): abre el pase del mapa de sombras sin cargar su "
                    "contenido previo (loadOp = DONT_CARE). Medido en la 112: no hay nada que ganar, el "
                    "loadOp de la profundidad no se paga en esta GPU (el pase con 0 triangulos cuesta "
                    "0,037 ms)");
REXCVAR_DEFINE_INT32(nfsmw_nativo_omitir_sombras_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba): con N > 0 omite las sombras en los tramos impares "
                     "de N segundos y anota cada cambio, para comparar capturas del mismo sitio");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_vertices_repetidos, false, "NFSMW",
                    "Renderizador nativo (diagnostico): cuenta los bytes de vertices que repiten "
                    "direccion, tamano y contenido en el mismo fotograma o respecto a uno anterior");
// On the console, the vertex copy was ~3 of the ring thread's ~9 us per draw ("subidas" stage), and with the
// game's busy-waits removed there are free cores.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_subidas_hilo, true, "NFSMW",
                    "Renderizador nativo: las copias de vertices al bufer de subida las hace un hilo aparte "
                    "mientras el hilo del anillo sigue grabando; se esperan antes de enviar el trabajo a la "
                    "GPU y antes de devolver el puntero de lectura al juego")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The ring no longer waits for a copy thread that has no core (see EsperarSubidas).
 *
 * In a race, the ring waited 14.7 ms for the copy thread during an alley stutter (3,965 draws) and 17.9 ms in
 * the next one, and the game spent 21.1 ms without room in the ring. The thread ran at 0x3B, below the two
 * game threads (0x3A), and with the CPU at 278 % out of 300 it sat ready without a core: of those ms, only
 * ~6-7 were copying.
 */
/*
 * Copy thread priority. It used to run at its creation priority (0x3B, below the two game threads at 0x3A)
 * and, with the ring's help path (nfsmw_nativo_subidas_ayuda), the ring always reached the copies first: the
 * thread copied 0 MB and the ring 100 % (1.4-1.8 GB, ~1 s every 10 s: ~3 ms per frame on the thread that is
 * the bottleneck). At 0x2E, like the hash thread, it takes the copies as soon as they are queued, on another
 * core; the ring (0x2D) preempts it if they share a core, and the help path only handles what is left when
 * waiting.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_subidas_hilo_prioridad, 0x2E, "NFSMW",
                     "Renderizador nativo (26/09, build 187): prioridad del hilo de copias de vertices. 0x2E: por "
                     "debajo del anillo (0x2D) y por encima del invitado (0x3A-0x3B), para que copie mientras el "
                     "anillo graba. 0x3B = como hasta la 186")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The copy thread on a different core from the ring. At the Heritage & Omega exit the thread only did 20 % of
 * the copies: the ring, with higher priority (0x2D) and at 94 %, did not leave it its core, and did them
 * itself while waiting (2.5-5 ms per frame). With its preferred core elsewhere, the thread takes CPU from the
 * game threads (0x3A, lower priority), which spend 97 % of their time waiting for the ring. Only the preferred
 * core is set: the affinity mask is untouched and the kernel can still move it (same as the hash thread).
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_subidas_hilo_nucleo, -2, "NFSMW",
                     "Renderizador nativo (26/09, build 191): nucleo preferido del hilo de copias de vertices. -2 = uno "
                     "distinto del del anillo; -1 = sin preferencia (como hasta la 190); 0-2 = ese")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * 16-bit indices with hand-written NEON (IndicesDe16). At one point the compiler stopped vectorizing that
 * loop (0 vector rev16 in Dibujar, 7 before) and the "indices" substage rose to 2.95 us per draw (the whole
 * stage had been 1.2-1.3 us): ~3 ms of ring time per frame. Same results (the same indices in the same order,
 * the same minimum and maximum). Guard: the first 20,000 draws, then 1 in 4,096, are compared with the plain
 * loop; on a DIFERENCIA the plain loop is kept for the session.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_indices_neon, true, "NFSMW",
                    "Renderizador nativo (26/09, build 187): los indices de 16 bits se copian y se miden (minimo y "
                    "maximo) con NEON, 16 por vuelta. Mismo resultado (se comprueba sola). false = el bucle de siempre")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_subidas_ayuda, true, "NFSMW",
                    "Renderizador nativo (26/09, build 185): cuando el hilo del anillo tiene que esperar las copias "
                    "de vertices, copia el mismo las que el hilo de copias aun no ha empezado y espera la que este en "
                    "curso con herencia de prioridad. Mismos datos (se comprueba sola). false = espera como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_vacias_grises, false, "NFSMW",
                    "Renderizador nativo: las texturas que aun no se soportan (cubo, 3D, "
                    "formatos pendientes) se muestrean grises en vez de a cero (solo pruebas)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_inv_tamano_tex, true, "NFSMW",
                    "Renderizador nativo (18/09, build 164): los shaders toman 1/tamano de textura de las "
                    "constantes en vez de preguntarselo a la textura en cada muestreo con desplazamiento. Es "
                    "la misma cuenta con el mismo numero: la imagen no cambia. Necesita una biblioteca de "
                    "shaders regenerada con el ayudante nuevo");
/*
 * Enabled by default: a single texel instead of the 3x3, measured and long enabled in a local toml. Three
 * cvars were only enabled through a local toml, and the toml overrides the default: shipping without that
 * line would lose the improvement without anyone noticing.
 */
/*
 * The cheap PCF was briefly disabled by default because, with a single shadow map sample, walls and garage
 * doors showed diagonal bands (shadow acne) that the Xbox 360 does not have; from a distance they looked
 * blurry. The 9-sample PCF fixed nothing: the grid looked the same with 9 samples as with 1, because it was
 * shadow map acne, which the depth slope bias removes (nfsmw_nativo_sombras_sesgo_pendiente). The 9 samples
 * only cost GPU time: together with the blur, race GPU time went from 25.1 to 27.3 real ms. So the cheap
 * PCF is enabled by default again.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pcf_barato, true, "NFSMW",
                    "Renderizador nativo (20/09): un solo muestreo del mapa de sombras en vez del patron "
                    "3x3 a medio texel. El humo (p_000101) hace once por pixel en rectangulos de pantalla "
                    "completa y es el 21 % de la escena. El borde de la sombra queda algo menos suave "
                    "(25/09, build 183: true por defecto otra vez; las bandas de las paredes eran acne y las "
                    "quita el desplazamiento de profundidad). Necesita la biblioteca de shaders regenerada");

/*
 * Anisotropic filtering, beyond the Xbox 360 (nfsmw_nativo_anisotropico).
 *
 * The game requests trilinear without anisotropy on all its textures ("C4 filtros pedidos": anisotropy 0,
 * bias 0), and the 435 sampling instructions of its 74 pixel shaders do not change that (all of them "use
 * the constant", read from the original binaries). That is why, on the 360 too, surfaces seen at a grazing
 * angle or from a distance look blurry and gain detail as you get closer: the white garage door in the alley,
 * for example. With N > 1, linear samplers with mips get anisotropy N (capped by the device). Single-level
 * ones (resolved targets, shadow map, screen effects) and point samplers stay as they were. It costs GPU time
 * in the scene: measured in "C2: GPU por Swap ... escena". 0 = trilinear, as on the 360.
 */
// 0 by default. At 8x it did not remove the streaks on the alley doors, and the scene went up ~2 ms of GPU
// time together with the 9 shadow samples.
REXCVAR_DEFINE_INT32(nfsmw_nativo_anisotropico, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 179): filtrado anisotropico de las texturas del mundo "
                     "(samplers lineales con mips). 0 = como la Xbox 360 (trilineal, borroso de lado y a "
                     "distancia); 2, 4, 8 o 16 = mas nitido, con coste de GPU")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * Alley door diagnostic (nfsmw_nativo_diag_mip_minimo).
 *
 * The garage door slats and the brickwork showed streaks and ripples that the Xbox 360 does not have, and
 * they did not go away at native 720p nor with 8x anisotropy. With N > 0, textures with mips start N levels
 * lower even up close (the sampler raises its minLod). With the car stopped in front of a door: if with 1, 2
 * or 3 it looks blurry but with its slats in place, its mip levels are fine and the streaks come from
 * elsewhere (the shadow); if streaks, breaks or a pattern that is not the door's show up, that level is
 * uploaded wrong. It can be changed at runtime from the settings menu: it is part of the sampler key and,
 * when it changes, the per-texture sampler caches are invalidated. 0 = normal.
 */
/*
 * Our own depth bias in the shadow map (the acne on the alley doors).
 *
 * Garage doors and walls showed a fine grid of dots and diagonal streaks that the Xbox 360 does not have.
 * Diagnosed with captures taken standing in front of a door: without the shadow map the grid disappears;
 * with 9 samples or with 1 it looks the same; and with the minimum mip at 1, 2 or 3 the texture blurs as
 * expected but the grid stays just as sharp. It is shadow acne: the surface shadows itself. The game sets no
 * depth bias in that pass (0 "desplazamiento de profundidad" lines in every log). Our own is added only to
 * the shadow map draws, and only if the game does not set one. Too high a value detaches the shadows from the
 * objects casting them. Both values can be changed at runtime from the settings menu. 0 and 0 = previous
 * behaviour.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_sombras_sesgo_pendiente, 20, "NFSMW",
                     "Renderizador nativo (25/09, build 181): desplazamiento de profundidad por pendiente en el mapa de "
                     "sombras, en decimas (20 = 2,0). Quita el acne (rejilla de puntos en diagonal en paredes y puertas "
                     "de garaje). Demasiado alto despega las sombras de los objetos. 0 = como antes. Se cambia en marcha")
    .range(0, 100);
REXCVAR_DEFINE_INT32(nfsmw_nativo_sombras_sesgo_constante, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 181): desplazamiento de profundidad constante en el mapa de "
                     "sombras, en miles de unidades de 24 bits (1 = 1000). 0 = sin constante. Se cambia en marcha")
    .range(0, 100);

REXCVAR_DEFINE_INT32(nfsmw_nativo_diag_mip_minimo, 0, "NFSMW",
                     "Diagnostico (25/09, build 180): las texturas con mips empiezan N niveles mas abajo, tambien de "
                     "cerca, para ver si sus niveles de mip estan bien. 0 = normal; 1, 2 o 3 para mirar. Se cambia en "
                     "marcha")
    .range(0, 4);
/*
 * Splitting the frame into two submissions.
 *
 * Measured on the console: there is a single vkQueueSubmit per frame, at the end, from Presentar. The GPU
 * runs out of work from the end of one frame until the CPU sends the next, and that is ~4 real ms of idle
 * time per frame ("hueco entre trabajos", constant in a steady race).
 *
 * Submitting as soon as the shadow pass closes lets the GPU start on the shadows while the CPU records the
 * scene. It does not change a single pixel: it is the same work in two pieces.
 */
/*
 * Disabled by default. The idea above is sound but the measurement ruled it out: the gap between
 * submissions went up 3.61 ms when the frame was split in two (two vkQueueSubmit calls = twice the GPU
 * start-up latency, and the second piece cannot start until the CPU finishes recording it). The default
 * follows the measurement: what helps ships enabled and what makes things worse ships disabled. The setting
 * stays so the test can be repeated.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_enviar_tras_sombras, false, "NFSMW",
                    "Renderizador nativo (20/09): enviar el trabajo a la GPU en cuanto se cierra el pase de "
                    "sombras, en vez de todo junto al final del fotograma. La GPU deja de estar parada "
                    "esperando a que la CPU acabe de grabar");
REXCVAR_DEFINE_INT32(nfsmw_nativo_pcf_barato_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (20/09): con N > 0 alterna cada N segundos el muestreo del mapa de "
                     "sombras entre el patron 3x3 y un solo texel, y lo anota. Sirve para medirlo A/B en la "
                     "MISMA carrera: comparar entre carreras distintas no vale porque el tramo cambia");
REXCVAR_DEFINE_INT32(nfsmw_nativo_inv_tamano_tex_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 164): con N > 0 alterna cada N segundos tomar "
                     "1/tamano de las constantes o preguntarselo a la textura");
REXCVAR_DEFINE_INT32(nfsmw_nativo_estadisticas_por_dibujo_s, 0, "NFSMW",
                     "Renderizador nativo (diagnostico, build 159): con N > 0 mide un fotograma de cada N "
                     "segundos con una consulta por dibujo y reparte los fragmentos por pixel shader");
REXCVAR_DEFINE_INT32(nfsmw_nativo_prueba_tijera, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 159): recorta a 1x1 los dibujos de una categoria "
                     "(1 sombras, 2 escena, 3 cubo, 4 posproceso). Se manda todo el estado y todos los "
                     "dibujos pero no se sombrea: la diferencia de tiempo es el suelo por dibujo. ROMPE LA "
                     "IMAGEN mientras esta puesta: solo para medir");
REXCVAR_DEFINE_INT32(nfsmw_nativo_prueba_tijera_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 159): con N > 0 alterna cada N segundos la tijera "
                     "de nfsmw_nativo_prueba_tijera");
/*
 * Separating the texture unit from the ALU (scene pass analysis).
 *
 * The game requests mipmapMode LINEAR on almost every sampler (mag/min/mip filters 1/1/1). On Maxwell a
 * trilinear sample takes the TMU two cycles and a bilinear one only one, so this halves the texture unit's
 * work without removing a single ALU instruction or shader sample. It is the only clean lever to decide
 * which of the two dominates the 18.94 ms of the scene. The jump between mip levels is visible when moving
 * away: for measurement only.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_prueba_mip_puntual, false, "NFSMW",
                    "Renderizador nativo (prueba, 21/09): el filtro ENTRE niveles de mip pasa de lineal a "
                    "puntual (trilineal -> bilineal). La TMU hace la mitad de trabajo y la ALU no cambia: "
                    "si la escena baja manda el muestreo, si no se mueve manda la ALU. Se ve el salto de "
                    "mip al alejarse: solo para medir");
REXCVAR_DEFINE_INT32(nfsmw_nativo_prueba_mip_puntual_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, 21/09): con N > 0 alterna cada N segundos "
                     "nfsmw_nativo_prueba_mip_puntual (los dos samplers conviven en la cache, no hay "
                     "parones al cambiar)");
/*
 * Do not upload the same vertices twice in the same frame.
 *
 * Each draw copies the [vmin..vmax] range of each binding to the upload buffer, byte-swapped. There was no
 * cache: the same piece of geometry was copied again in full every time it was drawn. Measured on PC with
 * nfsmw_nativo_diag_vertices_repetidos:
 *     5954.9 MB copied; 2584.2 MB repeated within the same frame and 3329.2 MB equal to an earlier
 *     frame  ->  99.0-99.6 % of the bytes are byte-for-byte repeats, across eight reports.
 * On the console that is 8.4 MB of vertices per frame at 2518 MB/s into uncached memory = 3.3 ms of
 * writing alone, split between the ring and the copy thread.
 *
 * This only does the safe half: repeats within the same frame (41-49 % of the bytes). And it is exact, not a
 * gamble: on the Xbox 360 the GPU reads the vertices when it executes the draw, so the game cannot rewrite a
 * range that is already referenced without first synchronizing with the GPU. The only point where it can is
 * a guest wait (WAIT_REG_MEM), and there everything recorded is forgotten (g_sincronizaciones_anillo).
 *
 * If geometry ever looks stuck or stretched, this is the first thing to disable.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_dedupe_vertices, true, "NFSMW",
                    "Renderizador nativo (21/09): si dos dibujos del mismo fotograma piden el mismo rango de "
                    "vertices, se sube una sola vez. El informe C6 anota aciertos y MB ahorrados");

/*
 * No vkCmdBindVertexBuffers in single-binding draws.
 *
 * Each draw copies its vertices to a new spot in the upload buffer, so its binding changed almost every
 * time: in a race, 0.32-0.46 us per call in 66-98 % of the draws (0.21-0.34 us per draw). In NVK that is
 * three wrapper functions (vk_common_CmdBindVertexBuffers -> 2EXT -> 3KHR) and 5 words with an MME macro per
 * binding.
 *
 * With a single binding, the copy is allocated at a multiple of the stride, the upload buffer stays bound
 * at 0 and the draw is shifted with vertexOffset (indexed) or firstVertex (non-indexed). The GPU reads the
 * same bytes: (offset / stride + i - vmin) * stride = offset + (i - vmin) * stride. The NFSMW shaders do not
 * read SV_VertexID (XenosRecomp only declares it with UNLEASHED_RECOMP and the library is generated with
 * NFSMW_RECOMP), so the base index is not visible anywhere. NVK passes vertexOffset/firstVertex as is to
 * the draw macro on every Draw, with or without this change: recording the draw costs the same.
 *
 * These still bind as usual: draws with two or more bindings, those that reuse (dedupe) a copy that does not
 * start at a multiple of their stride, and the deferred sky, which saves and replays its own binding (and
 * when emitted leaves enlaces_grabados_ at 0, so the next draw binds at 0 again).
 *
 * Guard (ComprobarBaseCero): the first 200,000 draws on this path, then 1 in 4,096, redo the computation
 * backwards in 64 bits and check the limits. On a single difference, that draw binds as usual, DIFERENCIA
 * is written to the log and the path switches off for the session. What the guard cannot see is the GPU:
 * if anything looked wrong, nfsmw_nativo_vertices_base_cero = false restores the usual path.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_vertices_base_cero, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): en los dibujos de un solo enlace de vertices, el bufer de "
                    "subida se queda enlazado en 0 y el dibujo se desplaza con vertexOffset/firstVertex, sin un "
                    "vkCmdBindVertexBuffers por dibujo. La GPU lee los mismos bytes. false = como antes");

REXCVAR_DEFINE_BOOL(nfsmw_nativo_ps_solo_alfa, true, "NFSMW",
                    "Renderizador nativo (18/09, build 158): en las pasadas sin destino de color, compila el "
                    "pixel shader sin las escrituras de color. La imagen no cambia (Vulkan las descarta) y el "
                    "driver borra por muerto lo que solo alimentaba el color: queda la prueba de alfa");
REXCVAR_DEFINE_INT32(nfsmw_nativo_ps_solo_alfa_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 158): con N > 0 alterna cada N segundos compilar el "
                     "pixel shader con y sin las escrituras de color en las pasadas sin color");

/*
 * Depth test before shading, where it can be done without changing the image.
 *
 * The problem, measured. The scene costs 13.86 raw ms = 22.55 real ms (in a race) and it is pure shading:
 * 6.5 M fragments over 0.92 M pixels, so every screen pixel is shaded 7 times. And our own counter says
 * that 28 % of the color draws prevent early rejection: they have an alpha test or a real kill.
 *
 * Why that costs so much. A pixel shader that can discard forces the hardware to shade first and test
 * depth afterwards (late-Z): otherwise a discarded fragment would already have written its Z. So everything
 * with alpha (smoke, particles, glass, decals) is shaded in full even when it is behind a building. In NVK
 * that is literal: nvk_shader.c only sets SET_API_MANDATED_EARLY_Z when the module declares
 * EarlyFragmentTests, and our modules never declare it.
 *
 * The part that can be fixed, and why it is pixel-identical. The reason for late-Z is the Z write, not the
 * test. If the draw writes neither depth nor stencil, moving the test earlier cannot corrupt anything: there
 * is nothing extra to write. The shader still discards the color the same way. So for every draw with the
 * Z test on, Z write off and stencil off, declaring EarlyFragmentTests is free and exact, and the GPU stops
 * shading what is hidden.
 *
 * Deliberately left out: opaque alpha-tested draws (vegetation) write Z, so they are not touched; and
 * neither are draws inside a game occlusion query, because testing earlier would change the samples it
 * counts, and the game reads them.
 *
 * The "C6 Z temprana" report says how many draws are fixed and how many cannot be because they write
 * depth: that second figure is the exact size of what only a depth pre-pass would solve.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_z_temprana, true, "NFSMW",
                    "Renderizador nativo (20/09): en los dibujos que prueban la profundidad pero NO la "
                    "escriben (humo, particulas, cristales, calcomanias), declarar EarlyFragmentTests en el "
                    "pixel shader para que la GPU pruebe la profundidad ANTES de sombrear en vez de despues. "
                    "Sin esto, cualquier shader con prueba de alfa o kill sombrea todos sus fragmentos "
                    "aunque queden tapados. La imagen es identica: no hay escritura de Z que adelantar");
REXCVAR_DEFINE_INT32(nfsmw_nativo_z_temprana_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, 20/09): con N > 0 alterna cada N segundos la prueba de "
                     "profundidad temprana, para medirla A/B en la MISMA carrera");
/*
 * The sky is drawn first and shades the whole screen for nothing.
 *
 * Measured, not estimated (FRAGMENT_SHADER_INVOCATIONS counter per draw, 20 frames): the sky dome is a
 * single draw per frame, 480 indices = 160 triangles, and yet it invokes 0.83 M fragments: 90 % of the
 * screen. Its pixel shader has 5 texture samples. Cost ~4.8 real ms of the scene's 19.2 ms, 25 %.
 *
 * Why. The game draws it before the world. At that point the Z-buffer is empty, so nothing can discard it:
 * it shades the whole screen and then the world paints over it.
 *
 * Why deferring it does not change a single pixel. The draw is opaque (ONE/ZERO blending: it does not read
 * the target) and already tests depth with LEQUAL without writing it (RB_DEPTHCONTROL 00700732). So without
 * deferral, wherever the world covers it, the sky's result is overwritten; deferred, in the same place, the
 * depth test discards it before shading. The final color is the same in both cases, pixel for pixel, and
 * depth is not touched before or after because the sky does not write it.
 *
 * Where it is emitted. In the same pass, as soon as a draw arrives that it cannot be moved past (one with
 * blending, which does read the background color, or an opaque one that does not write depth, which the
 * sky would cover because the Z-buffer would not have changed), or when the pass closes, whichever comes
 * first. Every copy or resolve of the target goes through TerminarPase (nfsmw_nativo_destinos.cpp), so it
 * is emitted there too before anything reads the color.
 *
 * The only thing that could show, and how to recognize it. If the game drew something at exactly the same
 * depth as the dome (far plane z), the LEQUAL test would let the sky through and cover it, where without
 * deferral it would be the other way round. Nothing should be there (the sky is the farthest thing in the
 * scene), but if the sky is ever seen eating very distant geometry, this is why: disable the cvar.
 */
/*
 * A first version broke the image, and this was why.
 *
 * It produced flickering, badly rendered geometry and odd colors in the menus and on the car. The counter
 * said it unambiguously:
 *
 *     "C6 cielo aplazado: 7,00 detectados por fotograma, 1,00 aplazados de verdad"
 *
 * The analysis expected 0.9 draws per frame for that pixel shader. In reality there are seven: the sky dome
 * is not the only user of that shader. Deferring one of the seven breaks the order of the other six, and
 * that is where the artifacts came from.
 *
 * What was missing: the fingerprint identifies the shader, not the draw. Two draws with the same shader and
 * similar state are indistinguishable by this criterion. And the counter that would have exposed it
 * (detected per frame) shipped in the same build as the change instead of before it. The dome draw has to
 * be identified as such (by index count, 480 = 160 triangles, by being the first of the pass, and by
 * checking that it really covers the screen), and verified with the counter before anything is deferred.
 */
/*
 * Enabled again, but it no longer decides alone.
 *
 * With this cvar on, earlier versions broke the image because seven draws share the shader's fingerprint.
 * Now the self-checking guard sits above it: even with the cvar on, nothing is deferred until 90
 * consecutive frames have been seen with a single dome, and as soon as two show up it switches off for the
 * rest of the session. See kCieloFotogramasPrueba and CerrarFotogramaDeLaGuardiaDelCielo. Enabling it
 * cannot break the image; at worst it does nothing.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_cielo_aplazado, true, "NFSMW",
                    "Renderizador nativo (22/09): aplazar el dibujo del cielo hasta despues de los opacos "
                    "del mismo pase, en vez de dibujarlo el primero sobre un Z-buffer vacio. Es un solo "
                    "dibujo opaco que prueba la profundidad y no la escribe, asi que la imagen es identica: "
                    "lo que hoy se sobreescribe, manana lo descarta la prueba de profundidad");
/*
 * Draws that paint nothing and are still shaded.
 *
 * Two exact cases, both read from the draw's own registers:
 *
 *  1. Blending is "0 x source + 1 x destination" (ADD) on every written channel: the result is the
 *     destination as is. The draw cannot change a single color pixel.
 *  2. The alpha test function is 0 (NEVER): the recompiled shader always calls clip() (alphaTestValue
 *     returns -1 for case 0), so every fragment dies, and dies before writing depth.
 *
 * In both cases, if the draw also writes neither depth nor stencil, it leaves no trace of any kind and can
 * be skipped entirely. If it writes depth, only its color write is removed (mask 0), which already saves
 * the blending and the write.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_saltar_invisibles, true, "NFSMW",
                    "Renderizador nativo (20/09): saltarse los dibujos que no pueden cambiar ni un pixel "
                    "(mezcla que copia el destino, o prueba de alfa con la funcion NUNCA). La imagen es "
                    "identica por definicion");
/*
 * Small per-draw savings on the ring thread. Each has its own cvar; the first three have a self-checking
 * guard (the first 200,000 cases, then 1 in 4,096, also go through the usual path and are compared; on a
 * difference, DIFERENCIA in the log and the saving is switched off).
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_encuadre_cache, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): el viewport, el ndc y la tijera de un dibujo se reutilizan "
                    "mientras no cambien los registros del encuadre (su generacion) ni el pase. Se comprueba sola. "
                    "false = se calculan en cada dibujo, como antes");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_alto_util_memo, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): el alto util del destino se apunta en el elemento del mapa "
                    "del ultimo pitch, sin buscarlo en cada dibujo. Se comprueba sola. false = como antes");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_clave_pase_rapida, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): si los destinos de un dibujo son los mismos bytes que los "
                    "del pase abierto no se recalcula su XXH3. Se comprueba sola. false = XXH3 en cada dibujo");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_cvars_por_fotograma, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): nfsmw_nativo_ps_solo_alfa y nfsmw_nativo_sin_ps_sin_color "
                    "(y sus alternancias) se leen una vez por fotograma y no en cada dibujo sin color. false = como "
                    "antes");
REXCVAR_DEFINE_INT32(nfsmw_nativo_sin_ps_sin_color_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 157): con N > 0 alterna cada N segundos montar el "
                     "pipeline con y sin etapa de fragmentos en los dibujos sin color, para comparar capturas "
                     "del mismo sitio con el juego en pausa");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_sin_ps_sin_color, true, "NFSMW",
                    "Renderizador nativo (17/09, build 157): en los dibujos que no escriben ningun color, "
                    "monta el pipeline sin etapa de fragmentos si el pixel shader no puede descartar pixeles "
                    "ni escribe profundidad. La imagen no cambia y la GPU no sombrea (mapa de sombras)");
REXCVAR_DEFINE_STRING(nfsmw_nativo_diag_omitir_ps, "", "NFSMW",
                      "Renderizador nativo: numeros de PS separados por comas cuyos dibujos no "
                      "se graban (solo pruebas, para localizar un dibujo)");
// A review of other Switch projects found that in NVK for Tegra the memory type the SDK picks for uploads
// (without HOST_CACHED) is an NvMap without CPU caching, and wine-nx measured on the console that writing
// there is slow. All vertex copies go to this buffer, so it can be chosen for an A/B test.
REXCVAR_DEFINE_INT32(nfsmw_nativo_subida_memoria, 0, "NFSMW",
                     "Renderizador nativo: memoria del bufer de subida. 0 = la que elige el SDK (en la Switch, sin "
                     "cache de CPU), 1 = con cache de CPU (se publica con vkFlushMappedMemoryRanges antes de "
                     "enviar), 2 = sin cache de CPU")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_compartidas_cache, true, "NFSMW",
                    "Renderizador nativo (23/09, build 156): las constantes compartidas de cada dibujo (488 bytes) "
                    "en un bufer pequeno CON cache de CPU, aparte del de subida. Medido en la 155: escribirlas sin "
                    "cache costaba 2,0 us por dibujo y con cache 0,4. Solo con las constantes por UBO")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Direct-mapped pipeline cache.
 *
 * PipelineDe costs 0.52-0.76 us per draw in a race ("C6 subetapas"), more than BindVertexBuffers. The
 * one-entry shortcut misses on ~1 in 3 draws (the same ones that then call BindPipeline), and each miss is
 * an XXH3 of 80 bytes, a division by libstdc++'s prime bucket count and 2-3 jumps to nodes scattered across
 * the heap, which with the ring streaming megabytes through the cache almost always miss all the way to
 * memory: ~1.3 us per miss.
 *
 * In front of the map sits a table of 256 contiguous slots (22 KB) indexed by the low bits of the same
 * XXH3, holding the full key: a hit is one memcmp on a single slot. It only stores key -> VkPipeline pairs
 * already in the map, and the map never erases, so it cannot return anything other than what the map
 * would. Guard: the first 200,000 hits, then 1 in 4,096, also look up the map and must get the same
 * VkPipeline; on a difference, DIFERENCIA in the log and it switches off for the session. false = map only,
 * as before.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pipelines_directa, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): cache directa de 256 pipelines delante del mapa de "
                    "PipelineDe. Mismo resultado que el mapa (se comprueba sola). false = solo el mapa, como antes");
/*
 * Measurement only, it changes no draw. What changes at each vkCmdBindPipeline of the ring: shaders, vertex
 * input, formats, specialization, or only fixed pipeline state (blending, masks, Z, stencil, cull face,
 * topology, bias, primitive restart). The state-only ones are those Vulkan dynamic state would avoid. Two
 * "C6 cambios de pipeline" lines every 20 s. false = nothing is counted.
 */
// Disabled by default: it was only a measurement and it has already produced its data.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_contar_cambios_pipeline, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): cuenta que cambia en cada vkCmdBindPipeline (solo "
                    "medida, no cambia ningun dibujo). Lineas C6 cambios de pipeline cada 20 s. false = no se cuenta");
/*
 * Dynamic state, phase 0a. A draw's pipeline is looked up with its key in canonical form (Canonizar):
 * whatever PipelineDe does not read, or reads but Vulkan ignores (the blend equation without blendEnable,
 * the half of the equation whose channels the mask does not write, the Z function without a Z test, stencil
 * operations without stencil, targets not in the pass), is set to a fixed value. The pipeline is the same in
 * everything Vulkan looks at: fewer pipelines and fewer vkCmdBindPipeline calls (the counter's "no effect"
 * category). Guard: the first 200,000 key changes, then 1 in 4,096, compare the fixed state of both keys
 * field by field (RellenarEstadoFijo, the relevant part of PipelineDe); on a difference, DIFERENCIA in the
 * log and it switches off for the session. false = the usual key.
 */
// Disabled by default. Measured: PipelineDe got more expensive and no avoidable bind was measured.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_clave_canonica, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el pipeline se busca con la clave en forma canonica (solo "
                    "lo que Vulkan mira): menos pipelines y menos vkCmdBindPipeline. Se comprueba sola. false = la "
                    "clave de siempre");
/*
 * Dynamic state, phase 0b. EmpezarPase no longer forgets the bound pipeline. Vulkan keeps the binding and
 * the dynamic state across passes of the same command buffer, and NVK only marks state as dirty when a pass
 * begins (nvk_cmd_buffer_dirty_render_pass). The same key carries the same formats and therefore a
 * compatible render pass (compatibility ignores loadOp and storeOp). A new command buffer still starts with
 * nothing bound. It removes the repeated vkCmdBindPipeline of a pass's first draw (the counter's "same key
 * after starting a pass"). false = forgotten at every pass, as before.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pipeline_entre_pases, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el pipeline enlazado se conserva al empezar un pase del "
                    "mismo bufer de comandos (Vulkan lo conserva). false = se vuelve a enlazar en cada pase, como "
                    "antes");
/*
 * Dynamic state, phase 1 (EDS1/EDS2, core in Vulkan 1.3; the console reports API 1.3.354). Cull mode, front
 * face, topology (within its class), Z test, write and function, stencil with its operations, depth bias
 * and primitive restart leave the pipeline: they are set with vkCmdSet* and only when they change. Two draws
 * that only differ in those share a pipeline and need no new vkCmdBindPipeline (3-4.5 us each on the
 * console). Pipelines in this mode carry a flag in the key (relleno2) and do not mix with the usual ones;
 * the mode is decided once per command buffer. Guard: the first 200,000 key changes, then 1 in 4,096,
 * compare what was set with what the usual pipeline would carry (RellenarEstadoFijo); on a difference,
 * DIFERENCIA in the log and it switches off for the session (pipelines with all state baked in come back,
 * and the draw with the difference already goes out with its own). false = everything in the pipeline, as
 * before.
 */
// Disabled by default. Measured: each BindPipeline went from 2.95 to 4.36 us and PipelineDe from 0.21 to
// 0.47 us per draw, for only 13 % fewer binds: a net loss of ~1 ms of ring time per frame.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_estado_dinamico, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): cara, frente, topologia, Z, estencil, sesgo y reinicio "
                    "con vkCmdSet* (EDS1/EDS2 de Vulkan 1.3) en vez de en el pipeline: menos vkCmdBindPipeline. Se "
                    "comprueba sola. false = todo en el pipeline, como antes");
/*
 * Dynamic state, phase 2 (VK_EXT_extended_dynamic_state3; NVK exposes it on Maxwell and the SDK enables it
 * with ui_vulkan_estado_dinamico3.patch). Blending (blendEnable), its equation and each target's color mask
 * are set with vkCmdSet* and stop splitting pipelines: among the first 64 pipelines created, the 12
 * state-only variants differed only in blending. If the SDK or the device does not provide it, it is not
 * used, and one log line says so. Same guard as phase 1. false = blending in the pipeline, as before.
 */
// Disabled by default, for the same measured reason as nfsmw_nativo_estado_dinamico (it goes with it).
REXCVAR_DEFINE_BOOL(nfsmw_nativo_estado_dinamico3, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): mezcla, ecuacion y mascara de color con vkCmdSet* "
                    "(VK_EXT_extended_dynamic_state3) en vez de en el pipeline. Se comprueba sola. false = en el "
                    "pipeline, como antes");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_memoria_subida, true, "NFSMW",
                    "Renderizador nativo: al crear el bufer de subida mide una vez cuantos MB/s se escriben desde la "
                    "CPU en cada tipo de memoria visible (8 MB) y cuanto cuesta publicarlos")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard lives in nfsmw_recorte_sombras.cpp, which owns the lever.
namespace nfsmw::guardia30 {
bool SinSombras(bool del_usuario);
}  // namespace nfsmw::guardia30

namespace nfsmw::nativo {

// Incremented by the ring thread when it handles a WAIT_REG_MEM (nfsmw_nativo_sistema.cpp). That is the
// point where vertex deduplication must forget what it recorded.
std::atomic<uint32_t> g_sincronizaciones_anillo{0};
namespace {

/*
 * A pass's category from its render target, in one place.
 *
 * It used to be decided by width: "1600 or more" meant shadows. With the scene at 1920
 * (nfsmw_1080p_prueba) that no longer tells them apart and the scene was counted as shadows, so its
 * category vanished from the report. The shadow map is recognized for what it is: a depth-only target, with
 * no color at all.
 */
inline uint32_t CategoriaDeDestino(uint32_t pitch, const uint64_t* claves) {
  const bool color = claves[0] || claves[1] || claves[2] || claves[3];
  if (!color && claves[4] && pitch >= 1600) {
    return kGpuSombras;
  }
  if (pitch >= 1280) {
    return claves[4] ? kGpuEscena : kGpuEscenaSinProfundidad;
  }
  if (pitch >= 640) {
    return kGpuReflejo;
  }
  if (pitch >= 320) {
    return kGpu320;
  }
  return kGpuMenores;
}

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;

constexpr uint32_t kRegConstantesVs = 0x4000;
constexpr uint32_t kRegConstantesPs = 0x4400;
constexpr uint32_t kRegFetch = 0x4800;
constexpr uint32_t kRegBooleanos = 0x4900;
constexpr uint32_t kRegistrosConstantes = 0x400;  // 256 constantes x 4

constexpr uint32_t kCapacidadMonton[4] = {4096, 16, 64, 512};  // 2D, 3D, cubo, samplers
// Work slots. The ones actually used are chosen by nfsmw_nativo_ranuras_trabajo; this is the room reserved
// for them, and it has to match the array in nfsmw_nativo_destinos.cpp.
constexpr size_t kRanurasDeTrabajo = 3;
constexpr VkDeviceSize kTamanoSubida = VkDeviceSize(64) << 20;
// Separate buffer for the shared constants, per slot. ~1,400 draws x 512 bytes = 0.7 MB per frame; if it
// fills up, the work is submitted just as with the upload buffer.
constexpr VkDeviceSize kTamanoCompartidas = VkDeviceSize(4) << 20;  // per work slot (there are three)
// The pipeline cache and the prewarm list go in a single file in <NRO folder>/cache/ (nothing loose next to
// the NRO, and a single pipelines .bin). Header "NFPC", version, list bytes and cache bytes (two uint32
// and two uint64), then both parts: the list as is (its "NFPL" header and the records) and the
// vkGetPipelineCacheData data.
constexpr const char* kCarpetaCache = "cache";
constexpr const char* kFicheroPipelines = "nfsmw_nativo_pipelines.bin";
constexpr uint32_t kMagiaFicheroPipelines = 0x4350464Eu;  // "NFPC" en little-endian
constexpr uint32_t kVersionFicheroPipelines = 1;
constexpr size_t kCabeceraFicheroPipelines = 2 * sizeof(uint32_t) + 2 * sizeof(uint64_t);
// The two files used by earlier versions, next to the NRO: if the new one does not exist yet they are read
// once (so the existing cache is not lost) and deleted when the new one is written.
constexpr const char* kFicheroCacheViejo = "nfsmw_nativo_pipelines.bin";
constexpr const char* kFicheroListaVieja = "nfsmw_nativo_pipelines_lista.bin";
// The pipeline prewarm list. It holds no game data: state keys, formats and fingerprints. A header of four
// uint32 ("NFPL", version, record size and record count) followed by the records (RegistroPipeline).
constexpr uint32_t kMagiaListaPipelines = 0x4C50464Eu;  // "NFPL" en little-endian
constexpr uint32_t kVersionListaPipelines = 1;
constexpr size_t kCabeceraLista = 4 * sizeof(uint32_t);
constexpr size_t kMaxRegistrosLista = 4096;
// 296 bytes: NFSMW's shader_common.h (g_NdcScale at +280 and g_NdcOffset at +288).
// Shared constants: texture and sampler indices (0-63), booleans, texcoords, half pixel, alpha threshold
// (68) and function (69), NDC (64-73) and g_InputRemap for the 16 locations (74-89).
// 90 words up to g_InputRemap (bytes 296..359) and 32 more for 1/size of the 16 texture slots (bytes
// 360..487), which avoid querying the texture size on every sample.
constexpr uint32_t kPalabrasCompartidas = 122;
constexpr uint32_t kPalabraInvTamano = 90;
// Constants through a dynamic UBO (nfsmw_nativo_constantes_ubo). The bit is SPEC_CONSTANT_CONSTANTES_UBO
// from shader_common.h, and the sizes are the blocks the shaders declare: 256 and 224 float4, and 23 shared
// float4.
constexpr uint32_t kSpecConstantesUbo = uint32_t(1) << 8;
// Internal bit of the pipeline key that no shader reads: the bright pass (PS n137, container
// kHuellaBrightPass = p_000094) uses the nfsmw_resplandor_cielo variant: natural
// (p_000094_resplandor_energia.hlsl) or soft (p_000094_resplandor_suave.hlsl).
// The shaders take 1/texture size from the shared constants (SPEC_CONSTANT_INV_TAMANO_TEX in XenosRecomp)
// and do not query the texture size on every sample.
constexpr uint32_t kSpecInvTamanoTex = uint32_t(1) << 9;
constexpr uint32_t kSpecResplandorNatural = uint32_t(1) << 12;
constexpr uint32_t kSpecResplandorSuave = uint32_t(1) << 13;
// The pixel shader is compiled without its color writes (pass without a color target).
constexpr uint32_t kSpecSoloAlfa = uint32_t(1) << 14;
// Cheap PCF. The shaders that sample the shadow map use a 3x3 pattern at half-texel offsets (nine samples
// per pixel; eleven in p_000101, the smoke, which is full-screen quads and 21 % of the scene). With this bit
// the eight outer offsets are set to zero, the nine samples become identical and the compiler merges them
// into one. Shadow edges lose some smoothness. Needs the library regenerated with
// shaders/nfsmw_regenerar_biblioteca_pcf.sh.
constexpr uint32_t kSpecPcfBarato = uint32_t(1) << 15;
// Bits 16-18 = the alpha test function (0-6). See SPEC_CONSTANT_ALPHA_FUNC_SHIFT in shader_common.h: it
// removes the 7-case switch from every pixel shader.
constexpr uint32_t kSpecFuncionAlfaDesplazamiento = 16;
// The radial blur of the final composite (see SPEC_CONSTANT_SIN_DESENFOQUE).
constexpr uint32_t kSpecSinDesenfoque = uint32_t(1) << 19;
// Internal bit of the pipeline key that no shader reads (shader_common.h goes up to bit 19). It marks that
// the pixel shader module is the copy with OpExecutionMode EarlyFragmentTests.
constexpr uint32_t kSpecZTemprana = uint32_t(1) << 20;
// nfsmw_nativo_sombra_minimo. SPEC_CONSTANT_SOMBRA_MINIMO from shader_common.h: tfetch2DSombraMin takes the
// minimum of the shadow map and its pair, which goes in the 3D index word of that register.
constexpr uint32_t kSpecSombraMinimo = uint32_t(1) << 23;
constexpr uint64_t kHuellaComposicion = 0x19C0C358044A29BFull;  // p_000139, the race VisualTreatment
// nfsmw_tratamiento_visual. The final composite (kHuellaComposicion, p_000139) tints each channel with a polynomial
// curve (Coeffs0..3 = c6..c9, evaluated at MISCMAP1.w), mixes in a desaturated part with its x component and adds a
// vignette (VisualEffectVignette.x = c1). With a neutral curve (Coeffs0 = (0, 1, 1, 1) and Coeffs1..3 = 0) and the
// vignette at 0 the picture has no filter; the glow (g_fBloomScale, c4) and the brightness of the fades
// (CombinedBrightness, c10) stay the same. suave: halfway between the game's values and the neutral ones (the curve is
// linear in its coefficients). The registers are those of the microcode, the same in every edition.
constexpr uint32_t kBytesTratamiento = 11 * 16;  // c0 to c10
void AplicarTratamientoVisual(float* c, int modo) {
  static constexpr float kNeutro[4][4] = {{0, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
  const float f = modo == 2 ? 1.0f : 0.5f;
  for (int k = 0; k < 4; ++k) {
    for (int i = 0; i < 4; ++i) {
      float& v = c[(6 + k) * 4 + i];
      v += (kNeutro[k][i] - v) * f;
    }
  }
  c[1 * 4 + 0] *= 1.0f - f;
}

/*
 * The render target height comes from the pitch, not from what the game uses.
 *
 * nfsmw_nativo_destinos.cpp creates every render target with `alto = max(720, pitch)`, because the real
 * height is not known when it is created. Measured result:
 *
 *   scene    pitch 1280 -> image 1280x1280, the game uses 1280x720
 *   cubemap  pitch  320 -> image  320x720,  the game uses  320x256
 *   bloom    pitch  320 -> image  320x720,  the game uses  320x180
 *   bloom    pitch  160 -> image  160x720,  the game uses  160x90
 *
 * And since the passes are opened with loadOp = LOAD and closed with storeOp = STORE, that whole area is
 * read and written in each of the ~27 pass openings per frame: 18.81 Mtexels opened when the game uses
 * ~8.5. That is ~130-165 MB per frame moved for nothing, over a 21.3 GB/s bus that the three cores also
 * share.
 *
 * The Xbox 360 paid none of this: its framebuffer lived in 10 MB of on-chip EDRAM at 256 GB/s, and the
 * resolve was a hardware operation. We move it back and forth.
 *
 * It is fixed through the renderArea, not the image size. Vulkan only loads and stores the renderArea;
 * everything outside is preserved. So the height does not have to be guessed when the image is created:
 * opening the pass over the rectangle the game really uses is enough.
 *
 * How that height is known without guessing: from the draws' scissor. The maximum seen per pitch is kept,
 * only grows, and is rounded up to a multiple of 64. Until there is data the whole pass is opened, so the
 * first frame never clips too much.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_pase_area_util, true, "NFSMW",
                    "Renderizador nativo (20/09): abrir cada pase sobre el rectangulo que el juego usa de "
                    "verdad en vez de sobre la imagen entera. El alto de los destinos se deduce del pitch, "
                    "asi que la escena abre 1280x1280 para dibujar 1280x720 y el cubo 320x720 para dibujar "
                    "320x256. La imagen es identica: Vulkan solo carga y guarda el renderArea");
constexpr uint64_t kHuellaBrightPass = 0xE849A9F6D3323B87ull;
/*
 * The pixel shader of the sky dome (nfsmw_nativo_cielo_aplazado).
 *
 * The fingerprint is the XXH3 of the shader's original container, not of the translated SPIR-V: it
 * identifies the game's shader and does not change when the library is regenerated (checked: the same
 * 28AA3CDAC6C19705 in nfsmw_validado_predicados and in nfsmw_fusion3, with SPIR-V of 1,859 and 3,306
 * words). It is the same mechanism as kHuellaBrightPass and kHuellaComposicion.
 *
 * What it is, beyond doubt: its constant table declares CloudIntensity, SkyAlphaTag and Brightness, and
 * four samplers (DIFFUSEMAP, MISCMAP1, MISCMAP2, MISCMAP3) with 5 samples. It is the only shader in the
 * library (152 containers) that names the sky: it cannot be confused with any other. In the log it shows
 * up as PS n29.
 */
constexpr uint64_t kHuellaCielo = 0x28AA3CDAC6C19705ull;
// The sky dome is 480 indices = 160 triangles, measured in the log (the C6 diag line of the draw with
// PS n29). That is the mark that tells the dome draw apart from the other six that share its pixel
// shader, which were the ones that broke the image in an earlier version.
constexpr uint32_t kIndicesDomoCielo = 480;
/*
 * How long the sky guard's test lasts, counting only the frames in which the dome appears (menus and
 * loading screens do not count, see CerrarFotogramaDeLaGuardiaDelCielo). 90 frames are about 3 seconds
 * of racing: enough not to decide on four samples, and short enough for the saving to start almost as
 * the race begins. All 90 must have exactly one dome: as soon as two appear in the same frame (the
 * earlier failure) it switches off for good.
 */
constexpr uint32_t kCieloFotogramasPrueba = 90;
constexpr uint32_t kCieloFotogramasConUno = 90;
constexpr VkDeviceSize kUboBytesVs = 256 * 16;
constexpr VkDeviceSize kUboBytesPs = 224 * 16;
constexpr VkDeviceSize kUboBytesCompartidas = 31 * 16;
constexpr uint32_t kRemapeoIdentidad = 0xFFF;
constexpr uint32_t kMaxVerticesPorDibujo = uint32_t(1) << 20;
constexpr uint32_t kMemoriaFisica = 0x20000000;
// Times a statement in the timed draws (C6 subetapas).
#define NFSMW_SUB(k, ...) \
  do { \
    if (cronometrar_) { \
      const auto t0_sub_ = std::chrono::steady_clock::now(); \
      __VA_ARGS__; \
      sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>( \
                                 std::chrono::steady_clock::now() - t0_sub_) \
                                 .count()); \
      ++sub_n_[k]; \
    } else { \
      __VA_ARGS__; \
    } \
  } while (0)

/*
 * From 8 to 64. Each timed draw reads the clock about 20 times (Etapa, NFSMW_SUB, samplers and
 * vertices). At 1 in 8 that averaged 0.15-0.4 us on every draw of the ring, and in the alley there are
 * 2,500-4,000 per frame. "C6 etapas", "C6 subetapas" and "ms copiando vertices" divide (or rescale) by
 * the draws that actually carried a stopwatch, so their scale does not change: they just have 8 times
 * fewer samples (~600-1,400 per second in a race).
 */
constexpr uint32_t kCronometroCada = 64;  // draws per stage-timed draw (power of 2)
constexpr size_t kCopiasPorAviso = 64;   // copies queued between wake-ups of the copy thread (power of 2)
// Copies taken in one batch (nfsmw_nativo_subidas_ayuda). In a race they are ~2 KB each: a batch is
// ~30 KB, about 20 us. That is how long the ring may have to wait for the thread, already with its
// priority lent.
constexpr size_t kCopiasPorTrozo = 16;
// Waits with pending copies in the observing phase (the ring waits as before and checks the marks)
// before it starts helping. In the menus that is a few seconds.
constexpr uint64_t kEsperasCopiasMirando = 512;

// One guest vertex binding into the upload buffer, as host words with the fetch constant's byte order.
// Done by the ring thread or by the copy thread (nfsmw_nativo_subidas_hilo).
struct TrabajoCopia {
  const uint8_t* origen = nullptr;
  uint8_t* destino = nullptr;
  uint32_t palabras = 0;
  xenos::Endian orden = xenos::Endian::kNone;
};

void CopiarVertices(const TrabajoCopia& t) {
  // The fields go into local variables: the destination is a byte pointer and, as far as the compiler
  // knows, writing through it could change the structure itself. With t.* inside the loop it was not
  // vectorized (935 ms per 5.3 GB of vertices in a race, against 490 ms with the loop inside Dibujar).
  const uint8_t* const origen = t.origen;
  uint8_t* const destino = t.destino;
  const uint32_t palabras = t.palabras;
  const xenos::Endian orden = t.orden;
  if (orden == xenos::Endian::k8in32) {
    for (uint32_t i = 0; i < palabras; ++i) {
      uint32_t v;
      std::memcpy(&v, origen + size_t(i) * 4, 4);
      v = std::byteswap(v);
      std::memcpy(destino + size_t(i) * 4, &v, 4);
    }
  } else {
    for (uint32_t i = 0; i < palabras; ++i) {
      uint32_t v;
      std::memcpy(&v, origen + size_t(i) * 4, 4);
      v = xenos::GpuSwap(v, orden);
      std::memcpy(destino + size_t(i) * 4, &v, 4);
    }
  }
}

// Allocator that leaves whatever resize adds uninitialized: indices_, convertidos_ and indices16_ are
// fully rewritten right after, and std::vector's zero fill was 1.4 % of the ring thread on PC.
template <class T>
struct SinInicializar : std::allocator<T> {
  using value_type = T;
  SinInicializar() = default;
  template <class U>
  SinInicializar(const SinInicializar<U>&) noexcept {}
  template <class U>
  struct rebind {
    using other = SinInicializar<U>;
  };
  template <class U>
  void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};

using FnDireccionBufer = VkDeviceAddress(VKAPI_PTR*)(VkDevice, const VkBufferDeviceAddressInfo*);
using FnCopiarImagen = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                                        VkImageLayout, uint32_t, const VkImageCopy*);

float Flotante(uint32_t valor) {
  return std::bit_cast<float>(valor);
}

// Arithmetic of the Xenos mip level layout (pipeline/texture/util.cpp: GetPackedMipLevel,
// GetPackedMipOffset and GetGuestTextureLayout).
uint32_t Log2Techo(uint32_t v) {
  return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1));
}

uint32_t Log2Suelo(uint32_t v) {
  return v ? 31 - uint32_t(std::countl_zero(v)) : 0;
}

/*
 * The sample used to recheck a stable texture (nfsmw_nativo_huellas_muestreo). From a region of guest
 * memory, its first 4 KB block, its last one and one in every kMuestraCada are read, counted from the
 * start of the region. The base and the mips of a texture start at 4 KB-aligned addresses, so each block
 * is a whole page. That reads ~13-19 % of the bytes of textures of 64 KB or more; for textures of few
 * blocks the sample is almost everything, and PrepararTextura does not use it if it exceeds half. Tested
 * on PC: BytesMuestra matches what is read for every size from 1 byte to 300 KB, and a one-byte change is
 * seen if and only if it falls in a sampled block.
 */
constexpr uint64_t kBloqueMuestra = 4096;
constexpr uint64_t kMuestraCada = 8;

// Bytes HuellaMuestra reads in a region of that size.
inline uint64_t BytesMuestra(uint64_t bytes) {
  if (!bytes) {
    return 0;
  }
  const uint64_t ultimo = (bytes - 1) / kBloqueMuestra;
  return (ultimo + kMuestraCada - 1) / kMuestraCada * kBloqueMuestra + (bytes - ultimo * kBloqueMuestra);
}

// Chained XXH3 of blocks 0, 8, 16... below the last one, and of the last one (which may be partial).
inline uint64_t HuellaMuestra(const uint8_t* datos, uint64_t bytes, uint64_t semilla) {
  if (!bytes) {
    return semilla;
  }
  const uint64_t ultimo = (bytes - 1) / kBloqueMuestra;
  uint64_t huella = semilla;
  for (uint64_t b = 0; b < ultimo; b += kMuestraCada) {
    huella = XXH3_64bits_withSeed(datos + b * kBloqueMuestra, size_t(kBloqueMuestra), huella);
  }
  return XXH3_64bits_withSeed(datos + ultimo * kBloqueMuestra, size_t(bytes - ultimo * kBloqueMuestra), huella);
}

// First level of the packed tail: once the short side is 16 texels or less.
uint32_t NivelEmpaquetado(uint32_t ancho, uint32_t alto) {
  const uint32_t l = Log2Techo(std::min(ancho, alto));
  return l > 4 ? l - 4 : 0;
}

// Blocks from the start of the packed tail to a level of a 2D texture; 0 if the level is not packed.
void DesplazamientoEmpaquetado(uint32_t ancho, uint32_t alto, uint32_t bloque, uint32_t nivel, uint32_t& x,
                               uint32_t& y) {
  const uint32_t l2_ancho = Log2Techo(ancho);
  const uint32_t l2_alto = Log2Techo(alto);
  const uint32_t l2 = std::min(l2_ancho, l2_alto);
  x = 0;
  y = 0;
  if (l2 > 4 + nivel) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = nivel - base;
  if (m < 3) {
    if (l2_ancho > l2_alto) {
      y = 16u >> m;  // wider than tall: levels are stacked vertically
    } else {
      x = 16u >> m;
    }
  } else if (l2_ancho > l2_alto) {
    x = (1u << (l2_ancho - base)) >> (m - 2);
  } else {
    y = (1u << (l2_alto - base)) >> (m - 2);
  }
  x /= bloque;
  y /= bloque;
}

// Address of a block in a texture tiled in 32x32 blocks, copied from GetTiledOffset2D
// (graphics/pipeline/texture/util.cpp). pitch in blocks.
int32_t DesplazamientoMosaico2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Fast untiling. During race stutters the ring spent ~30 ms preparing 5 MB of new textures (about 6 ms
// per MB): LeerNivel called DesplazamientoMosaico2D and a variable-size memcpy per block. Here, with the
// block size fixed at compile time:
//   - what depends on the row (y) is computed once per row;
//   - within a 16-byte group of the tiling the blocks are contiguous in the source (only the 4 low bits
//     of micro change), so 16 bytes are copied at once (8 for textures with 1 byte per block).
// It gives exactly the same addresses as DesplazamientoMosaico2D (checked block by block on PC).
template <uint32_t kLog2>
void DesenmosaicarNivel(const uint8_t* origen, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* destino) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGrupo = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  const int32_t macros_fila = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t fila = 0; fila < by; ++fila) {
    const int32_t y = int32_t(oy + fila);
    const int32_t macro_y = (y >> 5) * macros_fila;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* salida = destino + size_t(fila) * bx_host * kBytes;
    uint32_t columna = 0;
    while (columna < bx) {
      const int32_t x = int32_t(ox + columna);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t desplazamiento = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) +
                                     (((y8 + (x >> 3)) & 3) << 6) + (offset & 0x3F);
      if (kGrupo > 1 && (uint32_t(x) % kGrupo) == 0 && columna + kGrupo <= bx) {
        std::memcpy(salida + size_t(columna) * kBytes, origen + desplazamiento, kGrupo * kBytes);
        columna += kGrupo;
      } else {
        std::memcpy(salida + size_t(columna) * kBytes, origen + desplazamiento, kBytes);
        ++columna;
      }
    }
  }
}

// Byte swap of a whole texture in one go (instead of GpuSwap word by word, with the switch on the order
// inside the loop). Same result as GpuSwap: for 16-bit units only k8in16 changes anything; for 32-bit
// units, k8in16, k8in32 and k16in32. It only touches complete units, like the plain loop.
inline void CambiarOrdenBytes(uint8_t* datos, size_t bytes, uint32_t unidad, uint32_t orden) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;  // xenos::Endian
  size_t n = unidad == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unidad == 2 && orden != k8in16) || (unidad != 2 && unidad != 4) || orden == 0) {
    return;
  }
  size_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(datos + i);
    uint8x16_t r;
    if (unidad == 2 || orden == k8in16) {
      r = vrev16q_u8(v);
    } else if (orden == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(datos + i, r);
  }
#endif
  if (unidad == 2 || orden == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(datos[i], datos[i + 1]);
    }
  } else if (orden == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(datos[i], datos[i + 3]);
      std::swap(datos[i + 1], datos[i + 2]);
    }
  } else if (orden == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(datos[i], datos[i + 2]);
      std::swap(datos[i + 1], datos[i + 3]);
    }
  }
}

// The same for a 3D texture tiled in 32x32x4 blocks, copied from GetTiledOffset3D
// (graphics/pipeline/texture/util.cpp:438-459). pitch and height in blocks.
int32_t DesplazamientoMosaico3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t alto,
                                uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  alto = (alto + 31) & ~uint32_t(31);
  const int32_t macro_exterior = ((y >> 4) + (z >> 2) * int32_t(alto >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_exterior) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t exterior = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = exterior + ((((x >> 3) + (exterior << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t direccion = (offset1 & 1) << 3;
  direccion += (offset2 >> 6) & 7;
  direccion <<= 3;
  direccion += offset1 & ~1;
  direccion <<= 2;
  direccion += offset2 & ~511;
  direccion <<= 3;
  direccion += offset2 & 63;
  return direccion;
}

// USAGE_LOCATIONS de XenosRecomp (shader_recompiler.cpp).
int32_t UbicacionDeUso(uint8_t uso, uint8_t indice) {
  switch (uso) {
    case 0:  // posicion
      // Positions 2-6 only exist in NFS Carbon (NFSC_RECOMP in XenosRecomp).
      return indice == 0 ? 0 : (indice == 1 ? 15 : (indice <= 6 ? 14 + indice : -1));
    case 3:  // normal
      return indice == 0 ? 1 : -1;
    case 6:  // tangente
      return indice == 0 ? 2 : -1;
    case 7:  // binormal
      return indice == 0 ? 3 : -1;
    case 5:  // texcoord (8: NFS Carbon)
      return indice < 4 ? 4 + indice : (indice < 8 ? 12 + (indice - 4) : (indice == 8 ? 21 : -1));
    case 10:  // color
      return indice == 0 ? 8 : (indice == 1 ? 11 : -1);
    case 2:  // indices de mezcla
      return indice == 0 ? 9 : -1;
    case 1:  // pesos de mezcla
      return indice == 0 ? 10 : -1;
    default:
      return -1;
  }
}

// USAGE_TYPES: these inputs are uint4 in the translated shaders.
bool EntradaEntera(uint8_t uso) {
  // Only BLENDINDICES. Since the nfsmw_validado_normales library, normals, tangents and binormals are
  // float4: NFSMW stores them as 16-bit integers (format 26) or as floats, and with uint4 the shader's
  // asfloat gave degenerate directions (seen in the rear-view mirror).
  return uso == 2;
}

uint32_t ComponentesVertice(uint32_t formato) {
  switch (formato) {
    case 33:
    case 36:
      return 1;
    case 25:
    case 31:
    case 34:
    case 37:
      return 2;
    case 16:
    case 17:
    case 57:
      return 3;
    default:
      return 4;
  }
}

// Components written by a fetch swizzle (the ones that are not 7), as a 4-bit mask.
uint32_t MascaraEscrita(uint32_t swizzle) {
  uint32_t mascara = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((swizzle >> (i * 3)) & 0x7) != 7) {
      mascara |= 1u << i;
    }
  }
  return mascara;
}

// g_InputRemap code: the SPIR-V writes r[i] = entrada[orig[i]] and the fetch patched by D3D writes
// r[i] = dato[parcheado[i]] (or 0 / 1). For each written component, the host input at orig[i] has to
// come from parcheado[i]. 7 = the same component.
uint32_t CodigoRemapeo(uint32_t original, uint32_t parcheado) {
  uint32_t codigo = kRemapeoIdentidad;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (parcheado >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      codigo = (codigo & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return codigo;
}

VkFormat FormatoAtributo(uint32_t formato, bool entrada_entera, bool con_signo, bool entero,
                         bool rojo_azul, bool& r11g11b10) {
  r11g11b10 = false;
  const auto elegir = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled,
                          VkFormat uint_, VkFormat sint) {
    if (entrada_entera) {
      return con_signo ? sint : uint_;
    }
    if (entero) {
      return con_signo ? sscaled : uscaled;
    }
    return con_signo ? snorm : unorm;
  };
  switch (formato) {
    case 6:  // k_8_8_8_8
      return rojo_azul ? elegir(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM,
                                VK_FORMAT_B8G8R8A8_USCALED, VK_FORMAT_B8G8R8A8_SSCALED,
                                VK_FORMAT_B8G8R8A8_UINT, VK_FORMAT_B8G8R8A8_SINT)
                       : elegir(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                                VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                                VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT);
    case 7:  // k_2_10_10_10
      if (rojo_azul) break;
      return elegir(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                    VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                    VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32);
    case 16:  // k_10_11_11: packed normal decoded by the shader itself
      if (!entrada_entera || rojo_azul) break;
      r11g11b10 = true;
      return VK_FORMAT_R32_UINT;
    case 25:  // k_16_16
      if (rojo_azul) break;
      return elegir(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                    VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT);
    case 26:  // k_16_16_16_16
      if (rojo_azul) break;
      return elegir(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                    VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                    VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT);
    case 31:  // k_16_16_FLOAT
      return entrada_entera || rojo_azul ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_SFLOAT;
    case 32:  // k_16_16_16_16_FLOAT
      return entrada_entera || rojo_azul ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:  // k_32
      return entrada_entera && !rojo_azul ? (con_signo ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT)
                                         : VK_FORMAT_UNDEFINED;
    case 34:  // k_32_32
      return entrada_entera && !rojo_azul
                 ? (con_signo ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT)
                 : VK_FORMAT_UNDEFINED;
    case 35:  // k_32_32_32_32
      return entrada_entera && !rojo_azul
                 ? (con_signo ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                 : VK_FORMAT_UNDEFINED;
    // Floats: a uint4 input reinterprets the bits (asfloat in the shader).
    case 36:  // k_32_FLOAT
      return rojo_azul ? VK_FORMAT_UNDEFINED
                       : (entrada_entera ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT);
    case 37:  // k_32_32_FLOAT
      return rojo_azul ? VK_FORMAT_UNDEFINED
                       : (entrada_entera ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT);
    case 57:  // k_32_32_32_FLOAT
      return rojo_azul ? VK_FORMAT_UNDEFINED
                       : (entrada_entera ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT);
    case 38:  // k_32_32_32_32_FLOAT
      return rojo_azul ? VK_FORMAT_UNDEFINED
                       : (entrada_entera ? VK_FORMAT_R32G32B32A32_UINT
                                         : VK_FORMAT_R32G32B32A32_SFLOAT);
    default:
      break;
  }
  return VK_FORMAT_UNDEFINED;
}

constexpr uint16_t kSwizzleRRRR = 0;
constexpr uint16_t kSwizzleRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleRGBA = (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint16_t kSwizzleBGRA = 2 | (1 << 3) | (0 << 6) | (3 << 9);

uint32_t IndiceBc(VkFormat formato) {
  switch (formato) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return 1;
    case VK_FORMAT_BC2_UNORM_BLOCK: return 2;
    case VK_FORMAT_BC3_UNORM_BLOCK: return 3;
    case VK_FORMAT_BC4_UNORM_BLOCK: return 4;
    case VK_FORMAT_BC5_UNORM_BLOCK: return 5;
    default: return 0;
  }
}

struct FormatoTextura {
  VkFormat formato = VK_FORMAT_UNDEFINED;
  uint8_t bloque = 1;       // texels per block side
  uint8_t bytes = 1;        // bytes per block
  uint8_t unidad_orden = 0;  // 0 = no byte swap, else 2 or 4 bytes
  uint16_t swizzle_host = kSwizzleRGBA;
};

// Same host formats as the emulation (vulkan/texture_cache.cpp:123-397).
bool FormatoTexturaDe(uint32_t formato, FormatoTextura& f) {
  switch (formato) {
    case 2:  // k_8
    case 8:  // k_8_A
      f = {VK_FORMAT_R8_UNORM, 1, 1, 0, kSwizzleRRRR};
      return true;
    case 10:  // k_8_8
      f = {VK_FORMAT_R8G8_UNORM, 1, 2, 2, kSwizzleRGGG};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
      f = {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      f = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 18:  // k_DXT1
    case 51:
      f = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 2, kSwizzleRGBA};
      return true;
    case 19:  // k_DXT2_3
    case 52:
      f = {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 20:  // k_DXT4_5
    case 53:
      f = {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 49:  // k_DXN
      f = {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, 2, kSwizzleRGGG};
      return true;
    case 59:  // k_DXT5A
      f = {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, 2, kSwizzleRRRR};
      return true;
    case 24:  // k_16
      f = {VK_FORMAT_R16_UNORM, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 25:  // k_16_16
      f = {VK_FORMAT_R16G16_UNORM, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 26:  // k_16_16_16_16
      f = {VK_FORMAT_R16G16B16A16_UNORM, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 30:  // k_16_FLOAT
      f = {VK_FORMAT_R16_SFLOAT, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 31:  // k_16_16_FLOAT
      f = {VK_FORMAT_R16G16_SFLOAT, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 32:  // k_16_16_16_16_FLOAT
      f = {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 36:  // k_32_FLOAT
      f = {VK_FORMAT_R32_SFLOAT, 1, 4, 4, kSwizzleRRRR};
      return true;
    case 37:  // k_32_32_FLOAT
      f = {VK_FORMAT_R32G32_SFLOAT, 1, 8, 4, kSwizzleRGGG};
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      f = {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, 4, kSwizzleRGBA};
      return true;
    default:
      return false;
  }
}

bool EsProfundidad(VkFormat formato) {
  return formato == VK_FORMAT_D24_UNORM_S8_UINT || formato == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// GuestToHostSwizzle: the fetch constant's swizzle, mapped through the channels the host format has.
VkComponentMapping MapeoComponentes(uint32_t swizzle_guest, uint16_t swizzle_host) {
  VkComponentMapping mapeo{};
  VkComponentSwizzle* salida[4] = {&mapeo.r, &mapeo.g, &mapeo.b, &mapeo.a};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t valor = (swizzle_guest >> (i * 3)) & 0x7;
    if (valor <= 3) {
      const uint32_t host = (swizzle_host >> (valor * 3)) & 0x7;
      *salida[i] = VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + host);
    } else if (valor == 4) {
      *salida[i] = VK_COMPONENT_SWIZZLE_ZERO;
    } else {
      *salida[i] = VK_COMPONENT_SWIZZLE_ONE;
    }
  }
  return mapeo;
}

struct AtributoVertices {
  uint32_t ubicacion;
  uint32_t enlace;
  VkFormat formato;
  uint32_t offset;
};

struct EnlaceVertices {
  uint32_t ranura;   // fetch constant de vertices (0-95)
  uint32_t zancada;  // bytes
};

struct EntradaVertices {
  std::vector<AtributoVertices> atributos;
  std::vector<EnlaceVertices> enlaces;
  std::array<uint32_t, 16> remapeos{};  // g_InputRemap by location
  uint32_t especializacion = 0;
  uint64_t huella = 0;
};

// Copy of the SPIR-V without the writes to the color targets (output variables with Location 0..3).
// Returns empty if the module cannot be walked or there was nothing to remove.
std::vector<uint32_t> PodarEscriturasDeColor(const std::vector<uint32_t>& spirv, uint32_t& quitadas) {
  constexpr uint32_t kMagia = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpDecorate = 71, kOpVariable = 59, kOpStore = 62;
  constexpr uint32_t kOpAccessChain = 65, kOpInBoundsAccessChain = 66;
  constexpr uint32_t kDecoracionBuiltIn = 11, kDecoracionLocation = 30, kDecoracionIndex = 29;
  constexpr uint32_t kAlmacenSalida = 3;
  quitadas = 0;
  if (spirv.size() < 5 || spirv[0] != kMagia) {
    return {};
  }
  std::unordered_set<uint32_t> con_location, descartadas;
  // 1) decorations: Location 0..3 marks a color target; BuiltIn or Index != 0 rule it out (gl_FragDepth
  // and the second blend source are not touched).
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t palabras = spirv[i] >> 16;
    const uint32_t codigo = spirv[i] & 0xFFFF;
    if (palabras == 0 || i + palabras > spirv.size()) {
      return {};
    }
    if (codigo == kOpDecorate && palabras >= 3) {
      const uint32_t objetivo = spirv[i + 1];
      const uint32_t decoracion = spirv[i + 2];
      if (decoracion == kDecoracionLocation && palabras >= 4 && spirv[i + 3] <= 3) {
        con_location.insert(objetivo);
      } else if (decoracion == kDecoracionBuiltIn ||
                 (decoracion == kDecoracionIndex && palabras >= 4 && spirv[i + 3] != 0)) {
        descartadas.insert(objetivo);
      }
    }
    i += palabras;
  }
  // 2) pointers to those targets: the output variable and whatever is derived from it through
  // OpAccessChain.
  std::unordered_set<uint32_t> punteros;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t palabras = spirv[i] >> 16;
    const uint32_t codigo = spirv[i] & 0xFFFF;
    if (codigo == kOpVariable && palabras >= 4 && spirv[i + 3] == kAlmacenSalida) {
      const uint32_t id = spirv[i + 2];
      if (con_location.count(id) && !descartadas.count(id)) {
        punteros.insert(id);
      }
    } else if ((codigo == kOpAccessChain || codigo == kOpInBoundsAccessChain) && palabras >= 4 &&
               punteros.count(spirv[i + 3])) {
      punteros.insert(spirv[i + 2]);
    }
    i += palabras;
  }
  if (punteros.empty()) {
    return {};
  }
  // 3) the writes to those pointers are dropped. The rest of the module is copied as is: the output
  // variable stays declared and in the entry point's interface, which is valid even if it is never written.
  std::vector<uint32_t> salida;
  salida.reserve(spirv.size());
  salida.insert(salida.end(), spirv.begin(), spirv.begin() + 5);
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t palabras = spirv[i] >> 16;
    const uint32_t codigo = spirv[i] & 0xFFFF;
    const bool fuera = codigo == kOpStore && palabras >= 3 && punteros.count(spirv[i + 1]);
    if (fuera) {
      ++quitadas;
    } else {
      salida.insert(salida.end(), spirv.begin() + i, spirv.begin() + i + palabras);
    }
    i += palabras;
  }
  (void)kOpEntryPoint;
  return quitadas ? salida : std::vector<uint32_t>();
}

/*
 * The same module, but declaring OpExecutionMode EarlyFragmentTests.
 *
 * In SPIR-V the execution modes have their own section, right after the OpEntryPoint instructions and
 * before the debug strings and decorations. So it is enough to insert the instruction after the module's
 * last OpExecutionMode (there is always at least one, OriginUpperLeft, which is what DXC emits). No new
 * ids are created, so the header's "bound" does not change and the module stays valid.
 *
 * Returns empty (and then the normal module is used) if the module has no fragment entry point, if it
 * already declared it, or if it writes gl_FragDepth (DepthReplacing): in that last case testing earlier
 * would change the result, because the Z being tested is computed by the shader itself.
 */
std::vector<uint32_t> ConPruebasTempranas(const std::vector<uint32_t>& spirv, const char*& motivo) {
  constexpr uint32_t kMagia = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpExecutionMode = 16;
  constexpr uint32_t kModeloFragmento = 4;
  constexpr uint32_t kModoEarly = 9, kModoDepthReplacing = 12;
  motivo = "";
  if (spirv.size() < 5 || spirv[0] != kMagia) {
    motivo = "no es SPIR-V";
    return {};
  }
  uint32_t entrada = 0;
  size_t donde = 0;  // first word after the execution mode section
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t palabras = spirv[i] >> 16;
    const uint32_t codigo = spirv[i] & 0xFFFF;
    if (palabras == 0 || i + palabras > spirv.size()) {
      motivo = "modulo mal formado";
      return {};
    }
    if (codigo == kOpEntryPoint && palabras >= 3) {
      if (spirv[i + 1] == kModeloFragmento) {
        if (entrada) {
          motivo = "mas de un punto de entrada de fragmentos";
          return {};
        }
        entrada = spirv[i + 2];
      }
      if (donde < i + palabras) {
        donde = i + palabras;
      }
    } else if (codigo == kOpExecutionMode && palabras >= 3) {
      if (spirv[i + 2] == kModoEarly) {
        motivo = "ya la declaraba";
        return {};
      }
      if (spirv[i + 2] == kModoDepthReplacing) {
        motivo = "escribe gl_FragDepth";
        return {};
      }
      if (donde < i + palabras) {
        donde = i + palabras;
      }
    }
    i += palabras;
  }
  if (!entrada || !donde) {
    motivo = "sin punto de entrada de fragmentos";
    return {};
  }
  std::vector<uint32_t> salida;
  salida.reserve(spirv.size() + 3);
  salida.insert(salida.end(), spirv.begin(), spirv.begin() + donde);
  salida.push_back((3u << 16) | kOpExecutionMode);
  salida.push_back(entrada);
  salida.push_back(kModoEarly);
  salida.insert(salida.end(), spirv.begin() + donde, spirv.end());
  return salida;
}

struct ClavePipeline {
  uint32_t vs = 0;
  uint32_t ps = 0;
  uint64_t entrada = 0;
  uint32_t topologia = 0;
  uint32_t especializacion = 0;
  uint32_t formatos[5] = {};
  uint32_t mezcla[4] = {};
  uint32_t mascaras = 0;
  uint32_t profundidad = 0;
  uint32_t rasterizado = 0;
  // The key is hashed and compared byte by byte (PipelineDe). With 76 bytes of fields and 8-byte alignment
  // it had 4 bytes of uninitialized implicit padding: stack garbage that made identical keys differ and
  // created duplicate pipelines (125-134, and 193-203 in a later version, with no change in the image).
  uint32_t relleno = 0;
  uint32_t relleno2 = 0;
};
static_assert(std::has_unique_object_representations_v<ClavePipeline>,
              "ClavePipeline no puede tener relleno implicito: se hashea y se compara byte a byte");

/*
 * One pipeline of the prewarm list (nfsmw_nativo_pipelines_precalentar).
 *
 * What is needed to recreate it in another session exactly as the ring created it: its key as PipelineDe
 * looks it up; the fingerprint of its two shaders in the library (if the library has changed, clave.vs
 * and clave.ps are no longer the same shaders and the record is skipped); and its vertex input, which the
 * key only carries as a fingerprint. It is stored on disk byte for byte, so it cannot have implicit
 * padding.
 */
struct AtributoRegistro {
  uint32_t ubicacion = 0;
  uint32_t enlace = 0;
  uint32_t formato = 0;  // VkFormat
  uint32_t offset = 0;
};

struct RegistroPipeline {
  static constexpr uint32_t kMaxAtributos = 16;
  static constexpr uint32_t kMaxEnlaces = 16;
  ClavePipeline clave;
  uint64_t huella_vs = 0;  // nfsmw::native::Shader::huella
  uint64_t huella_ps = 0;  // 0 without a fragment stage (clave.ps == 0)
  uint32_t n_atributos = 0;
  uint32_t n_enlaces = 0;
  AtributoRegistro atributos[kMaxAtributos] = {};
  uint32_t zancadas[kMaxEnlaces] = {};
};
static_assert(std::has_unique_object_representations_v<RegistroPipeline>,
              "RegistroPipeline se guarda en disco byte a byte: sin relleno implicito");

struct Textura {
  ImagenNativa imagen;
  uint32_t capas = 1;  // 6 for cubemaps
  uint32_t fondo = 0;  // slices of 3D textures; 0 for the rest
  uint64_t huella = 0;
  uint64_t fotograma = UINT64_MAX;
  uint64_t huella_cruda = 0;  // XXH3 of the guest bytes (2D and cubemaps)
  uint64_t siguiente = 0;     // frame of the next check
  uint32_t intervalo = 1;     // frames between checks: 1 to 32
  uint32_t aplazamientos = 0;  // consecutive checks deferred by the budget
  // Sample fingerprint (HuellaMuestra) of the same content as huella_cruda if muestra_valida, and how many
  // consecutive rechecks have been accepted on it alone (nfsmw_nativo_huellas_muestreo).
  uint64_t huella_muestra = 0;
  bool muestra_valida = false;
  uint8_t muestras_seguidas = 0;
  bool subir = false;
  std::vector<uint8_t> datos;  // levels already laid out for the host: level after level and, in each, layer after layer
  uint32_t niveles = 1;        // mip levels of the host image
  std::array<uint32_t, 16> desplazamiento_nivel{};  // data bytes up to each level
  uint64_t bytes = 0;          // what it counts in bytes_texturas_ (for eviction)
  // 1 + its index in en_vuelo_ while the bind thread runs its vkBindImageMemory; 0 = no. While set, the
  // image exists but has no memory: no command may reference it until RecogerEnlaces.
  uint32_t en_vuelo = 0;
  // Its data and huella_cruda are prepared by the hash thread (nfsmw_nativo_texturas_huella_hilo). 0 = no;
  // 1 + its index in huellas_planeadas_ until SubirTextura; kTrabajoHuellaPublicado until RecogerHuellas
  // sets its huella_cruda. While nonzero it is not evicted and its huella_cruda is not valid yet.
  uint32_t huella_trabajo = 0;
  // Measurement only (nfsmw_nativo_diag_reutilizar): its shape without the address (when the image is
  // created), its address (only for the log), the content key it is filed under in vivas_por_contenido_
  // (0 = none) and whether it is new and its first fingerprint has not been measured yet.
  uint64_t forma_contenido = 0;
  uint64_t clave_contenido = 0;
  uint32_t direccion = 0;
  bool contenido_por_medir = false;
};

struct Vista {
  VkImage imagen = VK_NULL_HANDLE;
  VkImageView vista = VK_NULL_HANDLE;
  uint32_t ranura = 0;
  uint32_t monton = 0;  // 0 2D textures, 2 cubemaps
};

class DibujosVulkanImpl final : public DibujosVulkan {
 public:
  DibujosVulkanImpl(const VulkanDevice* dispositivo, rex::memory::Memory* memoria,
                    ContextoDestinos* contexto)
      : dispositivo_(dispositivo),
        dfn_(dispositivo->functions()),
        device_(dispositivo->device()),
        memoria_(memoria),
        contexto_(contexto) {}

  ~DibujosVulkanImpl() override {
    PararPrecalentado();  // uses the pipeline cache and the layout: first
    PararCopias();
    PararEnlaces();  // in-flight vkBindImageMemory calls finish before any image is destroyed
    PararHuellas();  // the hash thread writes to the upload buffer: join it before releasing that
    GuardarCachePipelines();
    PararEscritorCache();  // writes whatever is still pending
    for (auto& [clave, par] : pipelines_) {
      dfn_.vkDestroyPipeline(device_, par.second, nullptr);
    }
    if (cache_pipelines_ != VK_NULL_HANDLE) {
      destruir_cache_(device_, cache_pipelines_, nullptr);
    }
    for (auto& [clave, framebuffer] : framebuffers_) {
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (VkFramebuffer framebuffer : fb_retirados_) {  // the ones OlvidarVista retired
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (auto& [clave, pase] : pases_) {
      dfn_.vkDestroyRenderPass(device_, pase, nullptr);
    }
    for (auto& [entrada, modulo] : modulos_) {
      dfn_.vkDestroyShaderModule(device_, modulo, nullptr);
    }
    for (auto& [entrada, modulo] : modulos_solo_alfa_) {
      if (modulo != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, modulo, nullptr);
      }
    }
    for (auto& [entrada, modulo] : modulos_z_temprana_) {
      if (modulo != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, modulo, nullptr);
      }
    }
    for (VkShaderModule modulo : modulos_variantes_) {
      if (modulo != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, modulo, nullptr);
      }
    }
    for (auto& [clave, vista] : vistas_) {
      dfn_.vkDestroyImageView(device_, vista.vista, nullptr);
    }
    for (auto& [clave, textura] : texturas_) {
      DestruirImagen(textura.imagen);
    }
    for (auto& [clave, par] : samplers_) {
      dfn_.vkDestroySampler(device_, par.first, nullptr);
    }
    for (ImagenNativa& vacia : vacias_) {
      DestruirImagen(vacia);
    }
    // The pool's slabs are released after destroying every image that lives in them. The other way round
    // would free memory that the VkImages still have bound.
    pool_texturas_.Terminar();
    if (sampler_vacio_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, sampler_vacio_, nullptr);
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    if (pool_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pool_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_ubo_, nullptr);
    if (layout_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout_ubo_, nullptr);
    for (VkDescriptorSetLayout layout : layouts_) {
      if (layout != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    for (const BuferSubida& s : compartidas_bufs_) {
      if (s.datos) dfn_.vkUnmapMemory(device_, s.memoria);
      if (s.bufer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.bufer, nullptr);
      if (s.memoria != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.memoria, nullptr);
    }
    for (const BuferSubida& s : subidas_) {
      if (s.datos) dfn_.vkUnmapMemory(device_, s.memoria);
      if (s.bufer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.bufer, nullptr);
      if (s.memoria != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.memoria, nullptr);
    }
  }

  bool Inicializar() {
    const auto& propiedades = dispositivo_->properties();
    const std::pair<bool, const char*> requisitos[] = {
        {propiedades.independentBlend, "independentBlend"},
        {propiedades.runtimeDescriptorArray, "runtimeDescriptorArray"},
        {propiedades.shaderSampledImageArrayDynamicIndexing,
         "shaderSampledImageArrayDynamicIndexing"},
        {propiedades.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {propiedades.descriptorBindingSampledImageUpdateAfterBind,
         "descriptorBindingSampledImageUpdateAfterBind"},
        {propiedades.descriptorBindingUpdateUnusedWhilePending,
         "descriptorBindingUpdateUnusedWhilePending"},
    };
    for (const auto& [presente, nombre] : requisitos) {
      if (!presente) {
        REXLOG_ERROR("[nativo] C6: el dispositivo Vulkan no tiene {}: no se dibuja", nombre);
        return false;
      }
    }
    const auto& ifn = dispositivo_->vulkan_instance()->functions();
    const VkFormat formatos_bc[] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK,
        VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK};
    const VkFormat formatos_cpu[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};
    constexpr VkFormatFeatureFlags requerido = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (uint32_t i = 0; i < 5; ++i) {
      VkFormatProperties fp{};
      ifn.vkGetPhysicalDeviceFormatProperties(dispositivo_->physical_device(), formatos_bc[i], &fp);
      bc_cpu_[i] = REXCVAR_GET(nfsmw_nativo_texturas_bc_cpu) || (fp.optimalTilingFeatures & requerido) != requerido;
      if (bc_cpu_[i]) {
        const VkFormat host = formatos_cpu[i < 3 ? 0 : i - 2];
        ifn.vkGetPhysicalDeviceFormatProperties(dispositivo_->physical_device(), host, &fp);
        if ((fp.optimalTilingFeatures & requerido) != requerido) {
          REXLOG_ERROR("[nativo] C3: no hay formato de destino para convertir BC{}", i + 1);
          return false;
        }
        REXLOG_INFO("[compatibilidad] BC{}: conversion CPU a {} (mips y cubos incluidos)",
                    i + 1, i < 3 ? "RGBA8" : i == 3 ? "R8" : "RG8");
      }
    }
    // 64-bit pointers (shaderInt64 and buffer device address) are optional: the shaders read every constant
    // from the set 4 UBOs, and with the current library they do not even contain the pointer path. Many Mali,
    // Adreno and PowerVR drivers have neither. Without them the push constants carry zeros.
    const bool simular_11 = REXCVAR_GET(nfsmw_nativo_simular_vulkan11);
    if (propiedades.shaderInt64 && propiedades.bufferDeviceAddress && !simular_11) {
      direccion_bufer_ = reinterpret_cast<FnDireccionBufer>(
          dispositivo_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_, "vkGetBufferDeviceAddress"));
    }
    // A Vulkan 1.1 driver only takes SPIR-V up to 1.3: the library's 1.5 modules are converted when created.
    spirv_13_ = propiedades.apiVersion < VK_MAKE_API_VERSION(0, 1, 2, 0) || simular_11;
    REXLOG_INFO("[nativo] C6: punteros de 64 bits {}; SPIR-V de la biblioteca {}",
                direccion_bufer_ ? "disponibles" : "NO disponibles (constantes solo por UBO)",
                spirv_13_ ? "convertido a 1.3 (Vulkan 1.1)" : "1.5 tal cual");
    copiar_imagen_ = reinterpret_cast<FnCopiarImagen>(
        dispositivo_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_, "vkCmdCopyImage"));
    CargarCachePipelines();
    CargarEstadoDinamico();  // dynamic state phases 1 and 2
    if (!CrearSubida() || !CrearDescriptores()) {
      return false;
    }
    // The pool is created after CrearDescriptores (where texturas_mb_max_ is read) and before CrearVacias,
    // so the three empty images can already come from it. The slabs are prewarmed here, while the game is
    // still loading: a 32 MB memset during a race would be a 10-15 ms stutter.
    pool_texturas_.Iniciar(dispositivo_, texturas_mb_max_);
    return CrearVacias();
  }

  bool Dibujar(const PeticionDibujo& p) override {
    VeredictoVegetacion(p);  // the ring's versus the game's (nfsmw_d3d_vegetacion_juego)
    // Stage stopwatch on 1 in kCronometroCada draws: reading the clock 12 times per draw took almost a
    // quarter of the ring thread's busy CPU on PC, and on the Switch each read costs more. The C6 averages
    // come from the sample.
    cronometrar_ = (++cronometro_contador_ & (kCronometroCada - 1)) == 0;
    auto marca = cronometrar_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const uint32_t* r = p.registros;
    if (!r || !p.vs) {
      return false;
    }
    // --- Tipo de primitiva ---------------------------------------------------
    const uint32_t modo_edram = r[gr::XE_GPU_REG_RB_MODECONTROL] & 0x7;
    if (modo_edram != uint32_t(xenos::EdramMode::kColorDepth) && modo_edram != 5) {
      return Rechazar(1, "modo EDRAM sin color ni profundidad");
    }
    // Mode 5 (depth only): the Xenos does not run the pixel shader (IsPixelShaderNeededWithRasterization,
    // graphics/util/draw.cpp:125-129). Without a PS there are no textures, pixel constants or alpha test,
    // and the pipeline has no fragment stage. The race shadows work this way, and some arrive with the PS
    // object set to 0.
    const EntradaShader* ps =  // can be removed if it writes no color and does not discard
        modo_edram == uint32_t(xenos::EdramMode::kColorDepth) ? p.ps : nullptr;
    if (!ps && modo_edram == uint32_t(xenos::EdramMode::kColorDepth)) {
      return false;
    }
    // Diagnostic: PS whose draws are skipped (nfsmw_nativo_diag_omitir_ps).
    const std::string& omitir = diag_omitir_ps_texto_;
    if (omitir != omitir_texto_) {
      omitir_texto_ = omitir;
      omitir_ps_.clear();
      uint32_t numero = 0;
      bool hay = false;
      for (char c : omitir + ",") {
        if (c >= '0' && c <= '9') {
          numero = numero * 10 + uint32_t(c - '0');
          hay = true;
        } else if (hay) {
          omitir_ps_.insert(numero);
          numero = 0;
          hay = false;
        }
      }
      if (!omitir_ps_.empty()) {
        REXLOG_WARN("[nativo] C6 diag: no se dibujan los PS {}", omitir_texto_);
      }
    }
    if (ps && !omitir_ps_.empty() && omitir_ps_.count(ps->numero)) {
      return true;
    }
    const uint32_t iniciador = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
    const uint32_t tipo = iniciador & 0x3F;
    const uint32_t fuente = (iniciador >> 6) & 0x3;
    const bool indices32 = (iniciador >> 11) & 0x1;
    const uint32_t cuenta = iniciador >> 16;
    if (!cuenta) {
      return true;
    }
    VkPrimitiveTopology topologia;
    bool cuadrilateros = false;
    bool rectangulos = false;  // the fourth corner is added to the vertices (below, with the bindings)
    bool admite_reinicio = false;
    switch (tipo) {
      case 2:
        topologia = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        break;
      case 3:
        topologia = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        admite_reinicio = true;
        break;
      case 4:
        topologia = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        break;
      case 5:
        topologia = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        admite_reinicio = true;
        break;
      case 6:
        topologia = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        admite_reinicio = true;
        break;
      case 13:
        topologia = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        cuadrilateros = true;
        break;
      case 8:  // rectangle list: three corners per rectangle (Direct3D's own fills in NFS Carbon)
        if (fuente == uint32_t(xenos::SourceSelect::kDMA) || cuenta % 3) {
          return Rechazar(100 + tipo, "lista de rectangulos con indices o cuenta suelta: todavia no");
        }
        topologia = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        rectangulos = true;
        break;
      default:
        return Rechazar(100 + tipo, "tipo de primitiva todavia no soportado");
    }
    if (fuente == uint32_t(xenos::SourceSelect::kImmediate)) {
      return Rechazar(2, "indices inmediatos");
    }
    // Clipping disabled on the Xenos: the position may come in pixels (videos). It is drawn with a viewport
    // the size of the render target and the transform in the VS, like the emulation
    // (graphics/util/draw.cpp:341-363).
    const bool sin_recorte = (r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 16) & 0x1;
    const uint32_t modo_sc = r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
    if (((modo_sc >> 3) & 0x3) == 2 && ((modo_sc >> 5) & 0x7) != 2) {
      return Rechazar(4, "poligonos dibujados como puntos o lineas");
    }

    const EntradaVertices* entrada = EntradaDe(p);
    if (!entrada) {
      return false;
    }
    Etapa(0, marca);
    std::chrono::steady_clock::time_point t_indices_185 = marca;  // C6 subetapas 15-18

    // --- Destinos --------------------------------------------------------------
    const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
    const uint32_t mascara_registro =
        modo_edram == uint32_t(xenos::EdramMode::kColorDepth) ? r[gr::XE_GPU_REG_RB_COLOR_MASK] : 0;
    static constexpr uint32_t kInfoColor[4] = {
        gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
        gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO};
    uint64_t claves[5] = {};
    uint32_t mascaras = 0;
    bool hay_destino = false;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t mascara = (mascara_registro >> (i * 4)) & 0xF;
      if (!mascara || !ps || !((ps->salidas >> i) & 0x1)) {
        continue;
      }
      const uint32_t info = r[kInfoColor[i]];
      const uint32_t formato = (info >> 16) & 0xF;
      if (FormatoHostDestinoColor(formato) == VK_FORMAT_UNDEFINED) {
        return Rechazar(200 + formato, "formato de destino de color todavia no soportado");
      }
      claves[i] = (uint64_t(1) << 63) | (uint64_t(info & 0xFFF) << 24) | (uint64_t(formato) << 16) |
                  pitch;
      mascaras |= mascara << (i * 4);
      hay_destino = true;
    }
    /*
     * Draws that cannot change a single pixel (nfsmw_nativo_saltar_invisibles).
     *
     * Two cases, read from this draw's own registers without any heuristics:
     *
     *  - Blending is "0 x source + 1 x destination" (ADD) on the written channels: the result is the
     *    destination as is. It is checked per channel group because the mask may write only RGB or only
     *    alpha, and then the other group's factors do not matter.
     *  - The alpha test function is 0 = NEVER: alphaTestValue (shader_common.h) returns -1 and clip() kills
     *    every fragment, before depth is written.
     *
     * If there is also no depth or stencil left to write, the whole draw is unnecessary: it is dropped. If
     * it writes depth it is drawn anyway and only counted, because setting the color mask to 0 would send it
     * down the "no color" path (kSpecSoloAlfa), where nfsmw_sombras_sin_vegetacion drops the whole draw and
     * its depth would be lost. Removing the color write saves too little to be worth it.
     * The pruning is skipped while an occlusion query is open: the game reads those samples.
     */
    if (saltar_invisibles_ && mascaras) {
      static constexpr uint32_t kMezclaDe[4] = {
          gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
          gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
      const uint32_t control_color_inv = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool alfa_nunca =
          ((control_color_inv >> 3) & 0x1) && (control_color_inv & 0x7) == 0;  // function NEVER
      uint32_t mascaras_utiles = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t mascara_i = (mascaras >> (i * 4)) & 0xF;
        if (!mascara_i) {
          continue;
        }
        const uint32_t m = r[kMezclaDe[i]] & 0x1FFF1FFF;
        const bool copia_color = (m & 0x1F) == 0 && ((m >> 8) & 0x1F) == 1 && ((m >> 5) & 0x7) == 0;
        const bool copia_alfa =
            ((m >> 16) & 0x1F) == 0 && ((m >> 24) & 0x1F) == 1 && ((m >> 21) & 0x7) == 0;
        const bool escribe_rgb = (mascara_i & 0x7) != 0 && !copia_color;
        const bool escribe_alfa = (mascara_i & 0x8) != 0 && !copia_alfa;
        if (escribe_rgb || escribe_alfa) {
          mascaras_utiles |= mascara_i << (i * 4);
        }
      }
      if ((!mascaras_utiles || alfa_nunca) && !oclusion_abierta_) {
        // With the NEVER function even the depth write dies (clip() runs in the shader, before the merger),
        // so the draw leaves no trace of any kind. With the blending that copies the destination, the draw can
        // only be dropped if it writes neither Z nor stencil either.
        const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
        const bool deja_rastro =
            !alfa_nunca && ((((dc >> 1) & 0x1) && ((dc >> 2) & 0x1)) || (dc & 0x1));
        ++cuentas_z_[alfa_nunca ? kInvisibleAlfa : kInvisibleMezcla];
        if (!deja_rastro) {
          return true;
        }
        ++cuentas_z_[kInvisibleSoloColor];  // drawn anyway: only counts what is left to gain
      }
    }
    // With no color to write, the pixel shader can only have an effect by discarding pixels (alpha test or
    // its own kill) or by writing depth. If it does neither, the pipeline has no fragment stage and the image
    // is identical: the GPU saves shading the whole shadow map.
    // Of the draws that do write color, how many can use early depth rejection and how many force shading
    // before testing. That is what decides whether a depth pre-pass would help at all.
    if (mascaras && ps && hay_destino) {
      const uint32_t control_color_ahora = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool prueba_alfa =
          ((control_color_ahora >> 3) & 0x1) && (control_color_ahora & 0x7) != 7;
      const bool descarta = prueba_alfa || ps->descarta || (ps->salidas & 0x10);
      if (pitch >= 1600) {
        // shadow map: already counted separately
      } else if (descarta) {
        ++escena_con_descarte_;
      } else {
        ++escena_sin_descarte_;
      }
    }
    bool solo_alfa = false;
    if (!mascaras && ps) {
      const uint32_t control_color_ahora = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool prueba_alfa =
          ((control_color_ahora >> 3) & 0x1) && (control_color_ahora & 0x7) != 7;
      if (!prueba_alfa && !ps->descarta && !(ps->salidas & 0x10)) {
        ++dibujos_ps_inutil_;
        if (cvars_por_fotograma_ ? sin_ps_sin_color_fotograma_ : SinPsSinColor()) {
          ps = nullptr;
        }
      } else {
        ++dibujos_ps_necesario_;
        // Diagnostic: in the shadow map the pixel shader reads constant c1 (g_bShadowMapAlphaEnabled). With
        // that constant at 0 it neither samples nor discards: the stage would be unnecessary.
        if (pitch >= 1600) {
          (Flotante(r[kRegConstantesPs + 4]) != 0.0f ? ++sombras_alfa_activa_ : ++sombras_alfa_apagada_);
        }
        // Needed for the alpha test or a kill, but its color goes nowhere: it is compiled without those
        // writes.
        if (cvars_por_fotograma_ ? ps_solo_alfa_fotograma_ : PsSoloAlfa()) {
          solo_alfa = true;
        }
      }
    }
    /*
     * Shadow map vegetation discard, as early as possible.
     *
     * This same discard used to live 640 lines further down, right before the pipeline lookup. So these
     * draws were paid in full and then thrown away: index conversion, vertex sources, PrepararTextura for all
     * their samplers, upload buffer allocation, pass change, vertex copy, index and constant memcpy,
     * viewport, scissor and pipeline lookup.
     *
     * And it is not a handful of draws: 604 per frame are dropped, 31 % of those that come in (the
     * `dibujos_ps_necesario` counter matches the lost ones exactly across twelve race intervals). At
     * ~8.5 us per incoming draw, that is 3-5 ms.
     *
     * Careful when measuring this: the divisor of `C6 etapas` is the recorded draws (~1,370), not the
     * incoming ones (~1,970), even though the label says otherwise. Multiplying by the incoming ones inflates
     * the budget by 45 %.
     *
     * The occlusion guard is free and removes the only doubt: with a game occlusion query open the draw
     * counts even if it is not visible, so there it takes the long path and the usual discard drops it.
     */
    const bool vegetacion_temprana = solo_alfa && sin_vegetacion_ && !oclusion_abierta_;
    // The ring's verdict for the nfsmw_d3d_vegetacion_juego guard (VeredictoVegetacion) must be this same
    // decision. With the settings read on every draw (cvars_por_fotograma_ false) the alternation can change
    // between the two reads: then no comparison is made.
    if (vegetacion_calculada_ && cvars_por_fotograma_ && vegetacion_anillo_ != vegetacion_temprana) {
      vegetacion_detalle_.temprana = vegetacion_temprana;
      AnotarVegetacionModelo(vegetacion_banderas_, vegetacion_detalle_);
    }
    if (vegetacion_temprana) {
      ++dibujos_vegetacion_pronto_;
      return true;
    }
    const uint32_t control_profundidad = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
    if (control_profundidad & 0x3) {  // stencil o z
      const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
      claves[4] = (uint64_t(1) << 62) | (uint64_t(info & 0xFFF) << 24) |
                  (uint64_t((info >> 16) & 0x1) << 16) | pitch;
      hay_destino = true;
    }
    if (!hay_destino || !pitch) {
      // Diagnostic: a draw inside an occlusion query that writes nothing counts no samples.
      if (oclusion_abierta_ && pitch >= 640 && avisos_oclusion_dibujo_ < 8) {
        ++avisos_oclusion_dibujo_;
        REXLOG_INFO("[nativo] C2 oclusion dibujo {}: SIN DESTINO (no se dibuja): VS n{} PS n{} tipo {} cuenta {} pitch {} "
                    "mascara {:08X} profundidad {:08X} modo EDRAM {}",
                    avisos_oclusion_dibujo_, p.vs->numero, ps ? int(ps->numero) : -1, tipo, cuenta, pitch,
                    mascara_registro, control_profundidad, modo_edram);
      }
      return true;  // writes nothing visible
    }
    // FPS test nfsmw_nativo_omitir_sombras: the shadow map is depth-only and 1600 wide. With
    // nfsmw_nativo_omitir_sombras_alternar_s = N they are skipped in the odd N-second intervals.
    const bool destino_sombras =  // also used for its depth bias
        pitch >= 1600 && claves[4] && !claves[0] && !claves[1] && !claves[2] && !claves[3];
    if (destino_sombras) {
      // Step 2 of the 30 FPS guard. Removing the whole pass does not flicker; skipping it on 2 of every
      // 3 frames does, which is why that step no longer exists.
      bool omitir = nfsmw::guardia30::SinSombras(REXCVAR_GET(nfsmw_nativo_omitir_sombras));
      const int32_t alternar = REXCVAR_GET(nfsmw_nativo_omitir_sombras_alternar_s);
      if (!omitir && alternar > 0) {
        const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - inicio_sombras_)
                                  .count();
        omitir = (segundos / alternar) % 2 == 1;
        if (omitir != sombras_omitidas_) {
          sombras_omitidas_ = omitir;
          REXLOG_INFO("[nativo] prueba de sombras: {} (fotograma {})",
                      omitir ? "sin sombras" : "con sombras", fotograma_);
        }
      }
      if (omitir) {
        return true;
      }
    }

    CortarSubetapa(15, t_indices_185);  // render targets and discards
    // --- Rango de vertices e indices --------------------------------------------
    const uint32_t desplazamiento = r[gr::XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
    const bool reinicio = admite_reinicio && ((modo_sc >> 21) & 0x1);
    // Depth bias as in the emulation with host render targets (GetPreferredFacePolygonOffset,
    // graphics/util/draw.cpp:92-118).
    float escala_sesgo = 0.0f, desplazamiento_sesgo = 0.0f;
    if (tipo >= 4) {  // poligonos
      if (((modo_sc >> 11) & 0x1) && !(modo_sc & 0x1)) {
        escala_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
        desplazamiento_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
      }
      if (((modo_sc >> 12) & 0x1) && !((modo_sc >> 1) & 0x1) && escala_sesgo == 0.0f &&
          desplazamiento_sesgo == 0.0f) {
        escala_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE]);
        desplazamiento_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET]);
      }
    } else if ((modo_sc >> 13) & 0x1) {
      escala_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
      desplazamiento_sesgo = Flotante(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
    }
    // Constant scaled by the guest format's minimum value (2^24-1 for D24S8, 2^24 for D24FS8) and slope
    // from subpixels to pixels (vulkan/command_processor.cpp:5786-5799).
    float sesgo[2] = {
        desplazamiento_sesgo * (((r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 0x1)
                                    ? float(uint32_t(1) << 24)
                                    : float((uint32_t(1) << 24) - 1)),
        escala_sesgo * (1.0f / 16.0f)};
    // nfsmw_nativo_sombras_sesgo_*, only in the shadow map and only if the game does not set its own.
    if (destino_sombras && tipo >= 4 && sesgo[0] == 0.0f && sesgo[1] == 0.0f &&
        (sombras_sesgo_constante_ != 0 || sombras_sesgo_pendiente_ != 0)) {
      sesgo[0] = float(sombras_sesgo_constante_) * 1000.0f;
      sesgo[1] = float(sombras_sesgo_pendiente_) * 0.1f;
    }
    const bool con_sesgo = sesgo[0] != 0.0f || sesgo[1] != 0.0f;
    if (con_sesgo && avisos_sesgo_ < 8 &&
        std::memcmp(sesgo, sesgo_avisado_, sizeof(sesgo)) != 0) {
      ++avisos_sesgo_;
      std::memcpy(sesgo_avisado_, sesgo, sizeof(sesgo));
      REXLOG_INFO("[nativo] C6: desplazamiento de profundidad: escala {} desplazamiento {} "
                  "(modo {:08X}) -> constante {} pendiente {}",
                  escala_sesgo, desplazamiento_sesgo, modo_sc, sesgo[0], sesgo[1]);
    }
    CortarSubetapa(16, t_indices_185);  // depth bias
    uint32_t vmin = UINT32_MAX;
    uint32_t vmax = 0;
    bool con_indices = false;
    bool indices_de_16 = false;  // in indices16_ instead of indices_
    indices_.clear();
    if (fuente == uint32_t(xenos::SourceSelect::kDMA)) {
      const uint32_t tamano = r[gr::XE_GPU_REG_VGT_DMA_SIZE];
      const auto orden = static_cast<xenos::Endian>(tamano >> 30);
      const uint32_t bytes = indices32 ? 4 : 2;
      const uint32_t base = r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes - 1);
      if (cuenta > (tamano & 0xFFFFFF) || uint64_t(base & 0x1FFFFFFF) + uint64_t(cuenta) * bytes >
                                               kMemoriaFisica) {
        if (avisos_indices_ < 8) {
          ++avisos_indices_;
          REXLOG_WARN("[nativo] C6 diag: indices: cuenta {} palabras {} VGT_DMA_SIZE {:08X} "
                      "VGT_DMA_BASE {:08X} 32 bits {} tipo {} VS n{} PS n{}",
                      cuenta, tamano & 0xFFFFFF, tamano, r[gr::XE_GPU_REG_VGT_DMA_BASE], indices32,
                      tipo, p.vs->numero, ps ? int(ps->numero) : -1);
        }
        return Rechazar(7, "indices fuera de su bufer");
      }
      const uint8_t* datos = memoria_->TranslatePhysical(base & 0x1FFFFFFF);
      const uint32_t indice_reinicio = r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
      if (!indices32 && !reinicio && !desplazamiento && !cuadrilateros &&
          (orden == xenos::Endian::k8in16 || orden == xenos::Endian::kNone)) {
        // The normal case in NFSMW: 16 bits without restart or offset. They are uploaded as 16 bits, with a
        // branch-free loop the compiler can vectorize.
        indices16_.resize(cuenta);
        uint16_t* salida = indices16_.data();
        uint32_t minimo = 0xFFFF;
        uint32_t maximo = 0;
        // With hand-written NEON and its guard (IndicesDe16, nfsmw_nativo_indices_neon).
        IndicesDe16(datos, cuenta, salida, orden == xenos::Endian::k8in16, minimo, maximo);
        vmin = minimo;
        vmax = maximo;
        indices_de_16 = true;
      } else {
        indices_.resize(cuenta);
        uint32_t* salida = indices_.data();
        for (uint32_t i = 0; i < cuenta; ++i) {
          uint32_t v;
          if (indices32) {
            uint32_t crudo;
            std::memcpy(&crudo, datos + size_t(i) * 4, 4);
            v = xenos::GpuSwap(crudo, orden) & 0xFFFFFF;
          } else {
            uint16_t crudo;
            std::memcpy(&crudo, datos + size_t(i) * 2, 2);
            v = xenos::GpuSwap(crudo, orden);
          }
          if (reinicio && v == indice_reinicio) {
            salida[i] = UINT32_MAX;
            continue;
          }
          v = (v + desplazamiento) & 0xFFFFFF;
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
          salida[i] = v;
        }
      }
      if (vmin > vmax) {
        return true;  // resets only
      }
      con_indices = true;
    } else {
      vmin = desplazamiento;
      vmax = desplazamiento + cuenta - 1;
      if (rectangulos) {
        // Corner, a, fourth, b: two triangles of the same winding as the game's three corners.
        const uint32_t n = cuenta / 3;
        vmax = desplazamiento + n * 4 - 1;
        indices_.resize(size_t(n) * 6);
        for (uint32_t q = 0; q < n; ++q) {
          uint32_t* o = &indices_[size_t(q) * 6];
          const uint32_t v = desplazamiento + q * 4;
          o[0] = v; o[1] = v + 1; o[2] = v + 2;
          o[3] = v; o[4] = v + 2; o[5] = v + 3;
        }
        con_indices = true;
      }
      if (cuadrilateros) {
        indices_.resize(cuenta);
        for (uint32_t i = 0; i < cuenta; ++i) {
          indices_[i] = desplazamiento + i;
        }
        con_indices = true;
      }
    }
    if (vmax - vmin >= kMaxVerticesPorDibujo) {
      return Rechazar(8, "rango de vertices demasiado grande");
    }
    if (cuadrilateros) {
      const size_t n = indices_.size() / 4;
      convertidos_.resize(n * 6);
      for (size_t q = 0; q < n; ++q) {
        const uint32_t* i = &indices_[q * 4];
        uint32_t* o = &convertidos_[q * 6];
        o[0] = i[0]; o[1] = i[1]; o[2] = i[2];
        o[3] = i[0]; o[4] = i[2]; o[5] = i[3];
      }
      indices_.swap(convertidos_);
    }
    CortarSubetapa(17, t_indices_185);  // indices
    // The indices stay as they come: vkCmdDrawIndexed subtracts vmin through vertexOffset.
    const uint32_t vertices = vmax - vmin + 1;

    // Bytes of each vertex binding in guest memory.
    struct Origen {
      const uint8_t* datos;
      xenos::Endian orden;
      uint32_t bytes;
      uint64_t direccion;  // physical, of the first byte used
    };
    std::array<Origen, 16> origenes{};
    if (entrada->enlaces.size() > origenes.size()) {
      return Rechazar(9, "demasiados streams de vertices");
    }
    VkDeviceSize bytes_vertices = 0;
    for (size_t b = 0; b < entrada->enlaces.size(); ++b) {
      const EnlaceVertices& enlace = entrada->enlaces[b];
      const uint32_t d0 = r[kRegFetch + enlace.ranura * 2];
      const uint32_t d1 = r[kRegFetch + enlace.ranura * 2 + 1];
      if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) {
        return Rechazar(10, "fetch constant de vertices invalida");
      }
      const uint64_t direccion = uint64_t(d0 & 0x1FFFFFFC);
      const uint64_t disponibles = uint64_t((d1 >> 2) & 0xFFFFFF) * 4;
      const uint64_t inicio = uint64_t(vmin) * enlace.zancada;
      uint64_t necesarios = uint64_t(rectangulos ? cuenta : vertices) * enlace.zancada;
      if (inicio + necesarios > disponibles) {
        Avisar(11, "vertices mas alla del final de su bufer: se recorta");
        necesarios = disponibles > inicio ? (disponibles - inicio) / enlace.zancada * enlace.zancada : 0;
      }
      if (!necesarios || direccion + inicio + necesarios > kMemoriaFisica) {
        return Rechazar(12, "vertices fuera de la memoria");
      }
      origenes[b] = {memoria_->TranslatePhysical(uint32_t(direccion + inicio)),
                     static_cast<xenos::Endian>(d1 & 0x3), uint32_t(necesarios),
                     direccion + inicio};
      if (!rectangulos) {
        bytes_vertices += (necesarios + 3) & ~VkDeviceSize(3);
      }
    }
    if (rectangulos) {
      /*
       * Rectangle lists: the Xenos completes each rectangle from three corners. As in Xenia, the corner with
       * the right angle is the one opposite the longest side, and the fourth is a + b - corner for every
       * float attribute (the others are copied from a). The vertices are rebuilt in a buffer of their own,
       * still in the guest's byte order, in the order corner, a, fourth, b (the indices above).
       */
      const uint32_t n = cuenta / 3;
      const AtributoVertices* posicion = nullptr;
      for (const AtributoVertices& a : entrada->atributos) {
        if (a.ubicacion == 0) {
          posicion = &a;
        }
      }
      if (!posicion || origenes[posicion->enlace].bytes < uint64_t(cuenta) * entrada->enlaces[posicion->enlace].zancada) {
        return Rechazar(108, "lista de rectangulos sin posicion legible");
      }
      const auto leer = [&](const Origen& o, size_t byte) {
        uint32_t palabra;
        std::memcpy(&palabra, o.datos + byte, 4);
        return Flotante(xenos::GpuSwap(palabra, o.orden));
      };
      std::array<uint8_t, 1024> esquinas_pequeno{};
      std::vector<uint8_t> esquinas_grande;
      uint8_t* esquinas = n <= esquinas_pequeno.size() ? esquinas_pequeno.data()
                                                       : (esquinas_grande.resize(n), esquinas_grande.data());
      {
        const Origen& o = origenes[posicion->enlace];
        const uint32_t zancada = entrada->enlaces[posicion->enlace].zancada;
        for (uint32_t q = 0; q < n; ++q) {
          float x[3], y[3];
          for (uint32_t k = 0; k < 3; ++k) {
            const size_t v = size_t(q * 3 + k) * zancada + posicion->offset;
            x[k] = leer(o, v);
            y[k] = leer(o, v + 4);
          }
          const auto d = [&](uint32_t i, uint32_t j) { return (x[i] - x[j]) * (x[i] - x[j]) + (y[i] - y[j]) * (y[i] - y[j]); };
          const float d01 = d(0, 1), d02 = d(0, 2), d12 = d(1, 2);
          esquinas[q] = d12 >= d01 && d12 >= d02 ? 0 : d02 >= d01 ? 1 : 2;  // opposite the longest side
        }
      }
      for (size_t b = 0; b < entrada->enlaces.size(); ++b) {
        const uint32_t zancada = entrada->enlaces[b].zancada;
        if (!zancada || zancada % 4) {
          return Rechazar(108, "lista de rectangulos con zancada no alineada");
        }
        // Which words of this binding's vertex are floats.
        std::array<bool, 64> flotante{};
        for (const AtributoVertices& a : entrada->atributos) {
          if (a.enlace != b) continue;
          const uint32_t palabras = a.formato == VK_FORMAT_R32_SFLOAT ? 1
                                    : a.formato == VK_FORMAT_R32G32_SFLOAT ? 2
                                    : a.formato == VK_FORMAT_R32G32B32_SFLOAT ? 3
                                    : a.formato == VK_FORMAT_R32G32B32A32_SFLOAT ? 4 : 0;
          for (uint32_t k = 0; k < palabras && a.offset / 4 + k < flotante.size(); ++k) {
            flotante[a.offset / 4 + k] = true;
          }
        }
        Origen& o = origenes[b];
        std::vector<uint8_t>& salida = rectangulos_vertices_[b];
        salida.resize(size_t(n) * 4 * zancada);
        for (uint32_t q = 0; q < n; ++q) {
          const uint32_t c = esquinas[q], a = (c + 1) % 3, bb = (c + 2) % 3;
          const uint8_t* v0 = o.datos + size_t(q * 3) * zancada;
          uint8_t* destino = salida.data() + size_t(q) * 4 * zancada;
          std::memcpy(destino, v0 + c * zancada, zancada);
          std::memcpy(destino + zancada, v0 + a * zancada, zancada);
          std::memcpy(destino + 3 * zancada, v0 + bb * zancada, zancada);
          uint8_t* cuarta = destino + 2 * zancada;
          std::memcpy(cuarta, v0 + a * zancada, zancada);
          for (uint32_t w = 0; w < zancada / 4 && w < flotante.size(); ++w) {
            if (!flotante[w]) continue;
            const float valor = leer(o, size_t(q * 3 + a) * zancada + w * 4) +
                                leer(o, size_t(q * 3 + bb) * zancada + w * 4) -
                                leer(o, size_t(q * 3 + c) * zancada + w * 4);
            uint32_t bits;
            std::memcpy(&bits, &valor, 4);
            bits = xenos::GpuSwap(bits, o.orden);  // back to the guest's order (the swap is its own inverse)
            std::memcpy(cuarta + w * 4, &bits, 4);
          }
        }
        o.datos = salida.data();
        o.bytes = uint32_t(salida.size());
        o.direccion = 0;  // not guest memory: copied at once and never deduplicated
        bytes_vertices += (o.bytes + 3) & ~VkDeviceSize(3);
      }
    }

    // Diagnostic: one line per combination of VS, PS and render target (at most 32).
    if (diagnosticos_ < 32) {
      const uint64_t clave_diagnostico = (uint64_t(p.vs->numero) << 44) ^
                                         (uint64_t(ps ? ps->numero + 1 : 0) << 32) ^ claves[0] ^
                                         (sin_recorte ? 1 : 0);
      if (diagnosticados_.insert(clave_diagnostico).second) {
        ++diagnosticos_;
        float posicion[4] = {};
        for (const AtributoVertices& a : entrada->atributos) {
          if (a.ubicacion != 0) {
            continue;
          }
          const Origen& origen = origenes[a.enlace];
          if (origen.bytes >= a.offset + 16) {
            for (uint32_t k = 0; k < 4; ++k) {
              uint32_t palabra;
              std::memcpy(&palabra, origen.datos + a.offset + k * 4, 4);
              posicion[k] = Flotante(xenos::GpuSwap(palabra, origen.orden));
            }
          }
          break;
        }
        REXLOG_INFO(
            "[nativo] C6 diag: VS n{} PS n{} tipo {} cuenta {} {} VTE {:08X} CLIP {:08X} "
            "vp x {}+-{} y {}+-{} z {}+{} ventana {:08X} sc {:08X} superficie {:08X} "
            "color0 {:08X} mascara {:08X} profundidad {:08X} edram {} vtx {:08X} "
            "pos0 ({:.3f} {:.3f} {:.3f} {:.3f}){}",
            p.vs->numero, ps ? int(ps->numero) : -1, tipo, cuenta,
            fuente == 0 ? "indices" : "auto",
            r[gr::XE_GPU_REG_PA_CL_VTE_CNTL], r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL],
            Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]),
            Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]),
            Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]),
            Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]),
            Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]),
            Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]), r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET],
            modo_sc, r[gr::XE_GPU_REG_RB_SURFACE_INFO], r[gr::XE_GPU_REG_RB_COLOR_INFO],
            r[gr::XE_GPU_REG_RB_COLOR_MASK], control_profundidad, modo_edram,
            r[gr::XE_GPU_REG_PA_SU_VTX_CNTL], posicion[0], posicion[1], posicion[2], posicion[3],
            sin_recorte ? " [sin recorte]" : "");
      }
    }

    CortarSubetapa(18, t_indices_185);  // vertices and diagnostics
    Etapa(1, marca);
    // --- Textures and samplers ----------------------------------------------------
    uint32_t compartidas[kPalabrasCompartidas] = {};
    VkDeviceSize bytes_texturas = 0;
    DescartarHuellasPlaneadas();  // those of the previous draw that never reached SubirTextura
    texturas_a_subir_.clear();
    const auto t_samplers =
        cronometrar_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    /*
     * nfsmw_nativo_profundidad_perezosa. In the final composite without blur, the depth sample (HEIGHTMAP)
     * is dead: C2 is told so that requesting it does not force a copy. The same value decides
     * kSpecSinDesenfoque further down, so textures and specialization always agree.
     */
    const bool composicion_sin_desenfoque =
        ps && ps->shader && ps->shader->huella == kHuellaComposicion && sin_desenfoque_fotograma_;
    if (composicion_sin_desenfoque) {
      contexto_->LecturasDeProfundidadMuertas(true);
    }
    bool lee_reflejo = false;  // nfsmw_reflejo_visibilidad
    for (const SamplerShader& sampler :
         ps ? std::span<const SamplerShader>(ps->samplers) : std::span<const SamplerShader>()) {
      if (sampler.registro >= 16) {
        continue;
      }
      const uint32_t* fetch = r + kRegFetch + uint32_t(sampler.registro) * 6;
      // nfsmw_reflejo_visibilidad. Here and not in TexturaResuelta, which, because of the caches, only sees
      // the first draw of each frame that samples it. Without checking the dimension: over-counting only
      // means the reflection gets drawn.
      if (medir_visibilidad_ && (fetch[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          ((fetch[1] & 0xFFFFF000u) & 0x1FFFFFFFu) == nfsmw::reflejo_demanda::kDireccion) {
        lee_reflejo = true;
      }
      ++samplers_preparados_;
      bool sin_cache = false;  // depth requested but not sampled, kept out of the caches
      // Per-register cache: same fetch constant, same frame and no C2 copies in between.
      CacheSampler& cache = cache_samplers_[sampler.registro];
      // Valid until cache.valido_hasta (with the cross-frame cache disabled, only its own frame)
      if ((cache_entre_fotogramas_ ? fotograma_ <= cache.valido_hasta : cache.fotograma == fotograma_) &&
          cache.generacion == generacion_texturas_ &&
          std::memcmp(cache.fetch.data(), fetch, sizeof(cache.fetch)) == 0) {
        compartidas[cache.monton * 16 + sampler.registro] = cache.ranura;
        compartidas[48 + sampler.registro] = cache.sampler;
        EscribirInvTamano(compartidas, sampler.registro, cache.ancho, cache.alto);
        ++samplers_cache_;
        continue;
      }
      uint32_t ranura_textura = 0;
      uint32_t monton = 0;
      uint32_t ranura_sampler = 0;
      uint32_t ancho_host = 0, alto_host = 0;
      // Second cache, keyed by the whole fetch constant within the same frame and generation: a register
      // changes texture between draws, but textures repeat a lot within a frame, and PrepararTextura already
      // queued their upload the first time.
      CacheSampler& por_fetch =
          cache_fetch_[XXH3_64bits(fetch, sizeof(uint32_t) * 6) & (cache_fetch_.size() - 1)];
      uint64_t valido_hasta = fotograma_;
      if ((cache_entre_fotogramas_ ? fotograma_ <= por_fetch.valido_hasta : por_fetch.fotograma == fotograma_) &&
          por_fetch.generacion == generacion_texturas_ &&
          std::memcmp(por_fetch.fetch.data(), fetch, sizeof(por_fetch.fetch)) == 0) {
        ranura_textura = por_fetch.ranura;
        monton = por_fetch.monton;
        ranura_sampler = por_fetch.sampler;
        valido_hasta = por_fetch.valido_hasta;
        ancho_host = por_fetch.ancho;
        alto_host = por_fetch.alto;
        ++samplers_cache_fetch_;
      } else {
        // Why the table misses ("C6 cache por fetch" report, every 10 s).
        if (std::memcmp(por_fetch.fetch.data(), fetch, sizeof(por_fetch.fetch)) != 0) {
          ++(por_fetch.fotograma == UINT64_MAX ? fetch_fallos_vacia_ : fetch_fallos_choque_);
        } else if (por_fetch.generacion != generacion_texturas_) {
          ++fetch_fallos_generacion_;
        } else {
          ++fetch_fallos_caducada_;
        }
        bool muestreo_puntual = false;
        PrepararTextura(fetch, ranura_textura, monton, bytes_texturas, muestreo_puntual, valido_hasta,
                        ancho_host, alto_host);
        ranura_sampler = RanuraSampler(fetch, muestreo_puntual);
        por_fetch.fotograma = fotograma_;
        por_fetch.generacion = generacion_texturas_;
        std::memcpy(por_fetch.fetch.data(), fetch, sizeof(por_fetch.fetch));
        por_fetch.ranura = ranura_textura;
        por_fetch.monton = monton;
        por_fetch.sampler = ranura_sampler;
        por_fetch.valido_hasta = valido_hasta;
        por_fetch.ancho = ancho_host;
        por_fetch.alto = alto_host;
        // A depth requested without being sampled is not stored in the caches: the next draw that really
        // samples it has to go through TexturaResuelta, which is where it is recorded if it was deferred.
        if (composicion_sin_desenfoque && muestreo_puntual) {
          por_fetch.fotograma = 0;
          por_fetch.valido_hasta = 0;
          valido_hasta = 0;
          sin_cache = true;
        }
      }
      compartidas[monton * 16 + sampler.registro] = ranura_textura;
      compartidas[48 + sampler.registro] = ranura_sampler;
      EscribirInvTamano(compartidas, sampler.registro, ancho_host, alto_host);
      cache.fotograma = fotograma_;
      cache.generacion = generacion_texturas_;
      std::memcpy(cache.fetch.data(), fetch, sizeof(cache.fetch));
      cache.ranura = ranura_textura;
      cache.monton = monton;
      cache.sampler = ranura_sampler;
      cache.valido_hasta = valido_hasta;
      cache.ancho = ancho_host;
      cache.alto = alto_host;
      if (sin_cache) {
        cache.fotograma = 0;  // with valido_hasta at 0 it is not valid across frames either
      }
    }
    if (composicion_sin_desenfoque) {
      contexto_->LecturasDeProfundidadMuertas(false);
    }
    /*
     * nfsmw_nativo_sombra_minimo. If this draw samples the shadow map texture with cars (the game's
     * textura[0], 086AE000) and C2 holds it with only the cars, its pair (the world map) goes in the 3D
     * index word of that register (the shadow map is 2D: that word is unused) and the pipeline carries
     * kSpecSombraMinimo: tfetch2DSombraMin takes the minimum of the two, the same value the copy would have
     * left. While C2 is observing, the pair is the texture itself (the same texel), so the pipelines with
     * the bit already exist when applying starts. Every read is reported to C2, which decides and watches.
     */
    bool sombra_minimo_dibujo = false;
    if (ps && !ps->samplers.empty()) {
      if (const uint32_t dir_coches = contexto_->DireccionSombraCoches()) {
        for (const SamplerShader& s : ps->samplers) {
          if (s.registro >= 16) {
            continue;
          }
          const uint32_t* f = r + kRegFetch + uint32_t(s.registro) * 6;
          if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture) ||
              (((f[1] >> 12) << 12) & 0x1FFFFFFF) != dir_coches) {
            continue;
          }
          const bool capaz = ps->sombra_minimo && s.mapa_sombras &&
                             ((f[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked);
          const ImagenNativa* pareja = contexto_->CompaneraSombraCoches(capaz, ps->numero);
          if (!pareja) {
            continue;
          }
          const uint32_t ranura_pareja =
              RanuraVista(pareja->imagen, pareja->formato, (f[3] >> 1) & 0xFFF, kSwizzleRRRR);
          if (!ranura_pareja) {
            contexto_->CompaneraSombraCoches(false, ps->numero);  // no view, no minimum: C2 records it
            continue;
          }
          compartidas[16 + s.registro] = ranura_pareja;
          sombra_minimo_dibujo = true;
        }
      }
    }

    if (cronometrar_) {  // C6 subetapas: the sampler loop inside the texturas stage
      sub_ns_[14] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_samplers)
                                  .count());
      ++sub_n_[14];
    }
    Etapa(2, marca);
    // --- Upload buffer space ----------------------------------------------------------
    const VkDeviceSize bytes_indices =
        indices_de_16 ? VkDeviceSize(indices16_.size()) * 2 : VkDeviceSize(indices_.size()) * 4;
    // With a single binding the copy goes to a multiple of the stride: up to stride - 4 bytes of padding.
    const VkDeviceSize hueco_base_cero =
        vertices_base_cero_ && entrada->enlaces.size() == 1 ? VkDeviceSize(entrada->enlaces[0].zancada) : 0;
    const VkDeviceSize necesarios = hueco_base_cero + bytes_vertices + bytes_indices + bytes_texturas +
                                    2 * VkDeviceSize(kRegistrosConstantes) * 4 +
                                    kPalabrasCompartidas * 4 + 64 * 8 +
                                    // With UBOs the blocks go whole and aligned to alineacion_ubo_
                                    (usar_ubo_ ? kUboBytesVs + kUboBytesPs + kUboBytesCompartidas +
                                                     3 * alineacion_ubo_
                                               : 0);
    if (necesarios > kTamanoSubida) {
      return Rechazar(13, "dibujo mayor que el bufer de subida");
    }
    const bool compartidas_llenas =
        compartidas_aparte_ && usar_ubo_ &&
        compartidas_usado_ + kUboBytesCompartidas + alineacion_ubo_ > kTamanoCompartidas;
    if (subida_usado_ + necesarios > kTamanoSubida || compartidas_llenas) {
      const auto antes_envio = std::chrono::steady_clock::now();
      const bool enviado = contexto_->EnviarYEsperar();
      ++envios_llenos_;
      ns_envios_llenos_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - antes_envio)
                                        .count());
      if (!enviado) {
        return false;
      }
    }

    // --- Render pass ----------------------------------------------------------------
    /*
     * No XXH3 on every draw (nfsmw_nativo_clave_pase_rapida). If the 40 bytes of render targets are the
     * same ones that produced pase_clave_ (EmpezarPase stores them next to it), their XXH3 is pase_clave_.
     * Otherwise it is computed as usual: the pass change decision is the same as before in every case,
     * collisions included.
     */
    uint64_t clave_pase;
    if (clave_pase_rapida_ && pase_claves_validas_ && std::memcmp(claves, pase_claves_, sizeof(claves)) == 0) {
      const uint64_t n = ++claves_pase_rapidas_;
      clave_pase = (n <= kClavesPaseAComprobar || (n & 4095) == 0) ? ComprobarClavePase(claves, n) : pase_clave_;
    } else {
      clave_pase = XXH3_64bits(claves, sizeof(claves));
    }
    if (!pase_activo_ || clave_pase != pase_clave_ ||
        pase_generacion_ != contexto_->GeneracionComandos()) {
      const auto antes_pase = std::chrono::steady_clock::now();
      if (pase_generacion_ != contexto_->GeneracionComandos()) {
        ++pases_por_generacion_;
      } else if (!pase_activo_ && clave_pase == pase_clave_) {
        ++pases_reanudados_;
      } else {
        ++pases_por_destino_;
      }
      TerminarPase();
      // The pass change is split. TerminarPase closes the previous pass; EmpezarPase finds or creates the
      // render targets (it may create images, record barriers and clear them), builds the render pass and
      // the framebuffer, and opens the pass.
      const auto tras_terminar = std::chrono::steady_clock::now();
      if (cronometrar_) {
        etapas_ns_[8] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            tras_terminar - antes_pase).count());
      }
      const bool empezado = EmpezarPase(r, claves, pitch, clave_pase);
      const auto tras_pase = std::chrono::steady_clock::now();
      ns_pases_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(tras_pase - antes_pase).count());
      // The pass change, separate from what the stage costs on each draw.
      if (cronometrar_) {
        etapas_ns_[7] += uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tras_pase - antes_pase).count());
      }
      if (!empezado) {
        return false;
      }
    }
    const VkCommandBuffer cmd = pase_comandos_;
    if (grabacion_generacion_ != contexto_->GeneracionComandos()) {
      grabacion_generacion_ = contexto_->GeneracionComandos();
      pipeline_enlazado_ = VK_NULL_HANDLE;
      eds_valido_ = false;  // phases 1 and 2: nothing is set in the new buffer
      clave_enlazada_valida_ = false;  // New command buffer (ContarCambioPipeline)
      sets_enlazados_ = false;
      ubo_enlazado_ = false;
      estado_grabado_ = false;
      enlaces_grabados_ = 0;  // different command buffer: nothing is bound
    }

    Etapa(3, marca);
    // --- Uploads: textures, vertices, indices and constants ------------------------------
    for (Textura* textura : texturas_a_subir_) {
      if (!SubirTextura(*textura)) {
        return false;
      }
    }
    std::array<VkDeviceSize, 16> offsets_vertices{};
    // If the guest has waited for the GPU since the last draw, it may legally have rewritten an already
    // referenced range: what was recorded is no longer valid.
    if (dedupe_activo_) {
      const uint32_t sinc = g_sincronizaciones_anillo.load(std::memory_order_relaxed);
      if (sinc != dedupe_sinc_vista_) {
        dedupe_sinc_vista_ = sinc;
        dedupe_.Olvidar();
      }
    }
    const auto antes_vertices =
        cronometrar_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (size_t b = 0; b < entrada->enlaces.size(); ++b) {
      const Origen& origen = origenes[b];
      if (diag_vertices_repetidos_) {
        AnotarVerticesRepetidos(origen.direccion, origen.bytes, uint32_t(origen.orden), origen.datos);
      }
      VkDeviceSize offset;
      // If this same range was already copied in this frame, its place in the upload buffer is reused and
      // nothing is copied. See nfsmw_nativo_vertices_dedupe.h.
      if (dedupe_activo_ && origen.direccion != 0 &&
          dedupe_.Buscar(origen.direccion, origen.bytes, uint32_t(origen.orden), offset)) {
        offsets_vertices[b] = offset;
        continue;
      }
      // With a single binding, at a multiple of its stride (nfsmw_nativo_vertices_base_cero).
      if (vertices_base_cero_ && entrada->enlaces.size() == 1) {
        ReservarMultiplo(origen.bytes, entrada->enlaces[0].zancada, offset);
      } else {
        Reservar(origen.bytes, 4, offset);
      }
      const TrabajoCopia trabajo{origen.datos, subida_datos_ + offset, origen.bytes / 4, origen.orden};
      // A rectangle list's buffer is rewritten by the next one: it is copied now, not by the copy thread.
      if (origen.direccion == 0 || !copias_activas_ || !EncolarCopia(trabajo)) {
        CopiarVertices(trabajo);
      }
      bytes_vertices_ += origen.bytes;
      offsets_vertices[b] = offset;
      if (dedupe_activo_ && origen.direccion != 0) {
        dedupe_.Anotar(origen.direccion, origen.bytes, uint32_t(origen.orden), offset);
      }
    }
    if (cronometrar_) {
      ns_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - antes_vertices)
                                   .count());
    }
    VkDeviceSize offset_indices = 0;
    if (con_indices) {
      Reservar(bytes_indices, 4, offset_indices);
      bytes_indices_subidos_ += bytes_indices;
      std::memcpy(subida_datos_ + offset_indices,
                  indices_de_16 ? static_cast<const void*>(indices16_.data())
                                : static_cast<const void*>(indices_.data()),
                  size_t(bytes_indices));
    }
    // Only the registers each shader reads (EntradaShader::constantes_bytes). With the same generation, the
    // copy is reused if it already covers what this shader needs.
    const uint32_t bytes_vs = std::min<uint32_t>(p.vs->constantes_bytes, kRegistrosConstantes * 4);
    if (constantes_vs_generacion_ != p.generacion_constantes_vs ||
        constantes_vs_epoca_ != epoca_subida_ || bytes_vs > constantes_vs_bytes_) {
      // Why they are uploaded again (measurement only; each upload is another set 4 offset).
      ++resubidas_vs_[constantes_vs_generacion_ != p.generacion_constantes_vs ? 0
                      : constantes_vs_epoca_ != epoca_subida_                 ? 1
                                                                              : 2];
      // With UBOs the block goes whole (the driver may read the full range) and aligned to the device minimum
      Reservar(usar_ubo_ ? std::max<VkDeviceSize>(bytes_vs, kUboBytesVs) : bytes_vs, usar_ubo_ ? alineacion_ubo_ : 16,
               constantes_vs_offset_);
      std::memcpy(subida_datos_ + constantes_vs_offset_, r + kRegConstantesVs, bytes_vs);
      constantes_vs_generacion_ = p.generacion_constantes_vs;
      constantes_vs_epoca_ = epoca_subida_;
      constantes_vs_bytes_ = bytes_vs;
    }
    const uint32_t bytes_ps =
        ps ? std::min<uint32_t>(ps->constantes_bytes, kRegistrosConstantes * 4) : 0;
    if (ps && (constantes_ps_generacion_ != p.generacion_constantes_ps ||
               constantes_ps_epoca_ != epoca_subida_ || bytes_ps > constantes_ps_bytes_)) {
      ++resubidas_ps_[constantes_ps_generacion_ != p.generacion_constantes_ps ? 0
                      : constantes_ps_epoca_ != epoca_subida_                 ? 1
                                                                              : 2];
      Reservar(usar_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps, usar_ubo_ ? alineacion_ubo_ : 16,
               constantes_ps_offset_);
      std::memcpy(subida_datos_ + constantes_ps_offset_, r + kRegConstantesPs, bytes_ps);
      constantes_ps_generacion_ = p.generacion_constantes_ps;
      constantes_ps_epoca_ = epoca_subida_;
      constantes_ps_bytes_ = bytes_ps;
    }
    CopiaComposicionTratada(ps, r, bytes_ps);  // nfsmw_tratamiento_visual (see the function)

    Etapa(4, marca);
    // --- Viewport, tijera y constantes compartidas ---------------------------------------
    /*
     * The framing (viewport, ndc and scissor), cached (nfsmw_nativo_encuadre_cache).
     *
     * The viewport, the ndc and the scissor are a function of the framing registers (PA_CL_VTE_CNTL,
     * PA_CL_VPORT_*, PA_SC_WINDOW_OFFSET/SCISSOR, PA_CL_CLIP_CNTL and PA_SU_SC_MODE_CNTL:
     * EsRegistroDeEncuadre in nfsmw_nativo_sistema.cpp, which bumps generacion_encuadre on every path that
     * writes them) and of the pass size and scale (pase_serie_). In a race the framing really changes
     * 0.07-0.14 times per draw ("C6 generaciones"). The depth bias is not included: it depends on
     * PA_SU_POLY_OFFSET_*, RB_DEPTH_INFO and the primitive type, which do not bump that generation.
     */
    const uint32_t vte = r[gr::XE_GPU_REG_PA_CL_VTE_CNTL];
    VkViewport viewport{};
    float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    VkRect2D tijera{};
    uint32_t encuadre_vacio = 0;
    const bool encuadre_en_cache = encuadre_cache_ && encuadre_valido_ &&
                                   p.generacion_encuadre == encuadre_generacion_ && pase_serie_ == encuadre_pase_serie_;
    bool comprobar_encuadre = false;
    if (encuadre_en_cache) {
      const uint64_t n = ++encuadre_aciertos_;
      comprobar_encuadre = n <= kEncuadresAComprobar || (n & 4095) == 0;
      ++encuadre_aciertos_informe_;
    }
    if (encuadre_en_cache && !comprobar_encuadre) {
      viewport = encuadre_viewport_;
      std::memcpy(ndc, encuadre_ndc_, sizeof(ndc));
      tijera = encuadre_tijera_;
      encuadre_vacio = encuadre_vacio_;
    } else {
      encuadre_vacio = CalcularEncuadre(r, vte, sin_recorte, modo_sc, viewport, ndc, tijera);
      ++encuadre_calculos_informe_;
      if (comprobar_encuadre) {
        CompararEncuadre(viewport, ndc, tijera, encuadre_vacio);  // with the stored values, before overwriting them
      }
      encuadre_viewport_ = viewport;
      std::memcpy(encuadre_ndc_, ndc, sizeof(ndc));
      encuadre_tijera_ = tijera;
      encuadre_vacio_ = encuadre_vacio;
      encuadre_generacion_ = p.generacion_encuadre;
      encuadre_pase_serie_ = pase_serie_;
      encuadre_valido_ = true;
    }
    if (encuadre_vacio != 0) {
      return true;  // empty viewport or scissor: as before
    }

    uint32_t especializacion = entrada->especializacion;
    if (composicion_sin_desenfoque) {  // the same value that decided the textures
      especializacion |= kSpecSinDesenfoque;
    }
#ifndef NFSMW_SIN_VARIANTES_RESPLANDOR
    if (ps && ps->shader && ps->shader->huella == kHuellaBrightPass) {
      const int modo = ResplandorCielo();
      especializacion |= modo == 1 ? kSpecResplandorNatural : modo == 2 ? kSpecResplandorSuave : 0;
    }
#endif
    const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    float umbral_alfa = 0.0f;
    uint32_t funcion_alfa = 7;  // always
    // The 8 Xenos functions (0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always) with alphaTestValue
    // (nfsmw_validado_normales library). Without a PS (mode 5) there is no test.
    if (ps && ((control_color >> 3) & 0x1) && (control_color & 0x7) != 7) {
      umbral_alfa = Flotante(r[gr::XE_GPU_REG_RB_ALPHA_REF]);
      funcion_alfa = control_color & 0x7;
      especializacion |= 0x2;
      // The function goes in the pipeline, not in the constants. RB_COLORCONTROL was already part of the key,
      // so this creates no pipelines that did not already exist.
      especializacion |= (funcion_alfa & 0x7u) << kSpecFuncionAlfaDesplazamiento;
    }
    compartidas[64] = (r[kRegBooleanos] & 0xFFFF) | ((r[kRegBooleanos + 4] & 0xFFFF) << 16);
    compartidas[65] = 0;  // g_SwappedTexcoords
    if (!(r[gr::XE_GPU_REG_PA_SU_VTX_CNTL] & 0x1)) {  // PixelCenter::kD3DZero
      const float medio[2] = {1.0f / viewport.width, -1.0f / std::abs(viewport.height)};
      std::memcpy(&compartidas[66], medio, sizeof(medio));
    }
    std::memcpy(&compartidas[68], &umbral_alfa, sizeof(umbral_alfa));
    compartidas[69] = funcion_alfa;  // g_AlphaFunction
    std::memcpy(&compartidas[70], ndc, sizeof(ndc));
    std::copy(entrada->remapeos.begin(), entrada->remapeos.end(), compartidas + 74);
    VkDeviceSize offset_compartidas;
    /*
     * How many draws really change the shared constants.
     *
     * This block is 488 bytes that are zeroed, filled, compared and, if they changed, copied twice, once
     * into memory without CPU caching (2654 MB/s). With ~2,400 draws per frame that is ~4.7 MB of traffic
     * per frame on the thread that is already at 96 % of a core.
     *
     * Which fix applies depends on the count: if almost no draw changes the block, the comparison is the
     * waste; if almost all do, the double memcpy is. They are two different fixes, and counting costs one
     * increment.
     */
    ++compartidas_miradas_;
    const auto t_compartidas =
        cronometrar_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (compartidas_epoca_ == epoca_subida_ &&
        std::memcmp(compartidas, compartidas_previas_, sizeof(compartidas)) == 0) {
      offset_compartidas = compartidas_offset_;  // the same as the previous draw
    } else {
      ++compartidas_cambiadas_;
      if (compartidas_aparte_ && usar_ubo_) {
        // To the separate CPU-cached buffer; binding 2 of set 4 points to it.
        offset_compartidas = (compartidas_usado_ + alineacion_ubo_ - 1) & ~(alineacion_ubo_ - 1);
        compartidas_usado_ = offset_compartidas + kUboBytesCompartidas;
        std::memcpy(compartidas_datos_ + offset_compartidas, compartidas, sizeof(compartidas));
      } else {
        Reservar(usar_ubo_ ? kUboBytesCompartidas : kPalabrasCompartidas * 4, usar_ubo_ ? alineacion_ubo_ : 16,
                 offset_compartidas);
        std::memcpy(subida_datos_ + offset_compartidas, compartidas, sizeof(compartidas));
      }
      std::memcpy(compartidas_previas_, compartidas, sizeof(compartidas));
      compartidas_offset_ = offset_compartidas;
      compartidas_epoca_ = epoca_subida_;
    }
    if (cronometrar_) {
      sub_ns_[12] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_compartidas)
                                  .count());
      ++sub_n_[12];
    }

    // --- Pipeline ---------------------------------------------------------------------------
    ClavePipeline clave;
    clave.vs = p.vs->numero + 1;
    clave.ps = ps ? ps->numero + 1 : 0;  // 0: mode 5, no fragment stage
    clave.entrada = entrada->huella;
    clave.topologia = uint32_t(topologia);
    if (usar_ubo_) {
      especializacion |= kSpecConstantesUbo;  // the shaders read from set 4
    }
    if (inv_tamano_tex_) {
      especializacion |= kSpecInvTamanoTex;
    }
    if (pcf_barato_) {
      especializacion |= kSpecPcfBarato;
    }
    if (solo_alfa && ps) {
      // If vegetation is excluded from the shadow map, this whole draw is unnecessary.
      if (sin_vegetacion_) {
        return true;
      }
      especializacion |= kSpecSoloAlfa;
    }
    /*
     * Test depth before shading where it costs nothing.
     *
     * A pixel shader that can discard (alpha test or its own kill) forces the hardware to shade first and
     * test Z afterwards: otherwise a discarded fragment would already have written its depth. But that only
     * matters if the draw writes depth. If it only tests it (smoke, particles, glass, decals, lights),
     * testing earlier is exact: there is no write to move, the shader still discards the color, and the GPU
     * stops shading what is hidden.
     *
     * The counters are kept even with the setting off, so the report says the same in both halves of an A/B
     * test, and the "NO se pueden porque escriben profundidad" line measures exactly what a depth pre-pass
     * would have to fix.
     */
    if (ps && claves[4] && mascaras) {
      const bool prueba_z = ((control_profundidad >> 1) & 0x1) != 0;
      const bool escribe_z = prueba_z && ((control_profundidad >> 2) & 0x1) != 0;
      const bool estencil = (control_profundidad & 0x1) != 0;
      const bool ps_escribe_z = (ps->salidas & 0x10) != 0;
      const bool tardio = (especializacion & 0x2) || ps->descarta || ps_escribe_z;
      if (!prueba_z) {
        // Without a depth test there is nothing to move earlier.
      } else if (!tardio) {
        ++cuentas_z_[kZYaTemprano];
      } else if (escribe_z || ps_escribe_z) {
        ++cuentas_z_[kZEscribeZ];
      } else if (estencil) {
        ++cuentas_z_[kZEstencil];
      } else if (oclusion_abierta_) {
        ++cuentas_z_[kZOclusion];  // the game reads those samples: leave it alone
      } else if (ps->shader && ps->shader->huella != kHuellaBrightPass &&
                 ps->shader->huella != kHuellaComposicion) {
        ++cuentas_z_[kZPuesta];
        if (z_temprana_) {
          especializacion |= kSpecZTemprana;
        }
      }
    }
    // nfsmw_nativo_sombra_minimo. The shadow map pair bit (see above, textures).
    if (sombra_minimo_dibujo) {
      especializacion |= kSpecSombraMinimo;
    }
    /*
     * nfsmw_nativo_sombra_minimo. In the car pass of the shadow map, C2 needs to know whether this draw
     * leaves in the depth buffer exactly the minimum of what was there and of its fragments: no stencil, no
     * occlusion query and, if it writes Z, a NEVER, LESS or LEQUAL test. The game's register
     * (RB_DEPTHCONTROL) is checked, not the pipeline key. EmpezarPase checks that the pass is depth-only.
     */
    if (pase_coches_sombra_) {
      const uint32_t control_z_coches = claves[4] ? control_profundidad : 0;
      const bool escribe_z_coches = ((control_z_coches >> 1) & 0x1) && ((control_z_coches >> 2) & 0x1);
      const uint32_t funcion_z_coches = (control_z_coches >> 4) & 0x7;
      contexto_->DibujoDeCochesSombra(!(control_z_coches & 0x1) && !oclusion_abierta_ &&
                                          (!escribe_z_coches || funcion_z_coches == 0 || funcion_z_coches == 1 ||
                                           funcion_z_coches == 3),
                                      control_z_coches);
    }
    clave.especializacion = especializacion;
    std::copy(std::begin(pase_formatos_), std::end(pase_formatos_), std::begin(clave.formatos));
    static constexpr uint32_t kMezcla[4] = {
        gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
        gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
    for (uint32_t i = 0; i < 4; ++i) {
      if (claves[i]) {
        clave.mezcla[i] = r[kMezcla[i]] & 0x1FFF1FFF;
      }
    }
    clave.mascaras = mascaras;
    clave.profundidad = claves[4] ? control_profundidad : 0;
    if (clave.profundidad && oclusion_abierta_ && pitch >= 640 && !mascaras &&
        REXCVAR_GET(nfsmw_nativo_prueba_oclusion_siempre)) {
      clave.profundidad |= 0x7 << 4;  // test: Z function always
    }
    clave.rasterizado = (modo_sc & 0x7) | (reinicio ? 0x8 : 0) | (con_sesgo ? 0x10 : 0);
    VkPipeline pipeline = VK_NULL_HANDLE;
    // Looked up with ClaveDeBusqueda (phase 0a: canonical form; phases 1 and 2: without the state set through
    // vkCmdSet*). clave stays raw: the deferred sky (opaco_en_todos), the counter and the dynamic state read
    // it.
    NFSMW_SUB(11, pipeline = PipelineDe(ClaveDeBusqueda(clave), *entrada, p));
    if (pipeline == VK_NULL_HANDLE) {
      return false;
    }
#if REX_PLATFORM_SWITCH
    // The pipeline about to be bound, prefetched into the cache several us before its vkCmdBindPipeline
    // (Mesa patch p06; PRFM only: it reads and writes nothing and cannot fail).
    if (nvk_precarga_app_ && pipeline != pipeline_enlazado_ && vk_switch_precargar_pipeline) {
      vk_switch_precargar_pipeline(pipeline);
    }
#endif

    Etapa(5, marca);
    /*
     * The three dynamic state values that used to be computed inside the recording block are computed here,
     * without recording anything. They are pure computations on the draw's registers; they are needed
     * earlier so they can be saved if this draw is the sky and gets deferred (see below).
     */
    // Scissor test. All the state and the draw are sent as usual; the only change is that there are no
    // pixels to shade. It breaks the image: only for measuring the per-draw floor.
    VkRect2D tijera_final = tijera;
    if (const uint32_t categoria_tijera = tijera_prueba_; categoria_tijera != 0) {
      const uint32_t categoria = CategoriaDeDestino(pitch, claves);
      static constexpr uint32_t kCategoriaDe[5] = {0, kGpuSombras, kGpuEscena, kGpu320,
                                                   kGpuEscenaSinProfundidad};
      if (categoria_tijera <= 4 && categoria == kCategoriaDe[categoria_tijera]) {
        tijera_final.extent = {1, 1};
      }
    }
    /*
     * Record how far down the game draws in this render target. It only grows, so the pass is never opened
     * smaller than what has already been seen drawn.
     */
    if (area_util_) {
      const uint32_t hasta = uint32_t(std::max(0, tijera.offset.y + int32_t(tijera.extent.height)));
      const uint32_t redondeado = (hasta + 63u) & ~63u;
      uint32_t& apuntado = AltoUtilDe(pitch);  // without a map lookup on every draw
      if (redondeado > apuntado) {
        apuntado = redondeado;
      }
    }
    const float mezcla_constante[4] = {Flotante(r[gr::XE_GPU_REG_RB_BLEND_RED]),
                                       Flotante(r[gr::XE_GPU_REG_RB_BLEND_GREEN]),
                                       Flotante(r[gr::XE_GPU_REG_RB_BLEND_BLUE]),
                                       Flotante(r[gr::XE_GPU_REG_RB_BLEND_ALPHA])};
    const uint32_t stencil_frente = r[gr::XE_GPU_REG_RB_STENCILREFMASK];
    const uint32_t stencil_dorso = ((control_profundidad >> 7) & 0x1)
                                       ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]
                                       : stencil_frente;
    /*
     * The sky, deferred until after the opaque draws (nfsmw_nativo_cielo_aplazado).
     *
     * How it is recognized, and why it cannot be mistaken for another draw. Three conditions at once, none of
     * them a magic position or draw count:
     *
     *  1. The pixel shader is the sky's, by the fingerprint of its original container (kHuellaCielo). It is
     *     the only one of the library's 152 shaders whose constant table names CloudIntensity and
     *     SkyAlphaTag. The fingerprint does not depend on the SPIR-V translation or on the library order.
     *  2. The draw writes color and is opaque on all its targets: blending is "1 x source + 0 x destination"
     *     (ADD) on the channels the mask writes, exactly the criterion PipelineDe uses to decide
     *     blendEnable. Opaque = it does not read the existing color, so whatever was painted before it does
     *     not matter.
     *  3. It tests depth and does not write it, and there is no stencil. Without a Z write, deferring it
     *     cannot change what later draws see; with the test on, in its new place the Z test discards it
     *     where the world used to overwrite it.
     *
     * If any of the three fails (another version of the game, another state) the draw takes the usual path
     * and nothing happens.
     *
     * It is not deferred while a game occlusion query is open: the game reads those samples and moving it
     * would change the count (the same guard the early Z uses).
     */
    // A draw with no color to write does not read the target: it does not count as transparent.
    bool opaco_en_todos = true;
    for (uint32_t i = 0; i < 4 && opaco_en_todos; ++i) {
      const uint32_t mascara_i = (mascaras >> (i * 4)) & 0xF;
      if (!mascara_i) {
        continue;
      }
      const uint32_t m = clave.mezcla[i];
      const bool color_directo = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool alfa_directo =
          ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      const bool mezcla_encendida =
          (((mascara_i & 0x7) != 0) && !color_directo) || (((mascara_i & 0x8) != 0) && !alfa_directo);
      if (mezcla_encendida) {
        opaco_en_todos = false;
      }
    }
    const bool prueba_z_dibujo = ((control_profundidad >> 1) & 0x1) != 0;
    const bool escribe_z_dibujo = prueba_z_dibujo && ((control_profundidad >> 2) & 0x1) != 0 &&
                                  claves[4] != 0;
    bool es_cielo = false;
    if (ps && ps->shader && ps->shader->huella == kHuellaCielo && mascaras) {
      ++cielo_vistos_;
      const bool estencil = (control_profundidad & 0x1) != 0;
      /*
       * Scene pass only. The same dome is also drawn in the car's cubemap and in the reflection, and there it
       * is not touched: the rear-view mirror works and is left alone, and the measured saving (4.8 of the
       * scene's 19.2 ms) is the scene's. This line is what keeps the cubemap out of it.
       */
      const bool en_la_escena = CategoriaDeDestino(pitch, claves) == kGpuEscena;
      /*
       * The fingerprint is not enough, and that broke an earlier version.
       *
       * The fingerprint identifies the shader, not the draw. The counter said so: "7,00 detectados por
       * fotograma" when the analysis expected 0.9. Seven draws use that pixel shader, and deferring one broke
       * the order of the other six: flickering and odd colors.
       *
       * Now it must also be the dome:
       *   - the index count measured by the analysis of the SPIR-V and the log: 480 = 160 triangles;
       *   - and it must be the first draw of the pass (the dome is painted on the empty Z-buffer, which is
       *     exactly why it shades the whole screen and is then covered).
       *
       * Both are cheap to check and both belong to the dome, not to the shader. If the counter still said
       * more than one per frame, nothing is deferred: there is a guard below.
       */
      const uint32_t indices_dibujo =
          uint32_t(indices_de_16 ? indices16_.size() : indices_.size());
      const bool geometria_de_domo = indices_dibujo == kIndicesDomoCielo;
      const bool primero_del_pase = dibujos_en_pase_ == 0;
      /* The full criterion, without the cvar or the guard: what is measured during the test. */
      const bool es_el_domo = en_la_escena && opaco_en_todos && prueba_z_dibujo &&
                              !escribe_z_dibujo && !estencil && !(ps->salidas & 0x10) &&
                              !oclusion_abierta_ && geometria_de_domo && primero_del_pase;
      if (es_el_domo) {
        ++cielo_candidatos_;  // the ones matching the dome's marks: this must be 1
        /*
         * The guard counts here, whether it defers or not. Two in the same frame is exactly what broke the
         * image before, so as soon as it happens it switches off and stays off for the whole session.
         */
        if (++cielo_guardia_en_fotograma_ > 1 && cielo_guardia_ != kCieloDescartado) {
          cielo_guardia_ = kCieloDescartado;
          REXLOG_WARN("[nativo] C6 cielo: la guardia ve {} domos en el MISMO fotograma; no se aplaza "
                      "nada en toda la sesion (es el fallo de la 137). La imagen queda intacta",
                      cielo_guardia_en_fotograma_);
        }
      }
      es_cielo = cielo_aplazado_ && es_el_domo && cielo_guardia_ == kCieloAplazando;
      if (!es_cielo && cielo_aplazado_) {
        ++cielo_no_aplazables_;  // only with the setting on: measures criterion failures, not the cvar
      }
    }
    /*
     * Which draws the sky can be moved past, and why that does not change a single pixel.
     *
     * The sky can only skip past a draw D if both hold:
     *
     *  - D is opaque: it does not read the existing color, so wherever D paints, the final color is its own
     *    whether or not the sky was underneath. A draw with blending does read the background: the sky goes
     *    first.
     *  - D writes depth: after D the Z-buffer holds D's z, which is closer than the dome, so the sky's LEQUAL
     *    test discards it exactly where D painted. If D did not write Z, the Z-buffer would stay as it was
     *    and the sky would be painted over D: that would change the image.
     *
     * So the sky is emitted as soon as the first draw that fails either condition arrives. In practice that
     * is the first transparent draw, because what follows the sky is the opaque world; the counter separates
     * the two reasons so this can be checked in the log.
     */
    if (cielo_pendiente_) {
      if (es_cielo) {
        ++cielo_dos_en_pase_;  // another sky in the same pass: emit the earlier one now, keeping the order
        EmitirCieloAplazado(cmd, kCieloPorOtroCielo);
      } else if (!(opaco_en_todos && escribe_z_dibujo)) {
        EmitirCieloAplazado(cmd, opaco_en_todos ? kCieloPorSinZ : kCieloPorMezcla);
      }
    }
    // --- Grabar ---------------------------------------------------------------------------------
    if (es_cielo && lee_reflejo) {
      nfsmw::reflejo_demanda::AnotarVisible(false);  // the deferred sky is not measured
    }
    if (es_cielo) {
      // The arguments of the vkCmd* calls this draw would have emitted are saved; nothing is recorded. The
      // upload buffer is an allocator that only moves forward (Reservar) and is not reset until UsarRanura,
      // which always comes after AntesDeEnviar -> TerminarPase: when the sky is emitted, its offsets still
      // point to the same data.
      CieloAplazado& c = cielo_;
      c.pipeline = pipeline;
      c.bufer = subida_;
      c.usa_ubo = usar_ubo_;
      c.push[0] = subida_direccion_ + constantes_vs_offset_;
      c.push[1] = subida_direccion_ + constantes_ps_offset_;
      c.push[2] = subida_direccion_ + offset_compartidas;
      c.offsets_ubo = usar_ubo_ ? std::array<uint32_t, 3>{uint32_t(constantes_vs_offset_),
                                                          uint32_t(constantes_ps_offset_),
                                                          uint32_t(offset_compartidas)}
                                : std::array<uint32_t, 3>{0, 0, 0};
      c.ranura_ubo = ranura_actual_;
      c.viewport = viewport;
      c.tijera = tijera_final;
      std::memcpy(c.mezcla, mezcla_constante, sizeof(c.mezcla));
      std::memcpy(c.sesgo, sesgo, sizeof(c.sesgo));
      c.con_estencil = claves[4] != 0;
      c.stencil[0] = stencil_frente;
      c.stencil[1] = stencil_dorso;
      c.n_enlaces = uint32_t(entrada->enlaces.size());
      c.offsets_vertices = offsets_vertices;
      c.con_indices = con_indices;
      c.indices_de_16 = indices_de_16;
      c.indices = uint32_t(indices_de_16 ? indices16_.size() : indices_.size());
      c.primer_indice = uint32_t(offset_indices / (indices_de_16 ? 2 : 4));
      c.vmin = vmin;
      c.cuenta = cuenta;
      c.ps_mas_uno = ps ? ps->numero + 1 : 0;
      c.categoria = CategoriaDeDestino(pitch, claves);
      c.dibujos_al_aplazar = dibujos_en_pase_;
      c.eds_modo = eds_modo_;  // phases 1 and 2: its pipeline lacks this state, so save it all
      if (eds_modo_) {
        EstadoEdsDe(clave, c.eds, eds_modo_);
      }
      cielo_pendiente_ = true;
      ++cielo_aplazados_;
      // Counted as if it had been recorded: it will be recorded before the pass closes.
      ++dibujos_por_categoria_[c.categoria];
      triangulos_por_categoria_[c.categoria] += (con_indices ? c.indices : cuenta) / 3;
      Etapa(6, marca);
      ++dibujados_;
      // dibujos_en_pase_ is not incremented here: this draw is not recorded yet. It is incremented in
      // EmitirCieloAplazado, and the difference with dibujos_al_aplazar gives the draws that went ahead of
      // it, which is exactly the geometry that now covers it.
      dibujados_cronometrados_ += cronometrar_ ? 1 : 0;
      return true;
    }
    if (pipeline != pipeline_enlazado_) {
      if (contar_cambios_pipeline_) {  // Measurement only (nfsmw_nativo_contar_cambios_pipeline)
        ContarCambioPipeline(clave, pipeline_enlazado_ == VK_NULL_HANDLE);
      }
      NFSMW_SUB(0, dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
      pipeline_enlazado_ = pipeline;
    }
    // Phases 1 and 2. The state that no longer goes in the pipeline, before the draw. If the guard sees a
    // difference, dynamic state switches off and this same draw is bound with its usual pipeline.
    if (eds_modo_ && !FijarEstadoDinamico(cmd, clave)) {
      NFSMW_SUB(11, pipeline = PipelineDe(ClaveDeBusqueda(clave), *entrada, p));
      if (pipeline == VK_NULL_HANDLE) {
        return false;
      }
      NFSMW_SUB(0, dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
      pipeline_enlazado_ = pipeline;
      clave_enlazada_valida_ = false;  // the counter does not classify this bind
    }
    if (!sets_enlazados_) {
      NFSMW_SUB(1, dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 4,
                                                sets_.data(), 0, nullptr));
      sets_enlazados_ = true;
    }
    // Dynamic state and push constants repeat a lot between consecutive draws: they are only recorded if
    // they change (recording: 0.4 us per draw in a race).
    const bool grabado = estado_grabado_;
    /*
     * With constants through UBOs, the push constants are dead.
     *
     * Every access in shader_common.h is `NFSMW_UBO ? conjunto 4 : g_PushConstants...`, and NFSMW_UBO is
     * specialization constant 1<<8, fixed when the pipeline is created: DXC folds the ternary and the shader
     * does not even reference g_PushConstants. Recording them meant recording something nobody reads.
     *
     * And it is not free: on Maxwell B, NVK has no hardware root table (nvk_use_hw_root_table requires
     * Turing), so each vkCmdPushConstants is a LOAD_CONSTANT_BUFFER_OFFSET plus the array, emitted on the
     * spot. The UBO is active in 100 % of the measured submissions.
     */
    if (!usar_ubo_) {
      const uint64_t push[3] = {subida_direccion_ + constantes_vs_offset_,
                                subida_direccion_ + constantes_ps_offset_,
                                subida_direccion_ + offset_compartidas};
      if (!grabado || std::memcmp(push, push_grabado_, sizeof(push)) != 0) {
        dfn_.vkCmdPushConstants(cmd, layout_pipeline_,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(push), push);
        std::memcpy(push_grabado_, push, sizeof(push));
      }
    }
    // Set 4 (dynamic UBOs). Always bound, because the new shaders use it statically: with the real offsets
    // if nfsmw_nativo_constantes_ubo is set, and 0 otherwise. Only recorded when something changes.
    {
      const std::array<uint32_t, 3> offsets_ubo =
          usar_ubo_ ? std::array<uint32_t, 3>{uint32_t(constantes_vs_offset_), uint32_t(constantes_ps_offset_),
                                              uint32_t(offset_compartidas)}
                    : std::array<uint32_t, 3>{0, 0, 0};
      ++set4_dibujos_;  // C6 set 4 report
      if (!ubo_enlazado_ || ranura_ubo_enlazada_ != ranura_actual_ || offsets_ubo != offsets_ubo_enlazados_) {
        // Which offsets change at each bind (measurement only). What NVK saves by differences depends on it:
        // every cbuf whose offset does not change is one rebind less in the Draw.
        if (!ubo_enlazado_ || ranura_ubo_enlazada_ != ranura_actual_) {
          ++set4_primeros_;
        } else {
          ++set4_cambios_[(offsets_ubo[0] != offsets_ubo_enlazados_[0] ? 1u : 0u) |
                          (offsets_ubo[1] != offsets_ubo_enlazados_[1] ? 2u : 0u) |
                          (offsets_ubo[2] != offsets_ubo_enlazados_[2] ? 4u : 0u)];
        }
        NFSMW_SUB(2, dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 4, 1,
                                                  &sets_ubo_[ranura_actual_], 3, offsets_ubo.data()));
        offsets_ubo_enlazados_ = offsets_ubo;
        ranura_ubo_enlazada_ = ranura_actual_;
        ubo_enlazado_ = true;
      }
    }
    if (!grabado || std::memcmp(&viewport, &viewport_grabado_, sizeof(viewport)) != 0) {
      NFSMW_SUB(3, dfn_.vkCmdSetViewport(cmd, 0, 1, &viewport));
      viewport_grabado_ = viewport;
    }
    if (!grabado || std::memcmp(&tijera_final, &tijera_grabada_, sizeof(tijera_final)) != 0) {
      NFSMW_SUB(4, dfn_.vkCmdSetScissor(cmd, 0, 1, &tijera_final));
      tijera_grabada_ = tijera_final;
    }
    if (!grabado ||
        std::memcmp(mezcla_constante, mezcla_grabada_, sizeof(mezcla_constante)) != 0) {
      NFSMW_SUB(5, dfn_.vkCmdSetBlendConstants(cmd, mezcla_constante));
      std::memcpy(mezcla_grabada_, mezcla_constante, sizeof(mezcla_constante));
    }
    // Also without bias: the state is dynamic in every pipeline and has to be recorded.
    if (!grabado || std::memcmp(sesgo, sesgo_grabado_, sizeof(sesgo)) != 0) {
      NFSMW_SUB(6, dfn_.vkCmdSetDepthBias(cmd, sesgo[0], 0.0f, sesgo[1]));
      std::memcpy(sesgo_grabado_, sesgo, sizeof(sesgo));
    }
    if (!grabado) {
      stencil_grabado_valido_ = false;
      tipo_indices_grabado_ = VK_INDEX_TYPE_MAX_ENUM;
    }
    if (claves[4]) {
      const uint32_t frente = stencil_frente;  // computed before the recording block
      const uint32_t dorso = stencil_dorso;
      if (!stencil_grabado_valido_ || frente != stencil_grabado_[0] ||
          dorso != stencil_grabado_[1]) {
        NFSMW_SUB(7, {
          dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, frente & 0xFF);
          dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, dorso & 0xFF);
          dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (frente >> 8) & 0xFF);
          dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (dorso >> 8) & 0xFF);
          dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (frente >> 16) & 0xFF);
          dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (dorso >> 16) & 0xFF);
        });
        stencil_grabado_[0] = frente;
        stencil_grabado_[1] = dorso;
        stencil_grabado_valido_ = true;
      }
    }
    estado_grabado_ = true;
    /*
     * nfsmw_nativo_vertices_base_cero. With a single binding whose copy starts at a multiple of its stride,
     * the upload buffer is bound at 0 (once per command buffer, or after the deferred sky) and the draw is
     * shifted with primer_vertice. The others bind as usual.
     */
    bool base_cero = false;
    uint32_t primer_vertice = 0;  // offset / stride: where the copy starts, in vertices
    if (vertices_base_cero_ && entrada->enlaces.size() == 1) {
      const VkDeviceSize zancada = entrada->enlaces[0].zancada;
      if (offsets_vertices[0] % zancada == 0) {
        base_cero = ComprobarBaseCero(offsets_vertices[0], zancada, origenes[0].bytes, vmin, primer_vertice);
      } else {
        ++base_cero_desalineados_;
      }
    } else if (entrada->enlaces.size() > 1) {
      ++base_cero_varios_enlaces_;
    }
    if (!entrada->enlaces.empty()) {
      // 16 identical pointers were filled in on every draw to bind one or two. It is filled when the upload
      // buffer changes (once per work slot), not 2,345 times per frame.
      if (bufers_vertices_[0] != subida_) {
        bufers_vertices_.fill(subida_);
        enlaces_grabados_ = 0;  // the buffer changed: earlier bindings are stale
      }
      /*
       * And do not bind again if it is exactly what is already bound.
       *
       * In NVK each binding is 5 dwords plus an invocation of the MME macro NVK_MME_BIND_VB
       * (nvk_cmd_draw.c:4658), with no redundancy check inside: ~2,800 MME macros per frame. With vertex
       * deduplication 28-33 % of the bindings reuse the same offset, so two consecutive draws of the same
       * mesh give exactly the same offsets.
       */
      // On the base-zero path what gets bound is the whole buffer, from 0.
      static constexpr VkDeviceSize kEnlaceEnCero = 0;
      const VkDeviceSize* a_enlazar = base_cero ? &kEnlaceEnCero : offsets_vertices.data();
      const uint32_t n_enlaces = uint32_t(entrada->enlaces.size());
      bool igual = n_enlaces == enlaces_grabados_;
      for (uint32_t i = 0; igual && i < n_enlaces; ++i) {
        igual = a_enlazar[i] == offsets_grabados_[i];
      }
      if (!igual) {
        NFSMW_SUB(8, dfn_.vkCmdBindVertexBuffers(cmd, 0, n_enlaces, bufers_vertices_.data(), a_enlazar));
        ++base_cero_enlaces_grabados_;
        enlaces_grabados_ = n_enlaces;
        for (uint32_t i = 0; i < n_enlaces; ++i) {
          offsets_grabados_[i] = a_enlazar[i];
        }
      } else {
        ++enlaces_ahorrados_;
      }
      ++base_cero_total_;
      base_cero_dibujos_ += base_cero ? 1 : 0;
    }
    // With a game occlusion query open, this draw counts toward it.
    if (oclusion_abierta_ && consulta_oclusion_ == UINT32_MAX) {
      NFSMW_SUB(13, consulta_oclusion_ = contexto_->EmpezarConsultaOclusion());
    }
    // The first 12, then one every 2 s (up to 150), to follow the sun over the course of a race.
    if (oclusion_abierta_ && pitch >= 640 && avisos_oclusion_dibujo_ < 16 &&
        (avisos_oclusion_dibujo_ < 12 ||
         std::chrono::steady_clock::now() - ultimo_aviso_oclusion_dibujo_ >= std::chrono::seconds(2))) {
      ++avisos_oclusion_dibujo_;
      ultimo_aviso_oclusion_dibujo_ = std::chrono::steady_clock::now();
      REXLOG_INFO("[nativo] C2 oclusion dibujo {}: consulta del host {}: VS n{} PS n{} tipo {} cuenta {} pitch {} "
                  "mascaras {:08X} profundidad {:08X} (escribe {}, prueba {}, funcion z {}) viewport {:.1f},{:.1f} "
                  "{:.1f}x{:.1f} z {:.3f}-{:.3f} tijera {},{} {}x{} con indices {}",
                  avisos_oclusion_dibujo_, consulta_oclusion_ == UINT32_MAX ? -1 : int64_t(consulta_oclusion_),
                  p.vs->numero, ps ? int(ps->numero) : -1, tipo, cuenta, pitch, mascaras, control_profundidad,
                  (control_profundidad >> 2) & 0x1, (control_profundidad >> 1) & 0x1, (control_profundidad >> 4) & 0x7,
                  viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth,
                  tijera.offset.x, tijera.offset.y, tijera.extent.width, tijera.extent.height, con_indices);
      // Diagnostic: positions of the first vertices and VS constants c0-c6, to compute the sun's z against
      // the scene depth.
      std::string posiciones;
      for (const AtributoVertices& a : entrada->atributos) {
        if (a.ubicacion != 0) {
          continue;
        }
        const Origen& origen = origenes[a.enlace];
        const uint32_t zancada = entrada->enlaces[a.enlace].zancada;
        for (uint32_t v = 0; v < std::min<uint32_t>(cuenta, 4); ++v) {
          const uint64_t desde = uint64_t(v) * zancada + a.offset;
          if (desde + 16 > origen.bytes) {
            break;
          }
          posiciones += " (";
          for (uint32_t k = 0; k < 4; ++k) {
            uint32_t palabra;
            std::memcpy(&palabra, origen.datos + desde + k * 4, 4);
            posiciones += fmt::format("{}{:.7g}", k ? "," : "", Flotante(xenos::GpuSwap(palabra, origen.orden)));
          }
          posiciones += ")";
        }
        posiciones += fmt::format(" formato {} zancada {}", int(a.formato), zancada);
        break;
      }
      std::string constantes;
      for (uint32_t k = 0; k < 7; ++k) {
        constantes += fmt::format(" c{}=(", k);
        for (uint32_t c = 0; c < 4; ++c) {
          constantes += fmt::format("{}{:.7g}", c ? "," : "", Flotante(r[kRegConstantesVs + k * 4 + c]));
        }
        constantes += ")";
      }
      REXLOG_INFO("[nativo] C2 oclusion dibujo {} (geometria): VTE {:08X} CLIP {:08X} sc {:08X} RB_DEPTH_INFO {:08X} "
                  "pase {}x{} ndc ({:.5g},{:.5g},{:.5g},{:.5g}) funcion z del pipeline {} vmin {}; posiciones{}; "
                  "constantes VS{}",
                  avisos_oclusion_dibujo_, vte, r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], modo_sc,
                  r[gr::XE_GPU_REG_RB_DEPTH_INFO], pase_ancho_, pase_alto_, ndc[0], ndc[1], ndc[2], ndc[3],
                  (clave.profundidad >> 4) & 0x7, vmin, posiciones, constantes);
    }
    // Draws and triangles per render target type (same classification as the GPU time).
    {
      const uint32_t categoria = CategoriaDeDestino(pitch, claves);
      const uint32_t vertices_dibujo = con_indices ? uint32_t(indices_de_16 ? indices16_.size() : indices_.size())
                                                   : cuenta;
      ++dibujos_por_categoria_[categoria];
      triangulos_por_categoria_[categoria] += vertices_dibujo / 3;
    }
    // In the diagnostic frame, one query per draw with its pixel shader.
    // The diagnostic window is decided by the context, at the Swap. With the diagnostic off (the normal
    // case) this does not even reach the call.
    const uint32_t consulta_dibujo =
        !diag_estadisticas_dibujo_
            ? UINT32_MAX
            : contexto_->EmpezarEstadisticasDibujo(
                  ps ? ps->numero + 1 : 0,
                  CategoriaDeDestino(pitch, claves));
    /*
     * nfsmw_reflejo_visibilidad. An occlusion query around this draw alone, if it samples the reflection:
     * 0 samples = it wrote nothing to any target. Two of the same type cannot be open at once, so while a
     * game query is open it is not measured and counts as visible. Every kTestigoCada frames the final
     * composite, which paints the whole screen, is measured the same way: it must produce samples (the
     * guard, in nfsmw_recortes_carrera.cpp).
     */
    uint32_t consulta_visibilidad = UINT32_MAX;
    if (lee_reflejo) {
      if (!oclusion_abierta_ && consulta_oclusion_ == UINT32_MAX) {
        consulta_visibilidad = contexto_->EmpezarConsultaVisibilidad(false);
      }
      if (consulta_visibilidad == UINT32_MAX) {
        nfsmw::reflejo_demanda::AnotarVisible(false);
      }
    } else if (testigo_pendiente_ && ps && ps->shader && ps->shader->huella == kHuellaComposicion &&
               !oclusion_abierta_ && consulta_oclusion_ == UINT32_MAX) {
      consulta_visibilidad = contexto_->EmpezarConsultaVisibilidad(true);
      if (consulta_visibilidad != UINT32_MAX) {
        testigo_pendiente_ = false;
        // While the first 8 are checked, one every 2 frames: the guard finishes in ~0.5 s of racing.
        testigo_siguiente_ =
            fotograma_ + (nfsmw::reflejo_demanda::VisibilidadComprobada() ? kTestigoCada : uint64_t(2));
      }
    }
    if (con_indices) {
      // The upload buffer is bound once per index type and each draw uses firstIndex.
      const VkIndexType tipo_indices = indices_de_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      if (tipo_indices != tipo_indices_grabado_) {
        NFSMW_SUB(9, dfn_.vkCmdBindIndexBuffer(cmd, subida_, 0, tipo_indices));
        tipo_indices_grabado_ = tipo_indices;
      }
      // On the base-zero path, vertexOffset also carries where the copy starts (in vertices).
      const int32_t desplazamiento_vertices =
          base_cero ? int32_t(int64_t(primer_vertice) - int64_t(vmin)) : -int32_t(vmin);
      NFSMW_SUB(10, dfn_.vkCmdDrawIndexed(cmd, uint32_t(indices_de_16 ? indices16_.size() : indices_.size()),
                                          1, uint32_t(offset_indices / (indices_de_16 ? 2 : 4)),
                                          desplazamiento_vertices, 0));
    } else {
      NFSMW_SUB(10, dfn_.vkCmdDraw(cmd, cuenta, 1, base_cero ? primer_vertice : 0, 0));  // firstVertex
    }
    if (consulta_visibilidad != UINT32_MAX) {
      contexto_->TerminarConsultaVisibilidad(consulta_visibilidad);
    }
    if (consulta_dibujo != UINT32_MAX) {
      contexto_->TerminarEstadisticasDibujo(consulta_dibujo);
    }
    Etapa(6, marca);
    if (cronometrar_) {
      ++sub_muestras_;
      InformeSubetapas();
    }
    ++dibujados_;
    ++dibujos_en_pase_;  // to know at which position of the pass the sky ends up emitted
    dibujados_cronometrados_ += cronometrar_ ? 1 : 0;
    return true;
  }

  /*
   * Emits the deferred sky draw (nfsmw_nativo_cielo_aplazado).
   *
   * It records exactly the same vkCmd* calls it would have recorded in its original place, with the
   * values saved then. It relies on nothing recorded afterwards: it sends all of this draw's dynamic
   * state, and when done it invalidates the tracked state so the next draw records its own again. That
   * way the order cannot slip through an "already set" comparison.
   *
   * The upload buffer offsets are still valid because Reservar only moves forward and the buffer is not
   * reset until UsarRanura, which always comes after AntesDeEnviar -> TerminarPase.
   */
  void EmitirCieloAplazado(VkCommandBuffer cmd, uint32_t motivo) {
    if (!cielo_pendiente_ || cmd == VK_NULL_HANDLE) {
      return;
    }
    cielo_pendiente_ = false;
    const CieloAplazado& c = cielo_;
    ++cielo_emitidos_[motivo < kCieloMotivos ? motivo : kCieloMotivos - 1];
    const uint64_t posicion = dibujos_en_pase_ - c.dibujos_al_aplazar;
    cielo_posicion_suma_ += posicion;
    cielo_posicion_max_ = std::max(cielo_posicion_max_, posicion);
    dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipeline);
    pipeline_enlazado_ = c.pipeline;
    if (c.eds_modo) {  // phases 1 and 2: all the state its pipeline lacks
      EmitirEstadoDinamico(cmd, c.eds, true, c.eds_modo);
    }
    eds_valido_ = false;  // the next draw sets all of its own again
    clave_enlazada_valida_ = false;  // The sky does not keep its key (ContarCambioPipeline)
    if (!sets_enlazados_) {
      dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 4,
                                   sets_.data(), 0, nullptr);
      sets_enlazados_ = true;
    }
    if (!c.usa_ubo) {
      dfn_.vkCmdPushConstants(cmd, layout_pipeline_,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(c.push), c.push);
    }
    dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 4, 1,
                                 &sets_ubo_[c.ranura_ubo], 3, c.offsets_ubo.data());
    dfn_.vkCmdSetViewport(cmd, 0, 1, &c.viewport);
    dfn_.vkCmdSetScissor(cmd, 0, 1, &c.tijera);
    dfn_.vkCmdSetBlendConstants(cmd, c.mezcla);
    dfn_.vkCmdSetDepthBias(cmd, c.sesgo[0], 0.0f, c.sesgo[1]);
    if (c.con_estencil) {
      dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, c.stencil[0] & 0xFF);
      dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, c.stencil[1] & 0xFF);
      dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (c.stencil[0] >> 8) & 0xFF);
      dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (c.stencil[1] >> 8) & 0xFF);
      dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (c.stencil[0] >> 16) & 0xFF);
      dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (c.stencil[1] >> 16) & 0xFF);
    }
    if (c.n_enlaces) {
      std::array<VkBuffer, 16> bufers{};
      bufers.fill(c.bufer);
      dfn_.vkCmdBindVertexBuffers(cmd, 0, c.n_enlaces, bufers.data(), c.offsets_vertices.data());
    }
    const uint32_t consulta = !diag_estadisticas_dibujo_
                                  ? UINT32_MAX
                                  : contexto_->EmpezarEstadisticasDibujo(c.ps_mas_uno, c.categoria);
    if (c.con_indices) {
      const VkIndexType tipo = c.indices_de_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      dfn_.vkCmdBindIndexBuffer(cmd, c.bufer, 0, tipo);
      dfn_.vkCmdDrawIndexed(cmd, c.indices, 1, c.primer_indice, -int32_t(c.vmin), 0);
    } else {
      dfn_.vkCmdDraw(cmd, c.cuenta, 1, 0, 0);
    }
    if (consulta != UINT32_MAX) {
      contexto_->TerminarEstadisticasDibujo(consulta);
    }
    ++dibujos_en_pase_;
    // Nothing tracked is valid any more: the dynamic state, the bindings and the index type are the sky's.
    // estado_grabado_ = false makes the next draw also re-record stencil and indices.
    estado_grabado_ = false;
    ubo_enlazado_ = false;
    enlaces_grabados_ = 0;
    stencil_grabado_valido_ = false;
    tipo_indices_grabado_ = VK_INDEX_TYPE_MAX_ENUM;
  }

  // Adds the time since the mark to the stage and moves the mark to now.
  /*
   * Measurement only. What takes the time inside the ring's grabar and pipeline stages (4.1 and 2.5 us per
   * recorded draw): each Vulkan command, PipelineDe and the shared constants block, on the same timed
   * draws (1 in 64). "C6 subetapas" line every 10 s: us per draw, us per call and calls per timed draw.
   */
  // Its lines, like those of the other ring reports, go to the report thread (NFSMW_INFORME_ANILLO).
  void InformeSubetapas() {
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora < sub_siguiente_) {
      return;
    }
    const bool primera = sub_siguiente_ == std::chrono::steady_clock::time_point{};
    sub_siguiente_ = ahora + std::chrono::seconds(10);
    if (primera || !sub_muestras_) {
      sub_ns_.fill(0);
      sub_n_.fill(0);
      sub_muestras_ = 0;
      return;
    }
    static constexpr const char* kNombres[19] = {
        "BindPipeline", "sets 0-3", "set 4 (UBO)", "Viewport", "Scissor", "BlendConstants", "DepthBias",
        "Stencil (6)", "BindVertexBuffers", "BindIndexBuffer", "Draw", "PipelineDe", "compartidas",
        "oclusion", "samplers (etapa texturas)",
        // The four segments of the "indices" stage (CortarSubetapa)
        "etapa indices: destinos y descartes", "etapa indices: desplazamiento de profundidad",
        "etapa indices: indices", "etapa indices: vertices y diagnostico"};
    std::string linea;
    double total = 0.0;
    for (size_t k = 0; k < 19; ++k) {
      if (!sub_n_[k]) {
        continue;
      }
      const double por_dibujo = double(sub_ns_[k]) / 1e3 / double(sub_muestras_);
      total += por_dibujo;
      linea += fmt::format(" | {} {:.2f} us/dibujo ({:.2f} us x {:.2f} por dibujo)", kNombres[k], por_dibujo,
                           double(sub_ns_[k]) / 1e3 / double(sub_n_[k]),
                           double(sub_n_[k]) / double(sub_muestras_));
    }
    NFSMW_INFORME_ANILLO("[nativo] C6 subetapas ({} dibujos cronometrados; suma {:.2f} us por dibujo){}", sub_muestras_,
                total, linea);
    sub_ns_.fill(0);
    sub_n_.fill(0);
    sub_muestras_ = 0;
  }

  /*
   * The base-zero path of a draw (nfsmw_nativo_vertices_base_cero). Returns false if it has to bind as
   * usual. Self-checking guard: the first kBaseCeroAComprobar draws, then 1 in 4,096, redo the computation
   * backwards in 64 bits (the first vertex the GPU reads falls on the first byte of the copy, with and
   * without indices), and check that vertexOffset fits in a signed 32-bit value and that the copy fits in
   * the buffer. On a single difference: that draw binds as usual, DIFERENCIA in the log and the path is off
   * for the session.
   */
  bool ComprobarBaseCero(VkDeviceSize offset, VkDeviceSize zancada, uint64_t bytes, uint32_t vmin,
                         uint32_t& primer_vertice) {
    const VkDeviceSize primero = offset / zancada;
    if (primero > VkDeviceSize(INT32_MAX)) {
      return false;  // does not fit in firstVertex/vertexOffset (cannot happen with a 64 MB buffer)
    }
    const uint64_t n = ++base_cero_comprobados_;
    if (n <= kBaseCeroAComprobar || (n & 4095) == 0) {
      const int64_t desplazamiento = int64_t(primero) - int64_t(vmin);  // the vertexOffset of indexed draws
      const bool bien = primero * zancada == offset &&
                        (desplazamiento + int64_t(vmin)) * int64_t(zancada) == int64_t(offset) &&
                        desplazamiento >= int64_t(INT32_MIN) && desplazamiento <= int64_t(INT32_MAX) &&
                        offset + bytes <= kTamanoSubida && offset + bytes <= subida_tamano_real_;
      if (!bien) {
        vertices_base_cero_apagado_ = true;
        vertices_base_cero_ = false;
        REXLOG_ERROR("[nativo] C6 vertices base cero: DIFERENCIA (offset {} zancada {} bytes {} vmin {} primero {}): "
                     "este dibujo y los demas de la sesion enlazan como siempre",
                     offset, zancada, bytes, vmin, primero);
        return false;
      }
      if (n == kBaseCeroAComprobar) {
        NFSMW_INFORME_ANILLO("[nativo] C6 vertices base cero: {} dibujos comprobados, 0 diferencias; sigue comprobando 1 de "
                    "cada 4096", n);
      }
    }
    primer_vertice = uint32_t(primero);
    return true;
  }

  /*
   * Set 4 by differences, once per submission (UsarRanura). Tells NVK whether it is wanted
   * (nfsmw_nativo_set4_diferencias and the test alternation) and, if the NVK guard has seen a DIFERENCIA,
   * reports it in the log as an error once: NVK has already switched it off for the rest of the session.
   */
  void ControlSet4() {
    bool pedido = REXCVAR_GET(nfsmw_nativo_set4_diferencias);
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_set4_diferencias_alternar_s);
    if (alternar > 0) {
      const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                             set4_inicio_alternancia_)
                                .count();
      pedido = (segundos / alternar) % 2 == 1;
    }
#if REX_PLATFORM_SWITCH
    NvkSwitchSet4* const nvk = &nvk_switch_set4;
    const bool con_parche = nvk != nullptr && nvk->version == 1;
#else
    const bool con_parche = false;
#endif
    if (pedido != set4_pedido_ || !set4_pedido_anotado_) {
      set4_pedido_ = pedido;
      set4_pedido_anotado_ = true;
      REXLOG_INFO("[nativo] C6 set 4 por diferencias (build 184): {} (fotograma {}){}",
                  pedido ? "pedido a NVK" : "NO pedido: NVK enlaza el conjunto entero", fotograma_,
                  con_parche ? "" : "; este NVK no tiene parche_nvk_set4 (Mesa anterior o PC): enlaza como siempre");
    }
#if REX_PLATFORM_SWITCH
    if (con_parche) {
      __atomic_store_n(&nvk->pedido, pedido ? 1 : 0, __ATOMIC_RELAXED);
      if (!set4_diferencia_avisada_ && __atomic_load_n(&nvk->apagado, __ATOMIC_RELAXED) != 0) {
        set4_diferencia_avisada_ = true;
        REXLOG_ERROR("[nativo] C6 set 4 por diferencias: DIFERENCIA vista por la guardia de NVK ({} cbufs mal "
                     "enlazados, cada uno arreglado en su dibujo; detalle en rex_stderr.log). Apagado para el resto "
                     "de la sesion: NVK enlaza el conjunto entero, como siempre",
                     __atomic_load_n(&nvk->total.diferencias, __ATOMIC_RELAXED));
      }
    }
#endif
  }

  /*
   * The draw path in NVK, once per submission (like ControlSet4). Tells NVK what to measure and which
   * improvements are wanted (they apply from the next command buffer) and, if an improvement's guard has
   * seen a DIFERENCIA, reports it in the log as an error once: NVK has already switched it off for the
   * rest of the session.
   */
  void ControlDibujoNvk() {
#if REX_PLATFORM_SWITCH
    NvkSwitchDibujo* const nvk = &nvk_switch_dibujo;
    if (nvk == nullptr || nvk->version != 1) {
      nvk_precarga_app_ = false;
      if (!nvk_dibujo_anotado_) {
        nvk_dibujo_anotado_ = true;
        REXLOG_INFO("[nativo] C6 NVK por dibujo (build 186): este NVK no tiene el contrato nvk_switch_dibujo (Mesa "
                    "anterior): sin medida ni mejoras");
      }
      return;
    }
    int32_t medir = std::max<int32_t>(0, REXCVAR_GET(nfsmw_nativo_nvk_medir));
    if (medir > 0) {
      medir = int32_t(std::bit_floor(uint32_t(medir)));  // NVK wants a power of 2
    }
    bool mejoras = true;
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_nvk_alternar_s);
    if (alternar > 0) {
      const auto segundos =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - nvk_dibujo_inicio_)
              .count();
      mejoras = (segundos / alternar) % 2 == 1;
    }
    const int32_t pedidos[5] = {mejoras && REXCVAR_GET(nfsmw_nativo_nvk_emision) ? 1 : 0,
                                mejoras && REXCVAR_GET(nfsmw_nativo_nvk_cbufs) ? 1 : 0,
                                mejoras && REXCVAR_GET(nfsmw_nativo_nvk_dinamico) ? 1 : 0,
                                0,  // the fast set 4 path (p05) is not applied
                                mejoras && REXCVAR_GET(nfsmw_nativo_nvk_precarga) ? 1 : 0};
    __atomic_store_n(&nvk->medir, medir, __ATOMIC_RELAXED);
    __atomic_store_n(&nvk->medir_fallos, REXCVAR_GET(nfsmw_nativo_nvk_medir_fallos) ? 1 : 0, __ATOMIC_RELAXED);
    for (size_t m = 0; m < 5; ++m) {
      __atomic_store_n(&nvk->mejoras[m].pedido, pedidos[m], __ATOMIC_RELAXED);
    }
    nvk_precarga_app_ = pedidos[4] != 0;
    if (!nvk_dibujo_anotado_ || mejoras != nvk_mejoras_pedidas_) {
      nvk_dibujo_anotado_ = true;
      nvk_mejoras_pedidas_ = mejoras;
      REXLOG_INFO("[nativo] C6 NVK por dibujo (build 186): mejoras {} (emision {}, cbufs {}, dinamico {}, precarga {}); "
                  "medida 1 de cada {} llamadas{}; entorno NVK_SWITCH_DIBUJO {} (fotograma {})",
                  mejoras ? "pedidas" : "NO pedidas", pedidos[0], pedidos[1], pedidos[2], pedidos[4], medir,
                  REXCVAR_GET(nfsmw_nativo_nvk_medir_fallos) ? " con los fallos de cache aparte" : "",
                  __atomic_load_n(&nvk->entorno, __ATOMIC_RELAXED), fotograma_);
    }
    static constexpr const char* kNombres[5] = {"emision", "cbufs", "dinamico", "set 4 rapido", "precarga"};
    for (size_t m = 0; m < 5; ++m) {
      if (!nvk_apagado_avisado_[m] && __atomic_load_n(&nvk->mejoras[m].apagado, __ATOMIC_RELAXED) != 0) {
        nvk_apagado_avisado_[m] = true;
        REXLOG_ERROR("[nativo] C6 NVK por dibujo: DIFERENCIA vista por la guardia de «{}» ({} comprobaciones "
                     "distintas; detalle en rex_stderr.log). Apagada para el resto de la sesion: NVK lo hace como "
                     "siempre",
                     kNombres[m], __atomic_load_n(&nvk->mejoras[m].diferencias, __ATOMIC_RELAXED));
      }
    }
#endif
  }

  /*
   * Every 10 s, the NVK figures. Per part, us per measured call and us per measured draw; per measured
   * draw, how much work it carries; per pipeline bind, how many state copies changed nothing; and per
   * improvement, uses, checks and differences. Totals are from command buffers that have already finished.
   */
  void InformeDibujoNvk() {
#if REX_PLATFORM_SWITCH
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - nvk_informe_ < std::chrono::seconds(10)) {
      return;
    }
    const bool primera = nvk_informe_ == std::chrono::steady_clock::time_point{};
    nvk_informe_ = ahora;
    NvkSwitchDibujo* const nvk = &nvk_switch_dibujo;
    if (nvk == nullptr || nvk->version != 1) {
      return;
    }
    uint64_t veces[16], ticks[16], cuentas[13], mejoras[5][4];
    for (size_t i = 0; i < 16; ++i) {
      const uint64_t v = __atomic_load_n(&nvk->partes[i].veces, __ATOMIC_RELAXED);
      const uint64_t t = __atomic_load_n(&nvk->partes[i].ticks, __ATOMIC_RELAXED);
      veces[i] = v - nvk_partes_previas_[i][0];
      ticks[i] = t - nvk_partes_previas_[i][1];
      nvk_partes_previas_[i][0] = v;
      nvk_partes_previas_[i][1] = t;
    }
    for (size_t i = 0; i < 13; ++i) {
      const uint64_t c = __atomic_load_n(&nvk->cuentas[i], __ATOMIC_RELAXED);
      cuentas[i] = c - nvk_cuentas_previas_[i];
      nvk_cuentas_previas_[i] = c;
    }
    for (size_t m = 0; m < 5; ++m) {
      const uint64_t c[4] = {__atomic_load_n(&nvk->mejoras[m].usos, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->mejoras[m].validadas, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->mejoras[m].diferencias, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->mejoras[m].sin_comprobar, __ATOMIC_RELAXED)};
      for (size_t k = 0; k < 4; ++k) {
        mejoras[m][k] = c[k] - nvk_mejoras_previas_[m][k];
        nvk_mejoras_previas_[m][k] = c[k];
      }
    }
    if (primera) {
      return;
    }
    const uint64_t tps = __atomic_load_n(&nvk->ticks_por_segundo, __ATOMIC_RELAXED);
    const double us_tick = tps ? 1e6 / double(tps) : 0.0;
    const uint64_t dibujos = cuentas[0];  // NVK_SW_C_DIBUJOS_MEDIDOS
    // us per measured call and, in brackets, us per measured draw.
    const auto parte = [&](size_t i) {
      return fmt::format("{:.2f} [{:.2f}]", veces[i] ? double(ticks[i]) * us_tick / double(veces[i]) : 0.0,
                         dibujos ? double(ticks[i]) * us_tick / double(dibujos) : 0.0);
    };
    const auto por_dibujo = [&](size_t i) { return dibujos ? double(cuentas[i]) / double(dibujos) : 0.0; };
    if (dibujos) {
      NFSMW_INFORME_ANILLO(
          "[nativo] C6 NVK por partes (build 186; us por llamada medida [us por dibujo medido]; {} dibujos medidos): "
          "dibujo entero {} | push desc {} | dinamico {} | toque shaders {} | shaders {} | cbufs {} | emitir {} | "
          "BindPipeline: shaders {} toque {} copia del estado {} | sets {} (raiz {} ensuciar {}) | tabla raiz {} | "
          "trozo nuevo {} | BindVertexBuffers {}",
          dibujos, parte(0), parte(1), parte(2), parte(3), parte(4), parte(5), parte(6), parte(7), parte(8),
          parte(9), parte(10), parte(11), parte(12), parte(13), parte(14), parte(15));
      NFSMW_INFORME_ANILLO(
          "[nativo] C6 NVK por dibujo medido: {:.1f} dwords; {:.2f} con estado dinamico sucio ({:.1f} bits); {:.2f} con "
          "shaders sucios; {:.2f} cbufs reenlazados | pipelines: {} enlaces medidos, {:.1f} % copias que no cambiaron "
          "nada, {:.2f} bits nuevos por enlace | {} subidas a la tabla raiz ({:.1f} palabras cada una), {} trozos nuevos",
          por_dibujo(1), por_dibujo(2), cuentas[2] ? double(cuentas[3]) / double(cuentas[2]) : 0.0, por_dibujo(4),
          por_dibujo(5), cuentas[6], cuentas[6] ? 100.0 * double(cuentas[7]) / double(cuentas[6]) : 0.0,
          cuentas[6] ? double(cuentas[8]) / double(cuentas[6]) : 0.0, cuentas[10],
          cuentas[10] ? double(cuentas[11]) / double(cuentas[10]) : 0.0, cuentas[12]);
    }
    static constexpr size_t kMostradas[4] = {0, 1, 2, 4};  // without the fast set 4 (p05), which is not applied
    static constexpr const char* kNombres[5] = {"emision", "cbufs", "dinamico", "set 4 rapido", "precarga"};
    std::string texto;
    for (const size_t m : kMostradas) {
      const char* estado = __atomic_load_n(&nvk->mejoras[m].apagado, __ATOMIC_RELAXED) != 0 ? "APAGADA por la guardia"
                           : __atomic_load_n(&nvk->mejoras[m].pedido, __ATOMIC_RELAXED) == 0 ? "no pedida"
                                                                                              : "encendida";
      texto += fmt::format(" | {} ({}): {} usos, {} comprobadas iguales, {} distintas, {} sin comprobar", kNombres[m],
                           estado, mejoras[m][0], mejoras[m][1], mejoras[m][2], mejoras[m][3]);
    }
    NFSMW_INFORME_ANILLO("[nativo] C6 NVK mejoras (build 186, ultimos 10 s){}", texto);
#endif
  }

  /*
   * Every 10 s, set 4. What changes at each bind (measured here, with or without the NVK patch) and, if
   * NVK has it, what is saved: root table writes per bind (4 without it), cbufs not rebound and the state
   * of its guard.
   */
  void InformeSet4() {
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - set4_informe_ < std::chrono::seconds(10)) {
      return;
    }
    const bool primera = set4_informe_ == std::chrono::steady_clock::time_point{};
    set4_informe_ = ahora;
    uint64_t enlaces = set4_primeros_;
    for (const uint64_t n : set4_cambios_) {
      enlaces += n;
    }
    if (!primera && set4_dibujos_ && enlaces) {
      const auto pct = [enlaces](uint64_t n) { return 100.0 * double(n) / double(enlaces); };
      NFSMW_INFORME_ANILLO(
          "[nativo] C6 set 4: {:.2f} enlaces por dibujo ({} en {} dibujos); cambia solo VS {:.1f} %, solo PS "
          "{:.1f} %, solo compartidas {:.1f} %, VS+PS {:.1f} %, VS+compartidas {:.1f} %, PS+compartidas {:.1f} %, "
          "las tres {:.1f} %, tras bufer, ranura o cielo {:.1f} % | constantes subidas otra vez: VS {} por "
          "generacion, {} por epoca y {} porque el shader lee mas; PS {}, {} y {}",
          double(enlaces) / double(set4_dibujos_), enlaces, set4_dibujos_, pct(set4_cambios_[1]),
          pct(set4_cambios_[2]), pct(set4_cambios_[4]), pct(set4_cambios_[3]), pct(set4_cambios_[5]),
          pct(set4_cambios_[6]), pct(set4_cambios_[7]), pct(set4_primeros_), resubidas_vs_[0], resubidas_vs_[1],
          resubidas_vs_[2], resubidas_ps_[0], resubidas_ps_[1], resubidas_ps_[2]);
#if REX_PLATFORM_SWITCH
      if (NvkSwitchSet4* const nvk = &nvk_switch_set4; nvk != nullptr && nvk->version == 1) {
        const uint64_t cuentas[9] = {__atomic_load_n(&nvk->total.enlaces_diferencia, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.enlaces_completos, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.escrituras_raiz, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.dwords_raiz, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.cbufs_sucios, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.cbufs_ahorrados, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.dibujos, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.comprobaciones, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.diferencias, __ATOMIC_RELAXED)};
        uint64_t d[9];
        for (size_t i = 0; i < 9; ++i) {
          d[i] = cuentas[i] - set4_nvk_previo_[i];
          set4_nvk_previo_[i] = cuentas[i];
        }
        const char* const estado = __atomic_load_n(&nvk->apagado, __ATOMIC_RELAXED) != 0 ? "APAGADO por la guardia"
                                   : __atomic_load_n(&nvk->entorno, __ATOMIC_RELAXED) == 0
                                       ? "apagado por NVK_SWITCH_DYN_UBO_DELTA"
                                   : !set4_pedido_ ? "apagado por el cvar"
                                                   : "encendido";
        NFSMW_INFORME_ANILLO(
            "[nativo] C6 set 4 por diferencias (NVK, {}): {} enlaces por diferencias y {} enteros; {:.2f} escrituras "
            "a la tabla raiz por enlace (antes 4) con {:.2f} palabras; cbufs ensuciados {} y sin ensuciar {} ({:.1f} % "
            "menos); guardia: {} cbufs comprobados ({} dibujos por el camino nuevo), {} diferencias",
            estado, d[0], d[1], d[0] ? double(d[2]) / double(d[0]) : 0.0, d[0] ? double(d[3]) / double(d[0]) : 0.0,
            d[4], d[5], d[4] + d[5] ? 100.0 * double(d[5]) / double(d[4] + d[5]) : 0.0, d[7], d[6], d[8]);
      }
#endif
    }
    set4_cambios_.fill(0);
    set4_primeros_ = 0;
    set4_dibujos_ = 0;
    resubidas_vs_.fill(0);
    resubidas_ps_.fill(0);
  }

  // Every 10 s, how many draws go without binding their own offset (base zero).
  void InformeBaseCero() {
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - base_cero_informe_ < std::chrono::seconds(10)) {
      return;
    }
    const bool primera = base_cero_informe_ == std::chrono::steady_clock::time_point{};
    base_cero_informe_ = ahora;
    if (!primera && base_cero_total_) {
      NFSMW_INFORME_ANILLO("[nativo] C6 vertices base cero ({}): {} de {} dibujos con vertices sin enlazar su propio "
                  "desplazamiento ({:.1f} %); {} con varios enlaces y {} con la copia del dedupe fuera de un "
                  "multiplo de la zancada; {:.2f} vkCmdBindVertexBuffers por dibujo",
                  vertices_base_cero_apagado_ ? "APAGADO por la guardia"
                  : vertices_base_cero_       ? "encendido"
                                              : "apagado",
                  base_cero_dibujos_, base_cero_total_, 100.0 * double(base_cero_dibujos_) / double(base_cero_total_),
                  base_cero_varios_enlaces_, base_cero_desalineados_,
                  double(base_cero_enlaces_grabados_) / double(base_cero_total_));
    }
    base_cero_dibujos_ = 0;
    base_cero_total_ = 0;
    base_cero_varios_enlaces_ = 0;
    base_cero_desalineados_ = 0;
    base_cero_enlaces_grabados_ = 0;
  }

  /*
   * The framing of a draw, moved out of Dibujar unchanged (nfsmw_nativo_encuadre_cache). Returns 0 if it
   * has to draw, 1 if the viewport is empty and 2 if the scissor is empty (in both cases Dibujar returns
   * without drawing, as before; the scissor is left at zero). Same operations in the same order. The only
   * multiplications GCC could fuse with an add or subtract (ancho * 0.5 and alto * 0.5) are exact, so
   * fused or not they give the same bits.
   */
  uint32_t CalcularEncuadre(const uint32_t* r, uint32_t vte, bool sin_recorte, uint32_t modo_sc, VkViewport& viewport,
                            float ndc[4], VkRect2D& tijera) {
    float escala_x = (vte & 0x1) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]) : 1.0f;
    float centro_x = (vte & 0x2) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]) : 0.0f;
    float escala_y = (vte & 0x4) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
    float centro_y = (vte & 0x8) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]) : 0.0f;
    const float escala_z = (vte & 0x10) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]) : 1.0f;
    const float centro_z = (vte & 0x20) ? Flotante(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]) : 0.0f;
    const uint32_t ventana = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    const int32_t ventana_x = int32_t((ventana & 0x7FFF) << 17) >> 17;
    const int32_t ventana_y = int32_t(((ventana >> 16) & 0x7FFF) << 17) >> 17;
    if ((modo_sc >> 16) & 0x1) {
      centro_x += float(ventana_x);
      centro_y += float(ventana_y);
    }
    // With the shadow map drawn smaller (nfsmw_nativo_sombras_escala), the guest still speaks in
    // 1600-pixel units. The viewport and the scissor are multiplied by the pass scale, and with that the
    // geometry lands where it should; the rest (ndc, half pixel) already comes from the pass size.
    if (pase_escala_ != 1.0f) {
      escala_x *= pase_escala_;
      centro_x *= pase_escala_;
      escala_y *= pase_escala_;
      centro_y *= pase_escala_;
    }
    viewport = VkViewport{};
    viewport.x = centro_x - std::abs(escala_x);
    viewport.width = 2.0f * std::abs(escala_x);
    // A negative YSCALE (the D3D norm) gives a positive height; a positive one, an inverted height.
    viewport.y = centro_y + escala_y;
    viewport.height = -2.0f * escala_y;
    if (!((r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 19) & 0x1)) {
      Avisar(14, "espacio de recorte OpenGL (dx_clip_space_def = 0): Z sin ajustar");
    }
    viewport.minDepth = std::clamp(centro_z, 0.0f, 1.0f);
    viewport.maxDepth = std::clamp(centro_z + escala_z, 0.0f, 1.0f);
    // g_NdcScale (x, y) and g_NdcOffset (x, y): identity when clipping is on.
    ndc[0] = 1.0f;
    ndc[1] = 1.0f;
    ndc[2] = 0.0f;
    ndc[3] = 0.0f;
    if (sin_recorte) {
      const float ancho = float(pase_ancho_);
      const float alto = float(pase_alto_);
      ndc[0] = escala_x * 2.0f / ancho;
      ndc[2] = (centro_x - ancho * 0.5f) * 2.0f / ancho;
      // DXC flips Y at the end of the VS (-fvk-invert-y): the D3D Y goes here.
      ndc[1] = -escala_y * 2.0f / alto;
      ndc[3] = -(centro_y - alto * 0.5f) * 2.0f / alto;
      viewport.x = 0.0f;
      viewport.y = 0.0f;
      viewport.width = ancho;
      viewport.height = alto;
    }
    tijera = VkRect2D{};
    if (viewport.width < 1.0f || std::abs(viewport.height) < 1.0f) {
      return 1;
    }
    const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
    const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
    int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
    if (!((tl >> 31) & 0x1)) {
      x0 += ventana_x;
      y0 += ventana_y;
      x1 += ventana_x;
      y1 += ventana_y;
    }
    if (pase_escala_ != 1.0f) {  // also in guest pixels
      x0 = int32_t(std::floor(float(x0) * pase_escala_));
      y0 = int32_t(std::floor(float(y0) * pase_escala_));
      x1 = int32_t(std::ceil(float(x1) * pase_escala_));
      y1 = int32_t(std::ceil(float(y1) * pase_escala_));
    }
    x0 = std::clamp(x0, 0, int32_t(pase_ancho_));
    x1 = std::clamp(x1, 0, int32_t(pase_ancho_));
    y0 = std::clamp(y0, 0, int32_t(pase_alto_));
    y1 = std::clamp(y1, 0, int32_t(pase_alto_));
    if (x1 <= x0 || y1 <= y0) {
      return 2;
    }
    tijera = VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    return 0;
  }

  // The C1 guard. The current computation against the saved one, byte by byte.
  void CompararEncuadre(const VkViewport& viewport, const float ndc[4], const VkRect2D& tijera, uint32_t vacio) {
    ++encuadre_comprobados_;
    const bool igual = vacio == encuadre_vacio_ &&
                       std::memcmp(&viewport, &encuadre_viewport_, sizeof(viewport)) == 0 &&
                       std::memcmp(ndc, encuadre_ndc_, sizeof(encuadre_ndc_)) == 0 &&
                       std::memcmp(&tijera, &encuadre_tijera_, sizeof(tijera)) == 0;
    if (!igual) {
      encuadre_cache_apagada_ = true;
      encuadre_cache_ = false;
      REXLOG_ERROR("[nativo] C6 encuadre en cache: DIFERENCIA en la comprobacion {} (vacio {} / {}; viewport {},{} "
                   "{}x{} / {},{} {}x{}; tijera {},{} {}x{} / {},{} {}x{}). Apagada para el resto de la sesion: este "
                   "dibujo usa el recalculado",
                   encuadre_comprobados_, vacio, encuadre_vacio_, viewport.x, viewport.y, viewport.width,
                   viewport.height, encuadre_viewport_.x, encuadre_viewport_.y, encuadre_viewport_.width,
                   encuadre_viewport_.height, tijera.offset.x, tijera.offset.y, tijera.extent.width,
                   tijera.extent.height, encuadre_tijera_.offset.x, encuadre_tijera_.offset.y,
                   encuadre_tijera_.extent.width, encuadre_tijera_.extent.height);
    } else if (encuadre_comprobados_ == kEncuadresAComprobar) {
      NFSMW_INFORME_ANILLO("[nativo] C6 encuadre en cache: {} aciertos comprobados contra el calculo, 0 diferencias; sigue "
                  "comprobando 1 de cada 4096", encuadre_comprobados_);
    }
  }

  /*
   * The alto_util_ element for that pitch (nfsmw_nativo_alto_util_memo). The pitch is the pass's and only
   * changes with the pass, so the last one is remembered. References to unordered_map elements are not
   * invalidated by insertion (only by erasure, and alto_util_ never erases). Guard: the first
   * kAltoUtilAComprobar reuses, then 1 in 4,096, also look it up in the map and must get the same element.
   */
  uint32_t& AltoUtilDe(uint32_t pitch) {
    if (!alto_util_memo_activo_) {
      return alto_util_[pitch];
    }
    if (alto_util_memo_ == nullptr || pitch != alto_util_memo_pitch_) {
      alto_util_memo_ = &alto_util_[pitch];
      alto_util_memo_pitch_ = pitch;
      return *alto_util_memo_;
    }
    const uint64_t n = ++alto_util_memo_aciertos_;
    if (n <= kAltoUtilAComprobar || (n & 4095) == 0) {
      uint32_t* const del_mapa = &alto_util_[pitch];
      if (del_mapa != alto_util_memo_) {
        alto_util_memo_apagado_ = true;
        alto_util_memo_activo_ = false;
        alto_util_memo_ = nullptr;
        REXLOG_ERROR("[nativo] C6 alto util recordado: DIFERENCIA (pitch {}, comprobacion {}): no es el elemento "
                     "del mapa. Apagado para el resto de la sesion", pitch, n);
        return *del_mapa;
      }
      if (n == kAltoUtilAComprobar) {
        NFSMW_INFORME_ANILLO("[nativo] C6 alto util recordado: {} comprobaciones contra el mapa, 0 diferencias; sigue "
                    "comprobando 1 de cada 4096", n);
      }
    }
    return *alto_util_memo_;
  }

  /*
   * The C3 guard. With the same render target bytes, the XXH3 must be pase_clave_. Returns the computed
   * one, which is the one that decides for this draw.
   */
  uint64_t ComprobarClavePase(const uint64_t claves[5], uint64_t n) {
    const uint64_t calculada = XXH3_64bits(claves, sizeof(uint64_t) * 5);
    if (calculada != pase_clave_) {
      clave_pase_rapida_apagada_ = true;
      clave_pase_rapida_ = false;
      REXLOG_ERROR("[nativo] C6 clave del pase: DIFERENCIA (comprobacion {}: {:016X} calculada, {:016X} guardada). "
                   "Apagada para el resto de la sesion", n, calculada, pase_clave_);
    } else if (n == kClavesPaseAComprobar) {
      NFSMW_INFORME_ANILLO("[nativo] C6 clave del pase: {} reutilizadas comprobadas con XXH3, 0 diferencias; sigue "
                  "comprobando 1 de cada 4096", n);
    }
    return calculada;
  }

  // Every 10 s, how much the small per-draw savings save.
  void InformeMinucias() {
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - minucias_informe_ < std::chrono::seconds(10)) {
      return;
    }
    const bool primera = minucias_informe_ == std::chrono::steady_clock::time_point{};
    minucias_informe_ = ahora;
    const uint64_t encuadres = encuadre_aciertos_informe_ + encuadre_calculos_informe_;
    if (!primera && encuadres) {
      NFSMW_INFORME_ANILLO("[nativo] C6 minucias (build 179): encuadre de la cache en {} de {} dibujos ({:.1f} %; {}{}); "
                  "clave del pase sin XXH3 {} veces ({}); alto util recordado {} veces ({}); ajustes de los "
                  "dibujos sin color {}",
                  encuadre_aciertos_informe_, encuadres,
                  100.0 * double(encuadre_aciertos_informe_) / double(encuadres),
                  encuadre_cache_apagada_ ? "APAGADA por la guardia" : encuadre_cache_ ? "encendida" : "apagada",
                  encuadre_comprobados_ >= kEncuadresAComprobar ? ", guardia superada" : ", comprobando",
                  claves_pase_rapidas_ - claves_pase_rapidas_previas_,
                  clave_pase_rapida_apagada_ ? "APAGADA por la guardia" : clave_pase_rapida_ ? "encendida" : "apagada",
                  alto_util_memo_aciertos_ - alto_util_memo_aciertos_previos_,
                  alto_util_memo_apagado_ ? "APAGADO por la guardia" : alto_util_memo_activo_ ? "encendido" : "apagado",
                  cvars_por_fotograma_ ? "una vez por fotograma" : "en cada dibujo");
    }
    encuadre_aciertos_informe_ = 0;
    encuadre_calculos_informe_ = 0;
    claves_pase_rapidas_previas_ = claves_pase_rapidas_;
    alto_util_memo_aciertos_previos_ = alto_util_memo_aciertos_;
  }

  // The guard of the direct-mapped pipeline cache. The same key in the map (the usual path) must give the
  // same VkPipeline as the slot. Otherwise it switches off and returns false.
  bool ComprobarCasillaPipeline(uint64_t huella, const ClavePipeline& clave, VkPipeline en_casilla, uint64_t n) {
    ++pipelines_directa_comprobadas_;
    VkPipeline del_mapa = VK_NULL_HANDLE;
    if (const auto it = pipelines_.find(huella);
        it != pipelines_.end() && std::memcmp(&it->second.first, &clave, sizeof(clave)) == 0) {
      del_mapa = it->second.second;
    }
    if (del_mapa != en_casilla) {
      pipelines_directa_apagada_ = true;
      pipelines_directa_ = false;
      REXLOG_ERROR("[nativo] C6 pipelines: DIFERENCIA entre la cache directa y el mapa (comprobacion {}, VS {} PS {} "
                   "especializacion {:08X}). Apagada para el resto de la sesion: manda el mapa",
                   n, clave.vs, clave.ps, clave.especializacion);
      return false;
    }
    if (n == kPipelinesAComprobar) {
      NFSMW_INFORME_ANILLO("[nativo] C6 pipelines: {} aciertos de la cache directa comprobados contra el mapa, 0 "
                  "diferencias; sigue comprobando 1 de cada 4096", n);
    }
    return true;
  }

  /*
   * Measurement only (nfsmw_nativo_contar_cambios_pipeline). What changes at each vkCmdBindPipeline.
   *
   * BindPipeline costs 3.3-4.5 us per call in a race (0.3-0.4 per draw), and on each one NVK copies all
   * the pipeline's fixed state (vk_dynamic_graphics_state_copy, ~80 groups). If the change is state only,
   * with dynamic state (vkCmdSet*) no new pipeline would be needed. Before touching anything, the counts
   * and kinds have to be known:
   *  - cull mode, topology within the same class, Z test/write/function, stencil, bias and restart are
   *    EDS1/EDS2, core in Vulkan 1.3: the console (API 1.3.354) already provides them without touching
   *    the SDK;
   *  - blending, color masks and topology of another class need VK_EXT_extended_dynamic_state3, which NVK
   *    exposes on Maxwell and the SDK only enables with ui_vulkan_estado_dinamico3.patch.
   * The new key is compared with the last one bound in the same command buffer, field by field and
   * without XXH3. Blending, masks and depth are compared in canonical form (EstadoCanonico: only what
   * PipelineDe really reads). If only bits PipelineDe does not use differ, the pipeline is effectively the
   * same: no effect. Cost: an 80-byte memcmp and about 60 operations per bind (0.3-0.4 binds per draw). It
   * changes nothing.
   */
  static uint32_t ClaseTopologia(uint32_t topologia) {
    switch (topologia) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
        return 0;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
        return 1;
      case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return 3;
      default:
        return 2;  // triangulos: lista, tira y abanico
    }
  }

  // What PipelineDe really reads from a key's blending, masks and depth, with its same criterion
  // (blendEnable per target; depthWriteEnable = test and write; the back face copies the front).
  static void EstadoCanonico(const ClavePipeline& c, uint32_t mezcla[4], uint32_t& mascaras, uint32_t& profundidad) {
    mascaras = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      mezcla[i] = 0;
      if (!c.formatos[i]) {
        continue;  // PipelineDe skips color targets not in the pass
      }
      const uint32_t mascara = (c.mascaras >> (i * 4)) & 0xF;
      mascaras |= mascara << (i * 4);
      const uint32_t m = c.mezcla[i];
      const bool color_directo = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool alfa_directo = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      if (((mascara & 0x7) && !color_directo) || ((mascara & 0x8) && !alfa_directo)) {
        mezcla[i] = m;  // blendEnable: factors count (without blending Vulkan ignores them)
      }
    }
    uint32_t d = c.formatos[4] ? c.profundidad : 0;  // without a depth target PipelineDe ignores it
    d &= ~0x8u;                                      // PipelineDe does not read bit 3
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;  // without a Z test, write and function do not count
    }
    if (!(d & 0x1)) {
      d &= 0x77u;  // sin estencil no cuentan sus funciones ni sus operaciones
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
    }
    profundidad = d;
  }

  // Classifies a vkCmdBindPipeline from Dibujar against the last key bound in this buffer.
  // tras_empezar_pase: EmpezarPase had forgotten the bound pipeline (pipeline_enlazado_ set to
  // VK_NULL_HANDLE).
  void ContarCambioPipeline(const ClavePipeline& nueva, bool tras_empezar_pase) {
    uint64_t* const n = cambios_pipeline_.data();
    ++n[kCambioEnlaces];
    n[kCambioTrasPase] += tras_empezar_pase ? 1 : 0;
    const ClavePipeline& a = clave_enlazada_;
    constexpr uint32_t kBitsPruebaAlfa = 0x2u | (0x7u << kSpecFuncionAlfaDesplazamiento);
    if (!clave_enlazada_valida_) {
      ++n[kCambioPrimero];
    } else if (std::memcmp(&a, &nueva, sizeof(nueva)) == 0) {
      ++n[kCambioIdentica];  // only after EmpezarPase: Vulkan kept the bound pipeline across passes
    } else if (a.vs != nueva.vs || a.ps != nueva.ps) {
      ++n[kCambioShaders];
    } else if (a.entrada != nueva.entrada) {
      ++n[kCambioEntrada];
    } else if (std::memcmp(a.formatos, nueva.formatos, sizeof(a.formatos)) != 0) {
      ++n[kCambioFormatos];
    } else if (const uint32_t spec = a.especializacion ^ nueva.especializacion; spec != 0) {
      ++n[(spec & ~kBitsPruebaAlfa) == 0                      ? kCambioSpecAlfa
          : (spec & ~(kBitsPruebaAlfa | kSpecZTemprana)) == 0 ? kCambioSpecZTemprana
                                                              : kCambioSpecOtra];
    } else {
      // Same shaders, input, formats and specialization: only fixed pipeline state changes.
      uint32_t mezcla_a[4], mezcla_n[4], mascaras_a, mascaras_n, prof_a, prof_n;
      EstadoCanonico(a, mezcla_a, mascaras_a, prof_a);
      EstadoCanonico(nueva, mezcla_n, mascaras_n, prof_n);
      const bool topologia = a.topologia != nueva.topologia;
      const bool clase = ClaseTopologia(a.topologia) != ClaseTopologia(nueva.topologia);
      const bool mezcla = std::memcmp(mezcla_a, mezcla_n, sizeof(mezcla_a)) != 0;
      const bool mascaras = mascaras_a != mascaras_n;
      const uint32_t prof = prof_a ^ prof_n;
      const uint32_t rast = a.rasterizado ^ nueva.rasterizado;
      if (!topologia && !mezcla && !mascaras && !prof && !rast) {
        ++n[kCambioSinEfecto];  // only bits PipelineDe does not read: effectively the same pipeline
        n[kCambioSinEfectoTrasPase] += tras_empezar_pase ? 1 : 0;
      } else {
        const bool eds12 = !mezcla && !mascaras && !clase;
        ++n[kCambioSoloEstado];
        n[kCambioEstadoTrasPase] += tras_empezar_pase ? 1 : 0;
        n[kCambioEds12] += eds12 ? 1 : 0;
        n[kCambioEds12TrasPase] += (eds12 && tras_empezar_pase) ? 1 : 0;
        n[kCampoTopologia] += topologia ? 1 : 0;
        n[kCampoClaseTopologia] += clase ? 1 : 0;
        n[kCampoMezcla] += mezcla ? 1 : 0;
        n[kCampoMascaras] += mascaras ? 1 : 0;
        n[kCampoZ] += (prof & 0x76u) ? 1 : 0;
        n[kCampoEstencil] += (prof & ~0x76u) ? 1 : 0;
        n[kCampoCara] += (rast & 0x7u) ? 1 : 0;
        n[kCampoReinicio] += (rast & 0x8u) ? 1 : 0;
        n[kCampoSesgo] += (rast & 0x10u) ? 1 : 0;
      }
    }
    clave_enlazada_ = nueva;
    clave_enlazada_valida_ = true;
  }

  // Every 20 s, what changes at each vkCmdBindPipeline of the ring (measurement only). Reads its cvar on
  // every call: InformePipelinesDirecta calls it, once per frame.
  void InformeCambiosPipeline(std::chrono::steady_clock::time_point ahora) {
    const bool contar = REXCVAR_GET(nfsmw_nativo_contar_cambios_pipeline);
    if (contar != contar_cambios_pipeline_) {
      contar_cambios_pipeline_ = contar;
      clave_enlazada_valida_ = false;  // what was tracked while not counting cannot be compared
    }
    if (ahora - cambios_informe_ < std::chrono::seconds(20)) {
      return;
    }
    const bool primera = cambios_informe_ == std::chrono::steady_clock::time_point{};
    const double segundos = std::chrono::duration<double>(ahora - cambios_informe_).count();
    cambios_informe_ = ahora;
    const uint64_t dibujos = dibujados_ - cambios_dibujos_previos_;
    const uint64_t fotogramas = fotograma_ - cambios_fotogramas_previos_;
    cambios_dibujos_previos_ = dibujados_;
    cambios_fotogramas_previos_ = fotograma_;
    const std::array<uint64_t, kCambiosN> n = cambios_pipeline_;
    cambios_pipeline_.fill(0);
    if (primera || !contar_cambios_pipeline_ || !n[kCambioEnlaces] || !dibujos || !fotogramas) {
      return;
    }
    const double enlaces = double(n[kCambioEnlaces]);
    const double f = double(fotogramas);
    const auto por_dibujo = [&](uint64_t x) { return double(x) / double(dibujos); };
    const auto pct = [&](uint64_t x) { return 100.0 * double(x) / enlaces; };
    const uint64_t spec = n[kCambioSpecAlfa] + n[kCambioSpecZTemprana] + n[kCambioSpecOtra];
    NFSMW_INFORME_ANILLO(
        "[nativo] C6 cambios de pipeline (build 184, solo medida): {} vkCmdBindPipeline en {:.0f} s ({:.3f} por "
        "dibujo, {:.1f} por fotograma; {} son el primero de un pase) | sin clave anterior {} | la MISMA clave tras "
        "empezar pase {} ({:.1f} %) | cambian shaders {} ({:.1f} %) | otra entrada {} | otros formatos {} | "
        "especializacion {} ({:.1f} %: prueba de alfa {}, con Z temprana {}, otra {}) | sin efecto (bits que "
        "PipelineDe no lee) {} ({:.1f} %) | SOLO ESTADO {} ({:.1f} %, {:.3f} por dibujo): EDS1/EDS2 sin tocar el SDK "
        "{}, el resto pide EDS3",
        n[kCambioEnlaces], segundos, por_dibujo(n[kCambioEnlaces]), enlaces / f, n[kCambioTrasPase],
        n[kCambioPrimero], n[kCambioIdentica], pct(n[kCambioIdentica]), n[kCambioShaders], pct(n[kCambioShaders]),
        n[kCambioEntrada], n[kCambioFormatos], spec, pct(spec), n[kCambioSpecAlfa], n[kCambioSpecZTemprana],
        n[kCambioSpecOtra], n[kCambioSinEfecto], pct(n[kCambioSinEfecto]), n[kCambioSoloEstado],
        pct(n[kCambioSoloEstado]), por_dibujo(n[kCambioSoloEstado]), n[kCambioEds12]);
    // Avoidable binds per frame with each fix, each one separately (those of the first draw of a pass are
    // not avoided by dynamic state or by the canonical key while EmpezarPase forgets the bound pipeline).
    constexpr double kUsPorEnlace = 3.0;  // BindPipeline: 3.3-4.5 us per call in a race
    const uint64_t canonica = n[kCambioSinEfecto] - n[kCambioSinEfectoTrasPase];
    const uint64_t eds12 = n[kCambioEds12] - n[kCambioEds12TrasPase];
    const uint64_t eds123 = n[kCambioSoloEstado] - n[kCambioEstadoTrasPase];
    NFSMW_INFORME_ANILLO(
        "[nativo] C6 cambios de pipeline, campos del solo estado: topologia {} (de clase {}), mezcla {}, mascaras {}, "
        "Z {}, estencil {}, cara {}, reinicio {}, sesgo {}; {} de ellos el primero de un pase | enlaces evitables por "
        "fotograma (a ~3 us cada uno): {:.1f} con la clave canonica ({:.2f} ms), {:.1f} sin olvidar el pipeline al "
        "empezar pase ({:.2f} ms), {:.1f} con EDS1/EDS2 ({:.2f} ms), {:.1f} con EDS1/EDS2/EDS3 ({:.2f} ms)",
        n[kCampoTopologia], n[kCampoClaseTopologia], n[kCampoMezcla], n[kCampoMascaras], n[kCampoZ],
        n[kCampoEstencil], n[kCampoCara], n[kCampoReinicio], n[kCampoSesgo], n[kCambioEstadoTrasPase],
        double(canonica) / f, double(canonica) / f * kUsPorEnlace / 1000.0, double(n[kCambioIdentica]) / f,
        double(n[kCambioIdentica]) / f * kUsPorEnlace / 1000.0, double(eds12) / f,
        double(eds12) / f * kUsPorEnlace / 1000.0, double(eds123) / f, double(eds123) / f * kUsPorEnlace / 1000.0);
  }

  // Every 10 s, the hit rate of the direct-mapped pipeline cache.
  void InformePipelinesDirecta() {
    const auto ahora = std::chrono::steady_clock::now();
    InformeCambiosPipeline(ahora);  // reads its cvar every frame and writes every 20 s
    IntentarPrecalentar();       // starts the thread as soon as the library is loaded
    InformePrecalentado(ahora);  // every 10 s, if there is anything new, and its guard
    if (ahora - pipelines_directa_informe_ < std::chrono::seconds(10)) {
      return;
    }
    const bool primera = pipelines_directa_informe_ == std::chrono::steady_clock::time_point{};
    pipelines_directa_informe_ = ahora;
    const uint64_t aciertos = pipelines_directa_aciertos_ - pipelines_directa_aciertos_previos_;
    const uint64_t fallos = pipelines_directa_fallos_ - pipelines_directa_fallos_previos_;
    pipelines_directa_aciertos_previos_ = pipelines_directa_aciertos_;
    pipelines_directa_fallos_previos_ = pipelines_directa_fallos_;
    if (!primera && (aciertos || fallos)) {
      NFSMW_INFORME_ANILLO("[nativo] C6 pipelines (build 179): cache directa {} aciertos y {} fallos ({:.1f} % de las "
                  "busquedas que no resuelve el atajo de una entrada); {} comprobadas contra el mapa ({})",
                  aciertos, fallos, 100.0 * double(aciertos) / double(aciertos + fallos),
                  pipelines_directa_comprobadas_,
                  pipelines_directa_apagada_ ? "APAGADA por la guardia"
                  : pipelines_directa_comprobadas_ >= kPipelinesAComprobar ? "guardia superada"
                                                                           : "comprobando");
    }
  }

  /*
   * The "indices" stage once rose from ~1.4 to ~2.6 us per draw without any change to its code. Four
   * cuts inside it (C6 subetapas 15-18), only on the timed draws (1 in 64): they cost four clock reads on
   * those draws, so the "indices" stage reads ~0.2-0.3 us higher than without them.
   */
  void CortarSubetapa(size_t k, std::chrono::steady_clock::time_point& t) {
    if (!cronometrar_) {
      return;
    }
    const auto ahora = std::chrono::steady_clock::now();
    sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(ahora - t).count());
    ++sub_n_[k];
    t = ahora;
  }

  void Etapa(size_t etapa, std::chrono::steady_clock::time_point& marca) {
    if (!cronometrar_) {
      return;
    }
    const auto ahora = std::chrono::steady_clock::now();
    etapas_ns_[etapa] += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ahora - marca).count());
    marca = ahora;
  }

  // Diagnostic nfsmw_nativo_diag_vertices_repetidos: vertex bytes that repeat address, size, byte order
  // and content within the same frame or relative to an earlier frame.
  void AnotarVerticesRepetidos(uint64_t direccion, uint32_t bytes, uint32_t orden,
                               const uint8_t* datos) {
    const auto antes = std::chrono::steady_clock::now();
    const uint64_t huella = XXH3_64bits(datos, bytes);
    const uint64_t clave =
        XXH3_64bits_withSeed(&direccion, sizeof(direccion), (uint64_t(bytes) << 2) | orden);
    if (vertices_vistos_.size() > 200000) {
      vertices_vistos_.clear();  // memory cap of the diagnostic
    }
    VerticesVistos& visto = vertices_vistos_[clave];
    if (visto.huella == huella && visto.fotograma == fotograma_) {
      bytes_repetidos_fotograma_ += bytes;
    } else if (visto.huella == huella && visto.fotograma != UINT64_MAX &&
               visto.fotograma < fotograma_) {
      bytes_iguales_anterior_ += bytes;
    }
    visto.fotograma = fotograma_;
    visto.huella = huella;
    ns_hash_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - antes)
                                      .count());
  }

  void TerminarPase() override {
    if (pase_activo_) {
      /*
       * Fallback path of the deferred sky. If nothing has emitted it yet (no transparent draw arrived, or the
       * pass closes because of a copy, a resolve or a submission), it is emitted here, inside the pass and
       * before vkCmdEndRenderPass. Losing the sky would be a visible and serious bug, so this is the last
       * place it can be, and it is always reached: the command buffer is only closed through EnviarTrabajo
       * -> AntesDeEnviar -> TerminarPase.
       */
      if (cielo_pendiente_) {
        EmitirCieloAplazado(pase_comandos_, kCieloPorFinDePase);
      }
      // A host occlusion query cannot stay open outside its pass.
      if (consulta_oclusion_ != UINT32_MAX) {
        contexto_->TerminarConsultaOclusion(consulta_oclusion_);
        consulta_oclusion_ = UINT32_MAX;
      }
      const auto antes_fin = std::chrono::steady_clock::now();
      dfn_.vkCmdEndRenderPass(pase_comandos_);
      if (estadisticas_pase_ != UINT32_MAX) {
        contexto_->TerminarEstadisticas(estadisticas_pase_);
        estadisticas_pase_ = UINT32_MAX;
      }
      ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - antes_fin).count());
      const uint32_t categoria_cerrada = categoria_pase_;
      pase_activo_ = false;
      // Just in case. A pending sky with the pass already closed cannot be recorded; it is counted so it
      // shows up in the report instead of disappearing silently.
      if (cielo_pendiente_) {
        cielo_pendiente_ = false;
        ++cielo_perdidos_;
      }
      // As soon as the shadow pass closes, submit to the GPU. Once per frame: the flag keeps reopened passes
      // from submitting again. EnviarYEsperar submits and keeps recording in the next slot; it waits for
      // nothing (the name is misleading).
      if (categoria_cerrada == kGpuSombras && !enviado_tras_sombras_ && contexto_ &&
          REXCVAR_GET(nfsmw_nativo_enviar_tras_sombras)) {
        enviado_tras_sombras_ = true;
        ++envios_tras_sombras_;
        contexto_->EnviarYEsperar();
      }
    }
  }

  void AntesDeEnviar() override {
    EsperarSubidas();  // deferred vertex copies, before flushing the mapping and submitting
    TerminarPase();
    // Whatever is still on the texture bind thread, with its views, descriptors, barriers and copies, goes
    // into this upload buffer before it is closed (nfsmw_nativo_texturas_enlace_hilo). It is the last thing
    // recorded: after the pass closes (which may emit the deferred sky), and meanwhile the thread has kept
    // binding.
    RecogerEnlaces(true);
    // The data of the new textures prepared by the hash thread, into this upload buffer before the mapping
    // is flushed and it is submitted (nfsmw_nativo_texturas_huella_hilo). Their barrier and copy are
    // already recorded.
    RecogerHuellas();
    if (subida_usado_ && !subida_coherente_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(dispositivo_, subida_memoria_, subida_tipo_, 0,
                                                    subida_tamano_real_, subida_usado_);
    }
    if (compartidas_aparte_ && compartidas_usado_ && !compartidas_coherente_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(dispositivo_, compartidas_memoria_, compartidas_tipo_, 0,
                                                    compartidas_tamano_real_, compartidas_usado_);
      compartidas_bytes_publicados_ += compartidas_usado_;
    }
  }

  void UsarRanura(uint32_t ranura) override {
    EsperarSubidas();  // already empty after AntesDeEnviar; just in case, before switching buffers
    InformeCopias();  // every 10 s, who made the vertex copies and how long the ring waited
    InformeCacheFetch();  // every 10 s, the per-fetch sampler table
    // A texture may arrive here with its bind in flight (it was prepared before this submission's first
    // Grabar) and that is normal; what it cannot have is its data already in the upload buffer just
    // submitted.
    ComprobarEnlacesAlCambiarDeBufer();
    ComprobarHuellasAlCambiarDeBufer();  // before resetting the upload buffer
    const BuferSubida& s = subidas_[ranura % subidas_.size()];
    subida_ = s.bufer;
    subida_memoria_ = s.memoria;
    subida_tamano_real_ = s.tamano_real;
    subida_datos_ = s.datos;
    subida_direccion_ = s.direccion;
    ranura_actual_ = uint32_t(ranura % subidas_.size());  // this slot's UBO set
    subida_usado_ = 0;
    if (compartidas_aparte_) {
      const BuferSubida& c = compartidas_bufs_[ranura_actual_];
      compartidas_datos_ = c.datos;
      compartidas_memoria_ = c.memoria;
      compartidas_tamano_real_ = c.tamano_real;
      compartidas_usado_ = 0;
    }
    ++epoca_subida_;
    ++fotograma_;
    CerrarFotogramaDeLaGuardiaDelCielo();
    // nfsmw_reflejo_visibilidad, once per submission (the guard may switch it off mid-session).
    medir_visibilidad_ = contexto_->OclusionGpuPermitida() && nfsmw::reflejo_demanda::MedirVisibilidad();
    if (medir_visibilidad_ && fotograma_ >= testigo_siguiente_) {
      testigo_pendiente_ = true;
    }
    // The upload buffer is reset here, so the recorded offsets are no longer valid. This also covers
    // EnviarYEsperar, which goes through here.
    dedupe_.NuevoFotograma(fotograma_);
    enviado_tras_sombras_ = false;  // the post-shadow submission is once per frame
    /*
     * Diagnostic cvars are read once per frame, not per draw.
     *
     * REXCVAR_GET is not a variable read: it is FLAGS_##name##_storage_(), an out-of-line function call
     * (cvar.h:343). Four of them were being made per draw (one of them per vertex binding, not per draw),
     * and one returned a std::string that was then compared. With 2,345 draws per frame that shows, and it
     * serves no purpose: none of them changes within a frame.
     */
    diag_omitir_ps_texto_ = REXCVAR_GET(nfsmw_nativo_diag_omitir_ps);
    diag_vertices_repetidos_ = REXCVAR_GET(nfsmw_nativo_diag_vertices_repetidos);
    dedupe_activo_ = REXCVAR_GET(nfsmw_nativo_dedupe_vertices);
    vertices_base_cero_ = REXCVAR_GET(nfsmw_nativo_vertices_base_cero) && !vertices_base_cero_apagado_;
    diag_estadisticas_dibujo_ = REXCVAR_GET(nfsmw_nativo_estadisticas_por_dibujo_s) > 0;
    area_util_ = REXCVAR_GET(nfsmw_nativo_pase_area_util);
    // The small per-draw savings.
    encuadre_cache_ = REXCVAR_GET(nfsmw_nativo_encuadre_cache) && !encuadre_cache_apagada_;
    alto_util_memo_activo_ = REXCVAR_GET(nfsmw_nativo_alto_util_memo) && !alto_util_memo_apagado_;
    clave_pase_rapida_ = REXCVAR_GET(nfsmw_nativo_clave_pase_rapida) && !clave_pase_rapida_apagada_;
    cvars_por_fotograma_ = REXCVAR_GET(nfsmw_nativo_cvars_por_fotograma);
    ps_solo_alfa_fotograma_ = PsSoloAlfa();
    sin_ps_sin_color_fotograma_ = SinPsSinColor();
    sin_vegetacion_ = REXCVAR_GET(nfsmw_sombras_sin_vegetacion);
    pipelines_directa_ = REXCVAR_GET(nfsmw_nativo_pipelines_directa) && !pipelines_directa_apagada_;
    clave_canonica_ = REXCVAR_GET(nfsmw_nativo_clave_canonica) && !clave_canonica_apagada_;  // phase 0a
    {
      const bool nuevo = REXCVAR_GET(nfsmw_nativo_pipeline_entre_pases);  // phase 0b
      if (nuevo != pipeline_entre_pases_ || !pipeline_entre_pases_anotado_) {
        pipeline_entre_pases_ = nuevo;
        pipeline_entre_pases_anotado_ = true;
        REXLOG_INFO("[nativo] C6 pipeline al empezar pase: {} (fotograma {})",
                    nuevo ? "se CONSERVA el enlazado (fase 0b del estado dinamico)" : "se vuelve a enlazar, como antes",
                    fotograma_);
      }
    }
    // Dynamic state phases 1 and 2, once per command buffer: the whole buffer uses one mode, because a
    // pipeline with a fixed state invalidates the dynamic value of that state.
    {
      uint32_t modo = 0;
      if (eds12_disponible_ && !eds_apagado_ && REXCVAR_GET(nfsmw_nativo_estado_dinamico)) {
        modo |= kEds12;
      }
      if (eds3_disponible_ && !eds_apagado_ && REXCVAR_GET(nfsmw_nativo_estado_dinamico3)) {
        modo |= kEds3;
      }
      if (modo != eds_modo_ || !eds_modo_anotado_) {
        eds_modo_anotado_ = true;
        REXLOG_INFO("[nativo] C6 estado dinamico: {} (fotograma {})", NombreModoEds(modo), fotograma_);
      }
      eds_modo_ = modo;
    }
    tijera_prueba_ = TijeraDePrueba();
    sin_desenfoque_fotograma_ = REXCVAR_GET(nfsmw_nativo_sin_desenfoque);  // once per frame
    // The cheap PCF bit also changes the pipeline: once per frame.
    {
      bool nuevo = REXCVAR_GET(nfsmw_nativo_pcf_barato);
      const int32_t alternar_pcf = REXCVAR_GET(nfsmw_nativo_pcf_barato_alternar_s);
      if (alternar_pcf > 0) {
        const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - inicio_alternancia_ps_)
                                  .count();
        nuevo = (segundos / alternar_pcf) % 2 == 1;
      }
      if (nuevo != pcf_barato_) {
        pcf_barato_ = nuevo;
        REXLOG_INFO("[nativo] C2 muestreo del mapa de sombras: {}",
                    nuevo ? "un solo texel (PCF barato)" : "patron 3x3 a medio texel");
      }
    }
    // The filter between mip levels is part of the sampler key, so it is also decided once per frame. Off
    // (the normal case), this is reading one cvar and comparing a bool.
    {
      bool nuevo = REXCVAR_GET(nfsmw_nativo_prueba_mip_puntual);
      const int32_t alternar_mip = REXCVAR_GET(nfsmw_nativo_prueba_mip_puntual_alternar_s);
      if (alternar_mip > 0) {
        const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - inicio_alternancia_ps_)
                                  .count();
        nuevo = (segundos / alternar_mip) % 2 == 1;
      }
      if (nuevo != mip_puntual_prueba_) {
        mip_puntual_prueba_ = nuevo;
        REXLOG_INFO("[nativo] C4 filtro entre niveles de mip: {}",
                    nuevo ? "PUNTUAL (prueba: trilineal -> bilineal, la TMU hace la mitad)"
                          : "lineal (el que pide el juego)");
      }
    }
    // nfsmw_nativo_diag_mip_minimo. Part of the sampler key; when it changes the per-texture sampler caches
    // are invalidated (generacion_texturas_, as when an image is retired) so it shows in the next frame.
    {
      const uint32_t nuevo = uint32_t(std::clamp(int32_t(REXCVAR_GET(nfsmw_nativo_diag_mip_minimo)), 0, 4));
      if (nuevo != diag_mip_minimo_) {
        diag_mip_minimo_ = nuevo;
        ++generacion_texturas_;
        REXLOG_INFO("[nativo] C4 diagnostico de mips: las texturas con mips empiezan {} niveles mas abajo{}", nuevo,
                    nuevo ? " (DIAGNOSTICO: todo se vera borroso)" : " (normal)");
      }
    }
    // The shadow map's own depth bias, once per frame.
    {
      const int32_t constante = std::clamp(int32_t(REXCVAR_GET(nfsmw_nativo_sombras_sesgo_constante)), 0, 100);
      const int32_t pendiente = std::clamp(int32_t(REXCVAR_GET(nfsmw_nativo_sombras_sesgo_pendiente)), 0, 100);
      if (constante != sombras_sesgo_constante_ || pendiente != sombras_sesgo_pendiente_ || !sombras_sesgo_anotado_) {
        sombras_sesgo_constante_ = constante;
        sombras_sesgo_pendiente_ = pendiente;
        sombras_sesgo_anotado_ = true;
        REXLOG_INFO("[nativo] C4 desplazamiento de profundidad del mapa de sombras: pendiente {:.1f}, constante {} "
                    "(x1000 unidades de 24 bits){}",
                    double(pendiente) * 0.1, constante, (constante || pendiente) ? "" : " (apagado, como antes)");
      }
    }
    // The 1/size bit changes the pipeline, so it is decided once per frame.
    {
      const int32_t alternar = REXCVAR_GET(nfsmw_nativo_inv_tamano_tex_alternar_s);
      bool nuevo = REXCVAR_GET(nfsmw_nativo_inv_tamano_tex);
      if (alternar > 0) {
        const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - inicio_alternancia_ps_)
                                  .count();
        nuevo = (segundos / alternar) % 2 == 1;
      }
      if (nuevo != inv_tamano_tex_) {
        inv_tamano_tex_ = nuevo;
        REXLOG_INFO("[nativo] C2 tamano de textura: {}",
                    nuevo ? "por constante" : "preguntado a la textura");
      }
    }
    /*
     * The early depth test also changes the pipeline (a different module), so it is decided once per
     * frame, like the PCF and the 1/size.
     */
    {
      const int32_t alternar = REXCVAR_GET(nfsmw_nativo_z_temprana_alternar_s);
      bool nuevo = REXCVAR_GET(nfsmw_nativo_z_temprana);
      if (alternar > 0) {
        const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - inicio_alternancia_z_)
                                  .count();
        nuevo = (segundos / alternar) % 2 == 1;
      }
      if (nuevo != z_temprana_ || !alternancia_z_anotada_) {
        z_temprana_ = nuevo;
        alternancia_z_anotada_ = true;
        REXLOG_INFO("[nativo] C6 prueba de profundidad: {} (fotograma {})",
                    nuevo ? "ANTES de sombrear donde no se escribe Z (Z temprana)"
                          : "despues de sombrear, como antes",
                    fotograma_);
      }
    }
    saltar_invisibles_ = REXCVAR_GET(nfsmw_nativo_saltar_invisibles);
    // The deferred sky, also once per frame. If it is switched off mid-frame with one already deferred, the
    // pending one is still emitted through its usual path.
    {
      const bool nuevo = REXCVAR_GET(nfsmw_nativo_cielo_aplazado);
      if (nuevo != cielo_aplazado_ || !cielo_anotado_) {
        cielo_aplazado_ = nuevo;
        cielo_anotado_ = true;
        REXLOG_INFO("[nativo] C6 cielo: {} (fotograma {})",
                    nuevo ? "APLAZADO hasta despues de los opacos del pase"
                          : "en su sitio original (el primero, sobre el Z vacio)",
                    fotograma_);
      }
    }
    InformeZTemprana();
    InformePipelinesDirecta();
    InformeMinucias();
    InformeBaseCero();
    ControlSet4();  // set 4 by differences in NVK (cvar, alternation and guard)
    InformeSet4();
    ControlDibujoNvk();  // the draw path in NVK (measurement, improvements and guards)
    InformeDibujoNvk();
    InformeClaveCanonica();  // Dynamic state, phase 0a
    InformePipelineEntrePases();  // Dynamic state, phase 0b
    InformeEstadoDinamico();  // dynamic state phases 1 and 2
    InformeEnlaces();  // texture bind thread, every 10 s
    InformeHuellas();  // texture hash thread, every 10 s
    InformeReutilizar();  // measurement only: new textures with a live one's content, every 10 s
    ExpulsarTexturasSiHaceFalta();
    pool_texturas_.PorFotograma(fotograma_);  // grows with headroom, never exactly on demand
    // Test nfsmw_nativo_constantes_ubo_alternar_s. Switched here because the new epoch forces all constants
    // to be allocated again: nothing allocated in one mode is reused in the other.
    if (alternar_ubo_s_ > 0) {
      const auto segundos =
          std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - inicio_ubo_).count();
      const bool ubo = (segundos / alternar_ubo_s_) % 2 == 1;
      if (ubo != usar_ubo_) {
        usar_ubo_ = ubo;
        // With UBOs no push constants are recorded, so when going back to pointers the tracked state is
        // stale.
        std::memset(push_grabado_, 0, sizeof(push_grabado_));
        REXLOG_INFO("[nativo] prueba de constantes: {} (fotograma {})", ubo ? "por UBO" : "por puntero", fotograma_);
      }
    }
    ++envios_;  // "C6 constantes por UBO" report
    if (usar_ubo_) {
      ++envios_ubo_;
    }
    // What the prewarm thread actually compiles also goes into the cache and has to be saved.
    if (const uint32_t compilados = precalentado_compilados_.load(std::memory_order_relaxed);
        compilados != precalentado_contados_) {
      pipelines_sin_guardar_ += compilados - precalentado_contados_;
      precalentado_contados_ = compilados;
    }
    // The pipeline cache is saved after 64 new pipelines, or after a minute with any new one: on the
    // Switch, exiting does not always reach the destructor.
    if (pipelines_sin_guardar_ &&
        (pipelines_sin_guardar_ >= 64 ||
         std::chrono::steady_clock::now() - cache_guardada_ >= std::chrono::seconds(60))) {
      GuardarCachePipelines();
    }
  }

  void InvalidarTexturas() override { ++generacion_texturas_; }

  // Only the cache entries that use a view of those images. The views stay valid (they go with their
  // image); what has to be redone is which slot each fetch constant gets.
  void InvalidarImagenes(VkImage a, VkImage b) override {
    std::array<std::pair<uint32_t, uint32_t>, 64> ranuras{};
    size_t n = 0;
    for (VkImage imagen : {a, b}) {
      if (imagen == VK_NULL_HANDLE || (imagen == b && a == b)) {
        continue;
      }
      const auto it = vistas_por_imagen_.find(imagen);
      if (it == vistas_por_imagen_.end()) {
        continue;
      }
      for (uint64_t clave : it->second) {
        const auto v = vistas_.find(clave);
        if (v != vistas_.end() && v->second.imagen == imagen && n < ranuras.size()) {
          ranuras[n++] = {v->second.monton, v->second.ranura};
        }
      }
    }
    if (!n) {
      return;
    }
    const auto afectada = [&](const CacheSampler& c) {
      for (size_t i = 0; i < n; ++i) {
        if (ranuras[i].first == c.monton && ranuras[i].second == c.ranura) {
          return true;
        }
      }
      return false;
    };
    for (CacheSampler& c : cache_samplers_) {
      if (afectada(c)) {
        c.fotograma = UINT64_MAX;
        c.valido_hasta = 0;
      }
    }
    for (CacheSampler& c : cache_fetch_) {
      if (afectada(c)) {
        c.fotograma = UINT64_MAX;
        c.valido_hasta = 0;
      }
    }
  }

  void OclusionAbierta(bool abierta) override {
    if (!abierta && consulta_oclusion_ != UINT32_MAX) {
      // Still inside the pass of the last counted draw: TerminarPase would have closed it already.
      contexto_->TerminarConsultaOclusion(consulta_oclusion_);
      consulta_oclusion_ = UINT32_MAX;
    }
    oclusion_abierta_ = abierta;
  }

  uint64_t Dibujados() const override { return dibujados_; }

  // nfsmw_nativo_texturas_mb_max: with the texture cache above the limit, evict the textures unused for
  // the longest until it drops to 75 %, at most once every 60 frames.
  // It is safe without waiting for the GPU: a texture not prepared for 120 frames cannot be referenced by
  // any pending submission. The slot caches are valid for at most 32 frames after preparing, and only the
  // previous submission can still be on the GPU (Grabar waits for the slot before reusing it). The heap
  // slots are updated with UPDATE_AFTER_BIND, and the new generation invalidates the caches.
  static constexpr uint64_t kFotogramasSinUsoParaSoltar = 120;
  /*
   * How much GPU memory there is and how much is left.
   *
   * The cache limit was picked by eye twice (384 MB and then 192) because this data was not available.
   * With VK_EXT_memory_budget the driver reports the budget and the usage; without it, at least the heap
   * sizes are visible. It goes with the cache report, every 256 new textures.
   */
  void AnotarMemoriaDeLaGpu() {
    const auto* instancia = dispositivo_->vulkan_instance();
    if (!instancia) {
      return;
    }
    const auto& ifn = instancia->functions();
    const VkPhysicalDevice fisico = dispositivo_->physical_device();
    const bool hay_presupuesto = dispositivo_->extensions().ext_EXT_memory_budget &&
                                 ifn.vkGetPhysicalDeviceMemoryProperties2 != nullptr;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT presupuesto{};
    presupuesto.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 propiedades2{};
    propiedades2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    propiedades2.pNext = hay_presupuesto ? &presupuesto : nullptr;
    if (hay_presupuesto) {
      ifn.vkGetPhysicalDeviceMemoryProperties2(fisico, &propiedades2);
    } else {
      ifn.vkGetPhysicalDeviceMemoryProperties(fisico, &propiedades2.memoryProperties);
    }
    const auto& mp = propiedades2.memoryProperties;
    std::string texto;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
      if (!texto.empty()) {
        texto += " | ";
      }
      const bool local = (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
      if (hay_presupuesto) {
        texto += fmt::format("monton {}{}: {} MB usados de {} MB presupuestados (tamano {} MB)", i,
                             local ? " (GPU)" : "", presupuesto.heapUsage[i] >> 20,
                             presupuesto.heapBudget[i] >> 20, mp.memoryHeaps[i].size >> 20);
      } else {
        texto += fmt::format("monton {}{}: {} MB", i, local ? " (GPU)" : "",
                             mp.memoryHeaps[i].size >> 20);
      }
    }
    REXLOG_INFO("[nativo] C3 memoria de la GPU: {}{}; la cache de texturas lleva {} MB de {} MB", texto,
                hay_presupuesto ? "" : " (sin VK_EXT_memory_budget: solo tamanos)", bytes_texturas_ >> 20,
                texturas_mb_max_);
    // Without this line there is no way to know whether the pool is working or fragmenting. A vegetation
    // counter once existed and was never printed.
    if (pool_texturas_.Activo()) {
      REXLOG_INFO("[nativo] C3 {}", pool_texturas_.Resumen());
    }
  }

  /*
   * Without stutters.
   *
   * The first version waited until the limit was exceeded and evicted 25 % at once. In a race that is a
   * stutter, and a stutter every few minutes is no better than running out of memory every fourteen.
   *
   * Now a few are evicted per frame as soon as the cache nears the limit, and only cold textures: those
   * unused for 120 frames, i.e. two to four seconds. They are not in the working set, so they do not have
   * to be uploaded again right away and there is no thrashing. If there are not enough cold ones, nothing
   * is forced: the cache is allowed to grow, since going a little over is better than uploading and
   * evicting the same thing.
   */
  static constexpr uint32_t kSoltarPorFotograma = 4;

  void ExpulsarTexturasSiHaceFalta() {
    const uint64_t limite = uint64_t(std::max(texturas_mb_max_, 0)) << 20;
    if (!limite) {
      return;
    }
    // Little by little, from 75 % of the limit. That is what avoids the stutter.
    if (bytes_texturas_ > limite / 4 * 3) {
      SoltarUnasPocasFrias();
    }
    // Safety net: if it still goes over the limit (because almost everything is hot), one batch every 60
    // frames. With the above working, this should almost never trigger.
    if (bytes_texturas_ > limite && fotograma_ >= intento_expulsion_ + 60) {
      intento_expulsion_ = fotograma_;
      SoltarTexturas(limite / 4 * 3, kFotogramasSinUsoParaSoltar, "por encima del limite");
    }
  }

  // The kSoltarPorFotograma oldest cold ones. Without sorting the whole cache: they are picked on the fly.
  void SoltarUnasPocasFrias() {
    std::array<std::pair<uint64_t, uint64_t>, kSoltarPorFotograma> elegidas{};  // (frame, key)
    uint32_t cuantas = 0;
    for (const auto& [clave, textura] : texturas_) {
      // Nor those with a bind in flight, nor those owned by the hash thread.
      if (textura.imagen.imagen == VK_NULL_HANDLE || textura.subir || textura.en_vuelo || textura.huella_trabajo ||
          textura.fotograma == UINT64_MAX ||
          textura.fotograma + kFotogramasSinUsoParaSoltar >= fotograma_) {
        continue;
      }
      if (cuantas < kSoltarPorFotograma) {
        elegidas[cuantas++] = {textura.fotograma, clave};
        continue;
      }
      // Replaces the most recent of the chosen ones, if this one is older.
      uint32_t peor = 0;
      for (uint32_t i = 1; i < cuantas; ++i) {
        if (elegidas[i].first > elegidas[peor].first) {
          peor = i;
        }
      }
      if (textura.fotograma < elegidas[peor].first) {
        elegidas[peor] = {textura.fotograma, clave};
      }
    }
    if (cuantas == 0) {
      return;  // all hot: let it grow rather than thrash
    }
    std::unordered_set<VkImage> imagenes;
    for (uint32_t i = 0; i < cuantas; ++i) {
      auto it = texturas_.find(elegidas[i].second);
      if (it == texturas_.end()) {
        continue;
      }
      imagenes.insert(it->second.imagen.imagen);
      bytes_texturas_ -= std::min(bytes_texturas_, it->second.bytes);
    }
    SoltarImagenes(imagenes);
    texturas_soltadas_ += imagenes.size();
    soltadas_poco_a_poco_ += imagenes.size();
    if (fotograma_ >= aviso_goteo_ + 600) {  // one warning every 600 frames, not one per texture
      aviso_goteo_ = fotograma_;
      REXLOG_INFO("[nativo] C3: cache de texturas cerca del limite: se sueltan frias a goteo ({} en total); "
                  "quedan {} texturas y {} MB de {} MB",
                  soltadas_poco_a_poco_, texturas_.size(), bytes_texturas_ >> 20, texturas_mb_max_);
    }
  }

  /*
   * The last resort, when the GPU has no more memory to give.
   *
   * Once nvMapCreate started failing even for 64 KB there was no way back: the driver ended up
   * quarantining ranges and the screen went black with the audio still playing. Before it gets to that,
   * half the texture cache is released.
   *
   * Normal eviction only touches what has been unused for 120 frames, because images are destroyed at
   * once and the GPU could still be reading them. Here that margin is not needed: it waits for the GPU to
   * finish everything in flight, and then none is in use. It costs a one-frame stutter; crashing costs
   * the game.
   */
  bool SoltarTexturasPorFaltaDeMemoria() override {
    if (texturas_.empty() || bytes_texturas_ == 0 || soltando_por_falta_de_memoria_) {
      return false;  // the guard keeps an allocation inside the cleanup itself from re-entering here
    }
    soltando_por_falta_de_memoria_ = true;
    bool algo = false;
    if (contexto_ && contexto_->EsperarGpuDelTodo()) {
      algo = SoltarTexturas(bytes_texturas_ / 2, 0, "SIN MEMORIA EN LA GPU") > 0;
    }
    soltando_por_falta_de_memoria_ = false;
    return algo;
  }

  // Actually retires a group of images. First the views and their slots (pointed at the empty texture),
  // then the images. Used by both the gradual and the batch eviction.
  void SoltarImagenes(const std::unordered_set<VkImage>& imagenes) {
    ++generacion_texturas_;
    for (auto it = vistas_.begin(); it != vistas_.end();) {
      if (!imagenes.count(it->second.imagen)) {
        ++it;
        continue;
      }
      EscribirImagen(it->second.monton, it->second.ranura, vacias_[it->second.monton].vista);
      montones_[it->second.monton].libres.push_back(it->second.ranura);
      dfn_.vkDestroyImageView(device_, it->second.vista, nullptr);
      vistas_por_imagen_.erase(it->second.imagen);  // all views of that image go
      it = vistas_.erase(it);
    }
    for (auto it = texturas_.begin(); it != texturas_.end();) {
      if (!imagenes.count(it->second.imagen.imagen)) {
        ++it;
        continue;
      }
      QuitarContenidoTextura(it->second, it->first);  // measurement only: nfsmw_nativo_diag_reutilizar
      DestruirImagen(it->second.imagen);
      it = texturas_.erase(it);
    }
  }

  // Evicts unused textures until below `objetivo`. Returns the bytes freed.
  uint64_t SoltarTexturas(uint64_t objetivo, uint64_t edad_minima, const char* motivo) {
    std::vector<std::pair<uint64_t, uint64_t>> candidatas;  // (last frame it was prepared, key)
    for (const auto& [clave, textura] : texturas_) {
      // Nor those with a bind in flight (their image has no memory yet), nor those owned by the hash thread.
      if (textura.imagen.imagen != VK_NULL_HANDLE && !textura.subir && !textura.en_vuelo && !textura.huella_trabajo &&
          textura.fotograma != UINT64_MAX &&
          textura.fotograma + edad_minima < fotograma_) {
        candidatas.emplace_back(textura.fotograma, clave);
      }
    }
    std::sort(candidatas.begin(), candidatas.end());
    const uint64_t antes = bytes_texturas_;
    std::unordered_set<VkImage> imagenes;
    for (const auto& [ultimo, clave] : candidatas) {
      if (bytes_texturas_ <= objetivo) {
        break;
      }
      auto it = texturas_.find(clave);
      imagenes.insert(it->second.imagen.imagen);
      bytes_texturas_ -= std::min(bytes_texturas_, it->second.bytes);
    }
    if (imagenes.empty()) {
      return 0;
    }
    SoltarImagenes(imagenes);
    texturas_soltadas_ += imagenes.size();
    REXLOG_INFO("[nativo] C3: cache de texturas {} ({} MB): fuera {} sin usar desde hace mas de {} fotogramas "
                "({} MB); quedan {} texturas y {} MB ({} soltadas en total)",
                motivo, antes >> 20, imagenes.size(), edad_minima,
                (antes - bytes_texturas_) >> 20, texturas_.size(), bytes_texturas_ >> 20, texturas_soltadas_);
    return antes - bytes_texturas_;
  }

  // ZCULL: clears a depth image by opening a pass with loadOp = CLEAR. Needed for images created without
  // TRANSFER_DST (the only ones the driver gives a ZCULL plane), because vkCmdClearDepthStencilImage
  // requires that usage (VUID-vkCmdClearDepthStencilImage-pRanges-02660). It costs no extra GPU time: it
  // is exactly how NVK implements that command internally (nvk_cmd_clear.c, clear_image opens a pass with
  // loadOp = CLEAR).
  bool BorrarProfundidadEnPase(VkCommandBuffer comandos, const ImagenNativa& imagen,
                               float profundidad, uint32_t stencil) override {
    if (comandos == VK_NULL_HANDLE || imagen.vista == VK_NULL_HANDLE || !imagen.ancho ||
        !imagen.alto) {
      return false;
    }
    TerminarPase();  // a pass cannot be opened inside another
    uint32_t formatos[5] = {0, 0, 0, 0, uint32_t(imagen.formato)};
    const VkRenderPass pase = PaseDe(formatos, kCargaBorrar);
    if (pase == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> vistas{};
    vistas[4] = imagen.vista;
    const VkFramebuffer framebuffer = FramebufferDe(pase, vistas, imagen.ancho, imagen.alto);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue borrado{};
    borrado.depthStencil = {profundidad, stencil};
    VkRenderPassBeginInfo inicio{};
    inicio.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    inicio.renderPass = pase;
    inicio.framebuffer = framebuffer;
    inicio.renderArea.extent = {imagen.ancho, imagen.alto};
    inicio.clearValueCount = 1;
    inicio.pClearValues = &borrado;
    dfn_.vkCmdBeginRenderPass(comandos, &inicio, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdEndRenderPass(comandos);
    ++borrados_profundidad_en_pase_;
    return true;
  }

  // nfsmw_nativo_borrar_area_util. Clears only the `area` rectangle of a color image with a
  // loadOp = CLEAR pass: what NVK does internally for vkCmdClearColorImage (nvk_cmd_clear.c), over that
  // rectangle. What lies outside is not touched (Vulkan only loads and stores the renderArea).
  bool BorrarColorEnPase(VkCommandBuffer comandos, const ImagenNativa& imagen, const VkClearColorValue& color,
                         const VkRect2D& area) override {
    if (comandos == VK_NULL_HANDLE || imagen.vista == VK_NULL_HANDLE || !area.extent.width || !area.extent.height ||
        area.offset.x < 0 || area.offset.y < 0 || uint32_t(area.offset.x) + area.extent.width > imagen.ancho ||
        uint32_t(area.offset.y) + area.extent.height > imagen.alto) {
      return false;
    }
    TerminarPase();  // a pass cannot be opened inside another
    uint32_t formatos[5] = {uint32_t(imagen.formato), 0, 0, 0, 0};
    const VkRenderPass pase = PaseDe(formatos, kCargaBorrar);
    if (pase == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> vistas{};
    vistas[0] = imagen.vista;
    const VkFramebuffer framebuffer = FramebufferDe(pase, vistas, imagen.ancho, imagen.alto);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue borrado{};
    borrado.color = color;
    VkRenderPassBeginInfo inicio{};
    inicio.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    inicio.renderPass = pase;
    inicio.framebuffer = framebuffer;
    inicio.renderArea = area;
    inicio.clearValueCount = 1;
    inicio.pClearValues = &borrado;
    dfn_.vkCmdBeginRenderPass(comandos, &inicio, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdEndRenderPass(comandos);
    return true;
  }

  // nfsmw_nativo_framebuffers_olvidan_vistas. C2 is about to destroy this view (Destruir, with the GPU
  // idle for that image): the FramebufferDe framebuffers that use it are destroyed and removed from the
  // cache. It walks the created framebuffers (dozens); never during a draw.
  // Self-checking guard: the cache and its view list must stay in step (only FramebufferDe inserts) and
  // no pass may be open (whoever destroys the view has already submitted the work). Otherwise: DIFERENCIA
  // in the log and, for the rest of the session, nothing is destroyed at runtime: the affected
  // framebuffers (or the whole cache, if they are out of step) are retired and destroyed at shutdown.
  // Retiring is never worse than the previous behaviour.
  void OlvidarVista(VkImageView vista) override {
    if (vista == VK_NULL_HANDLE || !REXCVAR_GET(nfsmw_nativo_framebuffers_olvidan_vistas)) {
      return;
    }
    const bool a_la_par = framebuffers_vistas_.size() == framebuffers_.size();
    if (!fb_retirar_ && (pase_activo_ || !a_la_par)) {
      fb_retirar_ = true;
      REXLOG_ERROR("[nativo] C6 framebuffers (build 184): DIFERENCIA al destruir la vista {:016X}: {}. Para el resto "
                   "de la sesion los framebuffers afectados se retiran sin destruirlos (se destruyen al cerrar)",
                   uint64_t(reinterpret_cast<uintptr_t>(vista)),
                   pase_activo_ ? "hay un pase abierto (quien destruye la vista no ha enviado el trabajo)"
                                : "la cache y su lista de vistas no van a la par (alguien crea framebuffers por otro "
                                  "camino)");
    }
    uint32_t fuera = 0;
    if (!a_la_par) {
      // Unknown which ones use the view: drop them all (they are rebuilt on request).
      for (auto& [clave, framebuffer] : framebuffers_) {
        fb_retirados_.push_back(framebuffer);
      }
      fuera = uint32_t(framebuffers_.size());
      framebuffers_.clear();
      framebuffers_vistas_.clear();
    } else {
      for (auto it = framebuffers_vistas_.begin(); it != framebuffers_vistas_.end();) {
        if (std::find(it->second.begin(), it->second.end(), vista) == it->second.end()) {
          ++it;
          continue;
        }
        const auto fb = framebuffers_.find(it->first);
        if (fb != framebuffers_.end()) {
          if (fb_retirar_) {
            fb_retirados_.push_back(fb->second);
          } else {
            dfn_.vkDestroyFramebuffer(device_, fb->second, nullptr);
          }
          framebuffers_.erase(fb);
          ++fuera;
        }
        it = framebuffers_vistas_.erase(it);
      }
    }
    ++fb_vistas_olvidadas_;
    fb_olvidados_ += fuera;
    if (fb_vistas_olvidadas_ <= 16 || (fb_vistas_olvidadas_ & 63) == 0) {
      NFSMW_INFORME_ANILLO("[nativo] C6 framebuffers (build 184): vista {:016X} destruida: {} framebuffers {} con "
                           "ella; quedan {} en la cache ({} vistas y {} framebuffers desde el arranque; {} "
                           "retirados)",
                           uint64_t(reinterpret_cast<uintptr_t>(vista)), fuera,
                           fb_retirar_ ? "retirados (sin destruir)" : "destruidos", framebuffers_.size(),
                           fb_vistas_olvidadas_, fb_olvidados_, fb_retirados_.size());
    }
  }

  void OlvidarImagen(VkImage imagen) override {
    ++generacion_texturas_;  // the sampler cache could point to a retired view
    const auto indice = vistas_por_imagen_.find(imagen);  // without walking vistas_
    if (indice == vistas_por_imagen_.end()) {
      return;
    }
    for (uint64_t clave : indice->second) {
      const auto it = vistas_.find(clave);
      if (it == vistas_.end() || it->second.imagen != imagen) {
        continue;
      }
      EscribirImagen(it->second.monton, it->second.ranura, vacias_[it->second.monton].vista);
      montones_[it->second.monton].libres.push_back(it->second.ranura);
      dfn_.vkDestroyImageView(device_, it->second.vista, nullptr);
      vistas_.erase(it);
    }
    vistas_por_imagen_.erase(indice);
  }

  EstadisticasDibujos Estadisticas() const override {
    EstadisticasDibujos e;
    e.dibujados = dibujados_;
    e.rechazados = rechazados_;
    e.pipelines = pipelines_.size();
    e.texturas = texturas_.size();
    e.subidas_textura = subidas_textura_;
    e.megas_subidos = megas_bytes_ >> 20;
    e.megas_texturas = bytes_texturas_ >> 20;
    e.ms_pipelines = ns_pipelines_ / 1000000;
    e.pases = pases_empezados_;
    e.envios_llenos = envios_llenos_;
    e.ns_envios_llenos = ns_envios_llenos_;
    e.bytes_vertices = bytes_vertices_;
    e.dedupe_aciertos = dedupe_.aciertos();
    e.dedupe_bytes = dedupe_.bytes_ahorrados();
    e.dedupe_colisiones = dedupe_.colisiones();
    e.bytes_indices = bytes_indices_subidos_;
    e.samplers = samplers_preparados_;
    e.samplers_cache = samplers_cache_ + samplers_cache_fetch_;
    e.ns_pases = ns_pases_;
    e.ns_vertices = ns_vertices_;
    e.entradas_calculadas = entradas_calculadas_;
    e.ns_entradas = ns_entradas_;
    e.entradas_reutilizadas = entradas_reutilizadas_ + entradas_cache_aciertos_;
    e.pases_por_generacion = pases_por_generacion_;
    e.pases_por_destino = pases_por_destino_;
    e.pases_reanudados = pases_reanudados_;
    e.ns_render_pass = ns_render_pass_;
    e.texels_pases = texels_pases_;
    e.texels_por_categoria = texels_por_categoria_;
    e.dibujos_por_categoria = dibujos_por_categoria_;
    e.dibujos_ps_inutil = dibujos_ps_inutil_;
    e.dibujos_ps_necesario = dibujos_ps_necesario_;
    e.sombras_alfa_activa = sombras_alfa_activa_;
    e.sombras_alfa_apagada = sombras_alfa_apagada_;
    e.triangulos_por_categoria = triangulos_por_categoria_;
    e.pases_por_categoria = pases_por_categoria_;
    e.envios = envios_;
    e.envios_ubo = envios_ubo_;
    e.compartidas_miradas = compartidas_miradas_;
    e.compartidas_cambiadas = compartidas_cambiadas_;
    e.bytes_repetidos_fotograma = bytes_repetidos_fotograma_;
    e.bytes_iguales_anterior = bytes_iguales_anterior_;
    e.ns_hash_vertices = ns_hash_vertices_;
    e.causas.assign(causas_.begin(), causas_.end());
    e.etapas_ns = etapas_ns_;
    e.escena_con_descarte = escena_con_descarte_;
    e.escena_sin_descarte = escena_sin_descarte_;
    e.dibujados_cronometrados = dibujados_cronometrados_;
    e.vegetacion_pronto = dibujos_vegetacion_pronto_;
    std::sort(e.causas.begin(), e.causas.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return e;
  }

 private:
  bool Rechazar(uint32_t causa, const char* texto) {
    ++rechazados_;
    ++causas_[causa];
    ultima_causa_ = causa;
    Avisar(causa, texto);
    return false;
  }

  void Avisar(uint32_t causa, const char* texto) {
    if (avisados_.insert(causa).second) {
      REXLOG_WARN("[nativo] C6: {} (causa {})", texto, causa);
    }
  }

  // --- Texture binds on a separate thread (nfsmw_nativo_texturas_enlace_hilo) ----------------------------------
  /*
   * Why. With the memory pool, creating a texture no longer asks the system for memory, but vkBindImageMemory
   * still makes two ioctls: reserving the plane's VA with its pte_kind and mapping the pool chunk into it
   * (nvk_image_plane_bind, nvk_image.c:1696-1710; vkCreateImage reserves no VA except for sparse images,
   * :1337-1350). All our textures are tiled with pte_kind GENERIC_16BX2 (nil/image.rs:439-440 and 876-918),
   * so both are always paid. In a race: 336 VA reservations in 95 ms and 336 mappings in 111 ms, ~0.6 ms of
   * wall time per texture with the ring asleep in the ioctl. Entering a new zone brings 33-45 textures in
   * one frame: 19-27 ms of stalled ring that did not show up in "texturas X ms".
   *
   * How. The ring does the CPU part (vkCreateImage, requirements, pool chunk, descriptor slot) and only
   * queues the vkBindImageMemory. The draw is recorded with its slot and SubirTextura leaves the data in the
   * upload buffer as usual. In AntesDeEnviar (RecogerEnlaces) it waits for whatever is missing, creates the
   * views, writes the descriptors (UPDATE_AFTER_BIND: legal until submission) and records the barrier and
   * the copy in that same upload buffer, which goes in the same vkQueueSubmit, ahead of the work. The GPU
   * receives exactly the same thing in the same submission.
   *
   * The driver. vkBindImageMemory from another thread is safe: the VA and the mapping are under
   * device->va_mutex (horizon/nouveau_horizon_vm.c:173 and 377), the mapping refcount under
   * memory_identity_mutex (nouveau_horizon_memory.c:1065), and everything else belongs to that image. The
   * pool is not touched from the thread: it stays single-threaded (nfsmw_nativo_texturas_pool.h).
   *
   * Threads (see docs/platform-notes.md, Threads). Shared state is under enlaces_mutex_, and every decision
   * to sleep or wake is taken with the lock held; the thread only reads the image, memory and offset of its
   * request and stores the result under the lock. The ring's wait has a timeout and logs every 2 s.
   * Persistent thread (never detached), joined in the destructor before any image is destroyed. Priority
   * 0x2D: a host service, above the guest.
   *
   * Self-checking guard. Observing phase: the first kEnlacesAComprobar wait for their bind right after
   * queuing it (same order as before: only the thread doing the ioctl changes). If all succeed, applying
   * phase. In any phase, a failed bind, a wait of more than 2 s or a deferred copy that can no longer go in
   * its upload buffer: REXLOG_ERROR with the DIFERENCIA and off for the session (the usual path returns;
   * whatever was in flight finishes through the fallback path). And if for 3 reports in a row the ring
   * waits for the thread longer than the thread takes to bind, it does not pay off: it switches off too.
   */
  void DecidirEnlaces() {
    const bool pedido = REXCVAR_GET(nfsmw_nativo_texturas_enlace_hilo);
    enlaces_prioridad_ = std::clamp<int32_t>(REXCVAR_GET(nfsmw_nativo_texturas_enlace_hilo_prioridad), 0x2C, 0x3B);
    enlaces_fase_ = pedido ? kEnlacesMirando : kEnlacesApagado;
    REXLOG_INFO("[nativo] C3: enlace de las texturas nuevas en un hilo aparte (nfsmw_nativo_texturas_enlace_hilo) = {}",
                pedido ? fmt::format("SI, prioridad {:#x}; fase MIRANDO: las {} primeras esperan su enlace al momento "
                                     "(solo texturas del pool, que esta {})",
                                     enlaces_prioridad_, kEnlacesAComprobar,
                                     pool_texturas_.Activo() ? "encendido" : "APAGADO: no se usara")
                       : std::string("no, en el hilo del anillo como siempre"));
  }

  void ApagarEnlaces(const std::string& motivo) {
    if (enlaces_fase_ == kEnlacesApagado) {
      return;
    }
    enlaces_fase_ = kEnlacesApagado;
    REXLOG_ERROR("[nativo] C3: hilo de enlaces de texturas APAGADO para el resto de la sesion: {}. Las texturas nuevas "
                 "vuelven a crearse enteras en el hilo del anillo ({} enlazadas en el hilo, {} fallidas, {} vistas y {} "
                 "copias aplazadas)",
                 motivo, enlaces_hilo_total_, enlaces_fallidos_, vistas_aplazadas_, copias_aplazadas_);
  }

  // Queues the vkBindImageMemory of an image. false = not queued (queue full or no thread): the usual path.
  bool EncolarEnlace(VkImage imagen, VkDeviceMemory memoria, VkDeviceSize offset, uint64_t& ticket) {
    if (!enlaces_hilo_.joinable()) {
      try {
        enlaces_hilo_ = std::thread([this] { BucleEnlaces(); });
      } catch (const std::system_error& error) {
        ApagarEnlaces(fmt::format("DIFERENCIA: no se pudo crear el hilo ({})", error.what()));
        return false;
      }
    }
    bool avisar = false;
    {
      std::lock_guard<std::mutex> cerrojo(enlaces_mutex_);
      if (enlaces_pedidos_ - enlaces_recogidos_ >= kColaEnlaces) {
        ++enlaces_cola_llena_;
        return false;
      }
      PeticionEnlace& peticion = cola_enlaces_[enlaces_pedidos_ & (kColaEnlaces - 1)];
      peticion.imagen = imagen;
      peticion.memoria = memoria;
      peticion.offset = offset;
      peticion.resultado = VK_NOT_READY;
      peticion.ns = 0;
      ticket = enlaces_pedidos_++;
      avisar = enlaces_durmiendo_;  // decided under the lock: if it sleeps, it is inside its wait with the predicate
    }
    if (avisar) {
      enlaces_cv_.notify_one();
    }
    return true;
  }

  // The thread: the requests' vkBindImageMemory calls, in order. It sleeps on a decision taken under the
  // lock; it never spins. It does not write to the log (the ring records the figures in InformeEnlaces).
  void BucleEnlaces() {
    rex::thread::set_current_thread_name("NFSMW enlaces de texturas");
#if REX_PLATFORM_SWITCH
    const bool prioridad_ok = RexSwitchSetCurrentThreadPriorityOk(int(enlaces_prioridad_));
#else
    const bool prioridad_ok = true;
#endif
    std::unique_lock<std::mutex> cerrojo(enlaces_mutex_);
    enlaces_prioridad_ok_ = prioridad_ok;
    for (;;) {
      if (enlaces_hechos_ == enlaces_pedidos_) {
        if (enlaces_parar_) {
          return;  // stop, nothing pending
        }
        if (enlaces_esperando_) {
          enlaces_hechos_cv_.notify_one();
        }
        enlaces_durmiendo_ = true;
        enlaces_cv_.wait(cerrojo, [this] { return enlaces_parar_ || enlaces_hechos_ != enlaces_pedidos_; });
        enlaces_durmiendo_ = false;
        continue;
      }
      // The slot is not reused until the ring collects it (enlaces_recogidos_): it stays ours without the lock.
      PeticionEnlace& peticion = cola_enlaces_[enlaces_hechos_ & (kColaEnlaces - 1)];
      const VkImage imagen = peticion.imagen;
      const VkDeviceMemory memoria = peticion.memoria;
      const VkDeviceSize offset = peticion.offset;
      cerrojo.unlock();
      const auto antes = std::chrono::steady_clock::now();
      const VkResult resultado = dfn_.vkBindImageMemory(device_, imagen, memoria, offset);
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - antes)
                                       .count());
      cerrojo.lock();
      peticion.resultado = resultado;
      peticion.ns = ns;
      ns_enlaces_hilo_ += ns;
      ns_enlace_peor_ = std::max(ns_enlace_peor_, ns);
      ++enlaces_hechos_;
      if (enlaces_esperando_) {
        enlaces_hechos_cv_.notify_one();
      }
    }
  }

  void PararEnlaces() {
    if (!enlaces_hilo_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> cerrojo(enlaces_mutex_);
      enlaces_parar_ = true;
    }
    enlaces_cv_.notify_one();
    enlaces_hilo_.join();  // first finishes every queued request
    REXLOG_INFO("[nativo] C3: hilo de enlaces de texturas parado ({} enlazadas en el hilo, {} fallidas, {} vistas y {} "
                "copias aplazadas, {} ranuras perdidas)",
                enlaces_hilo_total_, enlaces_fallidos_, vistas_aplazadas_, copias_aplazadas_, ranuras_perdidas_);
  }

  // The ring waits until the thread has done the requests before `objetivo`. With a timeout and a log line
  // every 2 s.
  void EsperarEnlaces(uint64_t objetivo) {
    std::unique_lock<std::mutex> cerrojo(enlaces_mutex_);
    if (enlaces_hechos_ >= objetivo) {
      return;
    }
    const auto antes = std::chrono::steady_clock::now();
    enlaces_esperando_ = true;
    if (enlaces_durmiendo_) {
      enlaces_cv_.notify_one();  // should not be needed (it has work); in case a wake-up was lost
    }
    int segundos = 0;
    while (!enlaces_hechos_cv_.wait_for(cerrojo, std::chrono::seconds(2),
                                        [this, objetivo] { return enlaces_hechos_ >= objetivo; })) {
      segundos += 2;
      enlaces_atascado_ = true;
      REXLOG_ERROR("[nativo] C3: el hilo del anillo lleva {} s esperando los enlaces de texturas: objetivo {}, hechos "
                   "{}, pedidos {}; el hilo esta {}",
                   segundos, objetivo, enlaces_hechos_, enlaces_pedidos_,
                   enlaces_durmiendo_ ? "DORMIDO (aviso perdido)" : "despierto (un ioctl que no vuelve)");
    }
    enlaces_esperando_ = false;
    cerrojo.unlock();
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - antes)
                                     .count());
    ns_espera_enlaces_total_ += ns;
    ns_espera_enlaces_informe_ += ns;
    ns_espera_enlaces_peor_ = std::max(ns_espera_enlaces_peor_, ns);
    ++esperas_enlaces_informe_;
    nfsmw::esperas::g_ns_esperando_enlaces.fetch_add(ns, std::memory_order_relaxed);
  }

  /*
   * The CPU part of CrearTextura (image, requirements, pool chunk) here, and its vkBindImageMemory to the
   * thread. false = nothing was touched and the caller continues through CrearTextura: thread off, pool off
   * or full, queue full, or (in the observing phase) a failed bind whose fallback path also failed.
   */
  bool CrearTexturaEnHilo(Textura& textura, VkFormat formato, uint32_t ancho, uint32_t alto, uint32_t capas,
                          uint32_t fondo, uint32_t niveles) {
    if (enlaces_fase_ == kEnlacesSinDecidir) {
      DecidirEnlaces();
    }
    // With the out-of-memory test enabled, everything goes through CrearTextura: that test lives there.
    if (enlaces_fase_ == kEnlacesApagado || !pool_texturas_.Activo() || prueba_sin_memoria_cada_ > 0) {
      return false;
    }
    const VkImageCreateInfo info = InfoImagenTextura(formato, ancho, alto, capas, fondo, niveles);
    VkImage imagen = VK_NULL_HANDLE;
    if (dfn_.vkCreateImage(device_, &info, nullptr, &imagen) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    dfn_.vkGetImageMemoryRequirements(device_, imagen, &req);
    VkDeviceMemory bloque = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t id_bloque = 0xFFFFFFFFu;
    if (!pool_texturas_.Reservar(req, bloque, offset, id_bloque)) {
      dfn_.vkDestroyImage(device_, imagen, nullptr);
      return false;  // no room in the pool: CrearTextura, which can take the dedicated path
    }
    uint64_t ticket = 0;
    if (!EncolarEnlace(imagen, bloque, offset, ticket)) {
      dfn_.vkDestroyImage(device_, imagen, nullptr);  // before the chunk, as the pool requires (not bound yet)
      pool_texturas_.Liberar(id_bloque);
      return false;
    }
    textura.imagen.imagen = imagen;
    textura.imagen.memoria = VK_NULL_HANDLE;  // from the pool: not freed on its own
    textura.imagen.pool_bloque = id_bloque;
    textura.imagen.ancho = ancho;
    textura.imagen.alto = alto;
    textura.imagen.formato = formato;
    textura.imagen.preparada = false;
    TexturaEnVuelo vuelo;
    vuelo.textura = &textura;
    vuelo.ticket = ticket;
    vuelo.imagen = imagen;
    vuelo.formato = formato;
    vuelo.ancho = ancho;
    vuelo.alto = alto;
    vuelo.capas = capas;
    vuelo.fondo = fondo;
    vuelo.niveles = niveles;
    en_vuelo_.push_back(vuelo);
    textura.en_vuelo = uint32_t(en_vuelo_.size());
    imagenes_en_vuelo_.insert(imagen);
    if (enlaces_fase_ == kEnlacesMirando) {
      // Observing phase: wait now, before RanuraVista and SubirTextura: everything stays as without the thread.
      RecogerEnlaces(true);
      if (textura.imagen.imagen == VK_NULL_HANDLE) {
        return false;  // neither the thread nor the fallback (already logged): CrearTextura, with its emergency path
      }
      if (++enlaces_comprobados_ >= kEnlacesAComprobar && enlaces_fase_ == kEnlacesMirando) {
        enlaces_fase_ = kEnlacesAplicando;
        uint64_t ns_hilo = 0;
        bool prioridad_ok = true;
        {
          std::lock_guard<std::mutex> cerrojo(enlaces_mutex_);
          ns_hilo = ns_enlaces_hilo_;
          prioridad_ok = enlaces_prioridad_ok_;
        }
        REXLOG_INFO("[nativo] C3: hilo de enlaces de texturas: {} enlaces con espera inmediata y ninguno fallido ({:.0f} "
                    "us de media dentro de vkBindImageMemory; el anillo espero {:.0f} us de media por textura). Pasa a "
                    "fase APLICANDO: la espera va antes de cada envio{}",
                    enlaces_comprobados_, double(ns_hilo) / 1e3 / double(enlaces_comprobados_),
                    double(ns_espera_enlaces_total_) / 1e3 / double(enlaces_comprobados_),
                    prioridad_ok ? "" : " (el kernel NO acepto la prioridad del hilo)");
      }
    }
    return true;
  }

  // RanuraVista for an image with its bind in flight: the slot now, the view in RecogerEnlaces.
  uint32_t RanuraVistaEnVuelo(uint64_t clave, VkImage imagen, VkFormat formato, uint32_t swizzle,
                              uint16_t swizzle_host, uint32_t monton) {
    Vista vista;
    vista.imagen = imagen;
    vista.monton = monton;
    vista.ranura = ReservarRanura(monton);
    if (!vista.ranura) {
      Avisar(35, "monton de texturas lleno");
      return 0;
    }
    vistas_.emplace(clave, vista);  // vista.vista stays VK_NULL_HANDLE until RecogerEnlaces
    vistas_por_imagen_[imagen].push_back(clave);
    VistaEnVuelo aplazada;
    aplazada.clave = clave;
    aplazada.imagen = imagen;
    aplazada.formato = formato;
    aplazada.swizzle = swizzle;
    aplazada.swizzle_host = swizzle_host;
    aplazada.monton = monton;
    aplazada.ranura = vista.ranura;
    vistas_en_vuelo_.push_back(aplazada);
    ++vistas_aplazadas_;
    ++vistas_aplazadas_informe_;
    return vista.ranura;
  }

  // SubirTextura for a texture with its bind in flight: the data is already in the upload buffer; record
  // where.
  bool AplazarCopia(Textura& textura, VkDeviceSize offset, VkCommandBuffer subida) {
    const size_t indice = size_t(textura.en_vuelo) - 1;
    if (indice >= en_vuelo_.size() || en_vuelo_[indice].textura != &textura || en_vuelo_[indice].copia) {
      ApagarEnlaces(fmt::format("DIFERENCIA: indice de textura en vuelo incoherente ({} de {})", indice,
                                en_vuelo_.size()));
      return false;
    }
    TexturaEnVuelo& vuelo = en_vuelo_[indice];
    vuelo.copia = true;
    vuelo.copia_offset = offset;
    vuelo.copia_epoca = epoca_subida_;
    vuelo.copia_comandos = subida;
    ++copias_aplazadas_;
    ++copias_aplazadas_informe_;
    return true;
  }

  // The deferred views of an image that had to be recreated: new key (it goes with the image) and its list.
  void MoverVistasEnVuelo(VkImage vieja, VkImage nueva) {
    if (vieja == nueva) {
      return;
    }
    std::vector<uint64_t> claves;
    for (VistaEnVuelo& aplazada : vistas_en_vuelo_) {
      if (aplazada.imagen != vieja) {
        continue;
      }
      const auto it = vistas_.find(aplazada.clave);
      if (it == vistas_.end()) {
        continue;
      }
      Vista vista = it->second;
      vistas_.erase(it);
      vista.imagen = nueva;
      aplazada.imagen = nueva;
      if (nueva != VK_NULL_HANDLE) {
        aplazada.clave = ClaveVista(nueva, aplazada.swizzle, aplazada.swizzle_host, aplazada.monton);
      }
      if (!vistas_.emplace(aplazada.clave, vista).second) {
        aplazada.clave = 0;  // key collision: RecogerEnlaces puts the empty one in its slot
        continue;
      }
      claves.push_back(aplazada.clave);
    }
    vistas_por_imagen_.erase(vieja);  // an image in flight only has deferred views
    if (nueva != VK_NULL_HANDLE && !claves.empty()) {
      auto& lista = vistas_por_imagen_[nueva];
      lista.insert(lista.end(), claves.begin(), claves.end());
    }
  }

  /*
   * Finishes everything the bind thread has. Ring thread only. `con_copias` = false outside AntesDeEnviar
   * (the upload buffer would no longer be the one holding that data): those textures are uploaded again at
   * their next check.
   */
  void RecogerEnlaces(bool con_copias) {
    // No re-entry: if something in here ended up starting new work (UsarRanura), that call does nothing and
    // the outer one sees the upload buffer changed (DIFERENCIA and a full upload at the next check).
    if (en_vuelo_.empty() || recogiendo_enlaces_) {
      return;
    }
    recogiendo_enlaces_ = true;
    const auto inicio = std::chrono::steady_clock::now();
    const uint64_t espera_antes = ns_espera_enlaces_total_;
    const uint64_t objetivo = en_vuelo_.back().ticket + 1;
    EsperarEnlaces(objetivo);
    if (enlaces_atascado_) {
      enlaces_atascado_ = false;
      ApagarEnlaces("DIFERENCIA: una espera de mas de 2 s al hilo (ver las lineas de arriba)");
    }
    {
      std::lock_guard<std::mutex> cerrojo(enlaces_mutex_);
      for (TexturaEnVuelo& vuelo : en_vuelo_) {
        vuelo.resultado = cola_enlaces_[vuelo.ticket & (kColaEnlaces - 1)].resultado;
      }
      enlaces_recogidos_ = objetivo;  // from here on its slots can be reused
    }
    // 1. Results, in order. If a bind failed, that image is dropped (no command references it yet) and the
    //    texture is created again through CrearTextura without the emergency path: releasing half the cache
    //    submits the work, and we are before that.
    for (TexturaEnVuelo& vuelo : en_vuelo_) {
      Textura& textura = *vuelo.textura;
      imagenes_en_vuelo_.erase(vuelo.imagen);
      textura.en_vuelo = 0;
      if (vuelo.resultado == VK_SUCCESS) {
        ++enlaces_hilo_total_;
        ++enlaces_hilo_informe_;
        nfsmw::esperas::g_texturas_enlazadas_hilo.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ++enlaces_fallidos_;
      ApagarEnlaces(fmt::format("DIFERENCIA: vkBindImageMemory devolvio {} en el hilo para una textura de {}x{} ({} "
                                "niveles, {} capas)",
                                int32_t(vuelo.resultado), vuelo.ancho, vuelo.alto, vuelo.niveles, vuelo.capas));
      dfn_.vkDestroyImage(device_, vuelo.imagen, nullptr);
      pool_texturas_.Liberar(textura.imagen.pool_bloque);
      textura.imagen.imagen = VK_NULL_HANDLE;
      textura.imagen.pool_bloque = 0xFFFFFFFFu;
      if (!CrearTextura(textura.imagen, vuelo.formato, vuelo.ancho, vuelo.alto, vuelo.capas, vuelo.fondo,
                        vuelo.niveles, false)) {
        // Not even then. The texture is left without an image: its draws in this submission see the empty one
        // (the same as when it cannot be created) and the next time it is used it is created in full, with the
        // emergency path.
        textura.imagen = ImagenNativa{};
        bytes_texturas_ -= std::min(bytes_texturas_, textura.bytes);
        textura.bytes = 0;
      }
      MoverVistasEnVuelo(vuelo.imagen, textura.imagen.imagen);
    }
    // 2. Deferred views: created and written to their slot. If that is not possible, the slot keeps the
    //    empty one (what a draw sees when RanuraVista returns 0) and is not reused: this submission
    //    references it (it is counted).
    for (const VistaEnVuelo& aplazada : vistas_en_vuelo_) {
      const auto it = aplazada.clave ? vistas_.find(aplazada.clave) : vistas_.end();
      if (it != vistas_.end() && it->second.imagen != VK_NULL_HANDLE &&
          CrearVistaTextura(it->second.imagen, aplazada.formato, aplazada.swizzle, aplazada.swizzle_host,
                            aplazada.monton, it->second.vista) == VK_SUCCESS) {
        EscribirImagen(aplazada.monton, aplazada.ranura, it->second.vista);
        continue;
      }
      EscribirImagen(aplazada.monton, aplazada.ranura, vacias_[aplazada.monton].vista);
      if (it != vistas_.end()) {
        vistas_.erase(it);  // the image has no vistas_por_imagen_ left (MoverVistasEnVuelo) or is rebuilt when used
      }
      ++ranuras_perdidas_;
      Avisar(34, "no se pudo crear la vista de una textura");
    }
    vistas_en_vuelo_.clear();
    // 3. Deferred barriers and copies, in the upload buffer where SubirTextura left their data.
    VkCommandBuffer subida = VK_NULL_HANDLE;
    for (const TexturaEnVuelo& vuelo : en_vuelo_) {
      if (!vuelo.copia) {
        continue;
      }
      Textura& textura = *vuelo.textura;
      if (textura.imagen.imagen == VK_NULL_HANDLE) {
        continue;  // no image: created and fully uploaded next time it is used
      }
      if (con_copias && subida == VK_NULL_HANDLE) {
        subida = contexto_->ComandosSubida();
      }
      if (!con_copias || subida == VK_NULL_HANDLE || subida != vuelo.copia_comandos ||
          vuelo.copia_epoca != epoca_subida_) {
        ApagarEnlaces(fmt::format("DIFERENCIA: una copia aplazada ya no puede ir en su bufer de subida (epoca {} "
                                  "frente a {}, {})",
                                  vuelo.copia_epoca, epoca_subida_,
                                  con_copias ? "otro bufer de subida" : "recogida al cambiar de bufer"));
        textura.imagen.preparada = false;  // the next check uploads it again in full, with its barrier
        textura.siguiente = 0;
        continue;
      }
      if (!textura.imagen.preparada) {
        Barrera(subida, textura.imagen.imagen, textura.capas);
        textura.imagen.preparada = true;
      }
      GrabarCopiaTextura(subida, textura, vuelo.copia_offset);
    }
    en_vuelo_.clear();
    recogiendo_enlaces_ = false;
    if (!midiendo_creacion_) {  // inside PrepararTextura its stopwatch already counts it
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - inicio)
                                       .count());
      const uint64_t espera = ns_espera_enlaces_total_ - espera_antes;
      const uint64_t propio = ns > espera ? ns - espera : 0;
      nfsmw::esperas::g_ns_crear_texturas.fetch_add(propio, std::memory_order_relaxed);
      ns_crear_informe_ += propio;
    }
  }

  // UsarRanura: a texture with its bind in flight may reach new work (it was prepared before that work's
  // first Grabar), but not with its data already in the previous upload buffer: that should never happen.
  void ComprobarEnlacesAlCambiarDeBufer() {
    for (const TexturaEnVuelo& vuelo : en_vuelo_) {
      if (vuelo.copia) {
        RecogerEnlaces(false);  // logs the DIFERENCIA, switches off and leaves those textures to be uploaded again
        return;
      }
    }
  }

  // Every 10 s, if there were new textures: what the thread did and how long the ring waited. This is also
  // where it is decided whether it pays off: if for 3 reports in a row the ring waits for the thread longer
  // than the thread takes to bind, the ring would do better doing it itself, and it switches off.
  void InformeEnlaces() {
    if (enlaces_fase_ == kEnlacesSinDecidir) {
      return;
    }
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - informe_enlaces_ < std::chrono::seconds(10)) {
      return;
    }
    informe_enlaces_ = ahora;
    uint64_t ns_hilo = 0;
    uint64_t peor_hilo = 0;
    uint64_t cola_llena = 0;
    bool prioridad_ok = true;
    {
      std::lock_guard<std::mutex> cerrojo(enlaces_mutex_);
      ns_hilo = ns_enlaces_hilo_ - ns_enlaces_hilo_previo_;
      ns_enlaces_hilo_previo_ = ns_enlaces_hilo_;
      peor_hilo = ns_enlace_peor_;
      ns_enlace_peor_ = 0;
      cola_llena = enlaces_cola_llena_ - cola_llena_previa_;
      cola_llena_previa_ = enlaces_cola_llena_;
      prioridad_ok = enlaces_prioridad_ok_;
    }
    const uint64_t creadas = texturas_creadas_ - creadas_informe_previas_;
    creadas_informe_previas_ = texturas_creadas_;
    if (creadas || enlaces_hilo_informe_ || esperas_enlaces_informe_) {
      static constexpr const char* kFases[] = {"apagada", "MIRANDO", "APLICANDO"};
      NFSMW_INFORME_ANILLO(
          "[nativo] C3 texturas nuevas (build 184), ultimos 10 s: {} creadas, {} con el enlace en el hilo; el anillo "
          "gasto {:.1f} ms creando y espero al hilo {:.1f} ms en {} veces (peor {:.2f} ms); el hilo, {:.1f} ms dentro de "
          "vkBindImageMemory (media {:.0f} us, peor {:.2f} ms); {} vistas y {} copias aplazadas; {} por la ruta de "
          "siempre con la cola llena; fase {}{}",
          creadas, enlaces_hilo_informe_, double(ns_crear_informe_) / 1e6, double(ns_espera_enlaces_informe_) / 1e6,
          esperas_enlaces_informe_, double(ns_espera_enlaces_peor_) / 1e6, double(ns_hilo) / 1e6,
          enlaces_hilo_informe_ ? double(ns_hilo) / 1e3 / double(enlaces_hilo_informe_) : 0.0,
          double(peor_hilo) / 1e6, vistas_aplazadas_informe_, copias_aplazadas_informe_, cola_llena,
          kFases[std::clamp<int32_t>(enlaces_fase_, 0, 2)], prioridad_ok ? "" : " (el kernel NO acepto la prioridad)");
    }
    if (enlaces_fase_ == kEnlacesAplicando && enlaces_hilo_informe_ >= 16) {
      informes_sin_compensar_ = ns_espera_enlaces_informe_ > ns_hilo ? informes_sin_compensar_ + 1 : 0;
      if (informes_sin_compensar_ >= 3) {
        ApagarEnlaces(fmt::format("no compensa: en 3 informes seguidos el anillo espero al hilo mas de lo que el hilo "
                                  "tardo en enlazar (el ultimo, {:.1f} ms de espera por {:.1f} ms de enlaces)",
                                  double(ns_espera_enlaces_informe_) / 1e6, double(ns_hilo) / 1e6));
      }
    }
    enlaces_hilo_informe_ = 0;
    esperas_enlaces_informe_ = 0;
    ns_espera_enlaces_informe_ = 0;
    ns_espera_enlaces_peor_ = 0;
    ns_crear_informe_ = 0;
    vistas_aplazadas_informe_ = 0;
    copias_aplazadas_informe_ = 0;
  }

  // --- Hashing of new textures on a separate thread (nfsmw_nativo_texturas_huella_hilo) ------------------------
  /*
   * Why. During zone-change stutters the ring spends 5.3-8.7 ms of the frame in "texturas": the XXH3 of
   * guest memory (2.3-3.1 ms, with some rechecks included) and the untiling and byte swapping of the new
   * ones (3.0-5.6 ms, 1.8-3.1 ms per MB); and outside that figure, the copy to the upload buffer (~0.4 ms
   * per MB). The ring is the bottleneck (the GPU waits 5-11 ms per frame).
   *
   * Exact. The hash and the untiling read guest memory, which the game may reuse as soon as the ring
   * returns the read pointer or writes a fence. So the thread is never given guest addresses: the ring
   * copies, right there (where the inline path computes the hash), the bytes the hash covers (base and
   * mips, the same ranges) into a snapshot, and the thread works on that copy. They are the same bytes read
   * at the same point, and the hash, the untiling and the byte swap are functions of those bytes: the
   * results are identical. Every read of the plan falls inside the copy (PlanearLecturaHuella; the bound was
   * tested against both untiling paths on 145,500 rectangles). The thread writes the result into the space
   * SubirTextura reserves in the upload buffer (same size, same order), and the barrier and the copy are
   * recorded where they always are: the GPU does not read that buffer until vkQueueSubmit, and
   * AntesDeEnviar (RecogerHuellas) waits before that. Same submission, same bytes: no placeholder textures
   * and no frame of delay. The texture's huella_cruda is set when collected, before anything looks at it.
   *
   * The ring never waits for more than one job. When collecting, it takes the ones the thread has not
   * started (from the end of the queue) and does them itself; it only waits for the one the thread is in
   * the middle of. If the thread has no core, the ring ends up doing the usual work at submission, plus the
   * copy, and the "no compensa" guard switches it off.
   *
   * Threads (see docs/platform-notes.md, Threads). All shared state is under huellas_mutex_, and every
   * decision to sleep or wake is taken with the lock held: no "store mine and read yours" with atomics.
   * The thread only reads the snapshot and the plan of its job (written before publishing it) and only
   * writes its buffers, its space in the upload buffer and its job's results; it does not write to the log.
   * Persistent thread (never detached), joined in the destructor before the upload buffer is released.
   * Priority 0x2E: the ring (0x2D) preempts it as soon as it has work (equal priorities do not time-slice)
   * and the guest (0x3B) does not starve it of a core; it also prefers a core other than the ring's, which
   * is busy.
   *
   * Self-checking guard. Observing phase: the first kHuellasAComprobar new textures go through the usual
   * path (that is what gets uploaded) and also through the thread, which returns its raw hash and the XXH3
   * of its data; they are compared when collected. All equal: applying phase, and 1 in
   * kComprobarHuellaUnaDeCada keeps being compared. A DIFERENCIA, a wait of more than 2 s, a job that does
   * not add up, or no payoff for 3 reports in a row: REXLOG_ERROR and off for the session. After a
   * DIFERENCIA, the thread's remaining textures are uploaded again in full. The worst case is the usual
   * path.
   */
  struct LecturaHuella {  // one LeerNivel call, reading from the snapshot
    uint64_t origen = 0;  // desplazamiento en instantaneas_
    uint64_t fila_bytes = 0;
    size_t destino = 0;  // offset in the texture data
    uint32_t pitch_bloques = 0;
    uint32_t ox = 0;
    uint32_t oy = 0;
    uint32_t bx = 0;
    uint32_t by = 0;
    uint32_t bx_host = 0;
    bool mosaico = false;
    bool comprobar = false;  // the fast untiling guard repeats this level on the usual path
  };
  struct TrabajoHuella {
    // Written by the ring before publishing; whoever does the job (the thread or the ring) only reads it.
    Textura* textura = nullptr;  // used by the ring only; in huellas_planeadas_, nullptr = already published or discarded
    uint64_t clave = 0;
    uint64_t secuencia = 0;
    uint64_t inst_base = 0;  // the copy of the base (all layers) in instantaneas_
    uint64_t bytes_base = 0;
    uint64_t inst_mips = 0;  // the copy of the mips
    uint64_t bytes_mips = 0;
    size_t bytes_datos = 0;
    uint8_t* destino = nullptr;  // its space in the upload buffer; nullptr for comparison jobs
    uint64_t epoca = 0;          // epoca_subida_ of that buffer
    uint32_t lectura_inicio = 0;  // su plan: lecturas_huella_[lectura_inicio, lectura_inicio + lecturas)
    uint32_t lecturas = 0;
    uint32_t bytes_bloque = 1;
    uint32_t log2_bloque = 0;
    uint32_t unidad_orden = 0;
    uint32_t orden = 0;
    uint32_t direccion = 0;  // for the log
    uint32_t ancho = 0;
    uint32_t alto = 0;
    uint32_t formato = 0;
    bool rapido = false;           // mosaico_rapido_ al planear
    bool comprobar_orden = false;  // the fast byte swap guard checks this texture
    bool comparar = false;         // observing phase (or 1 in kComprobarHuellaUnaDeCada): only returns its hashes
    bool aviso_niveles = false;    // collected without differences: the "niveles comprobados ... todos iguales" line
    bool aviso_ordenes = false;
    // Written by whoever does the job; the ring reads it under huellas_mutex_ once it is done.
    int32_t estado = 0;
    bool por_anillo = false;
    bool orden_distinto = false;
    uint32_t niveles_distintos = 0;
    std::array<uint32_t, 5> distinto{};  // first differing level: blocks in x and y, pitch and origin (x, y)
    uint64_t huella_cruda = 0;
    uint64_t huella_datos = 0;  // comparison jobs only
    uint64_t ns_huella = 0;
    uint64_t ns_resto = 0;
  };
  struct ComparacionHuella {  // ring only: what the usual path produced for a comparison texture
    uint64_t cruda = 0;
    uint64_t datos = 0;
    bool anotada = false;
  };
  static constexpr size_t kLecturasPorTextura = 6 * 16;  // capas x niveles de mip
  static constexpr uint32_t kLecturasHuella = 8192;       // reads planned between two collections
  static constexpr uint64_t kColaHuellas = 256;           // potencia de 2
  static constexpr uint32_t kTrabajoHuellaPublicado = 0xFFFFFFFFu;  // in Textura::huella_trabajo
  static constexpr int32_t kTrabajoPendiente = 1;
  static constexpr int32_t kTrabajoEnHilo = 2;
  static constexpr int32_t kTrabajoEnAnillo = 3;
  static constexpr int32_t kTrabajoHecho = 4;
  static constexpr int32_t kHuellasSinDecidir = -1;
  static constexpr int32_t kHuellasApagado = 0;
  static constexpr int32_t kHuellasMirando = 1;
  static constexpr int32_t kHuellasAplicando = 2;
  static constexpr uint64_t kHuellasAComprobar = 64;
  static constexpr uint64_t kComprobarHuellaUnaDeCada = 128;

  void DecidirHuellas() {
    const bool pedido = REXCVAR_GET(nfsmw_nativo_texturas_huella_hilo);
    huellas_prioridad_ = std::clamp<int32_t>(REXCVAR_GET(nfsmw_nativo_texturas_huella_hilo_prioridad), 0x2C, 0x3B);
    huellas_nucleo_pedido_ = std::clamp<int32_t>(REXCVAR_GET(nfsmw_nativo_texturas_huella_hilo_nucleo), -2, 2);
    instantaneas_bytes_ = uint64_t(std::clamp<int32_t>(REXCVAR_GET(nfsmw_nativo_texturas_huella_hilo_mb), 4, 64))
                          << 20;
    huellas_fase_ = pedido ? kHuellasMirando : kHuellasApagado;
    REXLOG_INFO("[nativo] C3: huella y desenmosaicado de las texturas nuevas en un hilo aparte "
                "(nfsmw_nativo_texturas_huella_hilo) = {}",
                pedido ? fmt::format("SI, prioridad {:#x}, {} MB de instantaneas; fase MIRANDO: las {} primeras van "
                                     "tambien por el camino de siempre y se comparan",
                                     huellas_prioridad_, instantaneas_bytes_ >> 20, kHuellasAComprobar)
                       : std::string("no, en el hilo del anillo como siempre"));
  }

  void ApagarHuellas(const std::string& motivo) {
    if (huellas_fase_ == kHuellasApagado) {
      return;
    }
    huellas_fase_ = kHuellasApagado;
    REXLOG_ERROR("[nativo] C3: hilo de huellas de texturas APAGADO para el resto de la sesion: {}. Las texturas nuevas "
                 "vuelven a prepararse enteras en el hilo del anillo ({} hechas por el hilo, {} por el anillo, {} "
                 "comparadas)",
                 motivo, huellas_hilo_total_, huellas_anillo_total_, huellas_comparadas_);
  }

  /*
   * One LeerNivel call turned into a read from the snapshot. false = the texture goes through the usual
   * path: LeerNivel would go outside memory (its own check) or would read outside what the hash covers.
   * `rango_*` is the hash region (base or mips) and `en_copia` is where its copy starts in the snapshot.
   * The bound on what is read is the hash's (GetTiledAddressUpperBound2D): the corner of the last
   * 32x32-block tile plus the size of one tile; it was checked against what DesenmosaicarNivel and
   * LeerNivelMosaicoDeSiempre read.
   */
  static bool PlanearLecturaHuella(std::array<LecturaHuella, kLecturasPorTextura>& lecturas, uint32_t& n,
                                   uint64_t direccion, bool mosaico, uint32_t pitch_bloques, uint64_t fila_bytes,
                                   const FormatoTextura& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                                   uint32_t bx_host, size_t destino, uint64_t rango_inicio, uint64_t rango_bytes,
                                   uint64_t en_copia) {
    if (!bx || !by) {
      return true;  // LeerNivel reads nothing
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t holgado =
        mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u), pitch_bloques,
                                                                     log2),
                                             0))
                : fila_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (direccion + holgado > kMemoriaFisica) {
      return false;  // LeerNivel would return false: let the usual path report it
    }
    const uint64_t leido =
        mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((ox + bx - 1) & ~31u),
                                                                     int32_t((oy + by - 1) & ~31u), pitch_bloques,
                                                                     log2),
                                             0)) +
                      (log2 == 0 ? 0xA00u : log2 == 1 ? 0xC00u : (0x400u << log2))
                : fila_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (n >= lecturas.size() || direccion < rango_inicio || direccion + leido > rango_inicio + rango_bytes) {
      return false;
    }
    LecturaHuella& l = lecturas[n++];
    l.origen = en_copia + (direccion - rango_inicio);
    l.fila_bytes = fila_bytes;
    l.destino = destino;
    l.pitch_bloques = pitch_bloques;
    l.ox = ox;
    l.oy = oy;
    l.bx = bx;
    l.by = by;
    l.bx_host = bx_host;
    l.mosaico = mosaico;
    l.comprobar = false;
    return true;
  }

  /*
   * The rest of a new texture for the thread: space, snapshot, plan and job. true = applying phase: the
   * thread prepares it and the tail of PrepararTextura is done here (interval, upload and counters); the
   * caller returns. false = the usual path continues: it was not possible, or it is a comparison texture
   * (observing phase, and 1 in kComprobarHuellaUnaDeCada), whose job is already published and is compared
   * when collected.
   */
  bool PlanearHuella(Textura& textura, uint64_t clave, uint32_t n_lecturas, const uint8_t* crudo_base,
                     uint64_t extension, const uint8_t* crudo_mips, uint64_t extension_mips, const FormatoTextura& tf,
                     uint32_t orden, size_t bytes_datos, const std::array<size_t, 16>& desplazamientos,
                     uint32_t direccion, uint32_t ancho, uint32_t alto, uint32_t formato, VkDeviceSize& bytes_subida) {
    comparacion_textura_ = nullptr;
    if (huellas_fase_ == kHuellasSinDecidir) {
      DecidirHuellas();
    }
    if (huellas_fase_ == kHuellasApagado || textura.huella_trabajo != 0 ||
        (huellas_fase_ == kHuellasMirando &&
         huellas_comparaciones_planeadas_ >= kHuellasAComprobar + huellas_sin_comparar_)) {
      return false;  // while observing, with comparisons already running, the rest only take the usual path
    }
    if (!instantaneas_) {
      instantaneas_.reset(new (std::nothrow) uint8_t[size_t(instantaneas_bytes_)]);
      lecturas_huella_.reset(new (std::nothrow) LecturaHuella[kLecturasHuella]);
      if (!instantaneas_ || !lecturas_huella_) {
        instantaneas_.reset();
        lecturas_huella_.reset();
        ApagarHuellas(fmt::format("DIFERENCIA: no hay memoria para {} MB de instantaneas", instantaneas_bytes_ >> 20));
        return false;
      }
    }
    const uint64_t inst_base = (instantaneas_usado_ + 63) & ~uint64_t(63);
    const uint64_t inst_mips = inst_base + ((extension + 63) & ~uint64_t(63));  // the same space the plan assumes
    const uint64_t en_cola = huellas_publicados_ - huellas_recogidos_ + huellas_planeadas_pendientes_;
    if (en_cola >= kColaHuellas || lecturas_usadas_ + n_lecturas > kLecturasHuella ||
        inst_mips + extension_mips > instantaneas_bytes_) {
      ++huellas_sin_sitio_informe_;
      return false;
    }
    const bool comparar =
        huellas_fase_ == kHuellasMirando || (++huellas_nuevas_aplicando_ % kComprobarHuellaUnaDeCada) == 0;
    // The snapshot: the same bytes the usual path's hash covers, at the same point.
    const auto antes_copia = std::chrono::steady_clock::now();
    std::memcpy(instantaneas_.get() + inst_base, crudo_base, size_t(extension));
    if (extension_mips) {
      std::memcpy(instantaneas_.get() + inst_mips, crudo_mips, size_t(extension_mips));
    }
    const uint64_t ns_copia = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - antes_copia)
                                           .count());
    instantaneas_usado_ = inst_mips + extension_mips;
    ns_instantaneas_informe_ += ns_copia;
    bytes_instantaneas_informe_ += extension + extension_mips;
    nfsmw::esperas::g_ns_huella_instantanea.fetch_add(ns_copia, std::memory_order_relaxed);
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    TrabajoHuella t;
    t.textura = &textura;
    t.clave = clave;
    t.inst_base = inst_base;
    t.bytes_base = extension;
    t.inst_mips = inst_mips;
    t.bytes_mips = extension_mips;
    t.bytes_datos = bytes_datos;
    t.lectura_inicio = lecturas_usadas_;
    t.lecturas = n_lecturas;
    t.bytes_bloque = tf.bytes;
    t.log2_bloque = log2;
    t.unidad_orden = tf.unidad_orden;
    t.orden = orden;
    t.direccion = direccion;
    t.ancho = ancho;
    t.alto = alto;
    t.formato = formato;
    t.rapido = mosaico_rapido_ > 0;
    t.comparar = comparar;
    // The plan, with the decisions of the fast untiling and byte swap guards taken in the same order as the
    // usual path (niveles_vistos_ and ordenes_vistos_). Not for a comparison texture: the usual path also
    // processes it, and that is what keeps those counts.
    for (uint32_t i = 0; i < n_lecturas; ++i) {
      LecturaHuella l = lecturas_plan_[i];
      l.origen += inst_base;
      if (!comparar && l.mosaico && t.rapido && tf.bytes == (1u << log2)) {
        ++niveles_vistos_;
        l.comprobar = niveles_vistos_ <= kNivelesAComprobar || niveles_vistos_ % kComprobarUnaDeCada == 0;
        if (l.comprobar && ++niveles_comprobados_ == kNivelesAComprobar) {
          t.aviso_niveles = true;
        }
      }
      lecturas_huella_[lecturas_usadas_ + i] = l;
    }
    lecturas_usadas_ += n_lecturas;
    if (!comparar && t.rapido && orden != 0 && (tf.unidad_orden == 2 || tf.unidad_orden == 4)) {
      ++ordenes_vistos_;
      t.comprobar_orden = ordenes_vistos_ <= kNivelesAComprobar || ordenes_vistos_ % kComprobarUnaDeCada == 0;
      if (t.comprobar_orden && ++ordenes_comprobados_ == kNivelesAComprobar) {
        t.aviso_ordenes = true;
      }
    }
    ++huellas_planeadas_informe_;
    if (comparar) {
      // Published now, with no space in the upload buffer: the thread only returns its two hashes. What gets
      // uploaded is the usual path's result, which continues on return (AnotarComparacionHuella stores its
      // hashes).
      ++huellas_comparaciones_planeadas_;
      ++huellas_comparacion_informe_;
      const uint64_t secuencia = PublicarTrabajoHuella(t, nullptr);
      comparaciones_huella_[secuencia & (kColaHuellas - 1)] = ComparacionHuella{};
      comparacion_textura_ = &textura;
      comparacion_secuencia_ = secuencia;
      return false;
    }
    // Applying phase: the tail of PrepararTextura for a new texture, the same as there.
    huellas_planeadas_.push_back(t);
    ++huellas_planeadas_pendientes_;
    textura.huella_trabajo = uint32_t(huellas_planeadas_.size());
    textura.muestra_valida = false;
    textura.muestras_seguidas = 0;
    for (uint32_t n = 0; n < textura.niveles; ++n) {
      textura.desplazamiento_nivel[n] = uint32_t(desplazamientos[n]);
    }
    textura.intervalo = 1;
    textura.siguiente = fotograma_ + 1;
    textura.huella = 0;  // a new texture's data hash decides nothing: 0, as on the usual path
    textura.subir = true;
    texturas_a_subir_.push_back(&textura);
    nfsmw::esperas::g_texturas_subidas.fetch_add(1, std::memory_order_relaxed);
    nfsmw::esperas::g_bytes_subidos.fetch_add(bytes_datos, std::memory_order_relaxed);
    bytes_subida += (bytes_datos + 3) & ~size_t(3);
    return true;
  }

  // Queues a job for the thread (under the lock), waking it if it sleeps. Returns its sequence number. If
  // the thread cannot be created, the job stays queued anyway and the ring does it when collecting (and the
  // feature switches off).
  uint64_t PublicarTrabajoHuella(const TrabajoHuella& trabajo, uint8_t* destino) {
    if (!huellas_hilo_.joinable() && !huellas_sin_hilo_) {
#if REX_PLATFORM_SWITCH
      const int anillo = RexSwitchCurrentCore();  // the ring thread asks for itself
#else
      const int anillo = -1;
#endif
      huellas_nucleo_ = huellas_nucleo_pedido_ != -2 ? huellas_nucleo_pedido_ : anillo == 2 ? 1 : 2;
      try {
        huellas_hilo_ = std::thread([this] { BucleHuellas(); });
        REXLOG_INFO("[nativo] C3: hilo de huellas de texturas creado: nucleo preferido {} (el anillo corre en el {}), "
                    "prioridad {:#x}",
                    huellas_nucleo_, anillo, huellas_prioridad_);
      } catch (const std::system_error& error) {
        huellas_sin_hilo_ = true;
        ApagarHuellas(fmt::format("DIFERENCIA: no se pudo crear el hilo ({})", error.what()));
      }
    }
    bool avisar = false;
    uint64_t secuencia = 0;
    {
      std::lock_guard<std::mutex> cerrojo(huellas_mutex_);
      secuencia = huellas_publicados_;
      TrabajoHuella& casilla = cola_huellas_[secuencia & (kColaHuellas - 1)];
      casilla = trabajo;
      casilla.secuencia = secuencia;
      casilla.destino = destino;
      casilla.epoca = epoca_subida_;
      casilla.estado = kTrabajoPendiente;
      ++huellas_publicados_;
      avisar = huellas_durmiendo_;  // decided under the lock: if it sleeps, it is in its wait with the predicate
    }
    if (avisar) {
      huellas_cv_.notify_one();
    }
    return secuencia;
  }

  // The data bytes of a planned texture (SubirTextura reserves that space). If its index does not match
  // (should not happen), the texture's own, which come from the same per-level computation.
  size_t BytesHuellaPlaneada(const Textura& textura) const {
    const size_t indice = size_t(textura.huella_trabajo) - 1;
    return indice < huellas_planeadas_.size() && huellas_planeadas_[indice].textura == &textura
               ? huellas_planeadas_[indice].bytes_datos
               : size_t(textura.bytes);
  }

  // SubirTextura for a planned texture: its job, with the space just reserved for it, goes to the thread's
  // queue.
  void PublicarHuella(Textura& textura, uint8_t* destino, size_t bytes) {
    const size_t indice = size_t(textura.huella_trabajo) - 1;
    if (indice >= huellas_planeadas_.size() || huellas_planeadas_[indice].textura != &textura) {
      // Should never happen. Without a job its data would never arrive: zeros in its space, and it is uploaded
      // in full next time.
      std::memset(destino, 0, bytes);
      textura.huella_trabajo = 0;
      textura.huella_cruda = 0;
      textura.siguiente = 0;
      ApagarHuellas(fmt::format("DIFERENCIA: trabajo planeado incoherente ({} de {})", indice,
                                huellas_planeadas_.size()));
      return;
    }
    PublicarTrabajoHuella(huellas_planeadas_[indice], destino);
    huellas_planeadas_[indice].textura = nullptr;  // published: no longer counts as planned
    --huellas_planeadas_pendientes_;
    textura.huella_trabajo = kTrabajoHuellaPublicado;
  }

  // At the start of each draw: the previous draw's planned textures that never reached SubirTextura (the
  // draw was rejected before). As on the usual path, they stay un-uploaded and are prepared again at their
  // next check.
  void DescartarHuellasPlaneadas() {
    if (!huellas_planeadas_pendientes_) {
      return;
    }
    for (TrabajoHuella& t : huellas_planeadas_) {
      if (t.textura) {
        t.textura->huella_trabajo = 0;
        t.textura = nullptr;
        ++huellas_descartadas_;
      }
    }
    huellas_planeadas_pendientes_ = 0;
  }

  // Observing phase (and 1 in kComprobarHuellaUnaDeCada): the usual path's hashes of the texture whose
  // comparison job was just published. RecogerHuellas compares them.
  void AnotarComparacionHuella(const Textura& textura, const std::vector<uint8_t>& datos) {
    comparacion_textura_ = nullptr;
    if (comparacion_secuencia_ < huellas_recogidos_) {
      return;  // already collected (should not happen): left uncompared and counted
    }
    ComparacionHuella& c = comparaciones_huella_[comparacion_secuencia_ & (kColaHuellas - 1)];
    c.cruda = textura.huella_cruda;
    c.datos = XXH3_64bits(datos.data(), datos.size());
    c.anotada = true;
  }

  /*
   * One job: done by the thread (or by the ring, if it takes it when collecting). The same as
   * PrepararTextura's usual path and in the same order (raw hash, levels through LeerNivel, and byte swap
   * with its guard), but reading from the snapshot. It only reads this job's snapshot and plan, and only
   * writes `datos`, `comprobacion`, its space in the upload buffer and the results in `t`: nothing of the
   * ring's state. `rapido_apagado` belongs to whoever does the job: after a difference in its guard, the
   * rest takes the usual path, as on the ring (which also turns off mosaico_rapido_ when collecting it).
   */
  void EjecutarHuella(TrabajoHuella& t, std::vector<uint8_t>& datos, std::vector<uint8_t>& comprobacion,
                      bool& rapido_apagado) {
    const auto inicio = std::chrono::steady_clock::now();
    const uint8_t* const instantanea = instantaneas_.get();
    uint64_t huella = XXH3_64bits(instantanea + t.inst_base, size_t(t.bytes_base));
    if (t.bytes_mips) {
      huella = XXH3_64bits_withSeed(instantanea + t.inst_mips, size_t(t.bytes_mips), huella);
    }
    t.huella_cruda = huella;
    const auto tras_huella = std::chrono::steady_clock::now();
    datos.assign(t.bytes_datos, 0);
    const uint32_t b = t.bytes_bloque;
    for (uint32_t i = 0; i < t.lecturas; ++i) {
      const LecturaHuella& l = lecturas_huella_[t.lectura_inicio + i];
      const uint8_t* const origen = instantanea + l.origen;
      uint8_t* const destino = datos.data() + l.destino;
      if (!l.mosaico) {
        for (uint32_t y = 0; y < l.by; ++y) {
          std::memcpy(destino + size_t(y) * l.bx_host * b,
                      origen + uint64_t(l.oy + y) * l.fila_bytes + uint64_t(l.ox) * b, size_t(l.bx) * b);
        }
        continue;
      }
      if (t.rapido && !rapido_apagado && b == (1u << t.log2_bloque)) {
        switch (t.log2_bloque) {
          case 0: DesenmosaicarNivel<0>(origen, l.pitch_bloques, l.ox, l.oy, l.bx, l.by, l.bx_host, destino); break;
          case 1: DesenmosaicarNivel<1>(origen, l.pitch_bloques, l.ox, l.oy, l.bx, l.by, l.bx_host, destino); break;
          case 2: DesenmosaicarNivel<2>(origen, l.pitch_bloques, l.ox, l.oy, l.bx, l.by, l.bx_host, destino); break;
          case 3: DesenmosaicarNivel<3>(origen, l.pitch_bloques, l.ox, l.oy, l.bx, l.by, l.bx_host, destino); break;
          default: DesenmosaicarNivel<4>(origen, l.pitch_bloques, l.ox, l.oy, l.bx, l.by, l.bx_host, destino); break;
        }
        if (l.comprobar) {
          // The LeerNivel guard: repeated on the usual path (which is the result kept) and compared.
          comprobacion.assign(size_t(l.by) * l.bx_host * b, 0);
          for (uint32_t y = 0; y < l.by; ++y) {
            std::memcpy(comprobacion.data() + size_t(y) * l.bx_host * b, destino + size_t(y) * l.bx_host * b,
                        size_t(l.bx) * b);
          }
          LeerNivelMosaicoDeSiempre(origen, l.pitch_bloques, t.log2_bloque, b, l.ox, l.oy, l.bx, l.by, l.bx_host,
                                    destino);
          bool iguales = true;
          for (uint32_t y = 0; y < l.by && iguales; ++y) {
            iguales = std::memcmp(comprobacion.data() + size_t(y) * l.bx_host * b,
                                  destino + size_t(y) * l.bx_host * b, size_t(l.bx) * b) == 0;
          }
          if (!iguales) {
            if (!t.niveles_distintos++) {
              t.distinto = {l.bx, l.by, l.pitch_bloques, l.ox, l.oy};
            }
            rapido_apagado = true;
          }
        }
        continue;
      }
      LeerNivelMosaicoDeSiempre(origen, l.pitch_bloques, t.log2_bloque, b, l.ox, l.oy, l.bx, l.by, l.bx_host, destino);
    }
    const auto orden = static_cast<xenos::Endian>(t.orden);
    if (t.rapido && !rapido_apagado && orden != xenos::Endian::kNone && (t.unidad_orden == 2 || t.unidad_orden == 4)) {
      if (t.comprobar_orden) {
        comprobacion = datos;
      }
      CambiarOrdenBytes(datos.data(), datos.size(), t.unidad_orden, t.orden);
      if (t.comprobar_orden) {
        OrdenDeSiempre(comprobacion, t.unidad_orden, orden);
        if (comprobacion != datos) {
          t.orden_distinto = true;
          datos.swap(comprobacion);  // the usual one is kept, as on the ring
          rapido_apagado = true;
        }
      }
    } else {
      OrdenDeSiempre(datos, t.unidad_orden, orden);
    }
    if (t.comparar) {
      t.huella_datos = XXH3_64bits(datos.data(), datos.size());
    }
    if (t.destino) {
      std::memcpy(t.destino, datos.data(), datos.size());
    }
    const auto fin = std::chrono::steady_clock::now();
    t.ns_huella = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(tras_huella - inicio).count());
    t.ns_resto = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(fin - tras_huella).count());
  }

  // The thread: the queued jobs, in order, skipping those the ring has taken. It sleeps on a decision taken
  // under the lock; it never spins. It does not write to the log (the ring records the figures when
  // collecting).
  void BucleHuellas() {
    rex::thread::set_current_thread_name("NFSMW huellas de texturas");
#if REX_PLATFORM_SWITCH
    const bool prioridad_ok = RexSwitchSetCurrentThreadPriorityOk(int(huellas_prioridad_));
    const bool nucleo_ok = RexSwitchSetCurrentThreadCore(int(huellas_nucleo_));
    const int nucleo = RexSwitchCurrentCore();
#else
    const bool prioridad_ok = true;
    const bool nucleo_ok = true;
    const int nucleo = -1;
#endif
    std::vector<uint8_t> datos;         // owned by this thread only
    std::vector<uint8_t> comprobacion;  // owned by this thread only
    bool rapido_apagado = false;        // owned by this thread only: its fast-path guard saw a difference
    std::unique_lock<std::mutex> cerrojo(huellas_mutex_);
    huellas_prioridad_ok_ = prioridad_ok;
    huellas_nucleo_ok_ = nucleo_ok;
    huellas_nucleo_real_ = nucleo;
    for (;;) {
      while (huellas_tomados_ != huellas_publicados_ &&
             cola_huellas_[huellas_tomados_ & (kColaHuellas - 1)].estado != kTrabajoPendiente) {
        ++huellas_tomados_;  // taken by the ring
      }
      if (huellas_tomados_ == huellas_publicados_) {
        if (huellas_parar_) {
          return;  // stop, nothing pending
        }
        if (huellas_esperando_) {
          huellas_hechos_cv_.notify_one();
        }
        huellas_durmiendo_ = true;
        huellas_cv_.wait(cerrojo, [this] { return huellas_parar_ || huellas_tomados_ != huellas_publicados_; });
        huellas_durmiendo_ = false;
        continue;
      }
      // The slot belongs to this thread while it is EnHilo: the ring neither touches nor reuses it until it is
      // collected done.
      TrabajoHuella& t = cola_huellas_[huellas_tomados_ & (kColaHuellas - 1)];
      t.estado = kTrabajoEnHilo;
      ++huellas_tomados_;
      cerrojo.unlock();
      EjecutarHuella(t, datos, comprobacion, rapido_apagado);
      cerrojo.lock();
      t.estado = kTrabajoHecho;
      ++huellas_hechos_;
      if (huellas_esperando_) {
        huellas_hechos_cv_.notify_one();
      }
    }
  }

  /*
   * Waits for and collects all published jobs. Ring thread only: in AntesDeEnviar (before vkQueueSubmit)
   * and, if needed, before rechecking a texture. It takes the ones the thread has not started (from the
   * end) and does them itself; it only waits for the one the thread is in the middle of, with a timeout and
   * a log line every 2 s. Then it sets each texture's raw hash and compares the comparison jobs. If nothing
   * planned is left, the snapshots start over.
   */
  void RecogerHuellas() {
    if (recogiendo_huellas_) {
      return;
    }
    if (huellas_publicados_ == huellas_recogidos_) {
      if (!huellas_planeadas_pendientes_ && (instantaneas_usado_ || lecturas_usadas_)) {
        instantaneas_usado_ = 0;  // nothing published uncollected or planned: everything starts over
        lecturas_usadas_ = 0;
        huellas_planeadas_.clear();
      }
      return;
    }
    recogiendo_huellas_ = true;
    uint64_t ns_espera = 0;
    uint64_t ns_anillo = 0;
    uint64_t hechas_anillo = 0;
    {
      std::unique_lock<std::mutex> cerrojo(huellas_mutex_);
      int segundos = 0;
      while (huellas_hechos_ != huellas_publicados_) {
        // 1. One the thread has not started, from the end of the queue (the thread goes from the start).
        TrabajoHuella* mio = nullptr;
        for (uint64_t i = huellas_publicados_; i > huellas_tomados_; --i) {
          TrabajoHuella& t = cola_huellas_[(i - 1) & (kColaHuellas - 1)];
          if (t.estado == kTrabajoPendiente) {
            t.estado = kTrabajoEnAnillo;
            mio = &t;
            break;
          }
        }
        if (mio) {
          cerrojo.unlock();
          const auto antes = std::chrono::steady_clock::now();
          EjecutarHuella(*mio, huellas_datos_anillo_, huellas_comprobacion_anillo_, huellas_rapido_apagado_anillo_);
          ns_anillo += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - antes)
                                    .count());
          ++hechas_anillo;
          cerrojo.lock();
          mio->por_anillo = true;
          mio->estado = kTrabajoHecho;
          ++huellas_hechos_;
          continue;
        }
        // 2. What is left, the thread has half done: wait for it, deciding under the lock.
        const auto antes = std::chrono::steady_clock::now();
        huellas_esperando_ = true;
        if (huellas_durmiendo_) {
          huellas_cv_.notify_one();  // should not be needed (it has one in progress); in case a wake-up was lost
        }
        const bool listos = huellas_hechos_cv_.wait_for(cerrojo, std::chrono::seconds(2),
                                                         [this] { return huellas_hechos_ == huellas_publicados_; });
        huellas_esperando_ = false;
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - antes)
                                         .count());
        ns_espera += ns;
        ns_espera_huellas_peor_ = std::max(ns_espera_huellas_peor_, ns);
        ++esperas_huellas_informe_;
        if (!listos) {
          segundos += 2;
          huellas_atascado_ = true;
          REXLOG_ERROR("[nativo] C3: el hilo del anillo lleva {} s esperando al hilo de huellas de texturas: "
                       "publicados {}, tomados {}, hechos {}; el hilo esta {}",
                       segundos, huellas_publicados_, huellas_tomados_, huellas_hechos_,
                       huellas_durmiendo_ ? "DORMIDO (aviso perdido)" : "despierto (un trabajo que no acaba)");
        }
      }
      // The results were written by whoever did each job; the lock makes them visible here.
      huellas_recogidas_.clear();
      for (uint64_t s = huellas_recogidos_; s < huellas_publicados_; ++s) {
        huellas_recogidas_.push_back(cola_huellas_[s & (kColaHuellas - 1)]);
      }
      huellas_recogidos_ = huellas_publicados_;  // from here on its slots can be reused
    }
    comparacion_textura_ = nullptr;
    bool diferencia = false;
    std::string detalle;
    uint64_t hilo = 0;
    uint64_t ns_hilo = 0;
    for (TrabajoHuella& t : huellas_recogidas_) {
      if (t.por_anillo) {
        ++huellas_anillo_total_;
      } else {
        ++huellas_hilo_total_;
        if (!t.comparar) {  // what the thread actually took off the ring
          ++hilo;
          ns_hilo += t.ns_huella + t.ns_resto;
          ns_huella_cruda_hilo_informe_ += t.ns_huella;
        }
      }
      // The fast-path guards, with the same lines as the usual path (LeerNivel and the byte swap).
      if (t.niveles_distintos) {
        REXLOG_ERROR("[nativo] C3: el desenmosaicado rapido NO coincide ({}x{} bloques de {} bytes, pitch {}, desde "
                     "{},{}). Se APAGA y se usa el de siempre.",
                     t.distinto[0], t.distinto[1], t.bytes_bloque, t.distinto[2], t.distinto[3], t.distinto[4]);
        mosaico_rapido_ = 0;
      } else if (t.aviso_niveles) {
        REXLOG_INFO("[nativo] C3: desenmosaicado rapido: {} niveles comprobados contra el de siempre, todos iguales. "
                    "Sigue el rapido.",
                    kNivelesAComprobar);
      }
      if (t.orden_distinto) {
        REXLOG_ERROR("[nativo] C3: el cambio de orden rapido NO coincide (unidad {}, orden {}, {} bytes). Se APAGAN el "
                     "orden y el desenmosaicado rapidos.",
                     t.unidad_orden, t.orden, t.bytes_datos);
        mosaico_rapido_ = 0;
      } else if (t.aviso_ordenes) {
        REXLOG_INFO("[nativo] C3: cambio de orden rapido: {} texturas comprobadas contra el de siempre, todas iguales.",
                    kNivelesAComprobar);
      }
      if (t.comparar) {
        const ComparacionHuella& c = comparaciones_huella_[t.secuencia & (kColaHuellas - 1)];
        if (!c.anotada) {
          ++huellas_sin_comparar_;
          continue;
        }
        ++huellas_comparadas_;
        if (c.cruda != t.huella_cruda || c.datos != t.huella_datos) {
          diferencia = true;
          detalle = fmt::format("DIFERENCIA en la textura {:08X} {}x{} formato {}: huella cruda {:016X} en el anillo y "
                                "{:016X} en el hilo; datos {:016X} y {:016X}",
                                t.direccion, t.ancho, t.alto, t.formato, c.cruda, t.huella_cruda, c.datos,
                                t.huella_datos);
        }
        continue;
      }
      // A texture done by the thread: its raw hash is that of the same bytes that were uploaded. It must still
      // be in the cache as it was left, and its data must have gone to the current upload buffer (the one about
      // to be submitted).
      const auto it = texturas_.find(t.clave);
      if (it == texturas_.end() || &it->second != t.textura || t.textura->huella_trabajo != kTrabajoHuellaPublicado) {
        diferencia = true;
        detalle = fmt::format("DIFERENCIA: la textura {:08X} de un trabajo recogido ya no esta como se dejo",
                              t.direccion);
        t.textura = nullptr;
        continue;
      }
      if (t.epoca != epoca_subida_) {
        diferencia = true;  // se vuelve a subir entera (huellas_sospechosas_)
        detalle = fmt::format("DIFERENCIA: los datos de la textura {:08X} fueron a otro bufer de subida (epoca {} "
                              "frente a {})",
                              t.direccion, t.epoca, epoca_subida_);
      }
      t.textura->huella_cruda = t.huella_cruda;
      t.textura->huella_trabajo = 0;
      AnotarContenidoTextura(*t.textura, t.clave);  // measurement only: nfsmw_nativo_diag_reutilizar
    }
    if (diferencia) {
      huellas_sospechosas_ = true;
      ApagarHuellas(detalle);
    }
    if (huellas_atascado_) {
      huellas_atascado_ = false;
      ApagarHuellas("DIFERENCIA: una espera de mas de 2 s al hilo de huellas (ver las lineas de arriba)");
    }
    if (huellas_sospechosas_) {
      // The thread's textures planned before the DIFERENCIA may carry the same fault: they are uploaded again
      // in full, on the usual path (already switched off), at their next check.
      for (const TrabajoHuella& t : huellas_recogidas_) {
        if (!t.comparar && t.textura) {
          t.textura->imagen.preparada = false;
          t.textura->siguiente = 0;
          t.textura->huella_trabajo = 0;
          ++huellas_resubidas_;
        }
      }
    } else if (huellas_fase_ == kHuellasMirando && huellas_comparadas_ >= kHuellasAComprobar) {
      huellas_fase_ = kHuellasAplicando;
      bool prioridad_ok = true;
      bool nucleo_ok = true;
      int nucleo = -1;
      {
        std::lock_guard<std::mutex> cerrojo(huellas_mutex_);
        prioridad_ok = huellas_prioridad_ok_;
        nucleo_ok = huellas_nucleo_ok_;
        nucleo = huellas_nucleo_real_;
      }
      REXLOG_INFO("[nativo] C3: hilo de huellas de texturas: {} texturas nuevas hechas por los dos caminos, todas "
                  "iguales (huella cruda y datos; {} sin poder comparar). Pasa a fase APLICANDO: las prepara el hilo y "
                  "se sigue comparando 1 de cada {}. El hilo corre en el nucleo {}{}{}",
                  huellas_comparadas_, huellas_sin_comparar_, kComprobarHuellaUnaDeCada, nucleo,
                  nucleo_ok ? "" : " (el kernel NO acepto el nucleo preferido)",
                  prioridad_ok ? "" : " (el kernel NO acepto la prioridad)");
    }
    huellas_hilo_informe_ += hilo;
    huellas_anillo_informe_ += hechas_anillo;
    ns_huellas_hilo_informe_ += ns_hilo;
    ns_huellas_anillo_informe_ += ns_anillo;
    ns_espera_huellas_informe_ += ns_espera;
    // A few times per frame (on each submission with new textures): the fetch_add calls do not show.
    nfsmw::esperas::g_texturas_huella_hilo.fetch_add(hilo, std::memory_order_relaxed);
    nfsmw::esperas::g_ns_huella_hilo.fetch_add(ns_hilo, std::memory_order_relaxed);
    nfsmw::esperas::g_texturas_huella_anillo.fetch_add(hechas_anillo, std::memory_order_relaxed);
    nfsmw::esperas::g_ns_huella_anillo.fetch_add(ns_anillo, std::memory_order_relaxed);
    nfsmw::esperas::g_ns_esperando_huellas.fetch_add(ns_espera, std::memory_order_relaxed);
    if (!huellas_planeadas_pendientes_) {
      instantaneas_usado_ = 0;
      lecturas_usadas_ = 0;
      huellas_planeadas_.clear();
    }
    recogiendo_huellas_ = false;
  }

  // UsarRanura: everything published is collected in AntesDeEnviar. If anything reaches here (it should
  // not), its data was going to the buffer just closed without waiting for it: it is collected now (before
  // this one is reset) and those textures are uploaded again.
  void ComprobarHuellasAlCambiarDeBufer() {
    if (huellas_publicados_ == huellas_recogidos_) {
      return;
    }
    const uint64_t sin_recoger = huellas_publicados_ - huellas_recogidos_;
    huellas_sospechosas_ = true;  // whatever is collected now is uploaded again in full
    RecogerHuellas();
    ApagarHuellas(fmt::format("DIFERENCIA: {} trabajos del hilo de huellas sin recoger al cambiar de bufer de subida",
                              sin_recoger));
  }

  void PararHuellas() {
    if (!huellas_hilo_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> cerrojo(huellas_mutex_);
      huellas_parar_ = true;
    }
    huellas_cv_.notify_one();
    huellas_hilo_.join();  // first finishes the published jobs
    REXLOG_INFO("[nativo] C3: hilo de huellas de texturas parado ({} texturas hechas por el hilo, {} por el anillo, {} "
                "comparadas, {} descartadas, {} vueltas a subir)",
                huellas_hilo_total_, huellas_anillo_total_, huellas_comparadas_, huellas_descartadas_,
                huellas_resubidas_);
  }

  // Every 10 s, if there were new textures: what the thread and the ring did. This is also where it is
  // decided whether it pays off: if for 3 reports in a row (with 16 or more jobs) what the thread took off
  // the ring is less than what the ring spent copying snapshots and waiting for it, the ring would do better
  // alone, and it switches off.
  void InformeHuellas() {
    if (huellas_fase_ == kHuellasSinDecidir) {
      return;
    }
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - informe_huellas_ < std::chrono::seconds(10)) {
      return;
    }
    informe_huellas_ = ahora;
    bool prioridad_ok = true;
    int nucleo = -1;
    {
      std::lock_guard<std::mutex> cerrojo(huellas_mutex_);
      prioridad_ok = huellas_prioridad_ok_;
      nucleo = huellas_nucleo_real_;
    }
    if (huellas_planeadas_informe_ || huellas_hilo_informe_ || huellas_anillo_informe_ || huellas_sin_sitio_informe_) {
      static constexpr const char* kFases[] = {"apagada", "MIRANDO", "APLICANDO"};
      NFSMW_INFORME_ANILLO(
          "[nativo] C3 huellas de texturas nuevas (build 185), ultimos 10 s: {} planeadas ({} de comparacion); el hilo "
          "preparo {} en {:.1f} ms (huella {:.1f} ms) y el anillo {} al enviar en {:.1f} ms; el anillo copio {:.1f} MB "
          "de instantaneas en {:.1f} ms y espero al hilo {:.1f} ms en {} veces (peor {:.2f} ms); {} por la ruta de "
          "siempre sin sitio; {} comparadas desde el principio ({} sin poder); hilo en el nucleo {}, prioridad "
          "{:#x}{}; fase {}",
          huellas_planeadas_informe_, huellas_comparacion_informe_, huellas_hilo_informe_,
          double(ns_huellas_hilo_informe_) / 1e6, double(ns_huella_cruda_hilo_informe_) / 1e6,
          huellas_anillo_informe_, double(ns_huellas_anillo_informe_) / 1e6,
          double(bytes_instantaneas_informe_) / 1048576.0, double(ns_instantaneas_informe_) / 1e6,
          double(ns_espera_huellas_informe_) / 1e6, esperas_huellas_informe_, double(ns_espera_huellas_peor_) / 1e6,
          huellas_sin_sitio_informe_, huellas_comparadas_, huellas_sin_comparar_, nucleo, huellas_prioridad_,
          prioridad_ok ? "" : " (el kernel NO acepto la prioridad)", kFases[std::clamp<int32_t>(huellas_fase_, 0, 2)]);
    }
    if (huellas_fase_ == kHuellasAplicando && huellas_hilo_informe_ + huellas_anillo_informe_ >= 16) {
      const bool compensa = ns_huellas_hilo_informe_ > ns_instantaneas_informe_ + ns_espera_huellas_informe_;
      informes_sin_compensar_huellas_ = compensa ? 0 : informes_sin_compensar_huellas_ + 1;
      if (informes_sin_compensar_huellas_ >= 3) {
        ApagarHuellas(fmt::format("no compensa: en 3 informes seguidos el hilo le quito al anillo menos de lo que el "
                                  "anillo gasto copiando y esperandolo (el ultimo: hilo {:.1f} ms, copias {:.1f} ms, "
                                  "espera {:.1f} ms)",
                                  double(ns_huellas_hilo_informe_) / 1e6, double(ns_instantaneas_informe_) / 1e6,
                                  double(ns_espera_huellas_informe_) / 1e6));
      }
    }
    huellas_planeadas_informe_ = 0;
    huellas_comparacion_informe_ = 0;
    huellas_hilo_informe_ = 0;
    huellas_anillo_informe_ = 0;
    ns_huellas_hilo_informe_ = 0;
    ns_huella_cruda_hilo_informe_ = 0;
    ns_huellas_anillo_informe_ = 0;
    ns_instantaneas_informe_ = 0;
    bytes_instantaneas_informe_ = 0;
    ns_espera_huellas_informe_ = 0;
    ns_espera_huellas_peor_ = 0;
    esperas_huellas_informe_ = 0;
    huellas_sin_sitio_informe_ = 0;
  }

  // --- Vertex copies on a separate thread (nfsmw_nativo_subidas_hilo) ------------------------------------------
  // One producer (the ring thread, in Dibujar) and one consumer (copias_hilo_). The producer writes the job
  // into copias_escritas_ and publishes it; the thread copies up to there and publishes copias_hechas_.
  // With the queue full the copy is done on the ring, as before. The destinations are distinct ranges of
  // the upload buffer, so order does not matter. The copies must be waited for before submitting the work
  // (AntesDeEnviar) and before returning the read pointer to the game (EsperarSubidas from
  // nfsmw_nativo_sistema.cpp): after that the game may reuse its vertex buffers.
  //
  // Sleeping and waking are decided under the lock. Two earlier versions hung: the copy thread asleep with
  // 1,123 copies queued and the ring waiting for them. Each side used to decide without the lock whether to
  // wake the other, Dekker-style: the ring published copias_escritas_ and read copias_hechas_, and the
  // thread the other way round. With store(..., memory_order_release) MSVC's STL emits a plain mov, and on
  // x86 (TSO) a store may not yet be visible from another core: both sides missed the wake-up. ARM gives no
  // guarantee either. Now copias_durmiendo_ and copias_esperando_ are only touched under the lock, and
  // nothing is decided without it: the wake-up arrives at the next point that takes the lock (every
  // kCopiasPorAviso copies, when the ring waits, and when the thread runs out of work). There is no atomic
  // hint flag any more; the thread always signals from its no-work path, under the lock, which it goes
  // through as soon as it finishes.
  // Only the queuing thread may wait (single producer). Otherwise it wakes the thread once and waits
  // without helping.
  //
  /*
   * The ring helps instead of waiting (nfsmw_nativo_subidas_ayuda).
   *
   * Why. In a race the ring waited 14.7 ms for the copy thread during one stutter (3,965 draws, 5 waits)
   * and 17.9 ms in the next (3,890 draws, 3 waits; the game spent 21.1 ms without room in the ring). Copying
   * what those frames needed is ~9-11 MB, ~6-7 ms at the rate the profile gives the thread (1.5 GB/s): the
   * rest was the thread ready and without a core. It ran at 0x3B, below the two game threads (0x3A), and
   * Horizon only moves a ready thread to another core when that core runs out of work: with the CPU at
   * 278 % out of 300 (the profile block for that stutter), almost never. And the ring (0x2D) waited for it
   * on a condition variable, which does not lend priority: a textbook priority inversion.
   *
   * How. On every wait with pending copies (by the queuing thread, in the applying phase):
   *   1. the ring takes and copies, in batches of kCopiasPorTrozo, whatever the thread has not taken yet.
   *      If the thread is running on another core they share what is left; if it has no core, the ring
   *      does it all, without waiting for it.
   *   2. whatever is missing can only be the batch the thread is working on. The ring waits by taking
   *      copias_trozo_mutex_, which the thread holds while copying: a libnx mutex waits in the kernel
   *      (svcArbitrateLock) and Horizon lends the owner the waiter's priority, so the thread rises to 0x2D
   *      until it releases it. It is one batch.
   * The thread's own priority is not changed: it takes no core from anyone while the ring is not waiting
   * for it.
   *
   * Counters. copias_escritas_ (ring only), copias_tomadas_ (the thread or the ring, with CAS, in order)
   * and copias_hechas_ (how many are done, whoever did them): hechas <= tomadas <= escritas. Outside
   * EsperarSubidas only the thread takes, so it finishes in order and "hechas = h" still means the first h
   * are done: EncolarCopia's space accounting still holds. Inside, the ring does not queue, and it leaves
   * with everything done.
   *
   * Same data. Each copy is done exactly once by CopiarVertices with the same slot (source, destination,
   * words and byte order), and all of them before returning from EsperarSubidas, as before: before
   * submitting and before returning the read pointer. Only who does it changes, and sometimes it is done
   * earlier.
   *
   * Self-checking guard. Each copy leaves its index + 1 in copias_marcas_. On each wait the ring checks the
   * marks of what was queued since the previous one and that the counters add up; a copy without a mark is
   * done right there, before submitting and before returning the read pointer, and it is a DIFERENCIA
   * (REXLOG_ERROR). Phases: observing (the first kEsperasCopiasMirando waits with pending copies: the
   * thread already takes batches, the ring waits as before and checks), applying (help) and, after a
   * DIFERENCIA with help on, no help (the plain wait). A DIFERENCIA without help, or with the counters out
   * of step, turns the thread off for the session and the copies go back to the ring (the original
   * behaviour). The plain wait no longer waits forever: after 2 s it takes the batch mutex and finishes the
   * job. The batch mutex has no timeout because inside it the thread only takes and copies.
   */
  enum FaseCopias : int32_t { kCopiasSinAyuda = 0, kCopiasMirando = 1, kCopiasAplicando = 2 };

  void AvisarOtroHiloCopias(const char* donde) {
    if (!copias_otro_hilo_avisado_) {
      copias_otro_hilo_avisado_ = true;
      REXLOG_ERROR("[nativo] C6: {} desde un hilo que no es el que encola las copias de vertices", donde);
    }
  }

  bool EncolarCopia(const TrabajoCopia& trabajo) {
    if (copias_sin_hilo_) {
      // Thread switched off by a DIFERENCIA (or it could not be created): the copy goes on the ring.
      ++copias_inf_.en_linea;
      copias_inf_.bytes_en_linea += uint64_t(trabajo.palabras) * 4;
      return false;
    }
    if (!copias_hilo_.joinable()) {
      copias_productor_ = std::this_thread::get_id();
      copias_fase_ = copias_ayuda_pedida_ ? kCopiasMirando : kCopiasSinAyuda;
#if REX_PLATFORM_SWITCH
      const int anillo_nucleo = RexSwitchCurrentCore();  // asked by the ring, which creates the thread
#else
      const int anillo_nucleo = -1;
#endif
      try {
        copias_hilo_ = std::thread([this, anillo_nucleo] {
          rex::thread::set_current_thread_name("NFSMW copias de vertices");
#if REX_PLATFORM_SWITCH
          // nfsmw_nativo_subidas_hilo_prioridad (see the cvar).
          const int32_t prioridad = REXCVAR_GET(nfsmw_nativo_subidas_hilo_prioridad);
          const bool prioridad_ok = RexSwitchSetCurrentThreadPriorityOk(int(prioridad));
          // nfsmw_nativo_subidas_hilo_nucleo (see the cvar).
          const int32_t pedido = REXCVAR_GET(nfsmw_nativo_subidas_hilo_nucleo);
          const int nucleo = pedido == -2 ? (anillo_nucleo == 2 ? 1 : 2) : int(pedido);
          const bool nucleo_ok = nucleo < 0 || RexSwitchSetCurrentThreadCore(nucleo);
          REXLOG_INFO("[nativo] C6: hilo de copias de vertices a prioridad {:#x}{}; nucleo preferido {}{} (el anillo corre "
                      "en el {}), ahora en el {}",
                      prioridad, prioridad_ok ? "" : " (NO aceptada: se queda con la de su creacion)", nucleo,
                      nucleo_ok ? "" : " (NO aceptado)", anillo_nucleo, RexSwitchCurrentCore());
#endif
          BucleCopias();
        });
      } catch (const std::system_error& error) {
        copias_sin_hilo_ = true;
        REXLOG_ERROR("[nativo] C6: no se pudo crear el hilo de copias de vertices ({}): las copias van en el anillo",
                     error.what());
        return false;
      }
      REXLOG_INFO("[nativo] C6: copias de vertices en un hilo aparte (nfsmw_nativo_subidas_hilo); el anillo las copia "
                  "el mismo al esperarlas (nfsmw_nativo_subidas_ayuda) = {}",
                  copias_ayuda_pedida_
                      ? fmt::format("SI, en trozos de {}; fase MIRANDO durante las {} primeras esperas con copias "
                                    "pendientes",
                                    kCopiasPorTrozo, kEsperasCopiasMirando)
                      : std::string("no, espera al hilo como antes"));
    } else if (std::this_thread::get_id() != copias_productor_) {
      AvisarOtroHiloCopias("EncolarCopia");
    }
    const size_t escritas = copias_escritas_.load(std::memory_order_relaxed);
    if (escritas - copias_hechas_.load(std::memory_order_acquire) >= copias_.size()) {
      ++copias_en_linea_;
      ++copias_inf_.en_linea;
      copias_inf_.bytes_en_linea += uint64_t(trabajo.palabras) * 4;
      return false;
    }
    copias_[escritas & (copias_.size() - 1)] = trabajo;
    copias_escritas_.store(escritas + 1);
    ++copias_inf_.encoladas;
    copias_inf_.bytes_encolados += uint64_t(trabajo.palabras) * 4;
    // Wake-up every kCopiasPorAviso copies: waking the thread as soon as it slept woke it on almost every
    // copy, and every wake-up goes through the kernel (1.4 % of the ring and almost all of the copy thread's
    // CPU). Whatever is left without a wake-up is collected by EsperarSubidas.
    if (((escritas + 1) & (kCopiasPorAviso - 1)) == 0) {
      DespertarCopias();
    }
    return true;
  }

  void DespertarCopias() {
    bool avisar;
    {
      std::lock_guard<std::mutex> cerrojo(copias_mutex_);
      avisar = copias_durmiendo_;
    }
    if (avisar) {
      copias_cv_.notify_one();
    }
  }

  // Fence measurement only (sistema.cpp, AnotarVallaCopias).
  size_t CopiasPendientes() const override {
    if (!copias_hilo_.joinable()) {
      return 0;
    }
    const size_t escritas = copias_escritas_.load(std::memory_order_relaxed);
    const size_t hechas = copias_hechas_.load(std::memory_order_relaxed);
    return escritas > hechas ? escritas - hechas : 0;
  }

  void EsperarSubidas() override {
    if (!copias_hilo_.joinable()) {
      return;
    }
    const bool productor = std::this_thread::get_id() == copias_productor_;
    if (!productor) {
      AvisarOtroHiloCopias("EsperarSubidas");
    }
    const size_t objetivo = copias_escritas_.load(std::memory_order_relaxed);
    if (copias_hechas_.load(std::memory_order_acquire) == objetivo) {
      if (productor) {
        ComprobarCopias(objetivo);  // what the thread did since the last wait
      }
      return;
    }
    const auto antes = std::chrono::steady_clock::now();
    // What the thread had not taken yet on arrival (while observing, what the ring would have copied).
    copias_inf_.sin_tomar += objetivo - std::min(objetivo, copias_tomadas_.load(std::memory_order_acquire));
    uint64_t ns_ayudando = 0;
    uint64_t ayudadas = 0;
    if (productor && copias_fase_ == kCopiasAplicando) {
      // 1. What the thread has not taken yet, the ring copies in batches: if the thread is running, they
      //    share it.
      uint64_t bytes = 0;
      while (TomarYCopiar(true, ayudadas, bytes)) {
      }
      const auto tras_ayudar = std::chrono::steady_clock::now();
      ns_ayudando = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(tras_ayudar - antes).count());
      copias_ayudadas_ += ayudadas;
      copias_bytes_ayudados_ += bytes;
      copias_ns_ayudando_ += ns_ayudando;
      copias_inf_.ayudadas += ayudadas;
      copias_inf_.bytes_ayudados += bytes;
      copias_inf_.ns_ayudando += ns_ayudando;
      if (ayudadas) {
        ++copias_inf_.esperas_con_ayuda;
      }
      // 2. What is missing is the batch the thread is working on: wait on its mutex, which lends the thread
      //    the ring's priority until it releases it.
      if (copias_hechas_.load(std::memory_order_acquire) != objetivo) {
        {
          std::lock_guard<std::mutex> trozo(copias_trozo_mutex_);
          if (copias_hechas_.load(std::memory_order_acquire) != objetivo) {
            RepararCopias(objetivo, "con el cerrojo del trozo el hilo no tiene nada entre manos y aun faltan copias");
          }
        }
        const uint64_t ns_trozo = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - tras_ayudar)
                                               .count());
        ++copias_inf_.esperas_trozo;
        copias_inf_.ns_trozo += ns_trozo;
        copias_inf_.ns_trozo_peor = std::max(copias_inf_.ns_trozo_peor, ns_trozo);
      }
    } else {
      EsperarCopiasComoSiempre(objetivo);
    }
    if (productor) {
      ComprobarCopias(objetivo);
      if (copias_fase_ == kCopiasMirando && ++copias_esperas_mirando_ >= kEsperasCopiasMirando) {
        copias_fase_ = kCopiasAplicando;
        REXLOG_INFO("[nativo] C6: copias de vertices: {} esperas con copias pendientes en fase MIRANDO y {} copias "
                    "comprobadas, todas con su marca y los contadores cuadrados. Pasa a fase APLICANDO: el anillo "
                    "copia lo que el hilo no ha tomado y espera el trozo en curso con su cerrojo",
                    copias_esperas_mirando_, copias_comprobadas_);
      }
    }
    ++esperas_copias_;
    const uint64_t ns_total = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - antes)
                                           .count());
    // The waiting time now counts only the wait; what the ring copies is counted separately (as helping).
    const uint64_t ns_espera = ns_total > ns_ayudando ? ns_total - ns_ayudando : 0;
    ns_esperando_copias_ += ns_espera;
    ++copias_inf_.esperas;
    copias_inf_.ns_espera += ns_espera;
    copias_inf_.ns_espera_peor = std::max(copias_inf_.ns_espera_peor, ns_espera);
    // And to the "[tiron] anillo" line (nfsmw_esperas_tiron.h). This is only reached when the ring really
    // waits, a few times per frame: the fetch_add calls do not show.
    nfsmw::esperas::g_ns_esperando_copias.fetch_add(ns_espera, std::memory_order_relaxed);
    nfsmw::esperas::g_esperas_copias.fetch_add(1, std::memory_order_relaxed);
    nfsmw::esperas::g_ns_ayudando_copias.fetch_add(ns_ayudando, std::memory_order_relaxed);
    nfsmw::esperas::g_copias_ayudadas.fetch_add(ayudadas, std::memory_order_relaxed);
  }

  // The plain wait (observing and no-help phases, or when another thread waits): the ring sleeps until the
  // thread finishes. After 2 s it no longer keeps waiting forever (the earlier hang): it takes the batch
  // mutex, which lends its priority to the thread if it is copying, and if copies are still missing with
  // the thread out of its loop, the ring does them and switches the thread off.
  void EsperarCopiasComoSiempre(size_t objetivo) {
    bool a_tiempo = false;
    {
      std::unique_lock<std::mutex> cerrojo(copias_mutex_);
      if (copias_durmiendo_) {
        copias_cv_.notify_one();  // a wake-up EncolarCopia skipped (it only wakes every kCopiasPorAviso copies)
      }
      copias_esperando_ = true;
      a_tiempo = copias_hechas_cv_.wait_for(cerrojo, std::chrono::seconds(2), [this, objetivo] {
        return copias_hechas_.load(std::memory_order_acquire) == objetivo;
      });
      copias_esperando_ = false;
    }
    ++copias_inf_.esperas_enteras;
    if (a_tiempo) {
      return;
    }
    // Should never happen: diagnostic for a hang once seen in the menus.
    REXLOG_ERROR("[nativo] C6: el hilo del anillo lleva 2 s esperando las copias de vertices: objetivo {}, hechas {}, "
                 "tomadas {}, escritas {}, copiadas por el hilo {}",
                 objetivo, copias_hechas_.load(std::memory_order_acquire),
                 copias_tomadas_.load(std::memory_order_acquire), copias_escritas_.load(std::memory_order_acquire),
                 copias_progreso_.load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> trozo(copias_trozo_mutex_);
    if (copias_hechas_.load(std::memory_order_acquire) != objetivo) {
      RepararCopias(objetivo, "2 s esperando al hilo y, con el hilo fuera de su bucle, siguen faltando copias");
    }
  }

  // Takes the next untaken batch (up to kCopiasPorTrozo copies, in order) and copies it. Used by the
  // thread, holding copias_trozo_mutex_, and by the ring when helping. false = nothing was left to take.
  bool TomarYCopiar(bool anillo, uint64_t& copias, uint64_t& bytes) {
    const size_t escritas = copias_escritas_.load(std::memory_order_acquire);
    size_t tomadas = copias_tomadas_.load(std::memory_order_acquire);
    size_t n = 0;
    do {
      if (tomadas >= escritas) {
        return false;
      }
      n = std::min<size_t>(escritas - tomadas, kCopiasPorTrozo);
    } while (!copias_tomadas_.compare_exchange_weak(tomadas, tomadas + n, std::memory_order_acq_rel,
                                                    std::memory_order_acquire));
    const size_t mascara = copias_.size() - 1;
    for (size_t i = tomadas; i < tomadas + n; ++i) {
      const TrabajoCopia& trabajo = copias_[i & mascara];
      CopiarVertices(trabajo);
      bytes += uint64_t(trabajo.palabras) * 4;
      copias_marcas_[i & mascara].store(uint32_t(i + 1), std::memory_order_relaxed);
      if (!anillo) {
        copias_progreso_.store(i + 1, std::memory_order_relaxed);  // for the EsperarSubidas diagnostic
      }
    }
    copias += n;
    copias_hechas_.fetch_add(n, std::memory_order_acq_rel);  // publishes the copies and their marks
    return true;
  }

  bool HayCopiasSinTomar() const {
    return copias_tomadas_.load(std::memory_order_acquire) != copias_escritas_.load(std::memory_order_acquire);
  }

  // The guard, on each wait and with everything finished (the ring does not queue while waiting and the
  // thread has nothing half taken): what was queued since the previous one has its mark and the counters
  // add up. Ring only.
  void ComprobarCopias(size_t objetivo) {
    if (objetivo == copias_verificadas_ || copias_sin_hilo_) {
      return;
    }
    const size_t mascara = copias_.size() - 1;
    const size_t desde =
        std::max(copias_verificadas_, objetivo > copias_.size() ? objetivo - copias_.size() : size_t(0));
    size_t sin_marca = 0;
    size_t primera = 0;
    for (size_t i = desde; i < objetivo; ++i) {
      if (copias_marcas_[i & mascara].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        if (!sin_marca) {
          primera = i;
        }
        ++sin_marca;
      }
    }
    copias_comprobadas_ += objetivo - desde;
    copias_inf_.comprobadas += objetivo - desde;
    const size_t tomadas = copias_tomadas_.load(std::memory_order_acquire);
    const size_t hechas = copias_hechas_.load(std::memory_order_acquire);
    if (!sin_marca && tomadas == objetivo && hechas == objetivo) {
      copias_verificadas_ = objetivo;
      return;
    }
    const std::string motivo = sin_marca ? fmt::format("{} de {} copias sin su marca (la primera, la {})", sin_marca,
                                                       objetivo - desde, primera)
                                         : std::string("todas con su marca, pero los contadores no cuadran");
    std::lock_guard<std::mutex> trozo(copias_trozo_mutex_);  // the thread, out of its copy loop
    if (copias_fase_ == kCopiasAplicando && tomadas == objetivo && hechas == objetivo) {
      // With the counters right, the suspect is the help path: redo whatever is missing and go back to the
      // plain wait.
      const size_t rehechas = RehacerSinMarca(objetivo);
      copias_verificadas_ = objetivo;
      ++copias_diferencias_;
      copias_fase_ = kCopiasSinAyuda;
      REXLOG_ERROR("[nativo] C6: copias de vertices: DIFERENCIA: {} ({} encoladas, contadores cuadrados; {} rehechas "
                   "en el anillo antes de enviar). Ayuda del anillo APAGADA para el resto de la sesion: espera al hilo "
                   "como antes",
                   motivo, objetivo, rehechas);
      return;
    }
    RepararCopias(objetivo, motivo);
  }

  // Copies on the ring whatever was queued up to `objetivo` (since the last check, and at most one lap of
  // the queue) that lacks its mark, and marks it. With copias_trozo_mutex_ held. Returns how many.
  size_t RehacerSinMarca(size_t objetivo) {
    const size_t mascara = copias_.size() - 1;
    const size_t desde =
        std::max(copias_verificadas_, objetivo > copias_.size() ? objetivo - copias_.size() : size_t(0));
    size_t rehechas = 0;
    for (size_t i = desde; i < objetivo; ++i) {
      if (copias_marcas_[i & mascara].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        CopiarVertices(copias_[i & mascara]);
        copias_marcas_[i & mascara].store(uint32_t(i + 1), std::memory_order_relaxed);
        ++rehechas;
      }
    }
    return rehechas;
  }

  // Should never happen. With copias_trozo_mutex_ held, i.e. with the thread out of its copy loop and
  // nothing half taken: the ring copies whatever is missing up to `objetivo` (before submitting and before
  // returning the read pointer), the counters are left consistent and the thread is switched off for the
  // session.
  void RepararCopias(size_t objetivo, const std::string& motivo) {
    const size_t tomadas = copias_tomadas_.load(std::memory_order_acquire);
    const size_t hechas = copias_hechas_.load(std::memory_order_acquire);
    const size_t rehechas = RehacerSinMarca(objetivo);
    copias_tomadas_.store(objetivo, std::memory_order_release);
    copias_hechas_.store(objetivo, std::memory_order_release);
    copias_verificadas_ = objetivo;
    ++copias_diferencias_;
    copias_sin_hilo_ = true;
    REXLOG_ERROR("[nativo] C6: copias de vertices: DIFERENCIA: {} (encoladas {}, tomadas {}, hechas {}; {} rehechas en "
                 "el anillo antes de enviar). Hilo de copias APAGADO para el resto de la sesion: las copias van en el "
                 "anillo, como antes de la build 90",
                 motivo, objetivo, tomadas, hechas, rehechas);
  }

  const char* NombreFaseCopias() const {
    if (copias_sin_hilo_) {
      return "SIN HILO (copias en el anillo)";
    }
    return copias_fase_ == kCopiasAplicando ? "APLICANDO" : copias_fase_ == kCopiasMirando ? "MIRANDO" : "SIN AYUDA";
  }

  // Every 10 s, if there were copies: who did them and how long the ring really waited. Ring only.
  void InformeCopias() {
    if (!copias_hilo_.joinable() && !copias_sin_hilo_) {
      return;
    }
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - copias_informe_ < std::chrono::seconds(10)) {
      return;
    }
    copias_informe_ = ahora;
    const uint64_t hilo_n = copias_hilo_n_.load(std::memory_order_relaxed);
    const uint64_t hilo_bytes = copias_hilo_bytes_.load(std::memory_order_relaxed);
    const uint64_t d_hilo_n = hilo_n - copias_hilo_n_previo_;
    const uint64_t d_hilo_bytes = hilo_bytes - copias_hilo_bytes_previo_;
    copias_hilo_n_previo_ = hilo_n;
    copias_hilo_bytes_previo_ = hilo_bytes;
    const InformeCopiasCifras& c = copias_inf_;
    if (c.encoladas || c.en_linea || c.esperas) {
      NFSMW_INFORME_ANILLO(
          "[nativo] C6 copias de vertices (build 185), ultimos 10 s: {} encoladas ({:.1f} MB) y {} en el anillo sin "
          "pasar por la cola ({:.1f} MB); el hilo copio {} ({:.1f} MB) y el anillo ayudo con {} ({:.1f} MB, {:.1f} ms) "
          "en {} de {} esperas con copias pendientes ({} sin tomar al llegar); espero el trozo del hilo {} veces "
          "({:.2f} ms, peor {:.2f} ms) y entero {} veces; espera pura {:.1f} ms (peor {:.2f} ms); {} copias "
          "comprobadas, {} diferencias; fase {}",
          c.encoladas, double(c.bytes_encolados) / 1048576.0, c.en_linea, double(c.bytes_en_linea) / 1048576.0,
          d_hilo_n, double(d_hilo_bytes) / 1048576.0, c.ayudadas, double(c.bytes_ayudados) / 1048576.0,
          double(c.ns_ayudando) / 1e6, c.esperas_con_ayuda, c.esperas, c.sin_tomar, c.esperas_trozo,
          double(c.ns_trozo) / 1e6, double(c.ns_trozo_peor) / 1e6, c.esperas_enteras, double(c.ns_espera) / 1e6,
          double(c.ns_espera_peor) / 1e6, c.comprobadas, copias_diferencias_, NombreFaseCopias());
    }
    copias_inf_ = InformeCopiasCifras{};
  }

  // The per-fetch-constant sampler table, every 10 s: hits, and misses by cause.
  void InformeCacheFetch() {
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - fetch_informe_ < std::chrono::seconds(10)) {
      return;
    }
    fetch_informe_ = ahora;
    const std::array<uint64_t, 6> ahora_cifras{samplers_cache_fetch_, fetch_fallos_choque_, fetch_fallos_vacia_,
                                               fetch_fallos_generacion_, fetch_fallos_caducada_, samplers_cache_};
    std::array<uint64_t, 6> d{};
    for (size_t i = 0; i < d.size(); ++i) {
      d[i] = ahora_cifras[i] - fetch_informe_previos_[i];
    }
    fetch_informe_previos_ = ahora_cifras;
    const uint64_t fallos = d[1] + d[2] + d[3] + d[4];
    if (d[0] + fallos == 0) {
      return;
    }
    NFSMW_INFORME_ANILLO(
        "[nativo] C6 cache de samplers por fetch (build 192, {} casillas), ultimos 10 s: {} aciertos del registro, {} "
        "busquedas en la tabla con {} aciertos y {} fallos ({:.1f} %): {} choques con otra fetch en su casilla, {} en "
        "casilla vacia, {} por generacion y {} caducadas (toca comprobar la textura)",
        cache_fetch_.size(), d[5], d[0] + fallos, d[0], fallos,
        100.0 * double(fallos) / double(std::max<uint64_t>(d[0] + fallos, 1)), d[1], d[2], d[3], d[4]);
  }

  void PararCopias() {
    if (!copias_hilo_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> cerrojo(copias_mutex_);
      copias_parar_ = true;
    }
    copias_cv_.notify_one();
    copias_hilo_.join();
    REXLOG_INFO("[nativo] C6: hilo de copias de vertices parado ({} copias en el anillo por cola llena, {} "
                "esperas del anillo, {:.1f} ms esperando; 26/09 (build 185): el anillo copio {} al esperar ({:.1f} MB, "
                "{:.1f} ms), {} comprobadas, {} diferencias, fase {})",
                copias_en_linea_, esperas_copias_, double(ns_esperando_copias_) / 1e6, copias_ayudadas_,
                double(copias_bytes_ayudados_) / 1048576.0, double(copias_ns_ayudando_) / 1e6, copias_comprobadas_,
                copias_diferencias_, NombreFaseCopias());
  }

  void BucleCopias() {
    for (;;) {
      if (HayCopiasSinTomar()) {
        // Holding copias_trozo_mutex_ from the first batch until there is nothing left to take, and never
        // waiting on anything inside. If the ring waits for the current batch, it waits on this mutex and the
        // thread runs at the ring's priority until it releases it.
        uint64_t copias = 0;
        uint64_t bytes = 0;
        {
          std::lock_guard<std::mutex> trozo(copias_trozo_mutex_);
          while (TomarYCopiar(false, copias, bytes)) {
          }
        }
        copias_hilo_n_.fetch_add(copias, std::memory_order_relaxed);
        copias_hilo_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        continue;  // look again: if nothing is left, sleep through the path below
      }
      // Nothing to take (the ring may have taken the last ones). Under the lock: wake the ring if it waits,
      // and sleep until there is more. If the read of copias_escritas_ was stale, the ring signals at its next
      // point that takes the lock.
      std::unique_lock<std::mutex> cerrojo(copias_mutex_);
      if (copias_esperando_) {
        copias_hechas_cv_.notify_one();
      }
      copias_durmiendo_ = true;
      copias_cv_.wait(cerrojo, [this] { return copias_parar_ || HayCopiasSinTomar(); });
      copias_durmiendo_ = false;
      /*
       * The thread used to die on its own. It exited with "if (!HayCopiasSinTomar()) return;", on the
       * assumption that it was only woken without work in order to stop. But the ring takes untaken copies
       * when it waits for them (TomarYCopiar, without copias_mutex_): if the thread woke because there were
       * copies and the ring took them before this check, the thread ended for good. It was seen at the
       * Heritage & Omega exit with the ring at 94 %: from then on "el hilo copio 0" in every report, the
       * menu included, and the ring did all the copies (3-4 ms more per frame for the rest of the session).
       * Now it only exits when asked to stop; otherwise it looks again.
       */
      if (copias_parar_ && !HayCopiasSinTomar()) {
        return;  // stop, nothing pending
      }
    }
  }

  // nfsmw_tratamiento_visual. If the draw is the final composite and the filter is not the original one, uploads its
  // own copy of the constants with the color curve changed, and leaves the cache empty so the next draw uploads the
  // game's. If there is no room in the upload buffer (Dibujar does not count this copy), the frame keeps the original
  // filter.
  // It is out of line and Dibujar always calls it: that way Dibujar gains no branch and its control flow is still the
  // one in the PGO profile. With a branch inside, GCC would drop the whole profile of the ring's hottest function.
  [[gnu::noinline]] void CopiaComposicionTratada(const EntradaShader* ps, const uint32_t* r,
                                                 uint32_t bytes_ps) noexcept {
    if (!ps || !ps->shader || ps->shader->huella != kHuellaComposicion) {
      return;
    }
    const int modo = nfsmw::ajustes::TratamientoVisual();
    // One line per mode change, on the first draw of the composite: the values the game sets.
    static std::atomic<int> modo_anotado{-1};
    if (modo_anotado.exchange(modo, std::memory_order_relaxed) != modo) {
      const float* k = reinterpret_cast<const float*>(r + kRegConstantesPs);
      REXLOG_INFO("[nativo] filtro de color {}: composicion con {} bytes de constantes. El juego pone Coeffs0 ({:.3f}, "
                  "{:.3f}, {:.3f}, {:.3f}), Coeffs1 ({:.3f}, {:.3f}, {:.3f}, {:.3f}), Desaturation {:.3f} y vineta {:.3f}",
                  modo, bytes_ps, k[24], k[25], k[26], k[27], k[28], k[29], k[30], k[31], k[8], k[4]);
    }
    if (bytes_ps < kBytesTratamiento) {
      return;
    }
    VkDeviceSize offset = 0;
    if (modo == 0 || !Reservar(usar_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps,
                               usar_ubo_ ? alineacion_ubo_ : 16, offset)) {
      return;
    }
    std::memcpy(subida_datos_ + offset, r + kRegConstantesPs, bytes_ps);
    AplicarTratamientoVisual(reinterpret_cast<float*>(subida_datos_ + offset), modo);
    constantes_ps_offset_ = offset;
    constantes_ps_generacion_ = UINT64_MAX;
  }

  bool Reservar(VkDeviceSize bytes, VkDeviceSize alineacion, VkDeviceSize& offset) {
    const VkDeviceSize inicio = (subida_usado_ + alineacion - 1) & ~(alineacion - 1);
    if (inicio + bytes > kTamanoSubida) {
      offset = 0;
      return false;  // cannot happen: Dibujar checks the space first
    }
    offset = inicio;
    subida_usado_ = inicio + bytes;
    megas_bytes_ += bytes;
    return true;
  }

  // Like Reservar, but the offset is a multiple of `multiplo` (the stride: a multiple of 4, not always a
  // power of 2). Dibujar has already asked for space including that extra padding (hueco_base_cero).
  bool ReservarMultiplo(VkDeviceSize bytes, VkDeviceSize multiplo, VkDeviceSize& offset) {
    const VkDeviceSize inicio = (subida_usado_ + multiplo - 1) / multiplo * multiplo;
    if (inicio + bytes > kTamanoSubida) {
      offset = 0;
      return false;  // cannot happen: Dibujar checks the space first
    }
    offset = inicio;
    subida_usado_ = inicio + bytes;
    megas_bytes_ += bytes;
    return true;
  }

  // The pipelines file (cache/nfsmw_nativo_pipelines.bin): the Vulkan cache and the prewarm list.
  static std::filesystem::path RutaFicheroPipelines() {
    return rex::filesystem::GetExecutableFolder() / kCarpetaCache / kFicheroPipelines;
  }

  static void LeerFicheroEntero(const std::filesystem::path& ruta, std::vector<uint8_t>& datos) {
    std::ifstream fichero(ruta, std::ios::binary | std::ios::ate);
    const std::streamoff bytes = fichero ? std::streamoff(fichero.tellg()) : 0;
    if (bytes > 0 && bytes < (std::streamoff(256) << 20)) {
      datos.resize(size_t(bytes));
      fichero.seekg(0);
      if (!fichero.read(reinterpret_cast<char*>(datos.data()), std::streamsize(bytes))) {
        datos.clear();
      }
    }
  }

  // Reads the pipelines file and splits it into the cache and the list. If it does not exist yet, the two
  // older files (next to the NRO), which are deleted when the new one is written.
  void LeerFicheroPipelines(std::vector<uint8_t>& cache, std::vector<uint8_t>& lista) {
    std::vector<uint8_t> todo;
    LeerFicheroEntero(RutaFicheroPipelines(), todo);
    if (!todo.empty()) {
      uint32_t magia = 0;
      uint32_t version = 0;
      uint64_t bytes_lista = 0;
      uint64_t bytes_cache = 0;
      if (todo.size() >= kCabeceraFicheroPipelines) {
        std::memcpy(&magia, todo.data(), 4);
        std::memcpy(&version, todo.data() + 4, 4);
        std::memcpy(&bytes_lista, todo.data() + 8, 8);
        std::memcpy(&bytes_cache, todo.data() + 16, 8);
      }
      if (magia != kMagiaFicheroPipelines || version != kVersionFicheroPipelines ||
          bytes_lista > todo.size() || bytes_cache > todo.size() ||
          kCabeceraFicheroPipelines + bytes_lista + bytes_cache != todo.size()) {
        REXLOG_WARN("[nativo] C6: fichero de pipelines de otra version o danado: se empieza de cero");
        return;
      }
      const auto inicio = todo.begin() + kCabeceraFicheroPipelines;
      lista.assign(inicio, inicio + std::ptrdiff_t(bytes_lista));
      cache.assign(inicio + std::ptrdiff_t(bytes_lista), todo.end());
      return;
    }
    const std::filesystem::path carpeta = rex::filesystem::GetExecutableFolder();
    LeerFicheroEntero(carpeta / kFicheroCacheViejo, cache);
    LeerFicheroEntero(carpeta / kFicheroListaVieja, lista);
    ficheros_viejos_ = !cache.empty() || !lista.empty();
  }

  // On-disk pipeline cache (derived from the game's shaders: not distributed). If the driver rejects it
  // (another driver or corrupt data), it starts from scratch.
  void CargarCachePipelines() {
    const auto& ifn = dispositivo_->vulkan_instance()->functions();
    const auto crear = reinterpret_cast<FnCrearCachePipelines>(
        ifn.vkGetDeviceProcAddr(device_, "vkCreatePipelineCache"));
    datos_cache_ = reinterpret_cast<FnDatosCachePipelines>(
        ifn.vkGetDeviceProcAddr(device_, "vkGetPipelineCacheData"));
    destruir_cache_ = reinterpret_cast<FnDestruirCachePipelines>(
        ifn.vkGetDeviceProcAddr(device_, "vkDestroyPipelineCache"));
    if (!crear || !datos_cache_ || !destruir_cache_) {
      REXLOG_WARN("[nativo] C6: sin cache de pipelines (el driver no da sus funciones)");
      return;
    }
    const std::filesystem::path ruta = RutaFicheroPipelines();
    std::vector<uint8_t> datos;
    LeerFicheroPipelines(datos, lista_leida_);
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = datos.size();
    info.pInitialData = datos.empty() ? nullptr : datos.data();
    if (crear(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
      info.initialDataSize = 0;
      info.pInitialData = nullptr;
      datos.clear();
      if (crear(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
        cache_pipelines_ = VK_NULL_HANDLE;
      }
    }
    bytes_cache_guardados_ = datos.size();
    cache_guardada_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[nativo] C6: cache de pipelines {} ({} KB leidos de {}{})",
                cache_pipelines_ != VK_NULL_HANDLE ? "activa" : "no disponible",
                datos.size() >> 10, ruta.string(), ficheros_viejos_ ? ", de los dos ficheros de antes" : "");
    // The writer thread always saves both parts; it starts from what is on disk.
    escrito_cache_ = std::move(datos);
    if (ficheros_viejos_) {
      // they came from the two older files: moved to the new one on the first save even if nothing changed
      bytes_cache_guardados_ = 0;
      pipelines_sin_guardar_ = 1;
      lista_sin_guardar_ = 1;
    }
    CargarListaPipelines();  // the prewarm list
  }

  void GuardarCachePipelines() {
    cache_guardada_ = std::chrono::steady_clock::now();
    // The prewarm list is saved even if the cache does not grow (a new pipeline whose shaders were already
    // in it does not make it grow). The cache, as usual: only if it has new entries.
    std::vector<uint8_t> lista;
    if (lista_sin_guardar_) {
      lista_sin_guardar_ = 0;
      lista = SerializarListaPipelines();
    }
    std::vector<uint8_t> datos;
    if (pipelines_sin_guardar_) {
      pipelines_sin_guardar_ = 0;
      size_t bytes = 0;
      // without new entries it is not rewritten (the SD card is slow on the Switch)
      if (cache_pipelines_ != VK_NULL_HANDLE &&
          datos_cache_(device_, cache_pipelines_, &bytes, nullptr) == VK_SUCCESS && bytes &&
          bytes != bytes_cache_guardados_) {
        datos.resize(bytes);
        const VkResult resultado = datos_cache_(device_, cache_pipelines_, &bytes, datos.data());
        if ((resultado != VK_SUCCESS && resultado != VK_INCOMPLETE) || !bytes) {
          datos.clear();
        } else {
          datos.resize(bytes);
          bytes_cache_guardados_ = bytes;
        }
      }
    }
    if (datos.empty() && lista.empty()) {
      return;
    }
    // The file is written on its own thread: on the Switch, writing about 2 MB to the SD card from the ring
    // thread stalled it on every save. If the previous one has not been written yet, this one replaces it.
    std::lock_guard<std::mutex> cerrojo(escritor_mutex_);
    if (!escritor_cache_.joinable()) {
      escritor_cache_ = std::thread([this] {
        rex::thread::set_current_thread_name("NFSMW cache de pipelines");
        EscritorCacheMain();
      });
    }
    if (!datos.empty()) {  // only the list, if the cache did not grow
      escritor_datos_ = std::move(datos);
    }
    if (!lista.empty()) {
      escritor_lista_ = std::move(lista);
    }
    escritor_pendiente_ = true;
    escritor_aviso_.notify_one();
  }

  // Pipeline cache writer thread. Sleeping or continuing is decided with the lock held.
  void EscritorCacheMain() {
    std::vector<uint8_t> datos;
    std::vector<uint8_t> lista;  // the prewarm one
    for (;;) {
      {
        std::unique_lock<std::mutex> cerrojo(escritor_mutex_);
        escritor_aviso_.wait(cerrojo, [this] { return escritor_pendiente_ || escritor_parar_; });
        if (!escritor_pendiente_) {
          return;  // stop, nothing pending
        }
        datos = std::move(escritor_datos_);
        escritor_datos_ = {};
        lista = std::move(escritor_lista_);
        escritor_lista_ = {};
        escritor_pendiente_ = false;
      }
      // A single file with both parts. Only one may arrive (the cache did not grow, or the list did not
      // change): the other is the last one written.
      if (!datos.empty()) {
        escrito_cache_ = std::move(datos);
      }
      if (!lista.empty()) {
        escrito_lista_ = std::move(lista);
      }
      EscribirFicheroPipelines();
    }
  }

  // cache/nfsmw_nativo_pipelines.bin with the list and the cache, written to a temporary file that is then
  // renamed. Writer thread only. The first time it is written, the two older files are deleted if present.
  void EscribirFicheroPipelines() {
    const auto antes = std::chrono::steady_clock::now();
    const std::filesystem::path ruta = RutaFicheroPipelines();
    std::error_code error;
    std::filesystem::create_directories(ruta.parent_path(), error);
    std::filesystem::path temporal = ruta;
    temporal += ".tmp";
    {
      const uint32_t magia = kMagiaFicheroPipelines;
      const uint32_t version = kVersionFicheroPipelines;
      const uint64_t bytes_lista = escrito_lista_.size();
      const uint64_t bytes_cache = escrito_cache_.size();
      std::ofstream fichero(temporal, std::ios::binary | std::ios::trunc);
      if (!fichero || !fichero.write(reinterpret_cast<const char*>(&magia), 4) ||
          !fichero.write(reinterpret_cast<const char*>(&version), 4) ||
          !fichero.write(reinterpret_cast<const char*>(&bytes_lista), 8) ||
          !fichero.write(reinterpret_cast<const char*>(&bytes_cache), 8) ||
          !fichero.write(reinterpret_cast<const char*>(escrito_lista_.data()), std::streamsize(bytes_lista)) ||
          !fichero.write(reinterpret_cast<const char*>(escrito_cache_.data()), std::streamsize(bytes_cache))) {
        return;
      }
    }
    error.clear();
    std::filesystem::rename(temporal, ruta, error);
    if (error) {
      std::filesystem::remove(ruta, error);
      std::filesystem::rename(temporal, ruta, error);
    }
    if (!error && ficheros_viejos_) {
      const std::filesystem::path carpeta = rex::filesystem::GetExecutableFolder();
      std::error_code sin_importancia;
      std::filesystem::remove(carpeta / kFicheroCacheViejo, sin_importancia);
      std::filesystem::remove(carpeta / kFicheroListaVieja, sin_importancia);
      ficheros_viejos_ = false;
    }
    if (guardados_cache_++ < 8) {
      REXLOG_INFO("[nativo] C6: pipelines guardados ({} KB de cache y {} en la lista del precalentado, en {} ms, desde "
                  "su hilo{})",
                  escrito_cache_.size() >> 10,
                  escrito_lista_.size() >= kCabeceraLista
                      ? (escrito_lista_.size() - kCabeceraLista) / sizeof(RegistroPipeline)
                      : 0,
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - antes)
                      .count(),
                  error ? ", no se pudo renombrar" : "");
    }
  }

  void PararEscritorCache() {
    {
      std::lock_guard<std::mutex> cerrojo(escritor_mutex_);
      escritor_parar_ = true;
      escritor_aviso_.notify_one();
    }
    if (escritor_cache_.joinable()) {
      escritor_cache_.join();
    }
  }

  // One upload buffer per work slot of the render target code: the previous frame's may still be in use
  // on the GPU while the other is filled.
  bool CrearSubida() {
    for (BuferSubida& s : subidas_) {
      const bool creado = CrearBuferSubida();
      s = {subida_, subida_memoria_, subida_tamano_real_, subida_datos_, subida_direccion_};
      subida_ = VK_NULL_HANDLE;
      subida_memoria_ = VK_NULL_HANDLE;
      subida_datos_ = nullptr;
      if (!creado) {
        return false;
      }
    }
    const auto& tipos = dispositivo_->memory_types();
    REXLOG_INFO("[nativo] C6: bufer de subida en el tipo de memoria {} ({}, {}; nfsmw_nativo_subida_memoria = {})",
                subida_tipo_, ((tipos.host_cached >> subida_tipo_) & 1) ? "con cache de CPU" : "sin cache de CPU",
                subida_coherente_ ? "coherente, sin publicar" : "se publica antes de enviar",
                REXCVAR_GET(nfsmw_nativo_subida_memoria));
    MedirMemoriaSubida();
    CrearCompartidasAparte();
    UsarRanura(0);
    return true;
  }

  // A small CPU-cached buffer per slot, only for the shared constants. If anything fails, it continues as
  // before (in the upload buffer) and says so in the log.
  void CrearCompartidasAparte() {
    compartidas_aparte_ = false;
    if (!REXCVAR_GET(nfsmw_nativo_compartidas_cache)) {
      REXLOG_INFO("[nativo] C6: constantes compartidas en el bufer de subida (nfsmw_nativo_compartidas_cache = false)");
      return;
    }
    const auto& tipos = dispositivo_->memory_types();
    for (BuferSubida& c : compartidas_bufs_) {
      VkBufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      info.size = kTamanoCompartidas;
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (dfn_.vkCreateBuffer(device_, &info, nullptr, &c.bufer) != VK_SUCCESS) {
        REXLOG_WARN("[nativo] C6: no se pudo crear el bufer de constantes compartidas; van en el de subida");
        return;
      }
      VkMemoryRequirements requisitos;
      dfn_.vkGetBufferMemoryRequirements(device_, c.bufer, &requisitos);
      uint32_t tipo = 0;
      if (!rex::bit_scan_forward(requisitos.memoryTypeBits & tipos.host_visible & tipos.host_cached, &tipo)) {
        REXLOG_WARN("[nativo] C6: no hay memoria visible con cache de CPU; las constantes compartidas van en el de subida");
        return;
      }
      VkMemoryAllocateInfo reserva{};
      reserva.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserva.allocationSize = requisitos.size;
      reserva.memoryTypeIndex = tipo;
      if (dfn_.vkAllocateMemory(device_, &reserva, nullptr, &c.memoria) != VK_SUCCESS ||
          dfn_.vkBindBufferMemory(device_, c.bufer, c.memoria, 0) != VK_SUCCESS) {
        REXLOG_WARN("[nativo] C6: no se pudo reservar el bufer de constantes compartidas; van en el de subida");
        return;
      }
      void* mapeado = nullptr;
      if (dfn_.vkMapMemory(device_, c.memoria, 0, VK_WHOLE_SIZE, 0, &mapeado) != VK_SUCCESS) {
        REXLOG_WARN("[nativo] C6: no se pudo mapear el bufer de constantes compartidas; van en el de subida");
        return;
      }
      c.datos = static_cast<uint8_t*>(mapeado);
      c.tamano_real = requisitos.size;
      compartidas_tipo_ = tipo;
    }
    compartidas_coherente_ = (tipos.host_coherent >> compartidas_tipo_) & 0x1;
    compartidas_aparte_ = true;
    REXLOG_INFO("[nativo] C6: constantes compartidas aparte, en el tipo de memoria {} (con cache de CPU, {}), {} MB "
                "por ranura",
                compartidas_tipo_, compartidas_coherente_ ? "coherente" : "se publica antes de enviar",
                kTamanoCompartidas >> 20);
  }

  bool CrearBuferSubida() {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kTamanoSubida;
    // UNIFORM_BUFFER because it is also bound as a dynamic UBO (constants through UBOs, set 4).
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (direccion_bufer_) {
      info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &subida_) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requisitos;
    dfn_.vkGetBufferMemoryRequirements(device_, subida_, &requisitos);
    // nfsmw_nativo_subida_memoria: with 1 or 2, a host-visible type with or without CPU caching is looked
    // for; if there is none, the SDK's.
    const auto& tipos = dispositivo_->memory_types();
    subida_tipo_ = UINT32_MAX;
    const int32_t preferencia = REXCVAR_GET(nfsmw_nativo_subida_memoria);
    if (preferencia == 1 || preferencia == 2) {
      const uint32_t visibles = requisitos.memoryTypeBits & tipos.host_visible;
      uint32_t tipo = 0;
      if (rex::bit_scan_forward(visibles & (preferencia == 1 ? tipos.host_cached : ~tipos.host_cached), &tipo)) {
        subida_tipo_ = tipo;
      }
    }
    if (subida_tipo_ == UINT32_MAX) {
      subida_tipo_ = rex::ui::vulkan::util::ChooseMemoryType(tipos, requisitos.memoryTypeBits,
                                                             rex::ui::vulkan::util::MemoryPurpose::kUpload);
    }
    if (subida_tipo_ == UINT32_MAX) {
      return false;
    }
    subida_coherente_ = (dispositivo_->memory_types().host_coherent >> subida_tipo_) & 0x1;
    VkMemoryAllocateFlagsInfo banderas{};
    banderas.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    banderas.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo reserva{};
    reserva.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    reserva.pNext = direccion_bufer_ ? &banderas : nullptr;
    reserva.allocationSize = requisitos.size;
    reserva.memoryTypeIndex = subida_tipo_;
    if (dfn_.vkAllocateMemory(device_, &reserva, nullptr, &subida_memoria_) != VK_SUCCESS) {
      return false;
    }
    subida_tamano_real_ = requisitos.size;
    if (dfn_.vkBindBufferMemory(device_, subida_, subida_memoria_, 0) != VK_SUCCESS) {
      return false;
    }
    void* mapeado = nullptr;
    if (dfn_.vkMapMemory(device_, subida_memoria_, 0, VK_WHOLE_SIZE, 0, &mapeado) != VK_SUCCESS) {
      return false;
    }
    subida_datos_ = static_cast<uint8_t*>(mapeado);
    if (!direccion_bufer_) {
      subida_direccion_ = 0;
      return true;
    }
    VkBufferDeviceAddressInfo direccion{};
    direccion.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    direccion.buffer = subida_;
    subida_direccion_ = direccion_bufer_(device_, &direccion);
    return subida_direccion_ != 0;
  }

  // For the nfsmw_nativo_subida_memoria A/B test: CPU write MB/s into each host-visible memory type and the
  // cost of publishing those 8 MB (vkFlushMappedMemoryRanges; in NVK for Tegra, armDCacheClean). Once.
  void MedirMemoriaSubida() {
    if (!REXCVAR_GET(nfsmw_nativo_diag_memoria_subida)) {
      return;
    }
    constexpr VkDeviceSize kBytes = VkDeviceSize(8) << 20;
    std::vector<uint8_t> origen(static_cast<size_t>(kBytes));
    for (size_t i = 0; i < origen.size(); ++i) {
      origen[i] = uint8_t((i * 131) ^ (i >> 11));
    }
    const auto& tipos = dispositivo_->memory_types();
    std::string informe;
    for (uint32_t tipo = 0; tipo < 32; ++tipo) {
      if (!((tipos.host_visible >> tipo) & 1)) {
        continue;
      }
      VkMemoryAllocateInfo reserva{};
      reserva.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserva.allocationSize = kBytes;
      reserva.memoryTypeIndex = tipo;
      VkDeviceMemory memoria = VK_NULL_HANDLE;
      if (dfn_.vkAllocateMemory(device_, &reserva, nullptr, &memoria) != VK_SUCCESS) {
        continue;
      }
      void* mapeado = nullptr;
      if (dfn_.vkMapMemory(device_, memoria, 0, VK_WHOLE_SIZE, 0, &mapeado) == VK_SUCCESS) {
        using Reloj = std::chrono::steady_clock;
        std::memcpy(mapeado, origen.data(), size_t(kBytes));  // first pass: pages and caches
        const auto antes = Reloj::now();
        std::memcpy(mapeado, origen.data(), size_t(kBytes));
        const auto copiado = Reloj::now();
        const bool coherente = (tipos.host_coherent >> tipo) & 1;
        if (!coherente) {
          VkMappedMemoryRange rango{};
          rango.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
          rango.memory = memoria;
          rango.size = VK_WHOLE_SIZE;
          dfn_.vkFlushMappedMemoryRanges(device_, 1, &rango);
        }
        const auto publicado = Reloj::now();
        const double segundos = std::chrono::duration<double>(copiado - antes).count();
        informe += fmt::format(
            "{}tipo {} ({}{}): {:.0f} MB/s{}", informe.empty() ? "" : "; ", tipo,
            ((tipos.host_cached >> tipo) & 1) ? "con cache de CPU" : "sin cache de CPU", coherente ? ", coherente" : "",
            segundos > 0 ? 8.0 / segundos : 0.0,
            coherente ? std::string()
                      : fmt::format(", publicar {:.2f} ms",
                                    std::chrono::duration<double, std::milli>(publicado - copiado).count()));
        dfn_.vkUnmapMemory(device_, memoria);
      }
      dfn_.vkFreeMemory(device_, memoria, nullptr);
    }
    REXLOG_INFO("[nativo] C6: escritura desde la CPU por tipo de memoria (8 MB): {}",
                informe.empty() ? std::string("sin tipos medibles") : informe);
  }

  bool CrearDescriptores() {
    static constexpr VkDescriptorType kTipos[4] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    const VkDescriptorBindingFlags banderas_enlace =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    for (uint32_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutBinding enlace{};
      enlace.binding = 0;
      enlace.descriptorType = kTipos[i];
      enlace.descriptorCount = kCapacidadMonton[i];
      enlace.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo banderas{};
      banderas.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
      banderas.bindingCount = 1;
      banderas.pBindingFlags = &banderas_enlace;
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.pNext = &banderas;
      info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
      info.bindingCount = 1;
      info.pBindings = &enlace;
      if (dfn_.vkCreateDescriptorSetLayout(device_, &info, nullptr, &layouts_[i]) != VK_SUCCESS) {
        return false;
      }
      montones_[i].capacidad = kCapacidadMonton[i];
    }
    const VkDescriptorPoolSize tamanos[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         kCapacidadMonton[0] + kCapacidadMonton[1] + kCapacidadMonton[2]},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kCapacidadMonton[3]}};
    VkDescriptorPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    info_pool.maxSets = 4;
    info_pool.poolSizeCount = 2;
    info_pool.pPoolSizes = tamanos;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &pool_) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo reserva{};
    reserva.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserva.descriptorPool = pool_;
    reserva.descriptorSetCount = 4;
    reserva.pSetLayouts = layouts_.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reserva, sets_.data()) != VK_SUCCESS) {
      return false;
    }
    // --- Set 4, the constants through dynamic UBOs ------------------------------------------------------
    // Always created: the shaders of the current library use it statically even with the bit off (it is
    // then bound with offsets 0). With an older library it is unnecessary and harmless. No
    // UPDATE_AFTER_BIND: dynamic descriptors do not support it. CrearSubida runs first, so the buffers
    // already exist.
    // Always through UBOs: the library's shaders no longer have the pointer path (shader_common.h).
    usar_ubo_ = true;
    if (!REXCVAR_GET(nfsmw_nativo_constantes_ubo)) {
      REXLOG_WARN("[nativo] C6: nfsmw_nativo_constantes_ubo = false ya no se admite: los shaders solo leen los UBO");
    }
    cache_entre_fotogramas_ = REXCVAR_GET(nfsmw_nativo_cache_texturas_entre_fotogramas);
    REXLOG_INFO("[nativo] C6: caches de texturas entre fotogramas (nfsmw_nativo_cache_texturas_entre_fotogramas) = {}",
                cache_entre_fotogramas_ ? "SI" : "no");
    mipmaps_ = REXCVAR_GET(nfsmw_nativo_mipmaps);
    REXLOG_INFO("[nativo] C3: niveles de mip de las texturas (nfsmw_nativo_mipmaps) = {}", mipmaps_ ? "SI" : "no");
    texturas_mb_max_ = REXCVAR_GET(nfsmw_nativo_texturas_mb_max);
    prueba_sin_memoria_cada_ = REXCVAR_GET(nfsmw_nativo_prueba_sin_memoria_cada);
    if (prueba_sin_memoria_cada_ > 0) {
      REXLOG_WARN("[nativo] C3: PRUEBA activa: se fingira falta de memoria cada {} reservas de textura",
                  prueba_sin_memoria_cada_);
    }
    REXLOG_INFO("[nativo] C3: limite de la cache de texturas (nfsmw_nativo_texturas_mb_max) = {} MB{}", texturas_mb_max_,
                texturas_mb_max_ > 0 ? "" : " (sin limite)");
    diag_mips_ = REXCVAR_GET(nfsmw_nativo_diag_mips);
    if (diag_mips_) {
      REXLOG_INFO("[nativo] C3: diagnostico de mips (nfsmw_nativo_diag_mips) = SI");
    }
    alineacion_ubo_ = std::max<VkDeviceSize>(16, dispositivo_->properties().minUniformBufferOffsetAlignment);
    alternar_ubo_s_ = 0;  // the pointer half of that test no longer exists
    REXLOG_INFO("[nativo] C6: constantes por UBO dinamico (nfsmw_nativo_constantes_ubo) = {}; alternar cada {} s; "
                "alineacion {} bytes",
                usar_ubo_ ? "SI" : "no", alternar_ubo_s_, alineacion_ubo_);
    std::array<VkDescriptorSetLayoutBinding, 3> enlaces_ubo{};
    for (uint32_t b = 0; b < 3; ++b) {
      enlaces_ubo[b].binding = b;
      enlaces_ubo[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      enlaces_ubo[b].descriptorCount = 1;
      enlaces_ubo[b].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info_ubo{};
    info_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_ubo.bindingCount = 3;
    info_ubo.pBindings = enlaces_ubo.data();
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_ubo, nullptr, &layout_ubo_) != VK_SUCCESS) {
      return false;
    }
    const VkDescriptorPoolSize tamano_ubo{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * uint32_t(sets_ubo_.size())};
    VkDescriptorPoolCreateInfo info_pool_ubo{};
    info_pool_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_ubo.maxSets = uint32_t(sets_ubo_.size());
    info_pool_ubo.poolSizeCount = 1;
    info_pool_ubo.pPoolSizes = &tamano_ubo;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_ubo, nullptr, &pool_ubo_) != VK_SUCCESS) {
      return false;
    }
    std::array<VkDescriptorSetLayout, kRanurasDeTrabajo> layouts_ubo;
    layouts_ubo.fill(layout_ubo_);
    VkDescriptorSetAllocateInfo reserva_ubo{};
    reserva_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserva_ubo.descriptorPool = pool_ubo_;
    reserva_ubo.descriptorSetCount = uint32_t(sets_ubo_.size());
    reserva_ubo.pSetLayouts = layouts_ubo.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reserva_ubo, sets_ubo_.data()) != VK_SUCCESS) {
      return false;
    }
    for (size_t ranura = 0; ranura < sets_ubo_.size(); ++ranura) {
      const VkDescriptorBufferInfo bloques[3] = {{subidas_[ranura].bufer, 0, kUboBytesVs},
                                                 {subidas_[ranura].bufer, 0, kUboBytesPs},
                                                 {compartidas_aparte_ ? compartidas_bufs_[ranura].bufer
                                                                      : subidas_[ranura].bufer,
                                                  0, kUboBytesCompartidas}};
      std::array<VkWriteDescriptorSet, 3> escrituras_ubo{};
      for (uint32_t b = 0; b < 3; ++b) {
        escrituras_ubo[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        escrituras_ubo[b].dstSet = sets_ubo_[ranura];
        escrituras_ubo[b].dstBinding = b;
        escrituras_ubo[b].descriptorCount = 1;
        escrituras_ubo[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        escrituras_ubo[b].pBufferInfo = &bloques[b];
      }
      dfn_.vkUpdateDescriptorSets(device_, 3, escrituras_ubo.data(), 0, nullptr);
    }
    const VkPushConstantRange rango{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    24};
    VkPipelineLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const std::array<VkDescriptorSetLayout, 5> layouts_pipeline = {layouts_[0], layouts_[1], layouts_[2],
                                                                    layouts_[3], layout_ubo_};
    info_layout.setLayoutCount = 5;
    info_layout.pSetLayouts = layouts_pipeline.data();
    info_layout.pushConstantRangeCount = 1;
    info_layout.pPushConstantRanges = &rango;
    return dfn_.vkCreatePipelineLayout(device_, &info_layout, nullptr, &layout_pipeline_) ==
           VK_SUCCESS;
  }

  // Slot 0 of each heap: a transparent black texture and a basic sampler, as the emulation does for an
  // invalid fetch constant.
  bool CrearVacias() {
    const std::array<std::pair<VkImageViewType, uint32_t>, 3> tipos = {
        {{VK_IMAGE_VIEW_TYPE_2D, 1}, {VK_IMAGE_VIEW_TYPE_3D, 1}, {VK_IMAGE_VIEW_TYPE_CUBE, 6}}};
    for (uint32_t i = 0; i < 3; ++i) {
      VkImageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = i == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
      info.flags = i == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
      info.format = VK_FORMAT_R8G8B8A8_UNORM;
      info.extent = {1, 1, 1};
      info.mipLevels = 1;
      info.arrayLayers = tipos[i].second;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              dispositivo_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
              vacias_[i].imagen, vacias_[i].memoria)) {
        return false;
      }
      VkImageViewCreateInfo vista{};
      vista.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vista.image = vacias_[i].imagen;
      vista.viewType = tipos[i].first;
      vista.format = VK_FORMAT_R8G8B8A8_UNORM;
      vista.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, tipos[i].second};
      if (dfn_.vkCreateImageView(device_, &vista, nullptr, &vacias_[i].vista) != VK_SUCCESS) {
        return false;
      }
      vacias_[i].ancho = vacias_[i].alto = 1;
      vacias_[i].formato = VK_FORMAT_R8G8B8A8_UNORM;
      EscribirImagen(i, 0, vacias_[i].vista);
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    if (dfn_.vkCreateSampler(device_, &sampler, nullptr, &sampler_vacio_) != VK_SUCCESS) {
      return false;
    }
    EscribirSampler(0, sampler_vacio_);
    return true;
  }

  // The empty ones are initialized on the first submission: GENERAL and cleared to zero.
  bool PrepararVacias() {
    if (vacias_preparadas_) {
      return true;
    }
    const VkCommandBuffer subida = contexto_->ComandosSubida();
    if (!subida) {
      return false;
    }
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t capas = i == 2 ? 6 : 1;
      Barrera(subida, vacias_[i].imagen, capas);
      VkClearColorValue cero{};
      if (REXCVAR_GET(nfsmw_nativo_diag_vacias_grises)) {
        cero.float32[0] = cero.float32[1] = cero.float32[2] = 0.5f;
        cero.float32[3] = 1.0f;
      }
      const VkImageSubresourceRange rango{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, capas};
      dfn_.vkCmdClearColorImage(subida, vacias_[i].imagen, VK_IMAGE_LAYOUT_GENERAL, &cero, 1,
                                &rango);
      vacias_[i].preparada = true;
    }
    vacias_preparadas_ = true;
    return true;
  }

  void Barrera(VkCommandBuffer cmd, VkImage imagen, uint32_t capas) {
    VkImageMemoryBarrier barrera{};
    barrera.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrera.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrera.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrera.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrera.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrera.image = imagen;
    barrera.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, capas};  // and mips
    barrera.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrera);
  }

  void EscribirImagen(uint32_t monton, uint32_t ranura, VkImageView vista) {
    VkDescriptorImageInfo imagen{};
    imagen.imageView = vista;
    imagen.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet escritura{};
    escritura.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    escritura.dstSet = sets_[monton];
    escritura.dstBinding = 0;
    escritura.dstArrayElement = ranura;
    escritura.descriptorCount = 1;
    escritura.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    escritura.pImageInfo = &imagen;
    dfn_.vkUpdateDescriptorSets(device_, 1, &escritura, 0, nullptr);
  }

  void EscribirSampler(uint32_t ranura, VkSampler sampler) {
    VkDescriptorImageInfo imagen{};
    imagen.sampler = sampler;
    VkWriteDescriptorSet escritura{};
    escritura.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    escritura.dstSet = sets_[3];
    escritura.dstBinding = 0;
    escritura.dstArrayElement = ranura;
    escritura.descriptorCount = 1;
    escritura.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    escritura.pImageInfo = &imagen;
    dfn_.vkUpdateDescriptorSets(device_, 1, &escritura, 0, nullptr);
  }

  uint32_t ReservarRanura(uint32_t monton) {
    auto& m = montones_[monton];
    if (!m.libres.empty()) {
      const uint32_t ranura = m.libres.back();
      m.libres.pop_back();
      return ranura;
    }
    return m.siguiente < m.capacidad ? m.siguiente++ : 0;
  }

  // The draw's vertex input, recomputed only when the patched VS changes.
  const EntradaVertices* EntradaDe(const PeticionDibujo& p) {
    if (entrada_generacion_ == p.generacion_vs && entrada_vs_ == p.vs) {
      if (entrada_valida_) {
        ++entradas_reutilizadas_;
        return entrada_actual_;
      }
      // The same input that already failed: no recomputation and no warning.
      ++rechazados_;
      ++causas_[entrada_causa_];
      return nullptr;
    }
    entrada_generacion_ = p.generacion_vs;
    entrada_vs_ = p.vs;
    entrada_valida_ = false;
    // Cache: the same VS with the same patched fetches gives the same input.
    const uint64_t clave_entrada = ClaveEntrada(p);
    if (clave_entrada) {
      if (const auto it = entradas_cache_.find(clave_entrada); it != entradas_cache_.end()) {
        entrada_actual_ = &it->second;
        entrada_valida_ = true;
        entrada_causa_ = 0;
        ++entradas_cache_aciertos_;
        return entrada_actual_;
      }
    }
    entrada_ = EntradaVertices{};
    const uint64_t rechazados_antes = rechazados_;
    const auto antes_entrada = std::chrono::steady_clock::now();
    const EntradaVertices* entrada = CalcularEntrada(p);
    ns_entradas_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - antes_entrada)
                                 .count());
    ++entradas_calculadas_;
    entrada_causa_ = rechazados_ != rechazados_antes ? ultima_causa_ : 0;
    entrada_actual_ = &entrada_;
    if (entrada && clave_entrada && entradas_cache_.size() < 4096) {
      entradas_cache_.emplace(clave_entrada, entrada_);
    }
    return entrada;
  }

  // Key of the input cache: everything CalcularEntrada reads from the elements and from both microcodes
  // (instruction, usage and index; original and patched words of each fetch), seeded with the VS. 0 = do
  // not cache.
  uint64_t ClaveEntrada(const PeticionDibujo& p) const {
    const EntradaShader& vs = *p.vs;
    const auto& parcheado = p.vs_microcodigo;
    if (parcheado.size() != vs.microcodigo.size() || vs.elementos.size() > 32) {
      return 0;
    }
    std::array<uint32_t, 32 * 6> palabras;
    size_t n = 0;
    for (const ElementoVertice& elemento : vs.elementos) {
      const size_t i = size_t(elemento.instruccion) * 3;
      if (i + 2 >= parcheado.size()) {
        return 0;
      }
      palabras[n++] = (uint32_t(elemento.instruccion) << 16) | (uint32_t(elemento.uso) << 8) |
                      elemento.indice_uso;
      palabras[n++] = vs.microcodigo[i];
      palabras[n++] = vs.microcodigo[i + 1];
      palabras[n++] = parcheado[i];
      palabras[n++] = parcheado[i + 1];
      palabras[n++] = parcheado[i + 2];
    }
    const uint64_t clave =
        XXH3_64bits_withSeed(palabras.data(), n * sizeof(uint32_t), uint64_t(uintptr_t(p.vs)));
    return clave ? clave : 1;
  }

  const EntradaVertices* CalcularEntrada(const PeticionDibujo& p) {
    const EntradaShader& vs = *p.vs;
    entrada_.remapeos.fill(kRemapeoIdentidad);
    const auto& parcheado = p.vs_microcodigo;
    if (parcheado.size() != vs.microcodigo.size()) {
      Rechazar(20, "el VS del anillo no tiene la longitud del contenedor");
      return nullptr;
    }
    uint32_t ubicaciones_usadas = 0;
    for (const ElementoVertice& elemento : vs.elementos) {
      const uint32_t registro = (vs.microcodigo[size_t(elemento.instruccion) * 3] >> 12) & 0x3F;
      const uint32_t original = vs.microcodigo[size_t(elemento.instruccion) * 3 + 1] & 0xFFF;
      size_t q = SIZE_MAX;
      for (const ElementoVertice& otro : vs.elementos) {
        const size_t i = size_t(otro.instruccion) * 3;
        if (((parcheado[i] >> 12) & 0x3F) != registro || (parcheado[i] & 0x1F) != 0) {
          continue;
        }
        if (q == SIZE_MAX) {
          q = i;
        }
        if (MascaraEscrita(parcheado[i + 1] & 0xFFF) == MascaraEscrita(original)) {
          q = i;  // the one that writes the same components
          break;
        }
      }
      if (q == SIZE_MAX) {
        if (avisados_vs_.size() < 16 && avisados_vs_.insert(vs.numero).second) {
          std::string detalle;
          for (const ElementoVertice& otro : vs.elementos) {
            const size_t i = size_t(otro.instruccion) * 3;
            detalle += fmt::format(" {}{}@{}: original r{} op{}, parcheado r{} op{} {:08X} {:08X} {:08X};",
                                   NombreUso(otro.uso), otro.indice_uso, otro.instruccion,
                                   (vs.microcodigo[i] >> 12) & 0x3F, vs.microcodigo[i] & 0x1F,
                                   (parcheado[i] >> 12) & 0x3F, parcheado[i] & 0x1F, parcheado[i],
                                   parcheado[i + 1], parcheado[i + 2]);
          }
          REXLOG_WARN("[nativo] C6 diag: VS n{} sin fetch para {}{} (registro r{}):{}", vs.numero,
                      NombreUso(elemento.uso), elemento.indice_uso, registro, detalle);
        }
        Rechazar(21, "fetch de vertices no encontrado en el VS parcheado");
        return nullptr;
      }
      const uint32_t d0 = parcheado[q], d1 = parcheado[q + 1], d2 = parcheado[q + 2];
      const uint32_t ranura = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
      const uint32_t formato = (d1 >> 16) & 0x3F;
      const uint32_t zancada = (d2 & 0xFF) * 4;
      const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
      const int32_t ubicacion = UbicacionDeUso(elemento.uso, elemento.indice_uso);
      if (ubicacion < 0 || ((ubicaciones_usadas >> ubicacion) & 0x1)) {
        Avisar(22, "elemento de vertices sin ubicacion libre en el shader: se omite");
        continue;
      }
      const uint32_t codigo = CodigoRemapeo(original, d1 & 0xFFF);
      if (codigo != kRemapeoIdentidad && avisos_swizzle_ < 24) {
        ++avisos_swizzle_;
        REXLOG_INFO("[nativo] C6: VS n{} {}{} (formato {}): swizzle original {:03X}, parcheado {:03X}, "
                    "remapeo {:03X}",
                    vs.numero, NombreUso(elemento.uso), elemento.indice_uso, formato, original,
                    d1 & 0xFFF, codigo);
      }
      bool r11g11b10 = false;
      const VkFormat vk = FormatoAtributo(formato, EntradaEntera(elemento.uso), (d1 >> 12) & 0x1,
                                          (d1 >> 13) & 0x1, false, r11g11b10);
      if (vk == VK_FORMAT_UNDEFINED) {
        Rechazar(300 + formato, "formato de vertice todavia no soportado");
        return nullptr;
      }
      if (r11g11b10) {
        entrada_.especializacion |= 0x1;
      }
      uint32_t enlace = 0;
      while (enlace < entrada_.enlaces.size() && entrada_.enlaces[enlace].ranura != ranura) {
        ++enlace;
      }
      if (enlace == entrada_.enlaces.size()) {
        entrada_.enlaces.push_back({ranura, zancada});
      } else if (!entrada_.enlaces[enlace].zancada) {
        entrada_.enlaces[enlace].zancada = zancada;
      }
      if (offset < 0) {
        Rechazar(24, "offset de vertice negativo");
        return nullptr;
      }
      if (size_t(ubicacion) >= entrada_.remapeos.size() && codigo != kRemapeoIdentidad) {
        // The shaders only read g_InputRemap for the first 16 locations (NFS Carbon's extras are not remapped).
        Rechazar(26, "remapeo de swizzle en una ubicacion sin g_InputRemap");
        return nullptr;
      }
      entrada_.atributos.push_back({uint32_t(ubicacion), enlace, vk, uint32_t(offset)});
      if (size_t(ubicacion) < entrada_.remapeos.size()) {
        entrada_.remapeos[ubicacion] = codigo;
      }
      ubicaciones_usadas |= uint32_t(1) << ubicacion;
    }
    for (const EnlaceVertices& enlace : entrada_.enlaces) {
      if (!enlace.zancada) {
        Rechazar(25, "stream de vertices sin zancada");
        return nullptr;
      }
    }
    uint64_t huella = XXH3_64bits(entrada_.atributos.data(),
                                  entrada_.atributos.size() * sizeof(AtributoVertices));
    huella = XXH3_64bits_withSeed(entrada_.enlaces.data(),
                                  entrada_.enlaces.size() * sizeof(EnlaceVertices), huella);
    entrada_.huella = huella ^ entrada_.especializacion;
    entrada_valida_ = true;
    return &entrada_;
  }

  // Texture of a fetch constant: heap slot and, if it has to be uploaded, it is left in texturas_a_subir_
  // with its data already prepared. Base level of 2D textures and cubemaps.
  void PrepararTextura(const uint32_t* f, uint32_t& ranura, uint32_t& monton,
                       VkDeviceSize& bytes_subida, bool& muestreo_puntual, uint64_t& valido_hasta,
                       uint32_t& ancho_host_out, uint32_t& alto_host_out) {
    ranura = 0;
    monton = 0;
    ancho_host_out = 0;  // 0 = unknown (1/size is not written)
    alto_host_out = 0;
    muestreo_puntual = false;
    valido_hasta = fotograma_;  // by default, this frame only
    if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
      return;
    }
    const uint32_t dimension = (f[5] >> 9) & 0x3;
    const bool cubo = dimension == uint32_t(xenos::DataDimension::kCube);
    const bool volumen = dimension == uint32_t(xenos::DataDimension::k3D);
    if (dimension != uint32_t(xenos::DataDimension::k2DOrStacked) && !cubo && !volumen) {
      Avisar(30, "texturas 1D todavia no soportadas: se usa una vacia");
      return;
    }
    monton = cubo ? 2 : volumen ? 1 : 0;
    const uint32_t capas = cubo ? 6 : 1;
    if ((f[0] >> 2) & 0xFF) {
      Avisar(31, "texturas con signo o gamma: se leen sin signo");
    }
    const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
    const uint32_t base = (f[1] >> 12) << 12;
    if (!cubo && !volumen) {
      if (const ImagenNativa* resuelta = contexto_->TexturaResuelta(base & 0x1FFFFFFF)) {
        // Without this the shadows flicker.
        // With nfsmw_nativo_inv_tamano_tex the shader takes 1/size from the shared constants, and this path
        // used to return without reporting the size: 0 was written and the tfetch offsets (the taps of the
        // shadow map's PCF filter) all landed on the same texel. The filtering disappeared and the edge
        // shimmered as the camera moved. Resolved textures report their size like any other.
        ancho_host_out = resuelta->ancho;
        alto_host_out = resuelta->alto;
        // One trace per size, to check in the log that the 1/size constant of resolved textures is no longer
        // 0 (that was the cause of the shadow flicker).
        if (avisos_invtam_resuelta_.insert(uint64_t(resuelta->ancho) << 32 | resuelta->alto).second) {
          REXLOG_INFO("[nativo] C3: textura resuelta {}x{}: 1/tamano = {:.6f}, {:.6f}",
                      resuelta->ancho, resuelta->alto, 1.0f / float(resuelta->ancho),
                      1.0f / float(resuelta->alto));
        }
        if (EsProfundidad(resuelta->formato)) {
          // Depth copy (k_24_8): depth comes out in R and the fetch constant's swizzle distributes it. No
          // filtering: it is read as is.
          ranura = RanuraVista(resuelta->imagen, resuelta->formato, swizzle, kSwizzleRRRR);
          muestreo_puntual = true;
          // This used to be UINT64_MAX, and that is what forced the cross-frame cache off.
          // A resolved texture is a render target the game rewrites every frame. Saying its descriptor slot is
          // valid "until the generation changes" meant that, with the CPU two frames ahead (3 work slots), an
          // old view was reused: that was the smeared race leaderboard that kept
          // nfsmw_nativo_cache_texturas_entre_fotogramas at false for a while. The other paths are already tied
          // to textura.siguiente and relax nothing; these two were the only ones without a horizon.
          valido_hasta = fotograma_;
          return;
        }
        // The emulation writes the copy to memory with copy_dest_swap and loads it as a texture: the fetch
        // constant's swizzle (ZYXW for 8888) undoes that swap. Here the copy is image to image and does not
        // swap channels, so the swap goes into the host channels (without this, Mia came out blue).
        ranura = RanuraVista(resuelta->imagen, resuelta->formato, swizzle,
                             resuelta->intercambio_rb ? kSwizzleBGRA : kSwizzleRGBA);
        valido_hasta = fotograma_;  // see the long comment on the depth copy
        return;
      }
    }
    const uint32_t formato = f[1] & 0x3F;
    FormatoTextura tf;
    if (!FormatoTexturaDe(formato, tf)) {
      Avisar(400 + formato, "formato de textura todavia no soportado: se usa una vacia");
      return;
    }
    const uint32_t indice_bc = IndiceBc(tf.formato);
    const bool bc_cpu = indice_bc && bc_cpu_[indice_bc - 1];
    const auto formato_bc = static_cast<nfsmw::bc::Formato>(indice_bc);
    // Keep tf in the guest format for untiling, mip addresses and byte order.
    const VkFormat formato_host = !bc_cpu ? tf.formato : indice_bc == 4 ? VK_FORMAT_R8_UNORM
        : indice_bc == 5 ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    // size_2d: 13 + 13 bits; size_3d: 11 + 11 + 10 bits (xenos.h:1222-1233).
    const uint32_t ancho = volumen ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1;
    const uint32_t alto = volumen ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
    const uint32_t fondo = volumen ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
    if (volumen && tf.bloque > 1) {
      Avisar(38, "textura 3D comprimida: se usa una vacia");
      return;
    }
    const uint32_t bloques_x = (ancho + tf.bloque - 1) / tf.bloque;
    const uint32_t bloques_y = (alto + tf.bloque - 1) / tf.bloque;
    const uint32_t pitch_bloques = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.bloque, 1);
    // The faces of a cubemap are consecutive: each takes its base level with rows and columns aligned to
    // 32 blocks and the total to 4 KB (GetGuestTextureLayout, pipeline/texture/util.cpp:311-337).
    const uint64_t zancada_cara =
        (uint64_t((pitch_bloques + 31) & ~uint32_t(31)) * tf.bytes *
             ((bloques_y + 31) & ~uint32_t(31)) +
         4095) &
        ~uint64_t(4095);
    const uint32_t claves[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
    const uint64_t clave = XXH3_64bits(claves, sizeof(claves));

    // Mip levels with the rules of GetSubresourcesFromFetchConstant (pipeline/texture/util.cpp:67-97): no
    // mip address means no mips; the maximum is clamped to the size; if the base is missing or the minimum
    // is above 0, the base is not read. 3D textures still use only the base.
    const bool mosaico_textura = (f[0] >> 31) & 0x1;
    const uint64_t dir_base = uint64_t(base) & 0x1FFFFFFF;
    const uint64_t dir_mips = uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
    const uint32_t nivel_empaquetado =
        mipmaps_ && !volumen && ((f[5] >> 11) & 0x1) ? NivelEmpaquetado(ancho, alto) : UINT32_MAX;
    uint32_t nivel_max = 0;
    bool leer_base = true;
    if (mipmaps_ && !volumen && dir_mips != 0) {
      const uint32_t tam_max = Log2Suelo(std::max(ancho, alto));
      uint32_t nivel_min = std::min((f[4] >> 2) & 0xFu, tam_max);
      nivel_max = std::max(std::min((f[4] >> 6) & 0xFu, tam_max), nivel_min);
      if (nivel_max != 0) {
        if (dir_base == 0) {
          nivel_min = std::max(nivel_min, 1u);
        }
        leer_base = nivel_min == 0;
      }
    }
    // Mip regions from dir_mips (GetGuestTextureLayout): each level stored with rows of 32 blocks computed
    // from the size rounded up to a power of 2 (and aligned to 256 bytes if the texture is linear) and
    // layers aligned to 4 KB; the levels of the packed tail share the region of the first of them.
    struct RegionMip {
      uint64_t desplazamiento = 0;
      uint64_t fila_bytes = 0;
      uint64_t zancada = 0;
      uint32_t pitch_bloques = 0;
    };
    std::array<RegionMip, 16> regiones{};
    uint64_t extension_mips = 0;
    if (nivel_max != 0) {
      const uint32_t ultima = nivel_empaquetado == 0 ? 0 : std::min(nivel_max, nivel_empaquetado);
      for (uint32_t s = nivel_empaquetado == 0 ? 0 : 1; s <= ultima; ++s) {
        RegionMip& region = regiones[s];
        const uint32_t fila_texels = std::max(std::bit_ceil(ancho) >> s, 1u);
        const uint32_t filas_texels = std::max(std::bit_ceil(alto) >> s, 1u);
        region.pitch_bloques = ((fila_texels + tf.bloque - 1) / tf.bloque + 31) & ~31u;
        region.fila_bytes = uint64_t(region.pitch_bloques) * tf.bytes;
        if (!mosaico_textura) {
          region.fila_bytes = (region.fila_bytes + 255) & ~uint64_t(255);
        }
        const uint64_t filas_bloques = uint64_t(((filas_texels + tf.bloque - 1) / tf.bloque + 31) & ~31u);
        region.zancada = (region.fila_bytes * filas_bloques + 4095) & ~uint64_t(4095);
        region.desplazamiento = extension_mips;
        extension_mips += region.zancada * capas;
      }
      if (dir_mips + extension_mips > kMemoriaFisica) {
        nivel_max = 0;  // the mips would go past memory: base only
        extension_mips = 0;
        leer_base = true;
      }
    }
    // With the packed tail starting at level 0 (short side of 16 texels or less), the base does not start
    // at its address either (VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl applies the same
    // offset to level 0).
    uint32_t base_ox = 0;
    uint32_t base_oy = 0;
    if (nivel_empaquetado == 0) {
      DesplazamientoEmpaquetado(ancho, alto, tf.bloque, 0, base_ox, base_oy);
    }

    if (cubo && copiar_imagen_) {
      // The game's dynamic cubemap: C2 resolves its faces one by one to those same addresses. They are
      // copied to the cubemap's layers in the upload command buffer, which runs before the work one, so they
      // lag one frame behind.
      std::array<const ImagenNativa*, 6> caras{};
      bool resueltas = true;
      for (uint32_t c = 0; c < 6 && resueltas; ++c) {
        caras[c] = contexto_->TexturaResuelta(
            uint32_t((uint64_t(base) + c * zancada_cara) & 0x1FFFFFFF));
        resueltas = caras[c] && caras[c]->formato == VK_FORMAT_R8G8B8A8_UNORM &&
                    caras[c]->ancho >= ancho && caras[c]->alto >= alto;
      }
      if (resueltas) {
        const uint64_t clave_resuelto = clave ^ 0x9E3779B97F4A7C15ull;
        Textura& textura = texturas_[clave_resuelto];
        if (textura.imagen.imagen == VK_NULL_HANDLE) {
          if (!CrearTextura(textura.imagen, VK_FORMAT_R8G8B8A8_UNORM, ancho, alto, capas)) {
            texturas_.erase(clave_resuelto);
            Avisar(32, "no se pudo crear una textura");
            return;
          }
          textura.capas = capas;
          textura.bytes = uint64_t(ancho) * alto * 4 * capas;
          bytes_texturas_ += textura.bytes;
          REXLOG_INFO("[nativo] C3: cubo {:08X} {}x{} con sus caras resueltas por C2 (fetch {:08X} "
                      "{:08X} {:08X} {:08X} {:08X} {:08X})",
                      base, ancho, alto, f[0], f[1], f[2], f[3], f[4], f[5]);
        }
        if (textura.fotograma != fotograma_) {
          const VkCommandBuffer subida = contexto_->ComandosSubida();
          if (!subida) {
            return;
          }
          textura.fotograma = fotograma_;
          if (!textura.imagen.preparada) {
            Barrera(subida, textura.imagen.imagen, capas);
            textura.imagen.preparada = true;
          }
          for (uint32_t c = 0; c < 6; ++c) {
            VkImageCopy copia{};
            copia.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copia.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, c, 1};
            copia.extent = {ancho, alto, 1};
            copiar_imagen_(subida, caras[c]->imagen, VK_IMAGE_LAYOUT_GENERAL, textura.imagen.imagen,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &copia);
          }
          // Rear-view mirror diagnostic: how many times the faces are copied to the cubemap per submission.
          ++cubos_refrescados_;
          const auto ahora = std::chrono::steady_clock::now();
          if (ahora - informe_cubos_ >= std::chrono::seconds(10)) {
            // It used to be the last periodic line the ring wrote itself; it now goes to the report thread.
            NFSMW_INFORME_ANILLO("[nativo] C3 cubos con caras de C2: {} refrescos en {} trabajos",
                                 cubos_refrescados_ - cubos_refrescados_previos_,
                                 fotograma_ - fotograma_informe_cubos_);
            informe_cubos_ = ahora;
            cubos_refrescados_previos_ = cubos_refrescados_;
            fotograma_informe_cubos_ = fotograma_;
          }
        }
        ranura = RanuraVista(textura.imagen.imagen, VK_FORMAT_R8G8B8A8_UNORM, swizzle,
                             caras[0]->intercambio_rb ? kSwizzleBGRA : kSwizzleRGBA, monton);
        // The cubemap with the resolved faces must also report its size, or its 1/size stays at 0 and the
        // tfetch offsets are lost (see the branch above).
        ancho_host_out = textura.imagen.ancho;
        alto_host_out = textura.imagen.alto;
        return;
      }
    }

    Textura& textura = texturas_[clave];
    if (textura.imagen.imagen == VK_NULL_HANDLE) {
      // BC in Vulkan: the size is in whole blocks.
      const uint32_t ancho_host = tf.bloque > 1 ? (ancho + 3) & ~uint32_t(3) : ancho;
      const uint32_t alto_host = tf.bloque > 1 ? (alto + 3) & ~uint32_t(3) : alto;
      // With its mip levels (those that fit in the host size).
      const uint32_t niveles = std::min(nivel_max + 1, Log2Suelo(std::max(ancho_host, alto_host)) + 1);
      /*
       * The vkBindImageMemory goes to the bind thread if possible (nfsmw_nativo_texturas_enlace_hilo) and,
       * otherwise, CrearTextura as usual. What the ring spends creating is timed, without its wait for the
       * thread (that is counted separately), for the [tiron] anillo line and the 10 s report.
       */
      const auto antes_crear = std::chrono::steady_clock::now();
      const uint64_t espera_antes_crear = ns_espera_enlaces_total_;
      midiendo_creacion_ = true;
      const bool creada = CrearTexturaEnHilo(textura, formato_host, ancho_host, alto_host, capas, fondo, niveles) ||
                          CrearTextura(textura.imagen, formato_host, ancho_host, alto_host, capas, fondo, niveles);
      midiendo_creacion_ = false;
      {
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - antes_crear)
                                         .count());
        const uint64_t espera = ns_espera_enlaces_total_ - espera_antes_crear;
        const uint64_t propio = ns > espera ? ns - espera : 0;
        nfsmw::esperas::g_ns_crear_texturas.fetch_add(propio, std::memory_order_relaxed);
        ns_crear_informe_ += propio;
      }
      if (!creada) {
        texturas_.erase(clave);
        Avisar(32, "no se pudo crear una textura");
        return;
      }
      textura.capas = capas;
      textura.fondo = fondo;
      textura.niveles = niveles;
      niveles_mip_subidos_ += niveles - 1;
      // Measurement only: the shape without the address, to count, once its first hash is known, whether it
      // repeats the content of a live one (AnotarContenidoTextura). If this image is being recreated, the
      // previous one stops counting. The words are the key's without the base and mip addresses and without
      // bit 11 of f[1], which the sampler sets: format, byte order, tiling, pitch, size, mip limits, dimension
      // and packed tail.
      if (DiagReutilizar()) {
        QuitarContenidoTextura(textura, clave);
        const uint32_t forma[5] = {claves[0], f[1] & 0x7FF, claves[2], claves[3], (f[5] >> 9) & 0x7};
        textura.forma_contenido = XXH3_64bits(forma, sizeof(forma));
        textura.direccion = base;
        textura.contenido_por_medir = true;
      }
      for (uint32_t n = 0; n < niveles; ++n) {
        const uint32_t bloque_host = bc_cpu ? 1 : tf.bloque;
        const uint32_t bytes_host = bc_cpu ? nfsmw::bc::Canales(formato_bc) : tf.bytes;
        textura.bytes += uint64_t((std::max(ancho_host >> n, 1u) + bloque_host - 1) / bloque_host) *
                         ((std::max(alto_host >> n, 1u) + bloque_host - 1) / bloque_host) * bytes_host * capas *
                         (fondo ? fondo : 1);
      }
      bytes_texturas_ += textura.bytes;
      // Diagnostic: why the cache grows on the console. A new texture at an address that already had another
      // points to world zones reloaded at the same place; with the same address, format and size but a
      // different key, to key fields that change (mip limits, byte order...).
      {
        ++texturas_creadas_;
        nfsmw::esperas::g_texturas_creadas.fetch_add(1, std::memory_order_relaxed);
        const uint64_t forma = XXH3_64bits_withSeed(&base, sizeof(base), (uint64_t(formato) << 40) ^
                                                                              (uint64_t(ancho) << 20) ^ alto);
        if (creadas_por_direccion_[base]++ > 0) {
          ++creadas_en_direccion_vista_;
        }
        const auto [it_forma, nueva_forma] = clave_por_forma_.try_emplace(forma, clave);
        if (!nueva_forma && it_forma->second != clave) {
          ++creadas_misma_forma_otra_clave_;
          it_forma->second = clave;
          /*
           * In the main menu, ~100 textures per frame once came back with the same address, format and size and
           * a different key, and were created again every frame. This shows which key words change (the first
           * 40 times).
           */
          const auto [it_palabras, nuevas] = palabras_por_forma_.try_emplace(forma);
          /*
           * Key guard. The same five words cannot produce a different key: if one does, the key was not computed
           * from what its words say (as when XXH3 read claves[4] before it was written; see docs/toolchain.md).
           */
          if (!nuevas && std::equal(std::begin(claves), std::end(claves), it_palabras->second.begin())) {
            const uint64_t veces = ++claves_incoherentes_;
            if (veces == 1 || veces == 10 || veces == 100 || veces == 1000 || veces % 10000 == 0) {
              REXLOG_ERROR("[nativo] C3 clave de textura INCOHERENTE ({} veces): {:08X} {}x{} formato {} con las "
                           "mismas cinco palabras que la vez anterior y otra clave; la clave no sale de sus palabras",
                           veces, base, ancho, alto, formato);
            }
          }
          if (!nuevas && avisos_otra_clave_ < 40) {
            ++avisos_otra_clave_;
            const auto& antes = it_palabras->second;
            REXLOG_INFO("[nativo] C3 misma textura con otra clave: {:08X} {}x{} formato {} | antes {:08X} {:08X} "
                        "{:08X} {:02X} {:06X} | ahora {:08X} {:08X} {:08X} {:02X} {:06X} | cambian {:08X} {:08X} "
                        "{:08X} {:02X} {:06X}",
                        base, ancho, alto, formato, antes[0], antes[1], antes[2], antes[3], antes[4], claves[0],
                        claves[1], claves[2], claves[3], claves[4], antes[0] ^ claves[0], antes[1] ^ claves[1],
                        antes[2] ^ claves[2], antes[3] ^ claves[3], antes[4] ^ claves[4]);
          }
          std::copy(std::begin(claves), std::end(claves), it_palabras->second.begin());
        } else if (nueva_forma) {
          auto& palabras = palabras_por_forma_[forma];
          std::copy(std::begin(claves), std::end(claves), palabras.begin());
        }
        // From 256 to 1024. This line and the GPU memory line after it were the two longest stutters of the
        // "volcado de diagnostico" group: 147.6 ms and 107.5 ms of frame time on their own. With 1024 they
        // still appear once or twice per session, which is enough to see whether the duplicate key counter
        // spikes.
        if (texturas_creadas_ % 1024 == 0) {
          REXLOG_INFO("[nativo] C3 diag cache: {} texturas creadas ({} en la cache, {} MB): {} en una direccion que ya "
                      "tuvo otra textura; {} con la misma direccion, formato y tamano que otra pero distinta clave",
                      texturas_creadas_, texturas_.size(), bytes_texturas_ >> 20, creadas_en_direccion_vista_,
                      creadas_misma_forma_otra_clave_);
          AnotarMemoriaDeLaGpu();
        }
      }
      if (texturas_.size() <= 48) {
        REXLOG_INFO("[nativo] C3: textura {:08X} {}x{} formato {} {} orden {} pitch {} "
                    "swizzle {:03X} signos {:02X}{}; niveles {} (mips en {:08X}, empaquetados desde {})",
                    base, ancho, alto, formato, ((f[0] >> 31) & 0x1) ? "en mosaico" : "lineal",
                    (f[1] >> 6) & 0x3, ((f[0] >> 22) & 0x1FF) << 5, swizzle, (f[0] >> 2) & 0xFF,
                    cubo ? " (cubo)" : volumen ? " (3D)" : "", niveles, dir_mips,
                    nivel_empaquetado == UINT32_MAX ? std::string("ninguno") : std::to_string(nivel_empaquetado));
      }
    }
    ranura = RanuraVista(textura.imagen.imagen, formato_host, swizzle, tf.swizzle_host, monton);
    ancho_host_out = textura.imagen.ancho;  // the host's, which is what the shader sees
    alto_host_out = textura.imagen.alto;
    if (textura.fotograma == fotograma_) {
      // Valid until the frame before the next check (if it changed now, only this frame)
      valido_hasta = std::max<uint64_t>(fotograma_, textura.siguiente ? textura.siguiente - 1 : 0);
      return;  // already checked this frame
    }
    // Textures that do not change are checked less and less often (down to every 32 frames); those that
    // change (videos) go back to being checked every frame.
    if (textura.imagen.preparada && fotograma_ < textura.siguiente) {
      valido_hasta = textura.siguiente - 1;
      return;
    }
    textura.fotograma = fotograma_;
    // If its hash thread job is still uncollected (a buffer-full submission happened in the middle of its
    // draw), its huella_cruda is not valid yet: it is collected before checking, as if the ring had done
    // it.
    if (textura.huella_trabajo == kTrabajoHuellaPublicado) {
      RecogerHuellas();
    }
    // What it costs from here to the end (check, untile, prepare the upload).
    struct CronometroTextura {
      std::chrono::steady_clock::time_point inicio = std::chrono::steady_clock::now();
      ~CronometroTextura() {
        nfsmw::esperas::g_ns_texturas.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - inicio)
                         .count()),
            std::memory_order_relaxed);
      }
    } cronometro_textura;
    // 2D and cubemaps: before untiling, XXH3 of the guest bytes. If they have not changed there is nothing
    // to do (untiling and comparing every texture every frame took 62 of the 66 us of each menu draw).
    if (!volumen) {
      const uint32_t log2_bloque = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                                : tf.bytes >= 2 ? 1 : 0;
      // Extent of a layer: tiled, GetTiledAddressUpperBound2D (pipeline/texture/util.cpp:461-486); linear,
      // up to the last block. Includes the packed base offset (base_ox, base_oy; 0 for the rest).
      const uint32_t bloques_x_leidos = bloques_x + base_ox;
      const uint32_t bloques_y_leidos = bloques_y + base_oy;
      const uint64_t extension_capa =
          ((f[0] >> 31) & 0x1)
              ? uint64_t(std::max<int64_t>(
                    DesplazamientoMosaico2D(int32_t((bloques_x_leidos - 1) & ~31u),
                                            int32_t((bloques_y_leidos - 1) & ~31u), pitch_bloques,
                                            log2_bloque),
                    0)) +
                    (log2_bloque == 0   ? 0xA00u
                     : log2_bloque == 1 ? 0xC00u
                                        : (0x400u << log2_bloque))
              : uint64_t(std::max(pitch_bloques, bloques_x_leidos)) * tf.bytes * (bloques_y_leidos - 1) +
                    uint64_t(bloques_x_leidos) * tf.bytes;
      const uint64_t inicio_crudo = uint64_t(base) & 0x1FFFFFFF;
      const uint64_t extension = (capas - 1) * zancada_cara + extension_capa;
      if (inicio_crudo + extension <= kMemoriaFisica) {
        /*
         * Per-frame check budget.
         * During race stutters both game threads wait for room in the ring (20-50 ms) while the ring neither
         * stops nor waits for the GPU: the ring thread itself is stuck. During the stutters, that thread was in
         * PrepararTextura and in XXH3. Textures that arrive together (one zone) double their interval at the
         * same time (1, 2, 4... 32), so all of them are rechecked in the same frame. Here a stable texture
         * (interval 8 or more) whose check does not fit in the frame's budget is deferred a few frames, at
         * most kAplazamientoMax in a row. New textures, changing ones (videos) and just-uploaded ones are
         * never deferred.
         */
        /*
         * Sampled recheck of stable textures (nfsmw_nativo_huellas_muestreo).
         * A stable texture used to go through XXH3 in full every 32-39 frames only to conclude, almost always,
         * that it had not changed: in the alley that was 0.2-5.3 ms per frame of the ring thread. Now every
         * full check from interval 4 on also computes the hash of a sample of the same memory (HuellaMuestra:
         * first and last 4 KB block and 1 in 8, of the base and the mips) and, if the full hash matches, stores
         * it. Rechecks of stable textures compute only the sample: if it matches, the texture is taken as
         * unchanged; otherwise the usual full path follows. Textures where the sample would read more than half
         * the bytes (those with few blocks) work as before.
         * Self-checking guard: the first kMuestrasAComprobar rechecks of stable textures compute both hashes
         * and count disagreements (same sample, different full hash). After that, 1 in N rechecks of each
         * texture is still full (N = the cvar), so a change the sample misses is caught at most N rechecks
         * later. A single disagreement turns sampling off for the rest of the session, with a warning.
         */
        if (muestreo_huellas_ < 0) {
          const int32_t cada = REXCVAR_GET(nfsmw_nativo_huellas_muestreo);
          muestreo_huellas_ = cada >= 2 ? std::min<int32_t>(cada, 64) : 0;
          REXLOG_INFO("[nativo] C3: recomprobacion de texturas estables por muestreo (nfsmw_nativo_huellas_muestreo) "
                      "= {}",
                      muestreo_huellas_ ? fmt::format("SI, 1 de cada {} completa; las {} primeras con las dos huellas",
                                                      muestreo_huellas_, kMuestrasAComprobar)
                                        : std::string("no, siempre la huella completa"));
        }
        const uint64_t bytes_completa = extension + extension_mips;
        const uint64_t bytes_de_muestra = BytesMuestra(extension) + BytesMuestra(extension_mips);
        const bool con_muestra = muestreo_huellas_ > 0 && textura.imagen.preparada && textura.intervalo >= 4 &&
                                 bytes_de_muestra * 2 <= bytes_completa;
        const bool muestra_util = con_muestra && textura.intervalo >= 8 && textura.muestra_valida;
        const bool solo_muestra = muestra_util && muestras_comprobadas_ >= kMuestrasAComprobar &&
                                  textura.muestras_seguidas + 1u < uint32_t(muestreo_huellas_);
        const uint64_t bytes_muestra = con_muestra ? bytes_de_muestra : 0;
        // What will actually be read, which is what counts against the budget.
        const uint64_t bytes_huella = solo_muestra ? bytes_muestra : bytes_completa + bytes_muestra;
        if (fotograma_huellas_ != fotograma_) {
          fotograma_huellas_ = fotograma_;
          bytes_huella_fotograma_ = 0;
        }
        if (presupuesto_huellas_ < 0) {
          presupuesto_huellas_ = std::max(REXCVAR_GET(nfsmw_nativo_huellas_kb_fotograma), 0);
          REXLOG_INFO("[nativo] C3: presupuesto de comprobaciones de texturas = {} KB por fotograma{}",
                      presupuesto_huellas_, presupuesto_huellas_ ? "" : " (sin limite)");
        }
        if (presupuesto_huellas_ && textura.imagen.preparada && textura.intervalo >= 8 &&
            textura.aplazamientos < kAplazamientoMax && bytes_huella_fotograma_ > 0 &&
            bytes_huella_fotograma_ + bytes_huella > uint64_t(presupuesto_huellas_) * 1024) {
          ++textura.aplazamientos;
          textura.siguiente = fotograma_ + 1 + (clave & 1);
          valido_hasta = textura.siguiente - 1;
          nfsmw::esperas::g_huellas_aplazadas.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        textura.aplazamientos = 0;
        bytes_huella_fotograma_ += bytes_huella;
        nfsmw::esperas::g_bytes_huella.fetch_add(bytes_huella, std::memory_order_relaxed);
        const auto inicio_huella_cruda = std::chrono::steady_clock::now();
        const uint8_t* const crudo_base = memoria_->TranslatePhysical(uint32_t(inicio_crudo));
        const uint8_t* const crudo_mips = extension_mips ? memoria_->TranslatePhysical(uint32_t(dir_mips)) : nullptr;
        // The sample goes before the full hash. If the full one matches, the sample is of that same content,
        // and it is the one stored.
        uint64_t muestra = 0;
        if (con_muestra) {
          muestra = HuellaMuestra(crudo_base, extension, 0);
          if (extension_mips) {
            muestra = HuellaMuestra(crudo_mips, extension_mips, muestra);
          }
          bytes_muestra_ += bytes_muestra;
          InformeMuestreo(inicio_huella_cruda);
        }
        const bool muestra_igual = muestra_util && muestra == textura.huella_muestra;
        if (solo_muestra && muestra_igual) {
          nfsmw::esperas::g_ns_huella_cruda.fetch_add(
              uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                            inicio_huella_cruda)
                           .count()),
              std::memory_order_relaxed);
          ++textura.muestras_seguidas;
          ++muestras_aciertos_;
          bytes_ahorrados_muestra_ += bytes_completa - bytes_muestra;
          textura.intervalo = std::min<uint32_t>(textura.intervalo * 2, 32);
          textura.siguiente = fotograma_ + textura.intervalo + (textura.intervalo == 32 ? (clave >> 7) & 7 : 0);
          valido_hasta = textura.siguiente - 1;
          return;
        }
        if (solo_muestra) {
          // The sample changed: now the full hash, which was not in this frame's budget.
          ++muestras_distintas_;
          bytes_huella_fotograma_ += bytes_completa;
          nfsmw::esperas::g_bytes_huella.fetch_add(bytes_completa, std::memory_order_relaxed);
        }
        /*
         * New texture -> hash thread (nfsmw_nativo_texturas_huella_hilo; see PlanearHuella).
         * The plan is the same LeerNivel calls the usual path makes further down, with the same arguments, but
         * reading from a copy of the bytes this hash covers. If any read falls outside them, or LeerNivel
         * would go past memory, the texture continues on the usual path.
         */
        // The hash worker copies guest blocks directly into the GPU upload buffer.
        // Converted textures use the normal path so decoding happens before upload.
        if (!bc_cpu && !textura.imagen.preparada && huellas_fase_ != kHuellasApagado && !diag_mips_ && mosaico_rapido_ >= 0) {
          std::array<size_t, 16> desplazamientos{};
          std::array<size_t, 16> capa_nivel{};
          size_t bytes_plan = 0;
          for (uint32_t n = 0; n < textura.niveles; ++n) {
            const uint32_t bx_n = (std::max(textura.imagen.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
            const uint32_t by_n = (std::max(textura.imagen.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
            desplazamientos[n] = bytes_plan;
            capa_nivel[n] = size_t(bx_n) * by_n * tf.bytes;
            bytes_plan += capa_nivel[n] * capas;
          }
          const uint32_t bx_base = (textura.imagen.ancho + tf.bloque - 1) / tf.bloque;
          const uint64_t mips_en_copia = (extension + 63) & ~uint64_t(63);  // where the copy of the mips starts
          uint32_t n_lecturas = 0;
          bool plan_ok = true;
          for (uint32_t c = 0; plan_ok && c < (leer_base ? capas : 0u); ++c) {
            plan_ok = PlanearLecturaHuella(lecturas_plan_, n_lecturas, dir_base + c * zancada_cara, mosaico_textura,
                                           pitch_bloques, uint64_t(std::max(pitch_bloques, bloques_x)) * tf.bytes, tf,
                                           base_ox, base_oy, bloques_x, bloques_y, bx_base, capa_nivel[0] * c,
                                           inicio_crudo, extension, 0);
          }
          for (uint32_t n = 1; plan_ok && n < textura.niveles; ++n) {
            const uint32_t s = nivel_empaquetado == 0 ? 0 : std::min(n, nivel_empaquetado);
            uint32_t ox = 0;
            uint32_t oy = 0;
            if (n >= nivel_empaquetado) {
              DesplazamientoEmpaquetado(ancho, alto, tf.bloque, n, ox, oy);
            }
            const uint32_t bx_invitado = (std::max(ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
            const uint32_t by_invitado = (std::max(alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
            const uint32_t bx_host = (std::max(textura.imagen.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
            const uint32_t by_host = (std::max(textura.imagen.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
            for (uint32_t c = 0; plan_ok && c < capas; ++c) {
              plan_ok = PlanearLecturaHuella(lecturas_plan_, n_lecturas,
                                             dir_mips + regiones[s].desplazamiento + c * regiones[s].zancada,
                                             mosaico_textura, regiones[s].pitch_bloques, regiones[s].fila_bytes, tf,
                                             ox, oy, std::min(bx_invitado, bx_host), std::min(by_invitado, by_host),
                                             bx_host, desplazamientos[n] + capa_nivel[n] * c, dir_mips, extension_mips,
                                             mips_en_copia);
            }
          }
          if (plan_ok && PlanearHuella(textura, clave, n_lecturas, crudo_base, extension, crudo_mips, extension_mips,
                                       tf, (f[1] >> 6) & 0x3, bytes_plan, desplazamientos, base, ancho, alto,
                                       formato, bytes_subida)) {
            return;  // applying phase: the thread prepares it; the tail of this function is already done
          }
        }
        uint64_t huella_cruda = XXH3_64bits(crudo_base, size_t(extension));
        if (extension_mips) {  // and the bytes of all the mips
          huella_cruda = XXH3_64bits_withSeed(crudo_mips, size_t(extension_mips), huella_cruda);
        }
        nfsmw::esperas::g_ns_huella_cruda.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                          inicio_huella_cruda)
                         .count()),
            std::memory_order_relaxed);
        const bool completa_igual = textura.imagen.preparada && huella_cruda == textura.huella_cruda;
        if (muestra_util && !solo_muestra) {
          // The guard. This stable recheck computed both hashes.
          ++muestras_comprobadas_;
          ++muestras_con_las_dos_;
          if (muestra_igual && !completa_igual) {
            REXLOG_ERROR("[nativo] C3: la MUESTRA de la textura {:08X} {}x{} formato {} ({} KB de base y {} KB de "
                         "mips) no ha visto un cambio que la huella completa si. Se APAGA el muestreo de huellas "
                         "(tras {} recomprobaciones con las dos).",
                         uint32_t(inicio_crudo), ancho, alto, formato, extension >> 10, extension_mips >> 10,
                         muestras_comprobadas_);
            muestreo_huellas_ = 0;
          } else if (muestras_comprobadas_ == kMuestrasAComprobar) {
            REXLOG_INFO("[nativo] C3: muestreo de huellas: {} recomprobaciones con las dos huellas, ningun "
                        "desacuerdo. Sigue el muestreo, con 1 de cada {} completa.",
                        muestras_comprobadas_, muestreo_huellas_);
          }
        }
        if (completa_igual) {
          // The sample of this content, for the stable rechecks.
          if (con_muestra && muestreo_huellas_ > 0) {
            textura.huella_muestra = muestra;
            textura.muestra_valida = true;
          }
          textura.muestras_seguidas = 0;
          textura.intervalo = std::min<uint32_t>(textura.intervalo * 2, 32);
          // At the maximum interval, from 32 to 39 depending on the texture, so they do not coincide.
          textura.siguiente = fotograma_ + textura.intervalo + (textura.intervalo == 32 ? (clave >> 7) & 7 : 0);
          valido_hasta = textura.siguiente - 1;
          return;
        }
        textura.muestra_valida = false;  // the content changed; the sample is no longer valid
        textura.muestras_seguidas = 0;
        textura.huella_cruda = huella_cruda;
        AnotarContenidoTextura(textura, clave);  // measurement only: nfsmw_nativo_diag_reutilizar
      }
    }
    // Base level of each layer, untiled and in host byte order.
    const uint32_t bloques_x_host = (textura.imagen.ancho + tf.bloque - 1) / tf.bloque;
    const uint32_t bloques_y_host = (textura.imagen.alto + tf.bloque - 1) / tf.bloque;
    const size_t bytes_capa = size_t(bloques_x_host) * bloques_y_host * tf.bytes;
    std::vector<uint8_t>& datos = temporal_;
    // All levels back to back, each one layer after layer (SubirTextura copies them one by one).
    std::array<size_t, 16> bytes_capa_nivel{};
    size_t bytes_datos = 0;
    for (uint32_t n = 0; n < textura.niveles; ++n) {
      const uint32_t bx_host = (std::max(textura.imagen.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t by_host = (std::max(textura.imagen.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
      textura.desplazamiento_nivel[n] = uint32_t(bytes_datos);
      bytes_capa_nivel[n] = size_t(bx_host) * by_host * tf.bytes;
      bytes_datos += bytes_capa_nivel[n] * capas * (fondo ? fondo : 1);
    }
    datos.assign(bytes_datos, 0);
    const bool mosaico = (f[0] >> 31) & 0x1;
    if (volumen && !LeerVolumen(f, tf, bloques_x, bloques_y, fondo, bloques_x_host,
                                bloques_y_host, pitch_bloques, mosaico, datos)) {
      Avisar(33, "textura fuera de la memoria");
      if (!textura.imagen.preparada) {
        ranura = 0;  // the image remains uninitialized
      }
      return;
    }
    // Base level: not read if not needed (minimum level 1; it stays zeroed and the sampler never goes below
    // 1).
    for (uint32_t c = 0; c < (volumen || !leer_base ? 0u : capas); ++c) {
      const uint64_t direccion = dir_base + c * zancada_cara;
      if (!LeerNivel(direccion, mosaico, pitch_bloques, uint64_t(std::max(pitch_bloques, bloques_x)) * tf.bytes, tf,
                     base_ox, base_oy, bloques_x, bloques_y, bloques_x_host, datos.data() + bytes_capa * c)) {
        Avisar(33, "textura fuera de la memoria");
        if (!textura.imagen.preparada) {
          ranura = 0;  // the image remains uninitialized
        }
        return;
      }
    }
    // Mip levels. Those in the packed tail share a region and are located with DesplazamientoEmpaquetado;
    // the rest, each in its own.
    for (uint32_t n = 1; n < textura.niveles; ++n) {
      const uint32_t s = nivel_empaquetado == 0 ? 0 : std::min(n, nivel_empaquetado);
      uint32_t ox = 0;
      uint32_t oy = 0;
      if (n >= nivel_empaquetado) {
        DesplazamientoEmpaquetado(ancho, alto, tf.bloque, n, ox, oy);
      }
      const uint32_t bx_invitado = (std::max(ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t by_invitado = (std::max(alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t bx_host = (std::max(textura.imagen.ancho >> n, 1u) + tf.bloque - 1) / tf.bloque;
      const uint32_t by_host = (std::max(textura.imagen.alto >> n, 1u) + tf.bloque - 1) / tf.bloque;
      for (uint32_t c = 0; c < capas; ++c) {
        const uint64_t direccion = dir_mips + regiones[s].desplazamiento + c * regiones[s].zancada;
        if (!LeerNivel(direccion, mosaico, regiones[s].pitch_bloques, regiones[s].fila_bytes, tf, ox, oy,
                       std::min(bx_invitado, bx_host), std::min(by_invitado, by_host), bx_host,
                       datos.data() + textura.desplazamiento_nivel[n] + bytes_capa_nivel[n] * c)) {
          Avisar(39, "nivel de mip fuera de la memoria: se queda a cero");
          break;
        }
      }
    }
    const auto orden = static_cast<xenos::Endian>((f[1] >> 6) & 0x3);
    // In one go (CambiarOrdenBytes), with the same guard as the fast untiling.
    if (mosaico_rapido_ > 0 && orden != xenos::Endian::kNone && (tf.unidad_orden == 2 || tf.unidad_orden == 4)) {
      // The first 200, then 1 in 64. Checking every texture, this guard never got to switch itself off (there
      // were only 1,024 textures) and cost 2.1 ms per MB on every new texture.
      ++ordenes_vistos_;
      const bool comprobar = ordenes_vistos_ <= kNivelesAComprobar || ordenes_vistos_ % kComprobarUnaDeCada == 0;
      if (comprobar) {
        comprobacion_mosaico_ = datos;
      }
      CambiarOrdenBytes(datos.data(), datos.size(), tf.unidad_orden, uint32_t(orden));
      if (comprobar) {
        ++ordenes_comprobados_;
        OrdenDeSiempre(comprobacion_mosaico_, tf.unidad_orden, orden);
        if (comprobacion_mosaico_ != datos) {
          REXLOG_ERROR("[nativo] C3: el cambio de orden rapido NO coincide (unidad {}, orden {}, {} bytes). Se "
                       "APAGAN el orden y el desenmosaicado rapidos.",
                       tf.unidad_orden, uint32_t(orden), datos.size());
          datos.swap(comprobacion_mosaico_);
          mosaico_rapido_ = 0;
        } else if (ordenes_comprobados_ == kNivelesAComprobar) {
          REXLOG_INFO("[nativo] C3: cambio de orden rapido: {} texturas comprobadas contra el de siempre, todas "
                      "iguales.",
                      ordenes_comprobados_);
        }
      }
    } else {
      OrdenDeSiempre(datos, tf.unidad_orden, orden);
    }
    if (diag_mips_ && textura.niveles > 1 && leer_base && !volumen) {
      RevisarMips(base, formato, ancho, alto, tf, textura, bytes_capa_nivel, datos);
    }
    /*
     * For a new (unprepared) texture this hash decides nothing: it is uploaded regardless. 0 is stored, and
     * the first time its memory changes it will be computed and uploaded (at most, one extra upload).
     */
    const auto inicio_huella_datos = std::chrono::steady_clock::now();
    const uint64_t huella = textura.imagen.preparada ? XXH3_64bits(datos.data(), datos.size()) : 0;
    nfsmw::esperas::g_ns_huella_datos.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                      inicio_huella_datos)
                     .count()),
        std::memory_order_relaxed);
    if (textura.imagen.preparada && huella == textura.huella) {
      textura.intervalo = std::min<uint32_t>(textura.intervalo * 2, 32);
      textura.siguiente = fotograma_ + textura.intervalo;
      valido_hasta = textura.siguiente - 1;
      return;
    }
    // Observing phase of the hash thread: what the usual path produces, to compare when collecting.
    if (comparacion_textura_ == &textura) {
      AnotarComparacionHuella(textura, datos);
    }
    textura.intervalo = 1;
    textura.siguiente = fotograma_ + 1;
    if (bc_cpu) {
      if (!nfsmw::bc::Convertir(formato_bc, datos, textura.imagen.ancho, textura.imagen.alto,
                                capas, textura.niveles, temporal_bc_, textura.desplazamiento_nivel)) {
        REXLOG_ERROR("[compatibilidad] datos BC{} invalidos: {}x{}, {} capas, {} niveles, {} bytes",
                     indice_bc, textura.imagen.ancho, textura.imagen.alto, capas, textura.niveles, datos.size());
        ranura = 0;
        return;
      }
      datos.swap(temporal_bc_);
    }
    textura.huella = huella;
    textura.datos.swap(datos);
    textura.subir = true;
    texturas_a_subir_.push_back(&textura);
    nfsmw::esperas::g_texturas_subidas.fetch_add(1, std::memory_order_relaxed);
    nfsmw::esperas::g_bytes_subidos.fetch_add(textura.datos.size(), std::memory_order_relaxed);
    bytes_subida += (textura.datos.size() + 3) & ~size_t(3);
  }

  // --- Measurement only: new textures with the content of another (nfsmw_nativo_diag_reutilizar) ---------------
  /*
   * What it is for. The game reloads the zone packs at new addresses every time it returns to a zone (in
   * the cache diagnostic, of 1,024 textures created, only 6 landed at an address already used). Since the
   * address is part of the key, a returning texture is a new texture: the image is created, bound,
   * untiled and uploaded again, even though the same image, with the same bytes, is still in the cache
   * under the old key and nothing uses it. This measures how often that happens:
   *   - how many new textures have the same content and shape as a live one (the image would be the same);
   *   - how many of those others are cold: not checked for more than kFotogramasSinUsoParaSoltar frames,
   *     which is what eviction uses to mean "unused" (a texture in use is checked at least every 39
   *     frames). Those are the duplicates the cache holds that nothing in use depends on.
   * With MB, to see how much GPU memory and upload traffic is behind them. It changes nothing: no
   * decision of the ring reads what is recorded here.
   *
   * How. vivas_por_contenido_ holds every texture with a valid raw hash under its content key: the XXH3
   * of its shape without the address (the key words without the base and mip addresses, plus the
   * VkFormat, the host size, the levels and the layers) seeded with its raw hash (the guest bytes of base
   * and mips). Two textures with the same content key have the same bytes read with the same layout: the
   * same host data. It is maintained:
   *   - when the image is created (the previous one is removed if the image is recreated);
   *   - when the raw hash is set: the usual path of PrepararTextura, and RecogerHuellas (hash thread);
   *   - when the texture is released: SoltarImagenes, which the gradual, batch and out-of-memory
   *     evictions all go through.
   * A new texture, once its first hash is known, looks at those with the same content key, at most
   * kReutilizarMirarMax: O(1) per texture, never a walk over the live ones. Removing one is O(k), with k
   * the ones with the same content (the repeats; almost always 0 or 1). Candidates are validated when
   * looked at (still under that content key, with a prepared image and no pending hash thread job), so a
   * stale entry cannot count.
   */
  static constexpr uint32_t kReutilizarMirarMax = 8;  // candidates checked per new texture
  static constexpr uint32_t kReutilizarDetalles = 8;  // lines detailing the first matches

  bool DiagReutilizar() {
    if (diag_reutilizar_ < 0) {
      diag_reutilizar_ = REXCVAR_GET(nfsmw_nativo_diag_reutilizar) ? 1 : 0;
      REXLOG_INFO("[nativo] C3: medida de texturas nuevas con el contenido de otra viva "
                  "(nfsmw_nativo_diag_reutilizar) = {}",
                  diag_reutilizar_ ? "SI (solo cuenta: no cambia nada)" : "no");
    }
    return diag_reutilizar_ > 0;
  }

  // The content key of a texture with a valid raw hash. Never 0 (0 = not recorded).
  static uint64_t ClaveContenido(const Textura& textura) {
    const uint64_t forma[4] = {textura.forma_contenido, (uint64_t(textura.imagen.formato) << 32) | textura.niveles,
                               (uint64_t(textura.imagen.ancho) << 32) | textura.imagen.alto, textura.capas};
    return XXH3_64bits_withSeed(forma, sizeof(forma), textura.huella_cruda) | 1;
  }

  // Stops counting a texture (when it is released, when its image is recreated, or before recording it
  // with another hash).
  void QuitarContenidoTextura(Textura& textura, uint64_t clave) {
    if (!textura.clave_contenido) {
      return;
    }
    const auto [desde, hasta] = vivas_por_contenido_.equal_range(textura.clave_contenido);
    for (auto it = desde; it != hasta; ++it) {
      if (it->second == clave) {
        vivas_por_contenido_.erase(it);
        break;
      }
    }
    if (vivas_por_contenido_.find(textura.clave_contenido) == vivas_por_contenido_.end() && contenidos_distintos_) {
      --contenidos_distintos_;
    }
    textura.clave_contenido = 0;
  }

  // With the raw hash just set. If the texture is new, counts whether another live one has the same
  // content and shape, and whether that other one is cold. Then records it under its content key. Ring
  // thread only.
  void AnotarContenidoTextura(Textura& textura, uint64_t clave) {
    if (!DiagReutilizar()) {
      return;
    }
    QuitarContenidoTextura(textura, clave);
    const uint64_t contenido = ClaveContenido(textura);
    if (textura.contenido_por_medir) {
      textura.contenido_por_medir = false;
      ++reutilizar_nuevas_;
      reutilizar_bytes_nuevas_ += textura.bytes;
      const Textura* igual = nullptr;
      const Textura* fria = nullptr;
      uint32_t miradas = 0;
      const auto [desde, hasta] = vivas_por_contenido_.equal_range(contenido);
      for (auto it = desde; it != hasta; ++it) {
        if (miradas++ >= kReutilizarMirarMax) {
          ++reutilizar_sin_mirar_;
          break;
        }
        const auto otra = texturas_.find(it->second);
        if (otra == texturas_.end() || &otra->second == &textura) {
          continue;
        }
        const Textura& t = otra->second;
        if (t.clave_contenido != contenido || t.imagen.imagen == VK_NULL_HANDLE || !t.imagen.preparada ||
            t.huella_trabajo) {
          continue;
        }
        igual = &t;
        if (t.fotograma != UINT64_MAX && t.fotograma + kFotogramasSinUsoParaSoltar < fotograma_) {
          fria = &t;
          break;  // one cold one is enough
        }
      }
      if (igual) {
        ++reutilizar_coinciden_;
        reutilizar_bytes_coinciden_ += textura.bytes;
      }
      if (fria) {
        ++reutilizar_frias_;
        reutilizar_bytes_frias_ += textura.bytes;
      }
      if (igual && reutilizar_detalles_ < kReutilizarDetalles) {
        ++reutilizar_detalles_;
        const Textura& t = fria ? *fria : *igual;
        NFSMW_INFORME_ANILLO("[nativo] C3 reutilizar por contenido (build 186, solo medida): la textura nueva {:08X} "
                             "({}x{}, VkFormat {}, {} niveles, {} capas, {} KB) tiene el mismo contenido y forma que "
                             "la {:08X}, {}",
                             textura.direccion, textura.imagen.ancho, textura.imagen.alto,
                             uint32_t(textura.imagen.formato), textura.niveles, textura.capas, textura.bytes >> 10,
                             t.direccion,
                             fria ? fmt::format("sin comprobarse desde hace {} fotogramas", fotograma_ - t.fotograma)
                                  : std::string("que sigue en uso"));
      }
    }
    if (vivas_por_contenido_.find(contenido) == vivas_por_contenido_.end()) {
      ++contenidos_distintos_;
    }
    vivas_por_contenido_.emplace(contenido, clave);
    textura.clave_contenido = contenido;
  }

  // Every 10 s, if there were new textures with a hash. The cache snapshot (recorded, distinct contents)
  // is O(1).
  void InformeReutilizar() {
    if (diag_reutilizar_ <= 0) {
      return;
    }
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - informe_reutilizar_ < std::chrono::seconds(10)) {
      return;
    }
    informe_reutilizar_ = ahora;
    if (reutilizar_nuevas_) {
      const uint64_t apuntadas = uint64_t(vivas_por_contenido_.size());
      NFSMW_INFORME_ANILLO(
          "[nativo] C3 reutilizar por contenido (build 186, solo medida), ultimos 10 s: {} texturas nuevas con huella "
          "({:.1f} MB); {} con el mismo contenido y forma que otra viva ({:.1f} MB), {} de ellas con una sin "
          "comprobarse desde hace mas de {} fotogramas ({:.1f} MB); {} con mas de {} iguales sin mirar enteras | en la "
          "cache: {} texturas con huella en {} contenidos distintos ({} repetidas)",
          reutilizar_nuevas_, double(reutilizar_bytes_nuevas_) / 1048576.0, reutilizar_coinciden_,
          double(reutilizar_bytes_coinciden_) / 1048576.0, reutilizar_frias_, kFotogramasSinUsoParaSoltar,
          double(reutilizar_bytes_frias_) / 1048576.0, reutilizar_sin_mirar_, kReutilizarMirarMax, apuntadas,
          contenidos_distintos_, apuntadas - std::min(apuntadas, contenidos_distintos_));
    }
    reutilizar_nuevas_ = 0;
    reutilizar_coinciden_ = 0;
    reutilizar_frias_ = 0;
    reutilizar_sin_mirar_ = 0;
    reutilizar_bytes_nuevas_ = 0;
    reutilizar_bytes_coinciden_ = 0;
    reutilizar_bytes_frias_ = 0;
  }

  // Every 30 s, one line with what hash sampling has done (see PrepararTextura). The ring thread writes
  // it, but it is a single short line every 30 s.
  void InformeMuestreo(std::chrono::steady_clock::time_point ahora) {
    if (ahora - informe_muestreo_ < std::chrono::seconds(30)) {
      return;
    }
    if (informe_muestreo_ != std::chrono::steady_clock::time_point{}) {
      NFSMW_INFORME_ANILLO("[nativo] C3 muestreo de huellas: {} recomprobaciones resueltas con la muestra ({:.1f} MB leidos y "
                  "{:.1f} MB sin leer), {} con la muestra cambiada y {} con las dos huellas desde la linea anterior; "
                  "{} con las dos desde el principio",
                  muestras_aciertos_, double(bytes_muestra_) / 1048576.0,
                  double(bytes_ahorrados_muestra_) / 1048576.0, muestras_distintas_, muestras_con_las_dos_,
                  muestras_comprobadas_);
    }
    informe_muestreo_ = ahora;
    muestras_aciertos_ = 0;
    muestras_distintas_ = 0;
    muestras_con_las_dos_ = 0;
    bytes_muestra_ = 0;
    bytes_ahorrados_muestra_ = 0;
  }

  // nfsmw_nativo_diag_mips: average color of each level of the first layer against the base's, for
  // DXT1/3/5 (average of the two colors of each block, in RGB565) and 8888. Only levels of 2x2 blocks or
  // more: the small ones are too little data for an average. Counts the textures with any level more than
  // 32/255 away from the base in any channel and logs the first 12 with their averages.
  void RevisarMips(uint32_t base, uint32_t formato, uint32_t ancho, uint32_t alto, const FormatoTextura& tf,
                   const Textura& textura, const std::array<size_t, 16>& bytes_capa_nivel,
                   const std::vector<uint8_t>& datos) {
    const bool dxt = tf.formato == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || tf.formato == VK_FORMAT_BC2_UNORM_BLOCK ||
                     tf.formato == VK_FORMAT_BC3_UNORM_BLOCK;
    if (!dxt && tf.formato != VK_FORMAT_R8G8B8A8_UNORM) {
      return;
    }
    const auto media = [&](uint32_t n, std::array<double, 3>& rgb) {
      const size_t inicio = textura.desplazamiento_nivel[n];
      const size_t bytes = bytes_capa_nivel[n];
      if (inicio + bytes > datos.size() || bytes < 4u * tf.bytes) {
        return false;
      }
      rgb = {0.0, 0.0, 0.0};
      size_t cuenta = 0;
      for (size_t i = 0; i + tf.bytes <= bytes; i += tf.bytes) {
        const uint8_t* p = datos.data() + inicio + i;
        if (dxt) {
          const uint8_t* color = tf.formato == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? p : p + 8;
          for (uint32_t k = 0; k < 2; ++k) {
            const uint16_t v = uint16_t(color[2 * k] | (color[2 * k + 1] << 8));
            rgb[0] += ((v >> 11) & 31) * (255.0 / 31.0);
            rgb[1] += ((v >> 5) & 63) * (255.0 / 63.0);
            rgb[2] += (v & 31) * (255.0 / 31.0);
          }
          cuenta += 2;
        } else {
          rgb[0] += p[0];
          rgb[1] += p[1];
          rgb[2] += p[2];
          cuenta += 1;
        }
      }
      for (double& c : rgb) {
        c /= double(cuenta);
      }
      return true;
    };
    std::array<double, 3> base_rgb{};
    if (!media(0, base_rgb)) {
      return;
    }
    ++mips_revisadas_;
    std::string niveles = fmt::format("[0]={:.0f}/{:.0f}/{:.0f}", base_rgb[0], base_rgb[1], base_rgb[2]);
    double peor = 0.0;
    for (uint32_t n = 1; n < textura.niveles; ++n) {
      std::array<double, 3> rgb{};
      if (!media(n, rgb)) {
        break;
      }
      for (uint32_t c = 0; c < 3; ++c) {
        peor = std::max(peor, std::abs(rgb[c] - base_rgb[c]));
      }
      niveles += fmt::format(" [{}]={:.0f}/{:.0f}/{:.0f}", n, rgb[0], rgb[1], rgb[2]);
    }
    if (peor > 32.0) {
      if (++mips_raras_ <= 12) {
        REXLOG_WARN("[nativo] C3 diag mips: textura {:08X} {}x{} formato {} con {} niveles: un nivel se aleja {:.0f} "
                    "de la base; medias RGB {}",
                    base, ancho, alto, formato, textura.niveles, peor, niveles);
      }
    }
    if (mips_revisadas_ % 200 == 0) {
      REXLOG_INFO("[nativo] C3 diag mips: {} texturas revisadas, {} con algun nivel a mas de 32/255 de la base",
                  mips_revisadas_, mips_raras_);
    }
  }

  // The blocks of one level (one layer) of a 2D texture or cubemap, from block (ox, oy) of its guest
  // region (the packed tail or the base of a small texture do not start at 0), to the host destination
  // bx_host blocks wide. Linear: fila_bytes per row; tiled: pitch in blocks (GetTiledOffset2D). false if
  // it goes past memory.
  bool LeerNivel(uint64_t direccion, bool mosaico, uint32_t pitch_bloques, uint64_t fila_bytes,
                 const FormatoTextura& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                 uint8_t* destino) {
    if (!bx || !by) {
      return true;
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t extremo =
        mosaico ? uint64_t(std::max<int64_t>(DesplazamientoMosaico2D(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u),
                                                                     pitch_bloques, log2),
                                             0))
                : fila_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (direccion + extremo > kMemoriaFisica) {
      return false;
    }
    const uint8_t* origen = memoria_->TranslatePhysical(uint32_t(direccion));
    if (!mosaico) {
      for (uint32_t y = 0; y < by; ++y) {
        std::memcpy(destino + size_t(y) * bx_host * tf.bytes,
                    origen + uint64_t(oy + y) * fila_bytes + uint64_t(ox) * tf.bytes, size_t(bx) * tf.bytes);
      }
      return true;
    }
    // The fast untiling (DesenmosaicarNivel) when the block is 1, 2, 4, 8 or 16 bytes.
    if (mosaico_rapido_ < 0) {
      mosaico_rapido_ = REXCVAR_GET(nfsmw_nativo_mosaico_rapido) ? 1 : 0;
      REXLOG_INFO("[nativo] C3: desenmosaicado rapido (nfsmw_nativo_mosaico_rapido) = {}",
                  mosaico_rapido_ ? "SI, comprobando los primeros niveles contra el de siempre" : "no");
    }
    if (mosaico_rapido_ && tf.bytes == (1u << log2)) {
      switch (log2) {
        case 0: DesenmosaicarNivel<0>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
        case 1: DesenmosaicarNivel<1>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
        case 2: DesenmosaicarNivel<2>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
        case 3: DesenmosaicarNivel<3>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
        default: DesenmosaicarNivel<4>(origen, pitch_bloques, ox, oy, bx, by, bx_host, destino); break;
      }
      /*
       * Guard: the first kNivelesAComprobar levels are repeated on the usual path and compared byte by byte.
       * If one differs, the usual result is kept, a warning is logged and the fast path is off for the rest
       * of the session.
       */
      // The first 200, then 1 in 64 (checking every level, the guard cost 4.2 ms per MB).
      ++niveles_vistos_;
      if (niveles_vistos_ <= kNivelesAComprobar || niveles_vistos_ % kComprobarUnaDeCada == 0) {
        ++niveles_comprobados_;
        comprobacion_mosaico_.assign(size_t(by) * bx_host * tf.bytes, 0);
        for (uint32_t y = 0; y < by; ++y) {
          std::memcpy(comprobacion_mosaico_.data() + size_t(y) * bx_host * tf.bytes,
                      destino + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes);
        }
        LeerNivelMosaicoDeSiempre(origen, pitch_bloques, log2, tf.bytes, ox, oy, bx, by, bx_host, destino);
        bool iguales = true;
        for (uint32_t y = 0; y < by && iguales; ++y) {
          iguales = std::memcmp(comprobacion_mosaico_.data() + size_t(y) * bx_host * tf.bytes,
                                destino + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes) == 0;
        }
        if (!iguales) {
          REXLOG_ERROR("[nativo] C3: el desenmosaicado rapido NO coincide ({}x{} bloques de {} bytes, pitch {}, "
                       "desde {},{}). Se APAGA y se usa el de siempre.",
                       bx, by, tf.bytes, pitch_bloques, ox, oy);
          mosaico_rapido_ = 0;
        } else if (niveles_comprobados_ == kNivelesAComprobar) {
          REXLOG_INFO("[nativo] C3: desenmosaicado rapido: {} niveles comprobados contra el de siempre, todos "
                      "iguales. Sigue el rapido.",
                      niveles_comprobados_);
        }
      }
      return true;
    }
    LeerNivelMosaicoDeSiempre(origen, pitch_bloques, log2, tf.bytes, ox, oy, bx, by, bx_host, destino);
    return true;
  }

  // The original byte swap, word by word.
  static void OrdenDeSiempre(std::vector<uint8_t>& datos, uint32_t unidad, xenos::Endian orden) {
    if (unidad == 2 && orden != xenos::Endian::kNone) {
      for (size_t i = 0; i + 1 < datos.size(); i += 2) {
        uint16_t v;
        std::memcpy(&v, datos.data() + i, 2);
        v = xenos::GpuSwap(v, orden);
        std::memcpy(datos.data() + i, &v, 2);
      }
    } else if (unidad == 4 && orden != xenos::Endian::kNone) {
      for (size_t i = 0; i + 3 < datos.size(); i += 4) {
        uint32_t v;
        std::memcpy(&v, datos.data() + i, 4);
        v = xenos::GpuSwap(v, orden);
        std::memcpy(datos.data() + i, &v, 4);
      }
    }
  }

  // The original untiling, block by block.
  static void LeerNivelMosaicoDeSiempre(const uint8_t* origen, uint32_t pitch_bloques, uint32_t log2, uint32_t bytes,
                                        uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                                        uint8_t* destino) {
    for (uint32_t y = 0; y < by; ++y) {
      for (uint32_t x = 0; x < bx; ++x) {
        const int32_t desplazamiento =
            DesplazamientoMosaico2D(int32_t(ox + x), int32_t(oy + y), pitch_bloques, log2);
        std::memcpy(destino + (size_t(y) * bx_host + x) * bytes, origen + desplazamiento, bytes);
      }
    }
  }

  // Base level of a 3D texture, slice by slice (z, then y, then x), in the order vkCmdCopyBufferToImage
  // uploads it. Tiled: GetTiledOffset3D; linear: consecutive slices with rows and columns aligned to 32
  // blocks (GetGuestTextureLayout).
  bool LeerVolumen(const uint32_t* f, const FormatoTextura& tf, uint32_t bloques_x,
                   uint32_t bloques_y, uint32_t fondo, uint32_t bloques_x_host,
                   uint32_t bloques_y_host, uint32_t pitch_bloques, bool mosaico,
                   std::vector<uint8_t>& datos) {
    const uint64_t direccion = uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF;
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                             : tf.bytes >= 2 ? 1 : 0;
    const uint64_t fila = uint64_t((pitch_bloques + 31) & ~uint32_t(31)) * tf.bytes;
    const uint64_t corte = fila * ((bloques_y + 31) & ~uint32_t(31));
    const uint64_t extremo =
        mosaico ? uint64_t(std::max<int64_t>(
                      DesplazamientoMosaico3D(int32_t((bloques_x + 31) & ~31u),
                                              int32_t((bloques_y + 31) & ~31u),
                                              int32_t((fondo + 3) & ~3u), pitch_bloques,
                                              bloques_y, log2),
                      0))
                : corte * fondo;
    if (direccion + extremo > kMemoriaFisica) {
      return false;
    }
    const uint8_t* origen = memoria_->TranslatePhysical(uint32_t(direccion));
    const size_t bytes_corte = size_t(bloques_x_host) * bloques_y_host * tf.bytes;
    for (uint32_t z = 0; z < fondo; ++z) {
      for (uint32_t y = 0; y < bloques_y; ++y) {
        for (uint32_t x = 0; x < bloques_x; ++x) {
          const uint64_t desplazamiento =
              mosaico ? uint64_t(DesplazamientoMosaico3D(int32_t(x), int32_t(y), int32_t(z),
                                                         pitch_bloques, bloques_y, log2))
                      : z * corte + y * fila + uint64_t(x) * tf.bytes;
          std::memcpy(datos.data() + bytes_corte * z + (size_t(y) * bloques_x_host + x) * tf.bytes,
                      origen + desplazamiento, tf.bytes);
        }
      }
    }
    return true;
  }

  // A texture's VkImageCreateInfo, factored out so CrearTextura and CrearTexturaEnHilo create exactly the
  // same image.
  static VkImageCreateInfo InfoImagenTextura(VkFormat formato, uint32_t ancho, uint32_t alto, uint32_t capas,
                                             uint32_t fondo, uint32_t niveles) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = fondo ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.flags = capas == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.format = formato;
    info.extent = {ancho, alto, fondo ? fondo : 1};
    info.mipLevels = std::max(niveles, 1u);
    info.arrayLayers = capas;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
  }

  // `emergencia` = false only from RecogerEnlaces. Half the cache cannot be released there: that waits
  // for the GPU by submitting the work, and we are inside AntesDeEnviar. Out of memory: false, and the
  // texture is recreated later.
  bool CrearTextura(ImagenNativa& imagen, VkFormat formato, uint32_t ancho, uint32_t alto,
                    uint32_t capas = 1, uint32_t fondo = 0, uint32_t niveles = 1, bool emergencia = true) {
    const VkImageCreateInfo info = InfoImagenTextura(formato, ancho, alto, capas, fondo, niveles);
    /*
     * The pool first, and the usual path only if it does not fit.
     *
     * A dedicated allocation per texture costs 1.9 ms of CPU on Horizon, measured inside the ioctls:
     * nvMapCreate 422 us, plus two GPU address reservations (127 us each) and two mappings (626 us each).
     * There are two of each because the dedicated allocation makes its own address and mapping, and then
     * nvk_image_plane_bind makes others for the plane. And the only thing the dedicated allocation buys is
     * compression, which our textures never use (nvk_image.c:843-846 returns false with
     * SAMPLED|TRANSFER_DST). So we paid double and got nothing.
     *
     * Sub-allocating from large slabs removes the allocation's address and mapping: 1.9 -> 0.75 ms. For a
     * burst of 15 textures in one frame, from 28.5 to 11.3 ms. That is what shows when entering a new zone
     * of the map.
     *
     * It does not change a single pixel: same tiling, same format, same pte_kind. Binding at an offset
     * keeps the block-linear layout (nvk_image.c:1700-1710) and is the normal path of any sub-allocator in
     * NVK.
     */
    bool reservada = false;
    if (pool_texturas_.Activo()) {
      VkImage imagen_pool = VK_NULL_HANDLE;
      if (dfn_.vkCreateImage(device_, &info, nullptr, &imagen_pool) == VK_SUCCESS) {
        VkMemoryRequirements req{};
        dfn_.vkGetImageMemoryRequirements(device_, imagen_pool, &req);
        VkDeviceMemory bloque = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint32_t id_bloque = 0xFFFFFFFFu;
        if (pool_texturas_.Reservar(req, bloque, offset, id_bloque) &&
            dfn_.vkBindImageMemory(device_, imagen_pool, bloque, offset) == VK_SUCCESS) {
          imagen.imagen = imagen_pool;
          imagen.memoria = VK_NULL_HANDLE;  // from the pool: not freed on its own
          imagen.pool_bloque = id_bloque;
          reservada = true;
        } else {
          if (id_bloque != 0xFFFFFFFFu) {
            pool_texturas_.Liberar(id_bloque);
          }
          dfn_.vkDestroyImage(device_, imagen_pool, nullptr);
          pool_texturas_.AnotarDedicada();
        }
      }
    }
    if (!reservada) {
      imagen.pool_bloque = 0xFFFFFFFFu;
      reservada = rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          dispositivo_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, imagen.imagen,
          imagen.memoria);
    }
    if (reservada && prueba_sin_memoria_cada_ > 0 &&
        ++reservas_de_textura_ % uint64_t(prueba_sin_memoria_cada_) == 0) {
      // Test: undo the successful allocation and continue as if it had failed.
      DestruirImagen(imagen);
      reservada = false;
      REXLOG_INFO("[nativo] C3: prueba, se finge que falta memoria en la reserva {}", reservas_de_textura_);
    }
    if (!reservada) {
      // Out of GPU memory. Instead of giving up (which ends in a black screen), half the cache is released
      // with the GPU idle and the allocation is retried once.
      // The retry always takes the dedicated path, on purpose. Getting here means memory is short, and the
      // dedicated path is the one that can ask the system for more; the pool only hands out what it already
      // has. pool_bloque is left invalid so DestruirImagen does not touch the pool.
      imagen.pool_bloque = 0xFFFFFFFFu;
      if (!emergencia || !SoltarTexturasPorFaltaDeMemoria() ||
          !rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              dispositivo_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, imagen.imagen,
              imagen.memoria)) {
        REXLOG_ERROR("[nativo] C3: sin memoria en la GPU para una textura de {}x{} ({} niveles, {} capas) y "
                     "{}; quedan {} texturas y {} MB",
                     ancho, alto, std::max(niveles, 1u), capas,
                     emergencia ? "soltar la cache no ha bastado"
                                : "sin soltar la cache (desde RecogerEnlaces; se recrea en su proxima comprobacion)",
                     texturas_.size(), bytes_texturas_ >> 20);
        return false;
      }
    }
    imagen.ancho = ancho;
    imagen.alto = alto;
    imagen.formato = formato;
    imagen.preparada = false;
    return true;
  }

  bool SubirTextura(Textura& textura) {
    const VkCommandBuffer subida = contexto_->ComandosSubida();
    if (!subida || !textura.subir) {
      return subida != VK_NULL_HANDLE;
    }
    VkDeviceSize offset;
    /*
     * If the hash thread prepares it, its data does not exist yet: its space is reserved (same size, same
     * point) and the thread writes it there; RecogerHuellas waits for it before submitting. The barrier
     * and the copy are recorded below as usual: the GPU does not read the upload buffer until
     * vkQueueSubmit.
     */
    if (textura.huella_trabajo && textura.huella_trabajo != kTrabajoHuellaPublicado) {
      const size_t bytes = BytesHuellaPlaneada(textura);
      Reservar(bytes, 16, offset);  // BC: offset multiple of the block
      PublicarHuella(textura, subida_datos_ + offset, bytes);
    } else {
      Reservar(textura.datos.size(), 16, offset);  // BC: offset multiple of the block
      std::memcpy(subida_datos_ + offset, textura.datos.data(), textura.datos.size());
    }
    /*
     * If its vkBindImageMemory is still on the bind thread, neither the barrier nor the copy can be
     * recorded for the image (it has no memory). The data is already in the upload buffer; the barrier and
     * the copy are recorded in RecogerEnlaces, in this same upload buffer before it is closed
     * (AntesDeEnviar).
     */
    if (textura.en_vuelo && !AplazarCopia(textura, offset, subida)) {
      RecogerEnlaces(true);  // inconsistent index (AplazarCopia already logged it and switched off): collect everything here
    }
    if (!textura.en_vuelo && textura.imagen.imagen != VK_NULL_HANDLE) {
      if (!textura.imagen.preparada) {
        Barrera(subida, textura.imagen.imagen, textura.capas);
        textura.imagen.preparada = true;
      }
      GrabarCopiaTextura(subida, textura, offset);
    }
    textura.subir = false;
    /*
     * The copy of the pixels is no longer needed. They are already in the upload buffer, and to know
     * whether the texture changes its hashes are kept, not the bytes. Keeping the copy made the texture
     * cache take the same space again in CPU RAM (150-680 MB). That once ended in std::bad_alloc in the
     * main menu, with the cache at 682 MB.
     */
    // The larger buffer is kept as temporal_ (so the next new texture does not ask the system for memory
    // again or touch fresh pages); the other is released.
    if (textura.datos.capacity() > temporal_.capacity()) {
      temporal_.swap(textura.datos);
    }
    std::vector<uint8_t>().swap(textura.datos);
    ++subidas_textura_;
    return true;
  }

  // One range per mip level (with all its layers). Factored out for SubirTextura and for the deferred
  // copies of RecogerEnlaces.
  void GrabarCopiaTextura(VkCommandBuffer subida, const Textura& textura, VkDeviceSize offset) {
    std::array<VkBufferImageCopy, 16> copias{};
    for (uint32_t n = 0; n < textura.niveles; ++n) {
      VkBufferImageCopy& copia = copias[n];
      copia.bufferOffset = offset + textura.desplazamiento_nivel[n];
      copia.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, n, 0, textura.capas};
      copia.imageExtent = {std::max(textura.imagen.ancho >> n, 1u), std::max(textura.imagen.alto >> n, 1u),
                           textura.fondo ? textura.fondo : 1};
    }
    dfn_.vkCmdCopyBufferToImage(subida, subida_, textura.imagen.imagen, VK_IMAGE_LAYOUT_GENERAL,
                                textura.niveles, copias.data());
  }

  // A texture's view, the same for RanuraVista and for the deferred views of RecogerEnlaces.
  VkResult CrearVistaTextura(VkImage imagen, VkFormat formato, uint32_t swizzle, uint16_t swizzle_host, uint32_t monton,
                             VkImageView& vista) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = imagen;
    info.viewType = monton == 2   ? VK_IMAGE_VIEW_TYPE_CUBE
                    : monton == 1 ? VK_IMAGE_VIEW_TYPE_3D
                                  : VK_IMAGE_VIEW_TYPE_2D;
    info.format = formato;
    info.components = MapeoComponentes(swizzle, swizzle_host);
    info.subresourceRange = {EsProfundidad(formato)
                                 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                 : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                             0, VK_REMAINING_MIP_LEVELS, 0, monton == 2 ? 6u : 1u};  // with its mips
    return dfn_.vkCreateImageView(device_, &info, nullptr, &vista);
  }

  // The vistas_ key (image, heap and both swizzles), factored out for RecogerEnlaces.
  static uint64_t ClaveVista(VkImage imagen, uint32_t swizzle, uint16_t swizzle_host, uint32_t monton) {
    return XXH3_64bits_withSeed(&imagen, sizeof(imagen),
                                (uint64_t(monton) << 40) | (uint64_t(swizzle) << 16) | swizzle_host);
  }

  // Heap slot (0 for 2D textures, 2 for cubemaps) for a view of the image with that swizzle.
  uint32_t RanuraVista(VkImage imagen, VkFormat formato, uint32_t swizzle, uint16_t swizzle_host,
                       uint32_t monton = 0) {
    const uint64_t clave = ClaveVista(imagen, swizzle, swizzle_host, monton);  // The same key
    if (const auto it = vistas_.find(clave); it != vistas_.end()) {
      return it->second.ranura;
    }
    // An image whose vkBindImageMemory is still on the bind thread. The view carries the image's GPU
    // address, which comes from the bind: the slot is reserved now, and the view is created and written in
    // RecogerEnlaces, before submission (UPDATE_AFTER_BIND descriptors).
    if (!imagenes_en_vuelo_.empty() && imagenes_en_vuelo_.count(imagen)) {
      return RanuraVistaEnVuelo(clave, imagen, formato, swizzle, swizzle_host, monton);
    }
    Vista vista;
    vista.imagen = imagen;
    vista.monton = monton;
    if (CrearVistaTextura(imagen, formato, swizzle, swizzle_host, monton, vista.vista) != VK_SUCCESS) {
      Avisar(34, "no se pudo crear la vista de una textura");
      return 0;
    }
    vista.ranura = ReservarRanura(monton);
    if (!vista.ranura) {
      dfn_.vkDestroyImageView(device_, vista.vista, nullptr);
      Avisar(35, "monton de texturas lleno");
      return 0;
    }
    EscribirImagen(monton, vista.ranura, vista.vista);
    vistas_.emplace(clave, vista);
    vistas_por_imagen_[imagen].push_back(clave);
    return vista.ranura;
  }

  // point: no filtering (depth textures).
  uint32_t RanuraSampler(const uint32_t* f, bool puntual = false) {
    // Diagnostic: filters the game requests that this renderer does not apply (anisotropy, mip bias and
    // maximum mip level; the bias is not even part of the key). Each new value is logged once.
    {
      const uint32_t aniso = (f[3] >> 25) & 0x7;
      const int32_t sesgo = int32_t(f[4] << 10) >> 22;  // lod_bias: 10 bits con signo, 5 fraccionarios
      const uint32_t mip_max = (f[4] >> 6) & 0xF;
      const uint32_t indice_sesgo = uint32_t(sesgo + 512);
      if (!((aniso_vistos_ >> aniso) & 1u) || !sesgos_vistos_[indice_sesgo] || !((mip_max_vistos_ >> mip_max) & 1u)) {
        aniso_vistos_ |= 1u << aniso;
        sesgos_vistos_[indice_sesgo] = true;
        mip_max_vistos_ |= 1u << mip_max;
        REXLOG_INFO("[nativo] C4 filtros pedidos por el juego (valor nuevo): anisotropico {}, sesgo de mip {:.3f}, "
                    "nivel maximo de mip {}, filtros mag/min/mip {}/{}/{}{}",
                    aniso, float(sesgo) / 32.0f, mip_max, (f[3] >> 19) & 0x3, (f[3] >> 21) & 0x3,
                    (f[3] >> 23) & 0x3, puntual ? " (muestreo puntual)" : "");
      }
    }
    /*
     * Trilinear -> bilinear, for measurement only (scene pass analysis).
     *
     * The game requests mag/min/mip filters 1/1/1, i.e. mipmapMode LINEAR. On Maxwell (GM20B) a trilinear
     * sample takes the texture unit two cycles (two bilinear ones) and a bilinear sample only one: with the
     * filter between levels at NEAREST the TMU's work is halved without touching a single ALU instruction.
     * It is the only lever that separates the two:
     *   - if the scene drops by ~half of the TMU's share, sampling dominates;
     *   - if it does not move, the ALU dominates and the TMU has headroom.
     * A mip level jump is visible on receding surfaces (the PS2 "shimmer"). For measurement only. Off by
     * default: at 0 this is two bool comparisons per new sampler (samplers are cached) and not one more
     * instruction on the draw path.
     *
     * The modified mip field is part of the sampler key, so toggling at runtime creates two different
     * samplers and nothing has to be invalidated: the alternation works.
     */
    uint32_t campo_filtros = (f[3] >> 19) & 0xFFF;  // mag in 0-1, min in 2-3, mip in 4-5
    if (mip_puntual_prueba_ && ((campo_filtros >> 4) & 0x3) == 1) {
      campo_filtros &= ~(0x3u << 4);  // LINEAR -> NEAREST between mip levels
    }
    // With nfsmw_nativo_diag_mip_minimo the minimum level rises (without passing the maximum) and goes into
    // the key that way: each value has its own samplers, and going back to 0 reuses the usual ones.
    uint32_t campo_mips = (f[4] >> 2) & 0xFF;  // minimum in 0-3, maximum in 4-7
    if (diag_mip_minimo_ != 0) {
      const uint32_t minimo = campo_mips & 0xF;
      const uint32_t maximo = (campo_mips >> 4) & 0xF;
      if (maximo > minimo) {
        campo_mips = (campo_mips & 0xF0) | std::min(maximo, minimo + diag_mip_minimo_);
      }
    }
    const uint32_t clave = ((f[0] >> 10) & 0x1FF) | (campo_filtros << 9) |
                           (campo_mips << 21) | ((f[5] & 0x3) << 29) |
                           (puntual ? 0x80000000u : 0u);
    if (const auto it = samplers_.find(clave); it != samplers_.end()) {
      return it->second.second;
    }
    static constexpr VkSamplerAddressMode kModos[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
    const bool espejo = dispositivo_->properties().samplerMirrorClampToEdge;
    const auto modo = [&](uint32_t valor) {
      const VkSamplerAddressMode m = kModos[valor & 0x7];
      return m == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !espejo
                 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                 : m;
    };
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter =
        ((f[3] >> 19) & 0x3) == 1 && !puntual ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter =
        ((f[3] >> 21) & 0x3) == 1 && !puntual ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const uint32_t mip = (campo_filtros >> 4) & 0x3;  // already includes the point mip test
    info.mipmapMode =
        mip == 1 && !puntual ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = modo(f[0] >> 10);
    info.addressModeV = modo(f[0] >> 13);
    info.addressModeW = modo(f[0] >> 16);
    info.minLod = float(campo_mips & 0xF);  // with the mip diagnostic if it is enabled
    // With real mips, the fetch constant's maximum level also limits (like the emulation's sampler);
    // single-level textures stay as before.
    info.maxLod = mip == 2 ? info.minLod + 0.25f : std::max(info.minLod, float((f[4] >> 6) & 0xF));
    info.borderColor = (f[5] & 0x3) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                         : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    // nfsmw_nativo_anisotropico. Only linear samplers with mips (maxLod above minLod); the value is read at
    // start-up, so it does not need to be in the key: every sampler is created with the same one.
    {
      static const uint32_t aniso_pedido = uint32_t(std::max(0, int32_t(REXCVAR_GET(nfsmw_nativo_anisotropico))));
      const auto& propiedades = dispositivo_->properties();
      if (aniso_pedido > 1 && propiedades.samplerAnisotropy && info.minFilter == VK_FILTER_LINEAR &&
          info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR && info.maxLod > info.minLod) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::max(1.0f, std::min(float(aniso_pedido), propiedades.maxSamplerAnisotropy));
        if (!aniso_anotado_) {
          aniso_anotado_ = true;
          REXLOG_INFO("[nativo] C4 filtrado anisotropico {} en los samplers lineales con mips (pedido {}, tope del "
                      "dispositivo {}); los de un solo nivel y los puntuales, sin cambios",
                      info.maxAnisotropy, aniso_pedido, propiedades.maxSamplerAnisotropy);
        }
      }
    }
    VkSampler sampler;
    if (dfn_.vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS) {
      Avisar(36, "no se pudo crear un sampler");
      return 0;
    }
    const uint32_t ranura = ReservarRanura(3);
    if (!ranura) {
      dfn_.vkDestroySampler(device_, sampler, nullptr);
      Avisar(37, "monton de samplers lleno");
      return 0;
    }
    EscribirSampler(ranura, sampler);
    samplers_.emplace(clave, std::make_pair(sampler, ranura));
    return ranura;
  }

  // Category clipped to 1x1 (0 = none), with its alternating test.
  uint32_t TijeraDePrueba() {
    const int32_t categoria = REXCVAR_GET(nfsmw_nativo_prueba_tijera);
    if (categoria <= 0) {
      return 0;
    }
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_prueba_tijera_alternar_s);
    if (alternar <= 0) {
      return uint32_t(categoria);
    }
    const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - inicio_alternancia_ps_)
                              .count();
    const bool con_tijera = (segundos / alternar) % 2 == 1;
    if (con_tijera != alternancia_tijera_anotada_) {
      alternancia_tijera_anotada_ = con_tijera;
      REXLOG_INFO("[nativo] C2 tijera de prueba: {}", con_tijera ? "recortando a 1x1" : "normal");
    }
    return con_tijera ? uint32_t(categoria) : 0;
  }

  /*
   * The ring's verdict for vegetation filtered in the game (nfsmw_d3d_vegetacion_juego). If the draw
   * carries the game's verdict (the kVeg* flags of its record), the ring's own is computed from this
   * draw's registers and PS: the criterion of Dibujar's early discard, minus the earlier exits that have
   * no side effects (primitive type, vertex input, diagnostics) and do not draw either. It goes to the
   * guard (AnotarVegetacionAnillo) and, if Dibujar reaches its early discard, it must have decided the
   * same (AnotarVegetacionModelo otherwise).
   */
  void VeredictoVegetacion(const PeticionDibujo& p) {
    vegetacion_calculada_ = false;
    if (!(p.vegetacion_juego & kVegHay)) {
      return;
    }
    const uint32_t* r = p.registros;
    DetalleVegetacion a;
    if (r) {
      a.modo = r[gr::XE_GPU_REG_RB_MODECONTROL];
      a.mascara = r[gr::XE_GPU_REG_RB_COLOR_MASK];
      a.control = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    }
    a.vs = p.vs ? int32_t(p.vs->numero) : -1;
    a.ps = p.ps ? int32_t(p.ps->numero) : -1;
    if (p.ps) {
      a.salidas = p.ps->salidas;
      a.descarta = p.ps->descarta;
    }
    if (r && p.vs && p.ps && (a.modo & 0x7) == uint32_t(xenos::EdramMode::kColorDepth)) {
      bool color = false;
      for (uint32_t i = 0; i < 4; ++i) {
        color = color || (((a.mascara >> (i * 4)) & 0xF) && ((p.ps->salidas >> i) & 0x1));
      }
      const bool prueba_alfa = ((a.control >> 3) & 0x1) && (a.control & 0x7) != 7;
      a.estructura = !color && (prueba_alfa || p.ps->descarta || (p.ps->salidas & 0x10));
    }
    a.ajustes = (cvars_por_fotograma_ ? ps_solo_alfa_fotograma_ : PsSoloAlfa()) && sin_vegetacion_;
    a.oclusion = oclusion_abierta_;
    vegetacion_anillo_ = a.estructura && a.ajustes && !a.oclusion;
    vegetacion_calculada_ = true;
    vegetacion_banderas_ = p.vegetacion_juego;
    vegetacion_detalle_ = a;
    AnotarVegetacionAnillo(p.vegetacion_juego, a);
  }

  // The setting, or its alternating test.
  bool PsSoloAlfa() {
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_ps_solo_alfa_alternar_s);
    if (alternar <= 0) {
      return REXCVAR_GET(nfsmw_nativo_ps_solo_alfa);
    }
    const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - inicio_alternancia_ps_)
                              .count();
    const bool solo_alfa = (segundos / alternar) % 2 == 1;
    if (solo_alfa != alternancia_solo_alfa_anotada_) {
      alternancia_solo_alfa_anotada_ = solo_alfa;
      REXLOG_INFO("[nativo] C2 escrituras de color: {}",
                  solo_alfa ? "quitadas en las pasadas sin color" : "todas");
    }
    return solo_alfa;
  }

  // The setting, or its alternating test.
  bool SinPsSinColor() {
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_sin_ps_sin_color_alternar_s);
    if (alternar <= 0) {
      return REXCVAR_GET(nfsmw_nativo_sin_ps_sin_color);
    }
    const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - inicio_alternancia_ps_)
                              .count();
    const bool sin_ps = (segundos / alternar) % 2 == 1;
    if (sin_ps != alternancia_ps_anotada_) {
      alternancia_ps_anotada_ = sin_ps;
      REXLOG_INFO("[nativo] C2 etapa de fragmentos: {}",
                  sin_ps ? "quitada en los dibujos sin color" : "siempre montada");
    }
    return sin_ps;
  }

  // 1/size of a slot's host image, in the shared constants. With size 0 (a texture that could not be
  // prepared) 0 is written: the shader does not use it because it does not sample either.
  static void EscribirInvTamano(uint32_t* compartidas, uint32_t registro, uint32_t ancho, uint32_t alto) {
    const float inv[2] = {ancho ? 1.0f / float(ancho) : 0.0f, alto ? 1.0f / float(alto) : 0.0f};
    std::memcpy(compartidas + kPalabraInvTamano + registro * 2, inv, sizeof(inv));
  }

  bool EmpezarPase(const uint32_t* r, const uint64_t claves[5], uint32_t pitch,
                   uint64_t clave_pase) {
    inicio_pase_ = cronometrar_ ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    if (!PrepararVacias()) {
      return false;
    }
    std::array<ImagenNativa*, 5> imagenes{};
    for (uint32_t i = 0; i < 4; ++i) {
      if (claves[i]) {
        imagenes[i] = contexto_->DestinoColor(uint32_t(claves[i] >> 24) & 0xFFF,
                                              uint32_t(claves[i] >> 16) & 0xF, pitch);
        if (!imagenes[i]) {
          return Rechazar(40, "no hay destino de color");
        }
      }
    }
    if (claves[4]) {
      imagenes[4] = contexto_->DestinoProfundidad(uint32_t(claves[4] >> 24) & 0xFFF,
                                                  uint32_t(claves[4] >> 16) & 0x1, pitch);
      if (!imagenes[4]) {
        return Rechazar(41, "no hay destino de profundidad");
      }
    }
    (void)r;
    // The render targets are already resolved above; that segment is closed.
    const auto tras_destinos = cronometrar_ ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    if (cronometrar_) {
      etapas_ns_[9] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          tras_destinos - inicio_pase_).count());
    }
    uint32_t ancho = UINT32_MAX, alto = UINT32_MAX;
    std::array<VkImageView, 5> vistas{};
    uint32_t formatos[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
      if (imagenes[i]) {
        ancho = std::min(ancho, imagenes[i]->ancho);
        alto = std::min(alto, imagenes[i]->alto);
        vistas[i] = imagenes[i]->vista;
        formatos[i] = uint32_t(imagenes[i]->formato);
      }
    }
    // Shadow map: a depth-only target with a pitch of 1600 or more (same as omitir_sombras).
    const bool es_sombras = pitch >= 1600 && formatos[4] && !formatos[0] && !formatos[1] &&
                            !formatos[2] && !formatos[3];
    // nfsmw_nativo_sombra_minimo. If C2 says this is the car pass of the shadow map, each draw is validated
    // for it (and the pass must be depth-only).
    pase_coches_sombra_ = imagenes[4] != nullptr && contexto_->PaseDeCochesSombra(imagenes[4], es_sombras);
    // GPU time per pass type (C2 report), by render target width. Shadows are recognized by being
    // depth-only, not by measuring 1600 or more. With the scene at 1920 (nfsmw_1080p_prueba) the width no
    // longer tells them apart: the scene fell into the shadow bucket and vanished from its own category.
    categoria_pase_ = CategoriaDeDestino(pitch, claves);
    contexto_->MarcarGpu(categoria_pase_);
    const bool sombras_sin_load =
        es_sombras && REXCVAR_GET(nfsmw_nativo_pase_sombras_sin_load);
    const VkRenderPass pase = PaseDe(formatos, sombras_sin_load ? kCargaIgnorar : kCargaLeer);
    if (pase == VK_NULL_HANDLE) {
      return Rechazar(42, "no se pudo crear el render pass");
    }
    const VkFramebuffer framebuffer = FramebufferDe(pase, vistas, ancho, alto);
    if (framebuffer == VK_NULL_HANDLE) {
      return Rechazar(43, "no se pudo crear el framebuffer");
    }
    if (cronometrar_) {
      etapas_ns_[10] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - tras_destinos).count());
    }
    const auto antes_abrir = cronometrar_ ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    const VkCommandBuffer cmd = contexto_->ComandosTrabajo();
    if (!cmd) {
      return false;
    }
    VkRenderPassBeginInfo inicio{};
    inicio.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    inicio.renderPass = pase;
    inicio.framebuffer = framebuffer;
    // Only the rectangle the game uses (see nfsmw_nativo_pase_area_util).
    uint32_t alto_pase = alto;
    if (area_util_) {
      const auto it = alto_util_.find(pitch);
      if (it != alto_util_.end() && it->second < alto) {
        alto_pase = it->second;
      }
    }
    inicio.renderArea.extent = {ancho, alto_pase};
    // nfsmw_nativo_diag_borrados. What this pass loads and stores of each render target, to compare with
    // what is cleared of it (C2 clears per target). Before opening the pass.
    for (const ImagenNativa* imagen : imagenes) {
      if (imagen) {
        contexto_->AnotarAreaDePase(imagen, ancho, alto_pase);
      }
    }
    // The statistics cover the whole pass, from here to after EndRenderPass.
    estadisticas_pase_ = contexto_->EmpezarEstadisticas(categoria_pase_);
    const auto antes_inicio = std::chrono::steady_clock::now();
    dfn_.vkCmdBeginRenderPass(cmd, &inicio, VK_SUBPASS_CONTENTS_INLINE);
    ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - antes_inicio).count());
    if (cronometrar_) {
      etapas_ns_[11] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - antes_abrir).count());
    }
    pase_activo_ = true;
    ++pases_empezados_;
    texels_pases_ += uint64_t(ancho) * alto_pase;  // how much tile is loaded by loadOp = LOAD
    {
      // Same criterion as the per-category GPU time: the render target's pitch decides.
      const uint32_t categoria = CategoriaDeDestino(pitch, claves);
      texels_por_categoria_[categoria] += uint64_t(ancho) * alto_pase;
      ++pases_por_categoria_[categoria];
    }
    pase_comandos_ = cmd;
    dibujos_en_pase_ = 0;  // position within the pass (deferred sky report)
    pase_clave_ = clave_pase;
    std::memcpy(pase_claves_, claves, sizeof(pase_claves_));  // the bytes that produce pase_clave_
    pase_claves_validas_ = true;
    ++pase_serie_;  // the cached framing depends on the pass size and scale
    pase_generacion_ = contexto_->GeneracionComandos();
    pase_ancho_ = ancho;
    pase_alto_ = alto;
    // Only the shadow map is drawn at a size other than the one the guest thinks.
    pase_escala_ = (es_sombras && pitch && ancho && ancho != pitch)
                       ? float(ancho) / float(pitch)
                       : 1.0f;
    std::copy(std::begin(formatos), std::end(formatos), std::begin(pase_formatos_));
    pase_rp_ = pase;
    // Phase 0b (nfsmw_nativo_pipeline_entre_pases). Vulkan keeps the bound pipeline across passes of the
    // same buffer: the pass's first draw only binds again if its key differs. A new buffer still starts
    // with nothing bound (Dibujar, grabacion_generacion_).
    if (!pipeline_entre_pases_) {
      pipeline_enlazado_ = VK_NULL_HANDLE;
    } else if (pipeline_enlazado_ != VK_NULL_HANDLE) {
      ++pases_con_pipeline_;
    }
    estado_grabado_ = false;
    if (pipeline_enlazado_ == VK_NULL_HANDLE) {
      eds_valido_ = false;  // pass that forgets the pipeline (without 0b): set everything again
    }
    return true;
  }

  // ZCULL: three load modes. kCargaBorrar is needed to clear the depth of images created without
  // TRANSFER_DST, the only ones eligible for a ZCULL plane.
  enum : uint32_t { kCargaLeer = 0, kCargaIgnorar = 1, kCargaBorrar = 2 };

  VkRenderPass PaseDe(const uint32_t formatos[5], uint32_t modo_carga = kCargaLeer) {
    // The mode is part of the key: two render passes with the same formats but a different loadOp are
    // different.
    const uint64_t clave =
        XXH3_64bits_withSeed(formatos, sizeof(uint32_t) * 5, modo_carga);
    if (const auto it = pases_.find(clave); it != pases_.end()) {
      return it->second;
    }
    const VkRenderPass pase = CrearPase(formatos, modo_carga);  // the usual code, factored out
    if (pase == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
    pases_.emplace(clave, pase);
    return pase;
  }

  // The plain loop for 16-bit indices (byte-swapped or not), untouched.
  static void IndicesDe16Escalar(const uint8_t* datos, uint32_t cuenta, uint16_t* salida, bool girar, uint32_t& minimo,
                                 uint32_t& maximo) {
    minimo = 0xFFFF;
    maximo = 0;
    if (girar) {
      for (uint32_t i = 0; i < cuenta; ++i) {
        uint16_t crudo;
        std::memcpy(&crudo, datos + size_t(i) * 2, 2);
        const uint16_t v = std::byteswap(crudo);
        salida[i] = v;
        minimo = std::min<uint32_t>(minimo, v);
        maximo = std::max<uint32_t>(maximo, v);
      }
    } else {
      for (uint32_t i = 0; i < cuenta; ++i) {
        uint16_t v;
        std::memcpy(&v, datos + size_t(i) * 2, 2);
        salida[i] = v;
        minimo = std::min<uint32_t>(minimo, v);
        maximo = std::max<uint32_t>(maximo, v);
      }
    }
  }

  // The same loop with NEON, 16 indices per iteration (vrev16 swaps the bytes of each 16-bit index, the
  // same as std::byteswap), and the remainder with the plain loop. NEON loads and stores need no
  // alignment on AArch64.
  static void IndicesDe16Neon(const uint8_t* datos, uint32_t cuenta, uint16_t* salida, bool girar, uint32_t& minimo,
                              uint32_t& maximo) {
#if defined(__aarch64__)
    uint32_t i = 0;
    uint32_t mn = 0xFFFF;
    uint32_t mx = 0;
    if (cuenta >= 16) {
      uint16x8_t min_a = vdupq_n_u16(0xFFFF);
      uint16x8_t min_b = min_a;
      uint16x8_t max_a = vdupq_n_u16(0);
      uint16x8_t max_b = max_a;
      // Two loops (swapping is fixed per draw): no decision inside the iteration.
      if (girar) {
        for (; i + 16 <= cuenta; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(datos + size_t(i) * 2)));
          const uint16x8_t b = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(datos + size_t(i) * 2 + 16)));
          vst1q_u16(salida + i, a);
          vst1q_u16(salida + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      } else {
        for (; i + 16 <= cuenta; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vld1q_u8(datos + size_t(i) * 2));
          const uint16x8_t b = vreinterpretq_u16_u8(vld1q_u8(datos + size_t(i) * 2 + 16));
          vst1q_u16(salida + i, a);
          vst1q_u16(salida + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      }
      mn = vminvq_u16(vminq_u16(min_a, min_b));
      mx = vmaxvq_u16(vmaxq_u16(max_a, max_b));
    }
    for (; i < cuenta; ++i) {
      uint16_t v;
      std::memcpy(&v, datos + size_t(i) * 2, 2);
      if (girar) {
        v = std::byteswap(v);
      }
      salida[i] = v;
      mn = std::min<uint32_t>(mn, v);
      mx = std::max<uint32_t>(mx, v);
    }
    minimo = mn;
    maximo = mx;
#else
    IndicesDe16Escalar(datos, cuenta, salida, girar, minimo, maximo);
#endif
  }

  // The 16-bit indices of a draw (see nfsmw_nativo_indices_neon). Ring only.
  void IndicesDe16(const uint8_t* datos, uint32_t cuenta, uint16_t* salida, bool girar, uint32_t& minimo,
                   uint32_t& maximo) {
    if (indices_neon_ < 0) {
      indices_neon_ = REXCVAR_GET(nfsmw_nativo_indices_neon) ? 1 : 0;
      REXLOG_INFO("[nativo] C6 indices de 16 bits (build 187): {}",
                  indices_neon_ ? "con NEON; se comprueban los 20.000 primeros dibujos y despues 1 de cada 4.096"
                                : "con el bucle de siempre (nfsmw_nativo_indices_neon = false)");
    }
    if (indices_neon_ == 0) {
      IndicesDe16Escalar(datos, cuenta, salida, girar, minimo, maximo);
      return;
    }
    IndicesDe16Neon(datos, cuenta, salida, girar, minimo, maximo);
    const uint64_t n = ++indices_neon_dibujos_;
    if (n > 20000 && (n & 4095) != 0) {
      return;
    }
    indices_neon_prueba_.resize(cuenta);
    uint32_t mn = 0;
    uint32_t mx = 0;
    IndicesDe16Escalar(datos, cuenta, indices_neon_prueba_.data(), girar, mn, mx);
    ++indices_neon_comprobados_;
    if (mn != minimo || mx != maximo ||
        (cuenta && std::memcmp(indices_neon_prueba_.data(), salida, size_t(cuenta) * 2) != 0)) {
      // The plain path stays for this draw and for the rest of the session.
      if (cuenta) {
        std::memcpy(salida, indices_neon_prueba_.data(), size_t(cuenta) * 2);
      }
      minimo = mn;
      maximo = mx;
      indices_neon_ = 0;
      REXLOG_ERROR("[nativo] C6 indices de 16 bits: DIFERENCIA entre NEON y el bucle de siempre ({} indices, {}; minimo "
                   "{} frente a {}, maximo {} frente a {}). Apagado para el resto de la sesion: el bucle de siempre",
                   cuenta, girar ? "girados" : "sin girar", minimo, mn, maximo, mx);
      return;
    }
    if (indices_neon_comprobados_ == 20000) {
      REXLOG_INFO("[nativo] C6 indices de 16 bits (build 187): 20.000 dibujos comprobados contra el bucle de siempre, 0 "
                  "diferencias; sigue comprobando 1 de cada 4.096");
    }
  }

  // The PaseDe render pass without its cache, with nothing changed. The pipeline prewarm also uses it
  // from its thread (a compatible one: the same formats). It only reads its arguments.
  VkRenderPass CrearPase(const uint32_t formatos[5], uint32_t modo_carga) const {
    std::array<VkAttachmentDescription, 5> adjuntos{};
    std::array<VkAttachmentReference, 4> colores{};
    VkAttachmentReference profundidad{};
    uint32_t n = 0, n_colores = 0;
    for (uint32_t i = 0; i < 5; ++i) {
      if (!formatos[i]) {
        continue;
      }
      VkAttachmentDescription& a = adjuntos[n];
      a.format = VkFormat(formatos[i]);
      a.samples = VK_SAMPLE_COUNT_1_BIT;
      // With the test active, the shadow map does not load its previous content: the game clears it before
      // drawing it, so fetching the 1600x1600 tile only to throw it away is wasted work.
      a.loadOp = modo_carga == kCargaIgnorar ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                 : modo_carga == kCargaBorrar ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                              : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = i != 4                       ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                        : modo_carga == kCargaBorrar ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                     : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.stencilStoreOp = i == 4 ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      if (i < 4) {
        colores[n_colores++] = {n, VK_IMAGE_LAYOUT_GENERAL};
      } else {
        profundidad = {n, VK_IMAGE_LAYOUT_GENERAL};
      }
      ++n;
    }
    VkSubpassDescription subpase{};
    subpase.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpase.colorAttachmentCount = n_colores;
    subpase.pColorAttachments = colores.data();
    subpase.pDepthStencilAttachment = formatos[4] ? &profundidad : nullptr;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = n;
    info.pAttachments = adjuntos.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpase;
    VkSubpassDependency dependencias[2]{};
    if (REXCVAR_GET(nfsmw_nativo_sincronizacion_gpu)) {
      DependenciasImagenes(dependencias);
      info.dependencyCount = 2;
      info.pDependencies = dependencias;
    }
    VkRenderPass pase;
    if (dfn_.vkCreateRenderPass(device_, &info, nullptr, &pase) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pase;
  }

  VkFramebuffer FramebufferDe(VkRenderPass pase, const std::array<VkImageView, 5>& vistas,
                              uint32_t ancho, uint32_t alto) {
    struct Clave {
      VkRenderPass pase;
      std::array<VkImageView, 5> vistas;
      uint32_t ancho, alto;
    } clave{pase, vistas, ancho, alto};
    const uint64_t huella = XXH3_64bits(&clave, sizeof(clave));
    if (const auto it = framebuffers_.find(huella); it != framebuffers_.end()) {
      return it->second;
    }
    std::array<VkImageView, 5> adjuntos{};
    uint32_t n = 0;
    for (VkImageView vista : vistas) {
      if (vista != VK_NULL_HANDLE) {
        adjuntos[n++] = vista;
      }
    }
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pase;
    info.attachmentCount = n;
    info.pAttachments = adjuntos.data();
    info.width = ancho;
    info.height = alto;
    info.layers = 1;
    VkFramebuffer framebuffer;
    if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    framebuffers_.emplace(huella, framebuffer);
    framebuffers_vistas_[huella] = vistas;  // For OlvidarVista
    return framebuffer;
  }

  // The bright pass variants of nfsmw_resplandor_cielo (1 natural, 2 soft), compiled with the same DXC
  // options as the library.
  // vkCreateShaderModule for the library's SPIR-V, converted to 1.3 on a Vulkan 1.1 device (spirv_13_). Called
  // from the ring thread and from the prewarm thread: it only reads spirv_13_ and dfn_.
  VkResult CrearModulo(const uint32_t* spirv, size_t bytes, VkShaderModule* modulo) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = spirv;
    std::vector<uint32_t> convertido;
    if (spirv_13_ && nfsmw::spirv11::Necesita(spirv, bytes / 4)) {
      convertido = nfsmw::spirv11::Convertir(spirv, bytes / 4);
      if (!convertido.empty()) {
        info.codeSize = convertido.size() * sizeof(uint32_t);
        info.pCode = convertido.data();
      }
    }
    return dfn_.vkCreateShaderModule(device_, &info, nullptr, modulo);
  }

  VkShaderModule ModuloVariante(size_t indice, const uint32_t* spirv, size_t bytes) {
    if (!modulos_variantes_creados_[indice]) {
      modulos_variantes_creados_[indice] = true;
      if (CrearModulo(spirv, bytes, &modulos_variantes_[indice]) != VK_SUCCESS) {
        modulos_variantes_[indice] = VK_NULL_HANDLE;
      }
    }
    return modulos_variantes_[indice];
  }

  // nfsmw_resplandor_cielo (F4 menu, Graficos: 0 original, 1 natural, 2 soft), or its test rotation; logs
  // every change.
  int ResplandorCielo() {
    int modo = nfsmw::ajustes::ResplandorCielo();
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_resplandor_suave_alternar_s);
    if (alternar > 0) {
      const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - inicio_sombras_)
                                .count();
      modo = int((segundos / alternar) % 3);
    }
    modo = std::clamp(modo, 0, 2);
    if (modo != resplandor_anotado_) {
      resplandor_anotado_ = modo;
      static constexpr const char* kNombres[3] = {"original", "natural", "suave"};
      REXLOG_INFO("[nativo] resplandor del cielo: {} (fotograma {})", kNombres[modo], fotograma_);
    }
    return modo;
  }

  /*
   * The early Z and invisible draws report, every 10 s like the others.
   *
   * What matters is not what is saved but the second figure: "NO se pueden porque escriben profundidad"
   * is, draw by draw, the exact size of what only a depth pre-pass could address, and that is expensive
   * to implement. A small figure means a pre-pass is not worth the risk.
   */
  void InformeZTemprana() {
    ++fotogramas_z_;
    const auto ahora = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(ahora - ultimo_informe_z_).count() < 10) {
      return;
    }
    ultimo_informe_z_ = ahora;
    const uint64_t vistos = fotogramas_z_ - fotogramas_z_previos_;
    const double fotogramas = double(vistos ? vistos : 1);
    std::array<uint64_t, kZCuentas> d{};
    uint64_t total = 0;
    for (uint32_t i = 0; i < kZCuentas; ++i) {
      d[i] = cuentas_z_[i] - cuentas_z_previas_[i];
      cuentas_z_previas_[i] = cuentas_z_[i];
      total += d[i];
    }
    fotogramas_z_previos_ = fotogramas_z_;
    InformeCielo(fotogramas);  // goes before the !total cutoff, which has nothing to do with the sky
    if (!total) {
      return;
    }
    const uint64_t tardios = d[kZPuesta] + d[kZEscribeZ] + d[kZEstencil] + d[kZOclusion];
    NFSMW_INFORME_ANILLO(
        "[nativo] C6 Z temprana por fotograma: {:.0f} dibujos la aprovechan, {:.0f} NO se pueden porque "
        "escriben profundidad (esos son el tamano de la pasada previa de C88), {:.0f} por estencil, {:.0f} "
        "dentro de una consulta de oclusion; {:.0f} ya probaban antes. De los que obligaban a sombrear "
        "primero se arregla el {:.0f} %{}",
        double(d[kZPuesta]) / fotogramas, double(d[kZEscribeZ]) / fotogramas,
        double(d[kZEstencil]) / fotogramas, double(d[kZOclusion]) / fotogramas,
        double(d[kZYaTemprano]) / fotogramas,
        tardios ? 100.0 * double(d[kZPuesta]) / double(tardios) : 0.0,
        z_temprana_sin_modulo_ ? fmt::format(" ({} shaders sin modulo parcheado)", z_temprana_sin_modulo_)
                               : std::string());
    if (d[kInvisibleMezcla] || d[kInvisibleAlfa] || d[kInvisibleSoloColor]) {
      NFSMW_INFORME_ANILLO(
          "[nativo] C6 dibujos invisibles por fotograma: {:.1f} con la mezcla que copia el destino, {:.1f} "
          "con la prueba de alfa en NUNCA; {:.1f} se quedan solo con la profundidad",
          double(d[kInvisibleMezcla]) / fotogramas, double(d[kInvisibleAlfa]) / fotogramas,
          double(d[kInvisibleSoloColor]) / fotogramas);
    }
  }

  /*
   * The deferred sky report. Without it there is no way to measure whether it works.
   *
   * What to check: "aplazados" must be ~0.9 per frame in a race (the sky is a single draw and is
   * sometimes absent). "perdidos" must be 0: if not, some path closes the pass without going through
   * TerminarPase and the sky is being lost. And "posicion" says how many draws of the pass were recorded
   * between the old place and the new one: exactly the amount of geometry that now covers the sky before
   * it is shaded. If it is small, so is the saving.
   */
  void InformeCielo(double fotogramas) {
    const uint64_t aplazados = cielo_aplazados_ - cielo_aplazados_previos_;
    const uint64_t vistos = cielo_vistos_ - cielo_vistos_previos_;
    if (!vistos) {
      return;
    }
    std::array<uint64_t, kCieloMotivos> por_motivo{};
    uint64_t emitidos = 0;
    for (uint32_t i = 0; i < kCieloMotivos; ++i) {
      por_motivo[i] = cielo_emitidos_[i] - cielo_emitidos_previos_[i];
      cielo_emitidos_previos_[i] = cielo_emitidos_[i];
      emitidos += por_motivo[i];
    }
    const uint64_t posicion_suma = cielo_posicion_suma_ - cielo_posicion_suma_previa_;
    const uint64_t candidatos = cielo_candidatos_ - cielo_candidatos_previos_;
    cielo_candidatos_previos_ = cielo_candidatos_;
    cielo_vistos_previos_ = cielo_vistos_;
    cielo_aplazados_previos_ = cielo_aplazados_;
    cielo_posicion_suma_previa_ = cielo_posicion_suma_;
    /*
     * The line that decides whether the criterion works. "con la geometria del domo" is the number that
     * matters: it must be 1.00 per frame. The "detectados" are by fingerprint only and once gave 7.00,
     * which is what broke the image.
     */
    NFSMW_INFORME_ANILLO("[nativo] C6 cielo: {:.2f} detectados por huella y {:.2f} con la geometria del domo "
                "(480 indices y primeros del pase) por fotograma. El segundo tiene que dar 1,00. "
                "GUARDIA: {} ({} fotogramas con domo mirados, {} con exactamente uno, maximo {} en "
                "un fotograma)",
                double(vistos) / fotogramas, double(candidatos) / fotogramas,
                cielo_guardia_ == kCieloAplazando  ? "SUPERADA, aplazando"
                : cielo_guardia_ == kCieloDescartado ? "DESCARTADA, no se aplaza nada"
                                                     : "mirando todavia",
                cielo_guardia_fotogramas_, cielo_guardia_con_uno_, cielo_guardia_max_);
    NFSMW_INFORME_ANILLO(
        "[nativo] C6 cielo aplazado ({}): {:.2f} detectados por fotograma, {:.2f} aplazados de verdad "
        "({} no aplazables en total, {} con otro cielo ya pendiente); emitidos {} al llegar un dibujo "
        "con mezcla, {} al llegar un opaco que no escribe Z, {} al cerrarse el pase, {} por otro cielo; "
        "PERDIDOS {}; posicion media {:.0f} dibujos por delante de el (maximo {})",
        cielo_aplazado_ ? "encendido" : "APAGADO", double(vistos) / fotogramas,
        double(aplazados) / fotogramas, cielo_no_aplazables_, cielo_dos_en_pase_,
        por_motivo[kCieloPorMezcla], por_motivo[kCieloPorSinZ], por_motivo[kCieloPorFinDePase],
        por_motivo[kCieloPorOtroCielo], cielo_perdidos_,
        emitidos ? double(posicion_suma) / double(emitidos) : 0.0, cielo_posicion_max_);
  }

  /*
   * Closes the frame for the sky guard. See the block of fields.
   *
   * Runs once per frame and does nothing once the guard has decided, so its cost is one comparison. The
   * decision is taken only once per session and logged.
   */
  void CerrarFotogramaDeLaGuardiaDelCielo() {
    const uint32_t en_este = cielo_guardia_en_fotograma_;
    cielo_guardia_en_fotograma_ = 0;
    if (cielo_guardia_ != kCieloMirando) {
      return;
    }
    /*
     * A frame without a sky says nothing about the criterion: menus, the logo and loading screens, which
     * come before any race. If they counted, the test would run out in the menu and the guard would switch
     * off for good without ever seeing the dome. Only frames in which the dome appears count.
     */
    if (en_este == 0) {
      return;
    }
    cielo_guardia_max_ = std::max(cielo_guardia_max_, en_este);
    if (en_este == 1) {
      ++cielo_guardia_con_uno_;
    }
    if (++cielo_guardia_fotogramas_ < kCieloFotogramasPrueba) {
      return;
    }
    /*
     * The test is over. It only switches on if it came out clean: never more than one, and enough frames
     * with exactly one. In any other case it stays off and the image is identical to the non-deferred one,
     * which is known to work.
     */
    if (cielo_guardia_max_ == 1 && cielo_guardia_con_uno_ >= kCieloFotogramasConUno) {
      cielo_guardia_ = kCieloAplazando;
      REXLOG_INFO("[nativo] C6 cielo: guardia SUPERADA ({} de {} fotogramas con exactamente un domo, "
                  "nunca dos). Se aplaza el cielo a partir de ahora: son 3,4-4,3 ms de GPU sin "
                  "cambiar un pixel",
                  cielo_guardia_con_uno_, cielo_guardia_fotogramas_);
    } else {
      cielo_guardia_ = kCieloDescartado;
      REXLOG_WARN("[nativo] C6 cielo: guardia NO superada (maximo {} domos en un fotograma, {} de {} "
                  "fotogramas con exactamente uno; hacian falta {}). NO se aplaza nada: la imagen "
                  "queda exactamente como estaba",
                  cielo_guardia_max_, cielo_guardia_con_uno_, cielo_guardia_fotogramas_,
                  kCieloFotogramasConUno);
    }
  }

  VkShaderModule ModuloDe(const EntradaShader& entrada) {
    if (const auto it = modulos_.find(&entrada); it != modulos_.end()) {
      return it->second;
    }
    const auto& spirv = entrada.shader->spirv;
    VkShaderModule modulo = VK_NULL_HANDLE;
    if (CrearModulo(spirv.data(), spirv.size() * sizeof(uint32_t), &modulo) != VK_SUCCESS) {
      modulo = VK_NULL_HANDLE;
    }
    modulos_.emplace(&entrada, modulo);
    return modulo;
  }

  // Pixel shader module without the color writes, one per shader. If it cannot be pruned, the normal one
  // is returned: the image is the same, only the saving is lost.
  VkShaderModule ModuloSoloAlfa(const EntradaShader& entrada) {
    if (const auto it = modulos_solo_alfa_.find(&entrada); it != modulos_solo_alfa_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuloDe(entrada);
    }
    uint32_t quitadas = 0;
    const std::vector<uint32_t> podado = PodarEscriturasDeColor(entrada.shader->spirv, quitadas);
    VkShaderModule modulo = VK_NULL_HANDLE;
    if (!podado.empty()) {
      if (CrearModulo(podado.data(), podado.size() * sizeof(uint32_t), &modulo) != VK_SUCCESS) {
        modulo = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[nativo] C5a: PS n{} sin escrituras de color: {} quitadas, {} palabras de {} ({})",
                entrada.numero, quitadas, podado.size(), entrada.shader->spirv.size(),
                modulo != VK_NULL_HANDLE ? "modulo creado" : "se usa el normal");
    modulos_solo_alfa_.emplace(&entrada, modulo);
    return modulo != VK_NULL_HANDLE ? modulo : ModuloDe(entrada);
  }

  /*
   * Pixel shader module with EarlyFragmentTests declared, one per shader. If that is not possible (it
   * already had it, it writes gl_FragDepth...), the normal one is returned: the image is the same and only
   * the saving is lost. It is logged once per shader so the log shows which ones qualified.
   */
  VkShaderModule ModuloZTemprana(const EntradaShader& entrada) {
    if (const auto it = modulos_z_temprana_.find(&entrada); it != modulos_z_temprana_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuloDe(entrada);
    }
    const char* motivo = "";
    const std::vector<uint32_t> parcheado = ConPruebasTempranas(entrada.shader->spirv, motivo);
    VkShaderModule modulo = VK_NULL_HANDLE;
    if (!parcheado.empty()) {
      if (CrearModulo(parcheado.data(), parcheado.size() * sizeof(uint32_t), &modulo) != VK_SUCCESS) {
        modulo = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[nativo] C6 Z temprana: PS n{} {} ({} kills, {} palabras)", entrada.numero,
                modulo != VK_NULL_HANDLE
                    ? std::string("prueba la profundidad antes de sombrear")
                    : fmt::format("se queda como estaba: {}", motivo),
                entrada.kills, entrada.shader->spirv.size());
    modulos_z_temprana_.emplace(&entrada, modulo);
    if (modulo == VK_NULL_HANDLE) {
      ++z_temprana_sin_modulo_;
    }
    return modulo != VK_NULL_HANDLE ? modulo : ModuloDe(entrada);
  }

  /*
   * The fixed state of a pipeline, exactly as PipelineDe sets it. It is its usual code, moved here without
   * changing any computation: only its eight declarations become references into EstadoFijoPipeline. Used
   * by PipelineDe and by the guards of the canonical key (phase 0a) and of dynamic state (phases 1 and 2),
   * which thus compare against what the pipeline really carries and not against another copy of the same
   * computations. mezcla.pAttachments points to fijo.mezclas: the structure is not copied.
   */
  struct EstadoFijoPipeline {
    VkPipelineInputAssemblyStateCreateInfo ensamblado{};
    VkPipelineViewportStateCreateInfo vista{};
    VkPipelineRasterizationStateCreateInfo rasterizado{};
    VkPipelineMultisampleStateCreateInfo muestreo{};
    VkPipelineDepthStencilStateCreateInfo profundidad{};
    std::array<VkPipelineColorBlendAttachmentState, 4> mezclas{};
    uint32_t n_colores = 0;
    VkPipelineColorBlendStateCreateInfo mezcla{};
    EstadoFijoPipeline() = default;
    EstadoFijoPipeline(const EstadoFijoPipeline&) = delete;
    EstadoFijoPipeline& operator=(const EstadoFijoPipeline&) = delete;
  };

  // avisar = false from the prewarm thread (Avisar belongs to the ring only).
  void RellenarEstadoFijo(const ClavePipeline& clave, EstadoFijoPipeline& fijo, bool avisar = true) {
    VkPipelineInputAssemblyStateCreateInfo& ensamblado = fijo.ensamblado;
    ensamblado.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ensamblado.topology = VkPrimitiveTopology(clave.topologia);
    ensamblado.primitiveRestartEnable = (clave.rasterizado & 0x8) ? VK_TRUE : VK_FALSE;

    VkPipelineViewportStateCreateInfo& vista = fijo.vista;
    vista.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vista.viewportCount = 1;
    vista.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo& rasterizado = fijo.rasterizado;
    rasterizado.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizado.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizado.cullMode = ((clave.rasterizado & 0x1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                           ((clave.rasterizado & 0x2) ? VK_CULL_MODE_BACK_BIT : 0);
    // PA_SU_SC_MODE_CNTL.face: 1 = the front face is clockwise (not confirmed on screen).
    rasterizado.frontFace =
        (clave.rasterizado & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizado.lineWidth = 1.0f;
    rasterizado.depthBiasEnable = (clave.rasterizado & 0x10) ? VK_TRUE : VK_FALSE;
    rasterizado.depthClampEnable = REXCVAR_GET(nfsmw_nativo_prueba_depth_clamp) ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo& muestreo = fijo.muestreo;
    muestreo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    muestreo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    const uint32_t d = clave.profundidad;
    VkPipelineDepthStencilStateCreateInfo& profundidad = fijo.profundidad;
    profundidad.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (clave.formatos[4]) {
      profundidad.depthTestEnable = (d >> 1) & 0x1;
      profundidad.depthWriteEnable = ((d >> 1) & 0x1) && ((d >> 2) & 0x1);
      profundidad.depthCompareOp = VkCompareOp((d >> 4) & 0x7);
      profundidad.stencilTestEnable = d & 0x1;
      profundidad.front.failOp = VkStencilOp((d >> 11) & 0x7);
      profundidad.front.passOp = VkStencilOp((d >> 14) & 0x7);
      profundidad.front.depthFailOp = VkStencilOp((d >> 17) & 0x7);
      profundidad.front.compareOp = VkCompareOp((d >> 8) & 0x7);
      if ((d >> 7) & 0x1) {
        profundidad.back.compareOp = VkCompareOp((d >> 20) & 0x7);
        profundidad.back.failOp = VkStencilOp((d >> 23) & 0x7);
        profundidad.back.passOp = VkStencilOp((d >> 26) & 0x7);
        profundidad.back.depthFailOp = VkStencilOp((d >> 29) & 0x7);
      } else {
        profundidad.back = profundidad.front;
      }
    }

    static constexpr VkBlendFactor kFactores[32] = {
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
    };
    const auto operacion = [&](uint32_t op) {
      switch (op) {
        case 1:
          return VK_BLEND_OP_SUBTRACT;
        case 2:
          return VK_BLEND_OP_MIN;
        case 3:
          return VK_BLEND_OP_MAX;
        case 4:
          if (avisar) {
            Avisar(52, "mezcla con resta inversa: se usa resta");
          }
          return VK_BLEND_OP_SUBTRACT;
        default:
          return VK_BLEND_OP_ADD;
      }
    };
    std::array<VkPipelineColorBlendAttachmentState, 4>& mezclas = fijo.mezclas;
    uint32_t& n_colores = fijo.n_colores;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!clave.formatos[i]) {
        continue;
      }
      const uint32_t m = clave.mezcla[i];
      VkPipelineColorBlendAttachmentState& s = mezclas[n_colores++];
      s.colorWriteMask = (clave.mascaras >> (i * 4)) & 0xF;
      const uint32_t fuente = m & 0x1F, op = (m >> 5) & 0x7, destino = (m >> 8) & 0x1F;
      const uint32_t fuente_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7,
                     destino_a = (m >> 24) & 0x1F;
      /*
       * Blending is only enabled if it is needed on the channels that are written.
       *
       * It used to require all six fields to be "1 x source + 0 x destination, ADD". But if the mask does
       * not write RGB, the three color factors do not matter, and the same goes for alpha. With blendEnable
       * false the ROP does not have to read the destination or go through the blend unit. The result is the
       * same.
       */
      const bool escribe_rgb = (s.colorWriteMask & 0x7) != 0;
      const bool escribe_alfa = (s.colorWriteMask & 0x8) != 0;
      const bool color_directo = fuente == 1 && destino == 0 && op == 0;
      const bool alfa_directo = fuente_a == 1 && destino_a == 0 && op_a == 0;
      s.blendEnable = (escribe_rgb && !color_directo) || (escribe_alfa && !alfa_directo);
      s.srcColorBlendFactor = kFactores[fuente];
      s.dstColorBlendFactor = kFactores[destino];
      s.colorBlendOp = operacion(op);
      s.srcAlphaBlendFactor = kFactores[fuente_a];
      s.dstAlphaBlendFactor = kFactores[destino_a];
      s.alphaBlendOp = operacion(op_a);
    }
    VkPipelineColorBlendStateCreateInfo& mezcla = fijo.mezcla;
    mezcla.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    mezcla.attachmentCount = n_colores;
    mezcla.pAttachments = mezclas.data();
  }

  // One stencil face, in what Vulkan looks at in the pipeline (masks and reference are dynamic).
  static bool MismaCaraEstencil(const VkStencilOpState& a, const VkStencilOpState& b) {
    return a.failOp == b.failOp && a.passOp == b.passOp && a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp;
  }

  /*
   * Two fixed states, field by field, in what Vulkan looks at: the blend equation only with blendEnable,
   * each half only if the mask writes its channels, the Z function only with a Z test, stencil operations
   * only with stencil. Returns the first differing field, or nullptr if there is none.
   */
  static const char* DiferenciaEfectiva(const EstadoFijoPipeline& a, const EstadoFijoPipeline& b) {
    if (a.ensamblado.topology != b.ensamblado.topology) {
      return "topology";
    }
    if (a.ensamblado.primitiveRestartEnable != b.ensamblado.primitiveRestartEnable) {
      return "primitiveRestartEnable";
    }
    if (a.vista.viewportCount != b.vista.viewportCount || a.vista.scissorCount != b.vista.scissorCount) {
      return "viewportCount o scissorCount";
    }
    const VkPipelineRasterizationStateCreateInfo& ra = a.rasterizado;
    const VkPipelineRasterizationStateCreateInfo& rb = b.rasterizado;
    if (ra.polygonMode != rb.polygonMode || ra.lineWidth != rb.lineWidth ||
        ra.rasterizerDiscardEnable != rb.rasterizerDiscardEnable || ra.depthClampEnable != rb.depthClampEnable) {
      return "polygonMode, lineWidth, rasterizerDiscardEnable o depthClampEnable";
    }
    if (ra.cullMode != rb.cullMode) {
      return "cullMode";
    }
    if (ra.frontFace != rb.frontFace) {
      return "frontFace";
    }
    if (ra.depthBiasEnable != rb.depthBiasEnable) {
      return "depthBiasEnable";
    }
    if (a.muestreo.rasterizationSamples != b.muestreo.rasterizationSamples) {
      return "rasterizationSamples";
    }
    const VkPipelineDepthStencilStateCreateInfo& pa = a.profundidad;
    const VkPipelineDepthStencilStateCreateInfo& pb = b.profundidad;
    if (pa.depthTestEnable != pb.depthTestEnable) {
      return "depthTestEnable";
    }
    if (pa.depthWriteEnable != pb.depthWriteEnable) {
      return "depthWriteEnable";
    }
    if (pa.depthTestEnable && pa.depthCompareOp != pb.depthCompareOp) {
      return "depthCompareOp";
    }
    if (pa.depthBoundsTestEnable != pb.depthBoundsTestEnable) {
      return "depthBoundsTestEnable";
    }
    if (pa.stencilTestEnable != pb.stencilTestEnable) {
      return "stencilTestEnable";
    }
    if (pa.stencilTestEnable && !MismaCaraEstencil(pa.front, pb.front)) {
      return "front (estencil)";
    }
    if (pa.stencilTestEnable && !MismaCaraEstencil(pa.back, pb.back)) {
      return "back (estencil)";
    }
    if (a.n_colores != b.n_colores || a.mezcla.attachmentCount != b.mezcla.attachmentCount ||
        a.mezcla.logicOpEnable != b.mezcla.logicOpEnable) {
      return "attachmentCount o logicOpEnable";
    }
    for (uint32_t i = 0; i < a.n_colores && i < 4; ++i) {
      const VkPipelineColorBlendAttachmentState& x = a.mezclas[i];
      const VkPipelineColorBlendAttachmentState& y = b.mezclas[i];
      if (x.colorWriteMask != y.colorWriteMask) {
        return "colorWriteMask";
      }
      if (x.blendEnable != y.blendEnable) {
        return "blendEnable";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x7) &&
          (x.srcColorBlendFactor != y.srcColorBlendFactor || x.dstColorBlendFactor != y.dstColorBlendFactor ||
           x.colorBlendOp != y.colorBlendOp)) {
        return "ecuacion de color";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x8) &&
          (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor || x.dstAlphaBlendFactor != y.dstAlphaBlendFactor ||
           x.alphaBlendOp != y.alphaBlendOp)) {
        return "ecuacion de alfa";
      }
    }
    return nullptr;
  }

  /*
   * Phase 0a (nfsmw_nativo_clave_canonica). A draw's key in canonical form. Same criterion as
   * EstadoCanonico (the counter) with two differences: disabled blending becomes 1 x source + 0 x
   * destination, ADD (0x00010001; with 0, RellenarEstadoFijo would enable it with zero factors), and with
   * blending enabled the half of the equation (color or alpha) whose channels the mask does not write also
   * becomes that.
   */
  static void Canonizar(ClavePipeline& c) {
    constexpr uint32_t kDirecta = 0x00010001u;  // 1 x source + 0 x destination, ADD, for color and alpha
    uint32_t mascaras = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!c.formatos[i]) {
        c.mezcla[i] = 0;  // RellenarEstadoFijo skips targets not in the pass (Dibujar leaves them at 0)
        continue;
      }
      const uint32_t mascara = (c.mascaras >> (i * 4)) & 0xF;
      mascaras |= mascara << (i * 4);
      const uint32_t m = c.mezcla[i];
      const bool color_directo = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool alfa_directo = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      const bool escribe_rgb = (mascara & 0x7) != 0;
      const bool escribe_alfa = (mascara & 0x8) != 0;
      if (!((escribe_rgb && !color_directo) || (escribe_alfa && !alfa_directo))) {
        c.mezcla[i] = kDirecta;  // without blendEnable Vulkan ignores the equation
        continue;
      }
      uint32_t canonica = m;
      if (!escribe_rgb) {
        canonica = (canonica & 0xFFFF0000u) | 0x00000001u;  // the color equation reaches no channel
      }
      if (!escribe_alfa) {
        canonica = (canonica & 0x0000FFFFu) | 0x00010000u;  // neither does the alpha one
      }
      c.mezcla[i] = canonica;
    }
    c.mascaras = mascaras;
    uint32_t d = c.formatos[4] ? c.profundidad : 0;  // ignored without a depth target
    d &= ~0x8u;                                      // bit 3 is not read
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;  // without a Z test, write and function do not count
    }
    if (!(d & 0x1)) {
      d &= 0x77u;  // sin estencil no cuentan sus funciones ni sus operaciones
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
    }
    c.profundidad = d;
  }

  /*
   * The phase 0a guard. Canonizar only touches blending, masks and depth: the rest of the key must come
   * out the same, and the fixed state of both keys must match in everything Vulkan looks at
   * (DiferenciaEfectiva on RellenarEstadoFijo). On a difference the phase switches off for the session and
   * false is returned.
   */
  bool ComprobarClaveCanonica(const ClavePipeline& cruda, const ClavePipeline& canonica, uint64_t n) {
    ++canonica_comprobadas_;
    const char* campo = nullptr;
    if (cruda.vs != canonica.vs || cruda.ps != canonica.ps || cruda.entrada != canonica.entrada ||
        cruda.topologia != canonica.topologia || cruda.especializacion != canonica.especializacion ||
        cruda.rasterizado != canonica.rasterizado || cruda.relleno != canonica.relleno ||
        cruda.relleno2 != canonica.relleno2 ||
        std::memcmp(cruda.formatos, canonica.formatos, sizeof(cruda.formatos)) != 0) {
      campo = "shaders, entrada, topologia, especializacion, rasterizado o formatos";
    } else {
      EstadoFijoPipeline a;
      EstadoFijoPipeline b;
      RellenarEstadoFijo(cruda, a);
      RellenarEstadoFijo(canonica, b);
      campo = DiferenciaEfectiva(a, b);
    }
    if (campo) {
      clave_canonica_apagada_ = true;
      clave_canonica_ = false;
      canonica_valida_ = false;
      REXLOG_ERROR("[nativo] C6 clave canonica: DIFERENCIA en {} (comprobacion {}: VS {} PS {}, mezcla {:08X} -> "
                   "{:08X}, mascaras {:04X} -> {:04X}, profundidad {:08X} -> {:08X}). Apagada para el resto de la "
                   "sesion: se busca con la clave de siempre",
                   campo, n, cruda.vs, cruda.ps, cruda.mezcla[0], canonica.mezcla[0], cruda.mascaras,
                   canonica.mascaras, cruda.profundidad, canonica.profundidad);
      return false;
    }
    if (n == kCanonicasAComprobar) {
      NFSMW_INFORME_ANILLO("[nativo] C6 clave canonica: {} claves cambiadas comprobadas campo a campo contra la de "
                           "siempre, 0 diferencias; sigue comprobando 1 de cada 4096",
                           n);
    }
    return true;
  }

  /*
   * The key used to look up a draw's pipeline. The raw one (clave) is still used for everything else: the
   * deferred sky reads its blending (opaco_en_todos) and the counter compares it. Phase 0a: canonical
   * form, remembering the previous draw's (the same raw key gives the same canonical one).
   */
  ClavePipeline ClaveDeBusqueda(const ClavePipeline& clave) {
    ClavePipeline c = clave;
    if (clave_canonica_) {
      if (canonica_valida_ && std::memcmp(&clave, &canonica_cruda_, sizeof(clave)) == 0) {
        c = canonica_resultado_;
      } else {
        Canonizar(c);
        if (std::memcmp(&c, &clave, sizeof(c)) != 0) {
          const uint64_t n = ++canonica_cambiadas_;
          if ((n <= kCanonicasAComprobar || (n & 4095) == 0) && !ComprobarClaveCanonica(clave, c, n)) {
            c = clave;  // the guard saw a difference: this draw, and the rest of the session, use the usual one
          }
        }
        canonica_cruda_ = clave;
        canonica_resultado_ = c;
        canonica_valida_ = clave_canonica_;
      }
    }
    // Phases 1 and 2. What goes through vkCmdSet* leaves the key, and the flag (relleno2) keeps these
    // pipelines apart from the usual ones in the map, in the direct-mapped cache and in PipelineDe's
    // shortcut.
    const uint32_t modo_eds = eds_modo_;
    if (modo_eds & kEds12) {
      c.profundidad = 0;
      c.rasterizado = 0;
      c.topologia = RepresentanteTopologia(c.topologia);
    }
    if (modo_eds & kEds3) {  // phase 2: blending and masks go through vkCmdSet*; targets stay in formatos
      std::fill(std::begin(c.mezcla), std::end(c.mezcla), 0u);
      c.mascaras = 0;
    }
    c.relleno2 = modo_eds;
    return c;
  }

  // Every 20 s, phase 0a (nfsmw_nativo_clave_canonica).
  void InformeClaveCanonica() {
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - canonica_informe_ < std::chrono::seconds(20)) {
      return;
    }
    const bool primera = canonica_informe_ == std::chrono::steady_clock::time_point{};
    canonica_informe_ = ahora;
    const uint64_t cambiadas = canonica_cambiadas_ - canonica_cambiadas_previas_;
    canonica_cambiadas_previas_ = canonica_cambiadas_;
    const uint64_t pipelines = pipelines_.size();
    const uint64_t nuevos = pipelines - std::min<uint64_t>(pipelines, canonica_pipelines_previos_);
    canonica_pipelines_previos_ = pipelines;
    if (primera || (!cambiadas && !nuevos)) {
      return;
    }
    NFSMW_INFORME_ANILLO("[nativo] C6 clave canonica (build 184): {}; {} claves cambiadas en 20 s ({} comprobadas "
                         "desde el principio, {}); {} pipelines en el mapa, {} nuevos en 20 s",
                         clave_canonica_apagada_ ? "APAGADA por la guardia"
                         : clave_canonica_       ? "encendida"
                                                 : "apagada",
                         cambiadas, canonica_comprobadas_,
                         canonica_comprobadas_ >= kCanonicasAComprobar ? "guardia superada" : "comprobando", pipelines,
                         nuevos);
  }

  // Every 20 s, phase 0b (nfsmw_nativo_pipeline_entre_pases).
  void InformePipelineEntrePases() {
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - entre_pases_informe_ < std::chrono::seconds(20)) {
      return;
    }
    const bool primera = entre_pases_informe_ == std::chrono::steady_clock::time_point{};
    entre_pases_informe_ = ahora;
    const uint64_t pases = pases_empezados_ - entre_pases_pases_previos_;
    const uint64_t conservados = pases_con_pipeline_ - entre_pases_conservados_previos_;
    entre_pases_pases_previos_ = pases_empezados_;
    entre_pases_conservados_previos_ = pases_con_pipeline_;
    if (primera || !pases) {
      return;
    }
    NFSMW_INFORME_ANILLO("[nativo] C6 pipeline entre pases (build 184): {}; {} pases empezados en 20 s, {} con un "
                         "pipeline enlazado que se conserva (su primer dibujo no vuelve a enlazar si la clave es la "
                         "misma)",
                         pipeline_entre_pases_ ? "se conserva" : "se olvida, como antes", pases, conservados);
  }

  /*
   * Dynamic state phases 1 and 2. The state that leaves the pipeline, with the Vulkan values the usual
   * pipeline would carry (the criterion of RellenarEstadoFijo, which is PipelineDe's).
   */
  struct EstadoEds {
    uint32_t cara = 0;         // VkCullModeFlags
    uint32_t frente = 0;       // VkFrontFace
    uint32_t topologia = 0;    // VkPrimitiveTopology
    uint32_t reinicio = 0;     // primitiveRestartEnable
    uint32_t sesgo = 0;        // depthBiasEnable
    uint32_t prueba_z = 0;     // depthTestEnable
    uint32_t escribe_z = 0;    // depthWriteEnable
    uint32_t funcion_z = 0;    // VkCompareOp
    uint32_t estencil = 0;     // stencilTestEnable
    uint32_t delante[4] = {};  // failOp, passOp, depthFailOp (VkStencilOp) y compareOp (VkCompareOp)
    uint32_t detras[4] = {};   // the same for the back face
    uint32_t n_colores = 0;                    // phase 2: the pass's color targets, compacted as in PipelineDe
    VkBool32 mezcla_activa[4] = {};            // blendEnable
    VkColorBlendEquationEXT ecuacion[4] = {};  // color and alpha factors and operations
    VkColorComponentFlags mascara[4] = {};     // colorWriteMask
  };

  // Without EDS3 the dynamic topology must be of the same class as the pipeline's: the lookup key carries
  // one per class. Classes the ring does not draw stay as they are.
  static uint32_t RepresentanteTopologia(uint32_t topologia) {
    switch (ClaseTopologia(topologia)) {
      case 1:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      case 2:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      default:
        return topologia;
    }
  }

  static const char* NombreModoEds(uint32_t modo) {
    switch (modo & (kEds12 | kEds3)) {
      case kEds12:
        return "EDS1/EDS2 (fase 1)";
      case kEds3:
        return "EDS3 (fase 2)";
      case kEds12 | kEds3:
        return "EDS1/EDS2 y EDS3 (fases 1 y 2)";
      default:
        return "apagado: todo el estado en el pipeline, como antes";
    }
  }

  // The dynamic state of a raw key in the given mode (kEds12 | kEds3).
  void EstadoEdsDe(const ClavePipeline& c, EstadoEds& e, uint32_t modo) {
    e = EstadoEds{};
    if (modo & kEds12) {
      e.cara = ((c.rasterizado & 0x1) ? uint32_t(VK_CULL_MODE_FRONT_BIT) : 0u) |
               ((c.rasterizado & 0x2) ? uint32_t(VK_CULL_MODE_BACK_BIT) : 0u);
      e.frente = (c.rasterizado & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      e.reinicio = (c.rasterizado & 0x8) ? 1u : 0u;
      e.sesgo = (c.rasterizado & 0x10) ? 1u : 0u;
      e.topologia = c.topologia;
      if (c.formatos[4]) {  // without a depth target everything stays zero, as in RellenarEstadoFijo
        const uint32_t d = c.profundidad;
        e.prueba_z = (d >> 1) & 0x1;
        e.escribe_z = ((d >> 1) & 0x1) & ((d >> 2) & 0x1);
        e.funcion_z = (d >> 4) & 0x7;
        e.estencil = d & 0x1;
        e.delante[0] = (d >> 11) & 0x7;
        e.delante[1] = (d >> 14) & 0x7;
        e.delante[2] = (d >> 17) & 0x7;
        e.delante[3] = (d >> 8) & 0x7;
        if ((d >> 7) & 0x1) {
          e.detras[0] = (d >> 23) & 0x7;
          e.detras[1] = (d >> 26) & 0x7;
          e.detras[2] = (d >> 29) & 0x7;
          e.detras[3] = (d >> 20) & 0x7;
        } else {
          std::memcpy(e.detras, e.delante, sizeof(e.detras));
        }
      }
    }
    if (modo & kEds3) {  // phase 2: like RellenarEstadoFijo, target by target and compacted
      for (uint32_t i = 0; i < 4; ++i) {
        if (!c.formatos[i]) {
          continue;
        }
        const uint32_t a = e.n_colores++;
        const uint32_t m = c.mezcla[i];
        const uint32_t mascara = (c.mascaras >> (i * 4)) & 0xF;
        const uint32_t fuente = m & 0x1F, op = (m >> 5) & 0x7, destino = (m >> 8) & 0x1F;
        const uint32_t fuente_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7, destino_a = (m >> 24) & 0x1F;
        const bool color_directo = fuente == 1 && destino == 0 && op == 0;
        const bool alfa_directo = fuente_a == 1 && destino_a == 0 && op_a == 0;
        e.mascara[a] = mascara;
        e.mezcla_activa[a] =
            (((mascara & 0x7) != 0 && !color_directo) || ((mascara & 0x8) != 0 && !alfa_directo)) ? VK_TRUE : VK_FALSE;
        VkColorBlendEquationEXT& q = e.ecuacion[a];
        q.srcColorBlendFactor = kFactoresMezcla[fuente];
        q.dstColorBlendFactor = kFactoresMezcla[destino];
        q.colorBlendOp = OperacionMezcla(op);
        q.srcAlphaBlendFactor = kFactoresMezcla[fuente_a];
        q.dstAlphaBlendFactor = kFactoresMezcla[destino_a];
        q.alphaBlendOp = OperacionMezcla(op_a);
      }
    }
  }

  /*
   * Records the vkCmdSet* calls for what changed since the last state set in the buffer (eds_grabado_),
   * or for everything with todo = true, and tracks it. What Vulkan ignores in this draw (the Z function
   * without a Z test, stencil operations without stencil, the blend equation without blending) is not
   * re-recorded if only that changed; with todo = true it is recorded anyway, so everything is set at least
   * once in the buffer.
   */
  void EmitirEstadoDinamico(VkCommandBuffer cmd, const EstadoEds& d, bool todo, uint32_t modo) {
    EstadoEds& g = eds_grabado_;
    eds_llamadas_[kEdsTodo] += todo ? 1 : 0;
    if (modo & kEds12) {
      if (todo || d.cara != g.cara) {
        set_cara_(cmd, VkCullModeFlags(d.cara));
        g.cara = d.cara;
        ++eds_llamadas_[kEdsCara];
      }
      if (todo || d.frente != g.frente) {
        set_frente_(cmd, VkFrontFace(d.frente));
        g.frente = d.frente;
        ++eds_llamadas_[kEdsFrente];
      }
      if (todo || d.topologia != g.topologia) {
        set_topologia_(cmd, VkPrimitiveTopology(d.topologia));
        g.topologia = d.topologia;
        ++eds_llamadas_[kEdsTopologia];
      }
      if (todo || d.reinicio != g.reinicio) {
        set_reinicio_(cmd, VkBool32(d.reinicio));
        g.reinicio = d.reinicio;
        ++eds_llamadas_[kEdsReinicio];
      }
      if (todo || d.sesgo != g.sesgo) {
        set_sesgo_(cmd, VkBool32(d.sesgo));
        g.sesgo = d.sesgo;
        ++eds_llamadas_[kEdsSesgo];
      }
      if (todo || d.prueba_z != g.prueba_z) {
        set_prueba_z_(cmd, VkBool32(d.prueba_z));
        g.prueba_z = d.prueba_z;
        ++eds_llamadas_[kEdsPruebaZ];
      }
      if (todo || d.escribe_z != g.escribe_z) {
        set_escribe_z_(cmd, VkBool32(d.escribe_z));
        g.escribe_z = d.escribe_z;
        ++eds_llamadas_[kEdsEscribeZ];
      }
      if (todo || (d.prueba_z && d.funcion_z != g.funcion_z)) {
        set_funcion_z_(cmd, VkCompareOp(d.funcion_z));
        g.funcion_z = d.funcion_z;
        ++eds_llamadas_[kEdsFuncionZ];
      }
      if (todo || d.estencil != g.estencil) {
        set_estencil_(cmd, VkBool32(d.estencil));
        g.estencil = d.estencil;
        ++eds_llamadas_[kEdsEstencil];
      }
      const bool delante = todo || (d.estencil && std::memcmp(d.delante, g.delante, sizeof(d.delante)) != 0);
      const bool detras = todo || (d.estencil && std::memcmp(d.detras, g.detras, sizeof(d.detras)) != 0);
      if (delante && detras && std::memcmp(d.delante, d.detras, sizeof(d.delante)) == 0) {
        set_estencil_ops_(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VkStencilOp(d.delante[0]), VkStencilOp(d.delante[1]),
                          VkStencilOp(d.delante[2]), VkCompareOp(d.delante[3]));
        ++eds_llamadas_[kEdsEstencilOps];
      } else {
        if (delante) {
          set_estencil_ops_(cmd, VK_STENCIL_FACE_FRONT_BIT, VkStencilOp(d.delante[0]), VkStencilOp(d.delante[1]),
                            VkStencilOp(d.delante[2]), VkCompareOp(d.delante[3]));
          ++eds_llamadas_[kEdsEstencilOps];
        }
        if (detras) {
          set_estencil_ops_(cmd, VK_STENCIL_FACE_BACK_BIT, VkStencilOp(d.detras[0]), VkStencilOp(d.detras[1]),
                            VkStencilOp(d.detras[2]), VkCompareOp(d.detras[3]));
          ++eds_llamadas_[kEdsEstencilOps];
        }
      }
      if (delante) {
        std::memcpy(g.delante, d.delante, sizeof(g.delante));
      }
      if (detras) {
        std::memcpy(g.detras, d.detras, sizeof(g.detras));
      }
    }
    if (modo & kEds3) {  // fase 2
      const uint32_t n = d.n_colores;
      if (!n) {
        if (todo) {
          g.n_colores = 0;  // nothing set in this buffer: the first draw with color will set it all
        }
      } else {
        const bool otro_n = todo || n != g.n_colores;
        bool activa = otro_n;
        bool ecuacion = otro_n;
        bool mascara = otro_n;
        for (uint32_t a = 0; a < n && a < 4; ++a) {
          activa = activa || d.mezcla_activa[a] != g.mezcla_activa[a];
          mascara = mascara || d.mascara[a] != g.mascara[a];
          if (!ecuacion && d.mezcla_activa[a]) {  // without blending the equation does not count; each half, with its channels
            const VkColorBlendEquationEXT& x = d.ecuacion[a];
            const VkColorBlendEquationEXT& y = g.ecuacion[a];
            ecuacion = ((d.mascara[a] & 0x7) != 0 &&
                        (x.srcColorBlendFactor != y.srcColorBlendFactor ||
                         x.dstColorBlendFactor != y.dstColorBlendFactor || x.colorBlendOp != y.colorBlendOp)) ||
                       ((d.mascara[a] & 0x8) != 0 &&
                        (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor ||
                         x.dstAlphaBlendFactor != y.dstAlphaBlendFactor || x.alphaBlendOp != y.alphaBlendOp));
          }
        }
        if (activa) {
          set_mezcla_activa_(cmd, 0, n, d.mezcla_activa);
          std::memcpy(g.mezcla_activa, d.mezcla_activa, sizeof(g.mezcla_activa));
          ++eds_llamadas_[kEdsMezclaActiva];
        }
        if (ecuacion) {
          set_ecuacion_(cmd, 0, n, d.ecuacion);
          std::memcpy(g.ecuacion, d.ecuacion, sizeof(g.ecuacion));
          ++eds_llamadas_[kEdsEcuacion];
        }
        if (mascara) {
          set_mascara_(cmd, 0, n, d.mascara);
          std::memcpy(g.mascara, d.mascara, sizeof(g.mascara));
          ++eds_llamadas_[kEdsMascara];
        }
        g.n_colores = n;
      }
    }
  }

  /*
   * The guard of phases 1 and 2. What the ring believes is set in the buffer (eds_grabado_) must be what
   * this draw's usual pipeline would carry. That state comes from RellenarEstadoFijo with the raw key, the
   * same code PipelineDe uses to create the usual pipelines (not another copy of EstadoEdsDe), and it is
   * compared field by field in what Vulkan looks at. On a difference dynamic state switches off for the
   * session (both phases) and false is returned: the draw has to use its usual pipeline. Limit: it cannot
   * see the GPU, only that the computations and the record of what was recorded agree.
   */
  bool ComprobarEstadoDinamico(const ClavePipeline& clave, uint64_t n) {
    ++eds_comprobados_;
    EstadoFijoPipeline fijo;
    RellenarEstadoFijo(clave, fijo);
    const EstadoEds& g = eds_grabado_;
    const char* campo = nullptr;
    uint32_t fijado = 0;
    uint32_t esperado = 0;
    const auto mirar = [&](const char* nombre, uint32_t a, uint32_t b) {
      if (!campo && a != b) {
        campo = nombre;
        fijado = a;
        esperado = b;
      }
    };
    if (eds_modo_ & kEds12) {
      const VkPipelineRasterizationStateCreateInfo& r = fijo.rasterizado;
      const VkPipelineDepthStencilStateCreateInfo& z = fijo.profundidad;
      mirar("cullMode", g.cara, r.cullMode);
      mirar("frontFace", g.frente, r.frontFace);
      mirar("topology", g.topologia, fijo.ensamblado.topology);
      mirar("primitiveRestartEnable", g.reinicio, fijo.ensamblado.primitiveRestartEnable);
      mirar("depthBiasEnable", g.sesgo, r.depthBiasEnable);
      mirar("depthTestEnable", g.prueba_z, z.depthTestEnable);
      mirar("depthWriteEnable", g.escribe_z, z.depthWriteEnable);
      if (z.depthTestEnable) {
        mirar("depthCompareOp", g.funcion_z, z.depthCompareOp);
      }
      mirar("stencilTestEnable", g.estencil, z.stencilTestEnable);
      if (z.stencilTestEnable) {
        mirar("front.failOp", g.delante[0], z.front.failOp);
        mirar("front.passOp", g.delante[1], z.front.passOp);
        mirar("front.depthFailOp", g.delante[2], z.front.depthFailOp);
        mirar("front.compareOp", g.delante[3], z.front.compareOp);
        mirar("back.failOp", g.detras[0], z.back.failOp);
        mirar("back.passOp", g.detras[1], z.back.passOp);
        mirar("back.depthFailOp", g.detras[2], z.back.depthFailOp);
        mirar("back.compareOp", g.detras[3], z.back.compareOp);
      }
    }
    if ((eds_modo_ & kEds3) && fijo.n_colores) {  // phase 2 (without color targets there is nothing to check)
      mirar("attachmentCount", g.n_colores, fijo.n_colores);
      for (uint32_t a = 0; a < fijo.n_colores && a < 4; ++a) {
        const VkPipelineColorBlendAttachmentState& s = fijo.mezclas[a];
        const VkColorBlendEquationEXT& q = g.ecuacion[a];
        mirar("colorWriteMask", g.mascara[a], s.colorWriteMask);
        mirar("blendEnable", g.mezcla_activa[a], s.blendEnable);
        if (s.blendEnable && (s.colorWriteMask & 0x7)) {
          mirar("srcColorBlendFactor", q.srcColorBlendFactor, s.srcColorBlendFactor);
          mirar("dstColorBlendFactor", q.dstColorBlendFactor, s.dstColorBlendFactor);
          mirar("colorBlendOp", q.colorBlendOp, s.colorBlendOp);
        }
        if (s.blendEnable && (s.colorWriteMask & 0x8)) {
          mirar("srcAlphaBlendFactor", q.srcAlphaBlendFactor, s.srcAlphaBlendFactor);
          mirar("dstAlphaBlendFactor", q.dstAlphaBlendFactor, s.dstAlphaBlendFactor);
          mirar("alphaBlendOp", q.alphaBlendOp, s.alphaBlendOp);
        }
      }
    }
    if (campo) {
      eds_apagado_ = true;
      eds_modo_ = 0;
      eds_valido_ = false;
      pipeline_enlazado_ = VK_NULL_HANDLE;
      REXLOG_ERROR("[nativo] C6 estado dinamico: DIFERENCIA en {} (comprobacion {}: fijado {} y el pipeline de siempre "
                   "llevaria {}; VS {} PS {}, topologia {}, profundidad {:08X}, rasterizado {:02X}, mascaras {:04X}). "
                   "Apagado para el resto de la sesion: vuelven los pipelines con todo el estado fijo",
                   campo, n, fijado, esperado, clave.vs, clave.ps, clave.topologia, clave.profundidad,
                   clave.rasterizado, clave.mascaras);
      return false;
    }
    if (n == kEdsAComprobar) {
      NFSMW_INFORME_ANILLO("[nativo] C6 estado dinamico: {} cambios de clave comprobados contra el estado de su "
                           "pipeline de siempre, 0 diferencias; sigue comprobando 1 de cada 4096",
                           n);
    }
    return true;
  }

  /*
   * Phases 1 and 2, before every draw with dynamic state. With the same raw key as the last state set in
   * this buffer there is nothing to do; otherwise what changed is set and the guard checks it for the
   * first 200,000 changes and then 1 in 4,096. Returns false if the guard has switched dynamic state off.
   */
  bool FijarEstadoDinamico(VkCommandBuffer cmd, const ClavePipeline& clave) {
    ++eds_dibujos_;
    if (eds_valido_ && std::memcmp(&clave, &eds_clave_, sizeof(clave)) == 0) {
      ++eds_repetidos_;
      return true;
    }
    EstadoEds d;
    EstadoEdsDe(clave, d, eds_modo_);
    EmitirEstadoDinamico(cmd, d, !eds_valido_, eds_modo_);
    eds_clave_ = clave;
    eds_valido_ = true;
    const uint64_t n = ++eds_comprobables_;
    if (n <= kEdsAComprobar || (n & 4095) == 0) {
      return ComprobarEstadoDinamico(clave, n);
    }
    return true;
  }

  /*
   * Dynamic state phases 1 and 2. The EDS1/EDS2 vkCmdSet* functions are core in Vulkan 1.3 and the SDK's
   * table does not load them: they are requested from the driver, like vkCmdCopyImage. With an API below
   * 1.3 or with any of them missing, phase 1 is not used for the whole session and one log line says so.
   */
  void CargarEstadoDinamico() {
    const auto& ifn = dispositivo_->vulkan_instance()->functions();
    const auto pedir = [&](const char* nombre) { return ifn.vkGetDeviceProcAddr(device_, nombre); };
    set_cara_ = reinterpret_cast<PFN_vkCmdSetCullMode>(pedir("vkCmdSetCullMode"));
    set_frente_ = reinterpret_cast<PFN_vkCmdSetFrontFace>(pedir("vkCmdSetFrontFace"));
    set_topologia_ = reinterpret_cast<PFN_vkCmdSetPrimitiveTopology>(pedir("vkCmdSetPrimitiveTopology"));
    set_reinicio_ = reinterpret_cast<PFN_vkCmdSetPrimitiveRestartEnable>(pedir("vkCmdSetPrimitiveRestartEnable"));
    set_sesgo_ = reinterpret_cast<PFN_vkCmdSetDepthBiasEnable>(pedir("vkCmdSetDepthBiasEnable"));
    set_prueba_z_ = reinterpret_cast<PFN_vkCmdSetDepthTestEnable>(pedir("vkCmdSetDepthTestEnable"));
    set_escribe_z_ = reinterpret_cast<PFN_vkCmdSetDepthWriteEnable>(pedir("vkCmdSetDepthWriteEnable"));
    set_funcion_z_ = reinterpret_cast<PFN_vkCmdSetDepthCompareOp>(pedir("vkCmdSetDepthCompareOp"));
    set_estencil_ = reinterpret_cast<PFN_vkCmdSetStencilTestEnable>(pedir("vkCmdSetStencilTestEnable"));
    set_estencil_ops_ = reinterpret_cast<PFN_vkCmdSetStencilOp>(pedir("vkCmdSetStencilOp"));
    const uint32_t api = dispositivo_->properties().apiVersion;
    eds12_disponible_ = api >= VK_MAKE_API_VERSION(0, 1, 3, 0) && set_cara_ && set_frente_ && set_topologia_ &&
                        set_reinicio_ && set_sesgo_ && set_prueba_z_ && set_escribe_z_ && set_funcion_z_ &&
                        set_estencil_ && set_estencil_ops_;
    REXLOG_INFO("[nativo] C6 estado dinamico (build 184): EDS1/EDS2 {} (API del dispositivo {}.{}.{})",
                eds12_disponible_ ? "disponible (nucleo 1.3)" : "NO disponible: la fase 1 no se usa",
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
#if defined(REX_UI_VULKAN_ESTADO_DINAMICO3)
    // Phase 2. The driver only provides these three if the SDK enabled the extension; the SDK records the
    // three features in its properties (ui_vulkan_estado_dinamico3.patch).
    set_mezcla_activa_ = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(pedir("vkCmdSetColorBlendEnableEXT"));
    set_ecuacion_ = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(pedir("vkCmdSetColorBlendEquationEXT"));
    set_mascara_ = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(pedir("vkCmdSetColorWriteMaskEXT"));
    const auto& p3 = dispositivo_->properties();
    eds3_disponible_ = dispositivo_->extensions().ext_EXT_extended_dynamic_state3 &&
                       p3.extendedDynamicState3ColorBlendEnable && p3.extendedDynamicState3ColorBlendEquation &&
                       p3.extendedDynamicState3ColorWriteMask && set_mezcla_activa_ && set_ecuacion_ && set_mascara_;
    REXLOG_INFO("[nativo] C6 estado dinamico (build 184): EDS3 (mezcla, ecuacion y mascara de color) {}",
                eds3_disponible_ ? "disponible"
                                 : "NO disponible (el dispositivo no la da o no esta habilitada): la fase 2 no se usa");
#else
    REXLOG_INFO("[nativo] C6 estado dinamico (build 184): EDS3 NO disponible: este SDK no habilita "
                "VK_EXT_extended_dynamic_state3 (falta ui_vulkan_estado_dinamico3.patch); la fase 2 no se usa");
#endif
  }

  // Every 20 s, dynamic state phases 1 and 2.
  void InformeEstadoDinamico() {
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - eds_informe_ < std::chrono::seconds(20)) {
      return;
    }
    const bool primera = eds_informe_ == std::chrono::steady_clock::time_point{};
    eds_informe_ = ahora;
    const std::array<uint64_t, kEdsN> n = eds_llamadas_;
    eds_llamadas_.fill(0);
    const uint64_t dibujos = eds_dibujos_ - eds_dibujos_previos_;
    const uint64_t repetidos = eds_repetidos_ - eds_repetidos_previos_;
    const uint64_t fotogramas = fotograma_ - eds_fotogramas_previos_;
    eds_dibujos_previos_ = eds_dibujos_;
    eds_repetidos_previos_ = eds_repetidos_;
    eds_fotogramas_previos_ = fotograma_;
    if (primera || !dibujos) {
      return;
    }
    uint64_t llamadas = 0;
    for (size_t k = 0; k < kEdsN; ++k) {
      llamadas += k == kEdsTodo ? 0 : n[k];
    }
    NFSMW_INFORME_ANILLO(
        "[nativo] C6 estado dinamico (build 184): {}; {} dibujos en 20 s ({:.0f} por fotograma; {} repiten la clave "
        "del anterior) y {} vkCmdSet* ({:.3f} por dibujo; {} veces todo: bufer nuevo, cielo o pase que olvida el "
        "pipeline) | cara {}, frente {}, topologia {}, reinicio {}, sesgo {}, prueba Z {}, escritura Z {}, funcion Z "
        "{}, estencil {}, operaciones de estencil {} | mezcla {}, ecuacion {}, mascaras {} | pipelines creados: {} de "
        "siempre, {} EDS1/EDS2, {} EDS3, {} con los dos | guardia: {} comprobados ({})",
        eds_apagado_ ? "APAGADO por la guardia" : NombreModoEds(eds_modo_), dibujos,
        fotogramas ? double(dibujos) / double(fotogramas) : 0.0, repetidos, llamadas,
        double(llamadas) / double(dibujos), n[kEdsTodo], n[kEdsCara], n[kEdsFrente], n[kEdsTopologia],
        n[kEdsReinicio], n[kEdsSesgo], n[kEdsPruebaZ], n[kEdsEscribeZ], n[kEdsFuncionZ], n[kEdsEstencil],
        n[kEdsEstencilOps], n[kEdsMezclaActiva], n[kEdsEcuacion], n[kEdsMascara], pipelines_por_modo_[0],
        pipelines_por_modo_[kEds12], pipelines_por_modo_[kEds3], pipelines_por_modo_[kEds12 | kEds3],
        eds_comprobados_, eds_comprobados_ >= kEdsAComprobar ? "guardia superada" : "comprobando");
  }

  // Phase 2. The blend operation of a register field, like the lambda in RellenarEstadoFijo.
  VkBlendOp OperacionMezcla(uint32_t op) {
    switch (op) {
      case 1:
        return VK_BLEND_OP_SUBTRACT;
      case 2:
        return VK_BLEND_OP_MIN;
      case 3:
        return VK_BLEND_OP_MAX;
      case 4:
        Avisar(52, "mezcla con resta inversa: se usa resta");
        return VK_BLEND_OP_SUBTRACT;
      default:
        return VK_BLEND_OP_ADD;
    }
  }

  VkPipeline PipelineDe(const ClavePipeline& clave, const EntradaVertices& entrada,
                        const PeticionDibujo& p) {
    /*
     * One-entry shortcut.
     *
     * There are 105 pipelines in a whole race and 2,345 draws per frame, so consecutive draws almost always
     * repeat the key. The full lookup is an XXH3 of 80 bytes + an integer division by libstdc++'s prime
     * bucket count + two pointer hops (bucket and node, ~96 B) = 2-3 cache misses, all to end up comparing
     * the same 80 bytes this compares. Here there is just one memcmp on memory that is already hot.
     */
    if (ultima_clave_valida_ && std::memcmp(&ultima_clave_, &clave, sizeof(clave)) == 0) {
      return ultima_pipeline_;
    }
    const uint64_t huella = XXH3_64bits(&clave, sizeof(clave));
    // The direct-mapped cache (nfsmw_nativo_pipelines_directa) before the map.
    CasillaPipeline& casilla = casillas_pipeline_[huella & (kCasillasPipeline - 1)];
    if (pipelines_directa_ && casilla.pipeline != VK_NULL_HANDLE &&
        std::memcmp(&casilla.clave, &clave, sizeof(clave)) == 0) {
      const uint64_t n = ++pipelines_directa_aciertos_;
      if (!(n <= kPipelinesAComprobar || (n & 4095) == 0) ||
          ComprobarCasillaPipeline(huella, clave, casilla.pipeline, n)) {
        ultima_clave_ = clave;
        ultima_pipeline_ = casilla.pipeline;
        ultima_clave_valida_ = true;
        return casilla.pipeline;
      }
      // the guard saw a difference: this draw goes through the map, as before
    }
    if (pipelines_directa_) {
      ++pipelines_directa_fallos_;
    }
    if (const auto it = pipelines_.find(huella); it != pipelines_.end()) {
      if (std::memcmp(&it->second.first, &clave, sizeof(clave)) == 0) {
        ultima_clave_ = clave;
        ultima_pipeline_ = it->second.second;
        ultima_clave_valida_ = true;
        if (pipelines_directa_) {  // the slot keeps the map's pair
          casilla.clave = clave;
          casilla.pipeline = it->second.second;
        }
        return it->second.second;
      }
      Avisar(50, "colision de huellas de pipeline");
      return VK_NULL_HANDLE;
    }
    const VkShaderModule vs = ModuloDe(*p.vs);
    VkShaderModule ps = VK_NULL_HANDLE;
    if (clave.ps && (clave.especializacion & kSpecResplandorNatural)) {
      ps = ModuloVariante(1, kSpirvResplandorEnergia, sizeof(kSpirvResplandorEnergia));
    } else if (clave.ps && (clave.especializacion & kSpecResplandorSuave)) {
      ps = ModuloVariante(2, kSpirvResplandorSuave, sizeof(kSpirvResplandorSuave));
    } else if (clave.ps && (clave.especializacion & kSpecSoloAlfa)) {
      ps = ModuloSoloAlfa(*p.ps);
    } else if (clave.ps && (clave.especializacion & kSpecZTemprana)) {
      ps = ModuloZTemprana(*p.ps);
    } else if (clave.ps) {
      ps = ModuloDe(*p.ps);
    }
    if (vs == VK_NULL_HANDLE || (clave.ps && ps == VK_NULL_HANDLE)) {
      Rechazar(51, "no se pudo crear un modulo de shader");
      return VK_NULL_HANDLE;
    }
    // The VkGraphicsPipelineCreateInfo lives in CrearPipelineVulkan, the same function the prewarm uses from
    // its thread; what surrounds it stays here, as it was.
    uint32_t n_colores = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto inicio_creacion = std::chrono::steady_clock::now();
    if (CrearPipelineVulkan(clave, entrada, vs, ps, pase_rp_, true, pipeline, n_colores) != VK_SUCCESS) {
      Rechazar(53, "no se pudo crear un pipeline");
      pipeline = VK_NULL_HANDLE;
    } else {
      ++pipelines_sin_guardar_;
    }
    const uint64_t ns_creacion = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - inicio_creacion)
                                              .count());
    ns_pipelines_ += ns_creacion;
    AnotarPipelineCreado(clave, entrada, p, pipeline, ns_creacion);  // List and measurement
    pipelines_.emplace(huella, std::make_pair(clave, pipeline));
    ++pipelines_por_modo_[clave.relleno2 & 3];  // Dynamic state report
    if (pipelines_directa_ && pipeline != VK_NULL_HANDLE) {  // the same pair as the map
      casilla.clave = clave;
      casilla.pipeline = pipeline;
    }
    if (pipelines_.size() <= 64) {
      REXLOG_INFO("[nativo] C6: pipeline {} (VS n{} PS n{}, topologia {}, {} colores, "
                  "profundidad {:08X}, mezcla {:08X}, especializacion {})",
                  pipelines_.size(), p.vs->numero, clave.ps ? int(p.ps->numero) : -1,
                  clave.topologia, n_colores,
                  clave.profundidad, clave.mezcla[0], clave.especializacion);
    }
    return pipeline;
  }

  /*
   * The Vulkan pipeline for a key. It is PipelineDe's usual code, moved here without changes except for
   * the render pass (now an argument) and avisar, so the prewarm can create from its thread exactly the
   * same VkGraphicsPipelineCreateInfo as the ring. It only reads its arguments, layout_pipeline_ and the
   * pipeline cache, which Vulkan synchronizes internally; avisar = false outside the ring.
   */
  VkResult CrearPipelineVulkan(const ClavePipeline& clave, const EntradaVertices& entrada, VkShaderModule vs,
                               VkShaderModule ps, VkRenderPass pase, bool avisar, VkPipeline& pipeline,
                               uint32_t& n_colores_salida) {
    const VkSpecializationMapEntry mapa{0, 0, sizeof(uint32_t)};
    const VkSpecializationInfo especializacion{1, &mapa, sizeof(uint32_t), &clave.especializacion};
    VkPipelineShaderStageCreateInfo etapas[2]{};
    for (auto& etapa : etapas) {
      etapa.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      etapa.pName = "main";
      etapa.pSpecializationInfo = &especializacion;
    }
    etapas[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    etapas[0].module = vs;
    etapas[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    etapas[1].module = ps;

    std::vector<VkVertexInputBindingDescription> enlaces;
    for (uint32_t i = 0; i < entrada.enlaces.size(); ++i) {
      enlaces.push_back({i, entrada.enlaces[i].zancada, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> atributos;
    for (const AtributoVertices& a : entrada.atributos) {
      atributos.push_back({a.ubicacion, a.enlace, a.formato, a.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertices{};
    vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertices.vertexBindingDescriptionCount = uint32_t(enlaces.size());
    vertices.pVertexBindingDescriptions = enlaces.data();
    vertices.vertexAttributeDescriptionCount = uint32_t(atributos.size());
    vertices.pVertexAttributeDescriptions = atributos.data();

    // The fixed state is filled in by RellenarEstadoFijo: it is the usual code, moved there without
    // changing any computation, so the guards of the canonical key and of dynamic state compare with what
    // really goes into the VkGraphicsPipelineCreateInfo.
    EstadoFijoPipeline fijo;
    RellenarEstadoFijo(clave, fijo, avisar);  // notify
    const VkPipelineInputAssemblyStateCreateInfo& ensamblado = fijo.ensamblado;
    const VkPipelineViewportStateCreateInfo& vista = fijo.vista;
    const VkPipelineRasterizationStateCreateInfo& rasterizado = fijo.rasterizado;
    const VkPipelineMultisampleStateCreateInfo& muestreo = fijo.muestreo;
    const VkPipelineDepthStencilStateCreateInfo& profundidad = fijo.profundidad;
    const VkPipelineColorBlendStateCreateInfo& mezcla = fijo.mezcla;
    const uint32_t n_colores = fijo.n_colores;

    // Dynamic state phases 1 and 2. Their pipelines (flagged in relleno2) also declare as dynamic what is
    // now set with vkCmdSet*; their lookup key already has it zeroed (ClaveDeBusqueda).
    VkDynamicState dinamicos[24] = {
        VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS};
    uint32_t n_dinamicos = 7;
    if (clave.relleno2 & kEds12) {
      for (const VkDynamicState estado : kDinamicosEds12) {
        dinamicos[n_dinamicos++] = estado;
      }
    }
    if (clave.relleno2 & kEds3) {
      for (const VkDynamicState estado : kDinamicosEds3) {
        dinamicos[n_dinamicos++] = estado;
      }
    }
    VkPipelineDynamicStateCreateInfo dinamico{};
    dinamico.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dinamico.dynamicStateCount = n_dinamicos;
    dinamico.pDynamicStates = dinamicos;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = clave.ps ? 2 : 1;  // mode 5: VS only
    info.pStages = etapas;
    info.pVertexInputState = &vertices;
    info.pInputAssemblyState = &ensamblado;
    info.pViewportState = &vista;
    info.pRasterizationState = &rasterizado;
    info.pMultisampleState = &muestreo;
    info.pDepthStencilState = &profundidad;
    info.pColorBlendState = &mezcla;
    info.pDynamicState = &dinamico;
    info.layout = layout_pipeline_;
    info.renderPass = pase;  // the pass's (ring) or a compatible one (prewarm)
    info.basePipelineIndex = -1;
    n_colores_salida = n_colores;
    pipeline = VK_NULL_HANDLE;
    return dfn_.vkCreateGraphicsPipelines(device_, cache_pipelines_, 1, &info, nullptr, &pipeline);
  }

  // --- Pipeline prewarming (nfsmw_nativo_pipelines_precalentar) ---------------------------
  /*
   * Why. A pipeline that is not in the Vulkan cache is compiled on the fly on the ring: 68-159 ms each on
   * the console. In a first race after the library and the key had changed there were 56 (5.2 s of
   * stutter at the start). It happens the first time after any change to the library, the driver or the
   * key, and on a fresh install.
   *
   * How. The ring records every pipeline it creates (AnotarPipelineCreado) and the list is saved with the
   * cache (GuardarCachePipelines). In the next session, as soon as the library is loaded
   * (IntentarPrecalentar), a lowest-priority thread (BuclePrecalentado) walks the list in creation order
   * (the menu first) and recreates each pipeline with CrearPipelineVulkan, the same function the ring
   * uses, with modules from the same SPIR-V (the same variants and fallbacks as PipelineDe) and a
   * compatible render pass (CrearPase, the same formats); then destroys it. The Vulkan cache keeps the
   * compiled shaders, so when the ring asks for it, it comes from the cache (0-2 ms, like the 8 of the
   * menu). The ring changes nothing of what it draws: it only reads two counters of the thread for the
   * report and the guard.
   *
   * Guard. The thread's pipelines are destroyed unused: they cannot change the image. What can fail is
   * that they are not the ones the ring asks for. The report measures how long the ring takes to create
   * the listed pipelines the thread already prewarmed: if at least half of 8 or more take as long as a
   * compiled one (20 ms or more), it writes DIFERENCIA and stops the thread. Records whose shaders are no
   * longer in the library, or from another dynamic state mode, are skipped.
   */
  enum : uint8_t { kListaPendiente = 0, kListaPrecalentado = 1, kListaSinShader = 2, kListaFallido = 3,
                   kListaOtroModo = 4 };
  static constexpr uint64_t kNsCompilado = 5000000;  // more than this: really compiled (a cache hit is 0-2 ms)
  static constexpr uint64_t kNsLento = 20000000;     // the ring creating one already prewarmed: mismatch

  // At start-up (Inicializar): the previous session's list, before the thread exists. It is not a
  // separate file: it is the part of the pipelines file that CargarCachePipelines read.
  void CargarListaPipelines() {
    const std::filesystem::path ruta = RutaFicheroPipelines();
    std::vector<uint8_t> datos = std::move(lista_leida_);
    lista_leida_ = {};
    if (datos.size() < kCabeceraLista ||
        datos.size() > kCabeceraLista + kMaxRegistrosLista * sizeof(RegistroPipeline)) {
      datos.clear();
    }
    const char* motivo = nullptr;
    uint32_t cabecera[4] = {};
    if (datos.empty()) {
      motivo = "no hay lista: se empieza en esta sesion";
    } else {
      std::memcpy(cabecera, datos.data(), sizeof(cabecera));
      if (cabecera[0] != kMagiaListaPipelines || cabecera[1] != kVersionListaPipelines ||
          cabecera[2] != sizeof(RegistroPipeline) ||
          datos.size() != kCabeceraLista + size_t(cabecera[3]) * sizeof(RegistroPipeline)) {
        motivo = "lista de otra version o danada: se empieza de cero";
      }
    }
    if (!motivo) {
      lista_archivo_.reserve(cabecera[3]);
      for (uint32_t i = 0; i < cabecera[3]; ++i) {
        RegistroPipeline r;
        std::memcpy(&r, datos.data() + kCabeceraLista + size_t(i) * sizeof(RegistroPipeline), sizeof(r));
        if (r.n_atributos > RegistroPipeline::kMaxAtributos || r.n_enlaces > RegistroPipeline::kMaxEnlaces) {
          continue;
        }
        if (indice_lista_.emplace(XXH3_64bits(&r.clave, sizeof(r.clave)), lista_archivo_.size()).second) {
          lista_archivo_.push_back(r);
        }
      }
      escrito_lista_ = std::move(datos);  // the writer thread rewrites it unchanged if only the cache changes
    }
    estado_lista_.assign(lista_archivo_.size(), kListaPendiente);
    REXLOG_INFO("[nativo] C6 precalentado (build 186): {} pipelines en la lista de {}{}{}", lista_archivo_.size(),
                ruta.string(), motivo ? ": " : "", motivo ? motivo : "");
  }

  // Ring only (GuardarCachePipelines): the file's list without the records the thread found missing their
  // shaders, followed by this session's new ones. Above kMaxRegistrosLista the oldest are dropped.
  std::vector<uint8_t> SerializarListaPipelines() const {
    const size_t hasta = precalentado_hasta_.load(std::memory_order_acquire);
    std::vector<const RegistroPipeline*> registros;
    registros.reserve(lista_archivo_.size() + lista_sesion_.size());
    for (size_t i = 0; i < lista_archivo_.size(); ++i) {
      if (i < hasta && estado_lista_[i] == kListaSinShader) {
        continue;
      }
      registros.push_back(&lista_archivo_[i]);
    }
    for (const RegistroPipeline& r : lista_sesion_) {
      registros.push_back(&r);
    }
    if (registros.size() > kMaxRegistrosLista) {
      registros.erase(registros.begin(), registros.begin() + std::ptrdiff_t(registros.size() - kMaxRegistrosLista));
    }
    std::vector<uint8_t> datos(kCabeceraLista + registros.size() * sizeof(RegistroPipeline));
    const uint32_t cabecera[4] = {kMagiaListaPipelines, kVersionListaPipelines, uint32_t(sizeof(RegistroPipeline)),
                                  uint32_t(registros.size())};
    std::memcpy(datos.data(), cabecera, sizeof(cabecera));
    for (size_t i = 0; i < registros.size(); ++i) {
      std::memcpy(datos.data() + kCabeceraLista + i * sizeof(RegistroPipeline), registros[i], sizeof(RegistroPipeline));
    }
    return datos;
  }

  // Ring only (PipelineDe), with every pipeline it creates: the measurement for the report and the guard,
  // and new ones go to the list.
  void AnotarPipelineCreado(const ClavePipeline& clave, const EntradaVertices& entrada, const PeticionDibujo& p,
                            VkPipeline pipeline, uint64_t ns) {
    const uint64_t huella = XXH3_64bits(&clave, sizeof(clave));
    if (const auto it = indice_lista_.find(huella); it != indice_lista_.end()) {
      const size_t j = it->second;
      if (j < lista_archivo_.size() && j < precalentado_hasta_.load(std::memory_order_acquire) &&
          estado_lista_[j] == kListaPrecalentado) {
        ++anillo_precalentados_;
        ns_anillo_precalentados_ += ns;
        if (ns >= kNsLento) {
          ++anillo_precalentados_lentos_;
        }
      } else {
        ++anillo_de_lista_;
        ns_anillo_de_lista_ += ns;
      }
      return;
    }
    ++anillo_nuevos_;
    ns_anillo_nuevos_ += ns;
    if (pipeline == VK_NULL_HANDLE || !p.vs || !p.vs->shader || (clave.ps && (!p.ps || !p.ps->shader)) ||
        entrada.atributos.size() > RegistroPipeline::kMaxAtributos ||
        entrada.enlaces.size() > RegistroPipeline::kMaxEnlaces ||
        lista_archivo_.size() + lista_sesion_.size() >= kMaxRegistrosLista) {
      return;
    }
    RegistroPipeline r;
    r.clave = clave;
    r.huella_vs = p.vs->shader->huella;
    r.huella_ps = clave.ps ? p.ps->shader->huella : 0;
    r.n_atributos = uint32_t(entrada.atributos.size());
    for (uint32_t k = 0; k < r.n_atributos; ++k) {
      const AtributoVertices& a = entrada.atributos[k];
      r.atributos[k] = {a.ubicacion, a.enlace, uint32_t(a.formato), a.offset};
    }
    r.n_enlaces = uint32_t(entrada.enlaces.size());
    for (uint32_t k = 0; k < r.n_enlaces; ++k) {
      r.zancadas[k] = entrada.enlaces[k].zancada;
    }
    indice_lista_.emplace(huella, SIZE_MAX);  // from this session: not recorded again
    lista_sesion_.push_back(r);
    ++lista_sin_guardar_;
    ++anillo_a_lista_;
  }

  // Ring only, on each submission: starts the thread once, as soon as the library is loaded.
  void IntentarPrecalentar() {
    if (precalentado_decidido_) {
      return;
    }
    if (lista_archivo_.empty() || cache_pipelines_ == VK_NULL_HANDLE ||
        !REXCVAR_GET(nfsmw_nativo_pipelines_precalentar)) {
      precalentado_decidido_ = true;
      if (!lista_archivo_.empty()) {
        REXLOG_INFO("[nativo] C6 precalentado (build 186): no se precalienta ({})",
                    cache_pipelines_ == VK_NULL_HANDLE ? "sin cache de pipelines"
                                                       : "nfsmw_nativo_pipelines_precalentar = false");
      }
      return;
    }
    const ShadersNativos* biblioteca = BibliotecaActiva();
    if (!biblioteca || !biblioteca->cargada() || layout_pipeline_ == VK_NULL_HANDLE) {
      return;  // not yet: check again on the next submission
    }
    precalentado_decidido_ = true;
    biblioteca_precalentado_ = biblioteca;
    precalentado_eds_ = eds_modo_;  // the ring would not request those of another dynamic state mode
    precalentado_inicio_ = std::chrono::steady_clock::now();
    try {
      precalentado_hilo_ = std::thread([this] { BuclePrecalentado(); });
      REXLOG_INFO("[nativo] C6 precalentado (build 186): hilo creado para {} pipelines de la lista",
                  lista_archivo_.size());
    } catch (const std::system_error& error) {
      REXLOG_WARN("[nativo] C6 precalentado (build 186): no se pudo crear el hilo ({}); sin precalentar", error.what());
    }
  }

  // The thread. It only reads lista_archivo_, the library, the layout and the cache; it writes
  // estado_lista_[i] before publishing precalentado_hasta_ = i + 1, and its atomic counters. Its modules
  // and render pass are its own and it destroys them when done.
  void BuclePrecalentado() {
    rex::thread::set_current_thread_name("NFSMW precalentado de pipelines");
    int32_t prioridad = -1;
#if REX_PLATFORM_SWITCH
    // The lowest priority the system accepts, and never above the guest's (0x3B): compiling at the priority
    // threads are born with would take the core from the game and the ring. If none is accepted, nothing is
    // compiled.
    for (const int32_t candidata : {0x3F, 0x3E, 0x3D, 0x3C, 0x3B}) {
      if (RexSwitchSetCurrentThreadPriorityOk(int(candidata))) {
        prioridad = candidata;
        break;
      }
    }
    if (prioridad < 0) {
      REXLOG_WARN("[nativo] C6 precalentado (build 186): el sistema no acepta ninguna prioridad de 0x3B a 0x3F: sin "
                  "precalentar");
      precalentado_terminado_.store(true, std::memory_order_release);
      return;
    }
#endif
    precalentado_prioridad_.store(prioridad, std::memory_order_relaxed);
    const ShadersNativos& biblioteca = *biblioteca_precalentado_;
    std::unordered_map<uint64_t, VkShaderModule> modulos;  // (variante << 32) | numero
    std::unordered_map<uint64_t, VkRenderPass> pases;       // by formats
    const auto crear = [&](const uint32_t* spirv, size_t bytes) {
      VkShaderModule modulo = VK_NULL_HANDLE;
      if (CrearModulo(spirv, bytes, &modulo) != VK_SUCCESS) {
        modulo = VK_NULL_HANDLE;
      }
      return modulo;
    };
    // ModuloDe: the library's SPIR-V as is.
    const auto normal = [&](const EntradaShader& e) {
      auto it = modulos.find(e.numero);
      if (it == modulos.end()) {
        it = modulos.emplace(e.numero, crear(e.shader->spirv.data(), e.shader->spirv.size() * sizeof(uint32_t))).first;
      }
      return it->second;
    };
    // The pixel shader PipelineDe would choose, in the same order: the bright pass variants
    // (ModuloVariante), without color writes (ModuloSoloAlfa) and with early tests (ModuloZTemprana); the
    // latter two with the same fallback: if it cannot be pruned or patched, the normal one.
    const auto pixel = [&](const ClavePipeline& clave, const EntradaShader& e) {
      const uint32_t spec = clave.especializacion;
      const uint32_t variante = (spec & kSpecResplandorNatural) ? 1
                                : (spec & kSpecResplandorSuave) ? 2
                                : (spec & kSpecSoloAlfa)        ? 3
                                : (spec & kSpecZTemprana)       ? 4
                                                                : 0;
      if (variante == 0) {
        return normal(e);
      }
      const uint64_t clave_modulo = (uint64_t(variante) << 32) | (variante <= 2 ? 0xFFFFFFFFull : uint64_t(e.numero));
      auto it = modulos.find(clave_modulo);
      if (it == modulos.end()) {
        VkShaderModule modulo = VK_NULL_HANDLE;
        if (variante == 1) {
          modulo = crear(kSpirvResplandorEnergia, sizeof(kSpirvResplandorEnergia));
        } else if (variante == 2) {
          modulo = crear(kSpirvResplandorSuave, sizeof(kSpirvResplandorSuave));
        } else if (variante == 3) {
          uint32_t quitadas = 0;
          const std::vector<uint32_t> podado = PodarEscriturasDeColor(e.shader->spirv, quitadas);
          if (!podado.empty()) {
            modulo = crear(podado.data(), podado.size() * sizeof(uint32_t));
          }
        } else {
          const char* motivo = "";
          const std::vector<uint32_t> parcheado = ConPruebasTempranas(e.shader->spirv, motivo);
          if (!parcheado.empty()) {
            modulo = crear(parcheado.data(), parcheado.size() * sizeof(uint32_t));
          }
        }
        it = modulos.emplace(clave_modulo, modulo).first;
      }
      if (it->second != VK_NULL_HANDLE || variante <= 2) {
        return it->second;
      }
      return normal(e);
    };
    uint32_t hechos = 0, compilados = 0, sin_shader = 0, otro_modo = 0, fallidos = 0;
    uint64_t ns_compilados = 0;
    const size_t n = lista_archivo_.size();
    for (size_t i = 0; i < n; ++i) {
      if (precalentado_parar_.load(std::memory_order_relaxed)) {
        break;
      }
      const RegistroPipeline& r = lista_archivo_[i];
      uint8_t estado = kListaFallido;
      const EntradaShader* vs = r.clave.vs ? biblioteca.PorNumero(r.clave.vs - 1) : nullptr;
      const EntradaShader* ps = r.clave.ps ? biblioteca.PorNumero(r.clave.ps - 1) : nullptr;
      if (!vs || !vs->vertices || !vs->shader || vs->shader->huella != r.huella_vs ||
          (r.clave.ps && (!ps || ps->vertices || !ps->shader || ps->shader->huella != r.huella_ps))) {
        estado = kListaSinShader;
        ++sin_shader;
      } else if (r.clave.relleno2 != precalentado_eds_) {
        estado = kListaOtroModo;
        ++otro_modo;
      } else {
        const VkShaderModule modulo_vs = normal(*vs);
        const VkShaderModule modulo_ps = r.clave.ps ? pixel(r.clave, *ps) : VK_NULL_HANDLE;
        const uint64_t clave_pase = XXH3_64bits(r.clave.formatos, sizeof(r.clave.formatos));
        auto it_pase = pases.find(clave_pase);
        if (it_pase == pases.end()) {
          it_pase = pases.emplace(clave_pase, CrearPase(r.clave.formatos, kCargaLeer)).first;
        }
        EntradaVertices entrada;
        for (uint32_t k = 0; k < r.n_atributos; ++k) {
          const AtributoRegistro& a = r.atributos[k];
          entrada.atributos.push_back({a.ubicacion, a.enlace, VkFormat(a.formato), a.offset});
        }
        for (uint32_t k = 0; k < r.n_enlaces; ++k) {
          entrada.enlaces.push_back({0, r.zancadas[k]});
        }
        if (modulo_vs != VK_NULL_HANDLE && (!r.clave.ps || modulo_ps != VK_NULL_HANDLE) &&
            it_pase->second != VK_NULL_HANDLE) {
          VkPipeline pipeline = VK_NULL_HANDLE;
          uint32_t n_colores = 0;
          const auto t0 = std::chrono::steady_clock::now();
          const VkResult resultado =
              CrearPipelineVulkan(r.clave, entrada, modulo_vs, modulo_ps, it_pase->second, false, pipeline, n_colores);
          const uint64_t ns = uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
          if (resultado == VK_SUCCESS && pipeline != VK_NULL_HANDLE) {
            dfn_.vkDestroyPipeline(device_, pipeline, nullptr);
            estado = kListaPrecalentado;
            ++hechos;
            if (ns >= kNsCompilado) {
              ++compilados;
              ns_compilados += ns;
            }
          } else {
            ++fallidos;
          }
        } else {
          ++fallidos;
        }
      }
      estado_lista_[i] = estado;
      precalentado_hechos_.store(hechos, std::memory_order_relaxed);
      precalentado_compilados_.store(compilados, std::memory_order_relaxed);
      precalentado_ns_compilados_.store(ns_compilados, std::memory_order_relaxed);
      precalentado_sin_shader_.store(sin_shader, std::memory_order_relaxed);
      precalentado_otro_modo_.store(otro_modo, std::memory_order_relaxed);
      precalentado_fallidos_.store(fallidos, std::memory_order_relaxed);
      precalentado_hasta_.store(i + 1, std::memory_order_release);
    }
    for (const auto& [clave, modulo] : modulos) {
      if (modulo != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, modulo, nullptr);
      }
    }
    for (const auto& [clave, pase] : pases) {
      if (pase != VK_NULL_HANDLE) {
        dfn_.vkDestroyRenderPass(device_, pase, nullptr);
      }
    }
    const double segundos =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - precalentado_inicio_).count();
    REXLOG_INFO("[nativo] C6 precalentado (build 186): {} en {:.1f} s, prioridad {:#x}: {} de {} pipelines "
                "precalentados, {} compilados de verdad ({:.0f} ms, {:.1f} ms cada uno; el resto ya estaba en la cache), "
                "{} saltados porque la biblioteca ya no tiene sus shaders, {} de otro modo de estado dinamico y {} "
                "fallidos",
                precalentado_parar_.load(std::memory_order_relaxed) ? "parado" : "terminado", segundos, prioridad,
                hechos, n, compilados, double(ns_compilados) / 1e6,
                compilados ? double(ns_compilados) / 1e6 / double(compilados) : 0.0, sin_shader, otro_modo, fallidos);
    precalentado_terminado_.store(true, std::memory_order_release);
  }

  // Ring only, every 10 s if anything changed: the thread's progress, what the ring had to create, and
  // the guard (see above).
  void InformePrecalentado(std::chrono::steady_clock::time_point ahora) {
    if (ahora - precalentado_informe_ < std::chrono::seconds(10)) {
      return;
    }
    precalentado_informe_ = ahora;
    if (!precalentado_diferencia_ && anillo_precalentados_ >= 8 &&
        anillo_precalentados_lentos_ * 2 >= anillo_precalentados_) {
      precalentado_diferencia_ = true;
      precalentado_parar_.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("[nativo] C6 precalentado (build 186): DIFERENCIA: de {} pipelines que el anillo pidio ya "
                   "precalentados, {} tardaron como uno compilado (20 ms o mas): lo que precalienta el hilo no es lo "
                   "que pide el anillo. Se para el hilo; no cambia nada mas",
                   anillo_precalentados_, anillo_precalentados_lentos_);
    }
    const size_t hasta = precalentado_hasta_.load(std::memory_order_acquire);
    const bool terminado = precalentado_terminado_.load(std::memory_order_acquire);
    const uint64_t cambios = uint64_t(hasta) + anillo_precalentados_ + anillo_de_lista_ + anillo_nuevos_ +
                             (terminado ? 1 : 0) + (precalentado_decidido_ ? 1 : 0);
    if (cambios == precalentado_cambios_previos_) {
      return;
    }
    precalentado_cambios_previos_ = cambios;
    const auto media = [](uint64_t ns, uint64_t k) { return k ? double(ns) / double(k) / 1e6 : 0.0; };
    const uint32_t compilados = precalentado_compilados_.load(std::memory_order_relaxed);
    NFSMW_INFORME_ANILLO(
        "[nativo] C6 precalentado de pipelines (build 186): lista de {}; hilo {} (prioridad {:#x}): {} recorridos, {} "
        "precalentados, {} compilados de verdad ({:.0f} ms), {} sin sus shaders, {} de otro modo, {} fallidos | el "
        "anillo creo {} de la lista ya precalentados ({:.1f} ms de media, {} lentos), {} de la lista sin precalentar "
        "({:.1f} ms de media) y {} nuevos ({:.1f} ms de media; {} a la lista)",
        lista_archivo_.size(),
        !precalentado_hilo_.joinable() ? (precalentado_decidido_ ? "sin lanzar" : "esperando a la biblioteca")
        : terminado                    ? (precalentado_parar_.load(std::memory_order_relaxed) ? "parado" : "terminado")
                                       : "en marcha",
        uint32_t(precalentado_prioridad_.load(std::memory_order_relaxed)), hasta,
        precalentado_hechos_.load(std::memory_order_relaxed), compilados,
        double(precalentado_ns_compilados_.load(std::memory_order_relaxed)) / 1e6,
        precalentado_sin_shader_.load(std::memory_order_relaxed), precalentado_otro_modo_.load(std::memory_order_relaxed),
        precalentado_fallidos_.load(std::memory_order_relaxed), anillo_precalentados_,
        media(ns_anillo_precalentados_, anillo_precalentados_), anillo_precalentados_lentos_, anillo_de_lista_,
        media(ns_anillo_de_lista_, anillo_de_lista_), anillo_nuevos_, media(ns_anillo_nuevos_, anillo_nuevos_),
        anillo_a_lista_);
  }

  void PararPrecalentado() {
    precalentado_parar_.store(true, std::memory_order_relaxed);
    if (precalentado_hilo_.joinable()) {
      precalentado_hilo_.join();  // at most, as long as the pipeline being compiled takes
    }
  }

  void DestruirImagen(ImagenNativa& imagen) {
    if (imagen.vista != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, imagen.vista, nullptr);
    if (imagen.imagen != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, imagen.imagen, nullptr);
    // The pool chunk is returned after destroying the image that used it, and in that case `memoria` is
    // NULL on purpose: the memory belongs to a shared slab and is not freed on its own.
    if (imagen.pool_bloque != 0xFFFFFFFFu) pool_texturas_.Liberar(imagen.pool_bloque);
    if (imagen.memoria != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, imagen.memoria, nullptr);
    imagen = ImagenNativa{};
  }

  struct Monton {
    uint32_t capacidad = 0;
    uint32_t siguiente = 1;
    std::vector<uint32_t> libres;
  };

  const VulkanDevice* dispositivo_;
  std::array<bool, 5> bc_cpu_{};
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* memoria_;
  ContextoDestinos* contexto_;
  FnDireccionBufer direccion_bufer_ = nullptr;
  FnCopiarImagen copiar_imagen_ = nullptr;  // resolved faces of the dynamic cubemaps

  VkBuffer subida_ = VK_NULL_HANDLE;
  VkDeviceMemory subida_memoria_ = VK_NULL_HANDLE;
  uint32_t subida_tipo_ = UINT32_MAX;
  VkDeviceSize subida_tamano_real_ = 0;
  uint8_t* subida_datos_ = nullptr;
  VkDeviceAddress subida_direccion_ = 0;
  bool spirv_13_ = false;  // Vulkan 1.1 device: the library's SPIR-V is converted to 1.3 (CrearModulo)
  VkDeviceSize subida_usado_ = 0;
  bool subida_coherente_ = false;
  uint64_t epoca_subida_ = 0;
  uint64_t fotograma_ = 0;
  // Fast untiling and its guard (see LeerNivel).
  static constexpr uint32_t kNivelesAComprobar = 200;  // it used to be 2000
  static constexpr uint32_t kComprobarUnaDeCada = 64;   // after the first ones, by sampling
  uint64_t niveles_vistos_ = 0;
  uint64_t ordenes_vistos_ = 0;
  int mosaico_rapido_ = -1;
  uint32_t niveles_comprobados_ = 0;
  uint32_t ordenes_comprobados_ = 0;
  std::vector<uint8_t> comprobacion_mosaico_;
  // Texture check budget (see PrepararTextura).
  static constexpr uint32_t kAplazamientoMax = 8;
  int32_t presupuesto_huellas_ = -1;
  uint64_t fotograma_huellas_ = UINT64_MAX;
  uint64_t bytes_huella_fotograma_ = 0;
  // Sampled recheck of stable textures (see PrepararTextura and HuellaMuestra).
  static constexpr uint64_t kMuestrasAComprobar = 3000;  // the first ones, with both hashes: the guard
  int32_t muestreo_huellas_ = -1;      // -1 cvar not read; 0 off; N: 1 in N rechecks is full
  uint64_t muestras_comprobadas_ = 0;  // stable rechecks with both hashes, since start-up
  uint64_t muestras_con_las_dos_ = 0;  // the same, since the last report line
  uint64_t muestras_aciertos_ = 0;     // decided on the sample alone
  uint64_t muestras_distintas_ = 0;    // the sample changed and the full path followed
  uint64_t bytes_muestra_ = 0;
  uint64_t bytes_ahorrados_muestra_ = 0;
  std::chrono::steady_clock::time_point informe_muestreo_{};
  // Vertex copies on a separate thread (EncolarCopia). Power-of-2 capacity.
  const bool copias_activas_ = REXCVAR_GET(nfsmw_nativo_subidas_hilo);
  std::array<TrabajoCopia, 8192> copias_{};
  std::atomic<size_t> copias_escritas_{0};
  // How many copies are done, whether by the thread or the ring (see EsperarSubidas). Outside
  // EsperarSubidas they finish in order, and then it is also the index of the first one not done.
  std::atomic<size_t> copias_hechas_{0};
  bool copias_durmiendo_ = false;   // under copias_mutex_: the copy thread sleeps or is about to
  bool copias_esperando_ = false;   // under copias_mutex_: the ring waits in EsperarSubidas
  std::mutex copias_mutex_;
  std::condition_variable copias_cv_;
  std::condition_variable copias_hechas_cv_;
  bool copias_parar_ = false;  // con copias_mutex_
  std::thread copias_hilo_;
  uint64_t copias_en_linea_ = 0;
  uint64_t esperas_copias_ = 0;
  uint64_t ns_esperando_copias_ = 0;
  std::atomic<size_t> copias_progreso_{0};  // copies done by the thread, one by one (diagnostic)
  std::thread::id copias_productor_;
  bool copias_otro_hilo_avisado_ = false;
  // The ring's help (nfsmw_nativo_subidas_ayuda) and its guard. See EsperarSubidas.
  const bool copias_ayuda_pedida_ = REXCVAR_GET(nfsmw_nativo_subidas_ayuda);
  int32_t copias_fase_ = 0;         // FaseCopias; ring only
  bool copias_sin_hilo_ = false;    // off after a DIFERENCIA: copies go on the ring; ring only
  std::atomic<size_t> copias_tomadas_{0};  // how many were taken, in order, by the thread or the ring (with CAS)
  std::mutex copias_trozo_mutex_;          // held by the thread while it copies what it took (lends priority)
  std::array<std::atomic<uint32_t>, 8192> copias_marcas_{};  // index + 1 of the last copy done in each slot
  std::atomic<uint64_t> copias_hilo_n_{0};  // copies done by the thread, for the report
  std::atomic<uint64_t> copias_hilo_bytes_{0};
  size_t copias_verificadas_ = 0;  // marks checked up to here; ring only
  uint64_t copias_comprobadas_ = 0;
  uint64_t copias_esperas_mirando_ = 0;
  uint64_t copias_ayudadas_ = 0;
  uint64_t copias_bytes_ayudados_ = 0;
  uint64_t copias_ns_ayudando_ = 0;
  uint64_t copias_diferencias_ = 0;
  struct InformeCopiasCifras {  // the last 10 s (InformeCopias); ring only
    uint64_t encoladas = 0, bytes_encolados = 0, en_linea = 0, bytes_en_linea = 0;
    uint64_t ayudadas = 0, bytes_ayudados = 0, ns_ayudando = 0, esperas_con_ayuda = 0;
    uint64_t esperas = 0, sin_tomar = 0, esperas_trozo = 0, ns_trozo = 0, ns_trozo_peor = 0;
    uint64_t esperas_enteras = 0, ns_espera = 0, ns_espera_peor = 0, comprobadas = 0;
  };
  InformeCopiasCifras copias_inf_{};
  std::chrono::steady_clock::time_point copias_informe_{};
  uint64_t copias_hilo_n_previo_ = 0;
  uint64_t copias_hilo_bytes_previo_ = 0;
  struct BuferSubida {
    VkBuffer bufer = VK_NULL_HANDLE;
    VkDeviceMemory memoria = VK_NULL_HANDLE;
    VkDeviceSize tamano_real = 0;
    uint8_t* datos = nullptr;
    VkDeviceAddress direccion = 0;
  };
  // One per work slot (there are 3). With fewer than there are slots, two slots would share a buffer and
  // the CPU would write over what the GPU is still reading.
  std::array<BuferSubida, kRanurasDeTrabajo> subidas_{};  // subida_* are the current slot's
  // Separate shared constants, CPU-cached (nfsmw_nativo_compartidas_cache).
  std::array<BuferSubida, kRanurasDeTrabajo> compartidas_bufs_{};
  bool compartidas_aparte_ = false;
  bool compartidas_coherente_ = true;
  uint32_t compartidas_tipo_ = 0;
  uint8_t* compartidas_datos_ = nullptr;
  VkDeviceMemory compartidas_memoria_ = VK_NULL_HANDLE;
  VkDeviceSize compartidas_tamano_real_ = 0;
  VkDeviceSize compartidas_usado_ = 0;
  uint64_t compartidas_bytes_publicados_ = 0;

  std::array<VkDescriptorSetLayout, 4> layouts_{};
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 4> sets_{};
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;
  // Set 4, the constants through dynamic UBOs. One set per upload slot (each one is a VkBuffer).
  bool usar_ubo_ = false;
  VkDeviceSize alineacion_ubo_ = 256;
  VkDescriptorSetLayout layout_ubo_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ubo_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kRanurasDeTrabajo> sets_ubo_{};  // one per work slot
  uint32_t ranura_actual_ = 0;
  bool ubo_enlazado_ = false;
  uint32_t ranura_ubo_enlazada_ = UINT32_MAX;
  std::array<uint32_t, 3> offsets_ubo_enlazados_{};
  // Set 4 by differences (ControlSet4, InformeSet4).
  std::array<uint64_t, 8> set4_cambios_{};  // binds by what changes: bit 0 VS, bit 1 PS, bit 2 shared
  uint64_t set4_primeros_ = 0;              // binds after a new command buffer, a new slot or the sky
  uint64_t set4_dibujos_ = 0;
  std::array<uint64_t, 3> resubidas_vs_{};  // constants uploaded again: by generation, by epoch, because they grow
  std::array<uint64_t, 3> resubidas_ps_{};
  std::chrono::steady_clock::time_point set4_informe_{};
  std::chrono::steady_clock::time_point set4_inicio_alternancia_ = std::chrono::steady_clock::now();
  bool set4_pedido_ = true;
  bool set4_pedido_anotado_ = false;
#if REX_PLATFORM_SWITCH
  bool set4_diferencia_avisada_ = false;
  uint64_t set4_nvk_previo_[9] = {};  // NVK counts at the previous report
#endif
  // The draw path in NVK (ControlDibujoNvk and InformeDibujoNvk). Ring only.
  bool nvk_precarga_app_ = false;  // hand the pipeline to NVK after PipelineDe
  bool nvk_dibujo_anotado_ = false;
  bool nvk_mejoras_pedidas_ = true;
  bool nvk_apagado_avisado_[5] = {};
  std::chrono::steady_clock::time_point nvk_dibujo_inicio_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point nvk_informe_{};
  uint64_t nvk_partes_previas_[16][2] = {};
  uint64_t nvk_cuentas_previas_[13] = {};
  uint64_t nvk_mejoras_previas_[5][4] = {};
  int32_t alternar_ubo_s_ = 0;
  uint64_t envios_ = 0;
  uint64_t envios_ubo_ = 0;
  std::chrono::steady_clock::time_point inicio_ubo_ = std::chrono::steady_clock::now();
  std::array<Monton, 4> montones_{};
  std::array<ImagenNativa, 3> vacias_{};
  VkSampler sampler_vacio_ = VK_NULL_HANDLE;
  bool vacias_preparadas_ = false;

  std::unordered_map<const EntradaShader*, VkShaderModule> modulos_;
  std::array<VkShaderModule, 3> modulos_variantes_{};  // 1 natural, 2 soft
  std::array<bool, 3> modulos_variantes_creados_{};
  int resplandor_anotado_ = 0;
  std::unordered_map<uint64_t, std::pair<ClavePipeline, VkPipeline>> pipelines_;
  // One-entry shortcut for PipelineDe (see the comment there).
  ClavePipeline ultima_clave_{};
  VkPipeline ultima_pipeline_ = VK_NULL_HANDLE;
  bool ultima_clave_valida_ = false;
  // direct cache in front of pipelines_ (nfsmw_nativo_pipelines_directa).
  struct CasillaPipeline {
    ClavePipeline clave{};
    VkPipeline pipeline = VK_NULL_HANDLE;  // VK_NULL_HANDLE = casilla vacia
  };
  static constexpr size_t kCasillasPipeline = 256;  // potencia de 2; 256 x 88 bytes = 22 KB contiguos
  std::array<CasillaPipeline, kCasillasPipeline> casillas_pipeline_{};
  bool pipelines_directa_ = true;           // the cvar, once per frame
  bool pipelines_directa_apagada_ = false;  // the guard saw a difference
  uint64_t pipelines_directa_aciertos_ = 0;
  uint64_t pipelines_directa_fallos_ = 0;
  uint64_t pipelines_directa_comprobadas_ = 0;
  uint64_t pipelines_directa_aciertos_previos_ = 0;
  uint64_t pipelines_directa_fallos_previos_ = 0;
  std::chrono::steady_clock::time_point pipelines_directa_informe_{};
  static constexpr uint64_t kPipelinesAComprobar = 200000;
  // nfsmw_nativo_contar_cambios_pipeline (ContarCambioPipeline and InformeCambiosPipeline).
  enum : uint32_t {
    kCambioEnlaces = 0,        // vkCmdBindPipeline de Dibujar contados
    kCambioTrasPase,           // of those, the first of a pass (EmpezarPase forgets the bound pipeline)
    kCambioPrimero,            // no previous key in the buffer (new buffer or after the deferred sky)
    kCambioIdentica,           // the same key: only after EmpezarPase
    kCambioShaders,            // another VS or PS
    kCambioEntrada,            // same shaders, different vertex input
    kCambioFormatos,           // same shaders and input, different formats (another pass)
    kCambioSpecAlfa,           // all the above equal; specialization only changes the alpha test (bits 1, 16-18)
    kCambioSpecZTemprana,      // ... the alpha test and the early Z (bit 20)
    kCambioSpecOtra,           // ... other specialization bits
    kCambioSinEfecto,          // state only, in bits PipelineDe does not read: effectively the same pipeline
    kCambioSinEfectoTrasPase,  // ... and the first of a pass
    kCambioSoloEstado,         // real state-only changes: what dynamic state would avoid
    kCambioEstadoTrasPase,     // ... and the first of a pass
    kCambioEds12,              // ... without blending, masks or another topology class (EDS1/EDS2, core 1.3)
    kCambioEds12TrasPase,      // ... and the first of a pass
    kCampoTopologia,           // per field, on the canonical state (one bind may change several)
    kCampoClaseTopologia,
    kCampoMezcla,
    kCampoMascaras,
    kCampoZ,
    kCampoEstencil,
    kCampoCara,
    kCampoReinicio,
    kCampoSesgo,
    kCambiosN
  };
  std::array<uint64_t, kCambiosN> cambios_pipeline_{};
  ClavePipeline clave_enlazada_{};       // the last key bound in this command buffer
  bool clave_enlazada_valida_ = false;   // false: new buffer, after the deferred sky, or not counting
  bool contar_cambios_pipeline_ = true;  // the cvar, once per frame (InformeCambiosPipeline)
  uint64_t cambios_dibujos_previos_ = 0;
  uint64_t cambios_fotogramas_previos_ = 0;
  std::chrono::steady_clock::time_point cambios_informe_{};
  // Dynamic state, phase 0a (nfsmw_nativo_clave_canonica, ClaveDeBusqueda).
  static constexpr uint64_t kCanonicasAComprobar = 200000;
  bool clave_canonica_ = true;           // the cvar, once per command buffer (UsarRanura)
  bool clave_canonica_apagada_ = false;  // the guard saw a difference
  bool canonica_valida_ = false;         // canonica_cruda_ -> canonica_resultado_ is the previous draw's
  ClavePipeline canonica_cruda_{};
  ClavePipeline canonica_resultado_{};
  uint64_t canonica_cambiadas_ = 0;      // keys Canonizar changes (not counting consecutive repeats)
  uint64_t canonica_comprobadas_ = 0;
  uint64_t canonica_cambiadas_previas_ = 0;
  uint64_t canonica_pipelines_previos_ = 0;
  std::chrono::steady_clock::time_point canonica_informe_{};
  // Dynamic state, phase 0b (nfsmw_nativo_pipeline_entre_pases).
  bool pipeline_entre_pases_ = true;  // the cvar, once per command buffer (UsarRanura)
  bool pipeline_entre_pases_anotado_ = false;
  uint64_t pases_con_pipeline_ = 0;   // passes started with a bound pipeline that is kept
  uint64_t entre_pases_pases_previos_ = 0;
  uint64_t entre_pases_conservados_previos_ = 0;
  std::chrono::steady_clock::time_point entre_pases_informe_{};
  // Dynamic state phases 1 and 2 (FijarEstadoDinamico, its guard and its report).
  enum : uint32_t { kEds12 = 1, kEds3 = 2 };  // bits of eds_modo_ and of ClavePipeline::relleno2
  static constexpr VkDynamicState kDinamicosEds12[10] = {
      VK_DYNAMIC_STATE_CULL_MODE,           VK_DYNAMIC_STATE_FRONT_FACE,
      VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,  VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,   VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
      VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
  static constexpr uint64_t kEdsAComprobar = 200000;
  enum : uint32_t {
    kEdsCara = 0,
    kEdsFrente,
    kEdsTopologia,
    kEdsReinicio,
    kEdsSesgo,
    kEdsPruebaZ,
    kEdsEscribeZ,
    kEdsFuncionZ,
    kEdsEstencil,
    kEdsEstencilOps,
    kEdsMezclaActiva,  // fase 2
    kEdsEcuacion,      // fase 2
    kEdsMascara,       // fase 2
    kEdsTodo,          // times everything is set (not a call)
    kEdsN
  };
  PFN_vkCmdSetCullMode set_cara_ = nullptr;  // nucleo de Vulkan 1.3 (CargarEstadoDinamico)
  PFN_vkCmdSetFrontFace set_frente_ = nullptr;
  PFN_vkCmdSetPrimitiveTopology set_topologia_ = nullptr;
  PFN_vkCmdSetPrimitiveRestartEnable set_reinicio_ = nullptr;
  PFN_vkCmdSetDepthBiasEnable set_sesgo_ = nullptr;
  PFN_vkCmdSetDepthTestEnable set_prueba_z_ = nullptr;
  PFN_vkCmdSetDepthWriteEnable set_escribe_z_ = nullptr;
  PFN_vkCmdSetDepthCompareOp set_funcion_z_ = nullptr;
  PFN_vkCmdSetStencilTestEnable set_estencil_ = nullptr;
  PFN_vkCmdSetStencilOp set_estencil_ops_ = nullptr;
  bool eds12_disponible_ = false;  // API 1.3 and the ten functions
  bool eds_apagado_ = false;       // the guard saw a difference: for the rest of the session, everything in the pipeline
  bool eds_modo_anotado_ = false;
  uint32_t eds_modo_ = 0;          // kEds12 | kEds3 of this command buffer (UsarRanura; the guard sets it to 0)
  bool eds_valido_ = false;        // eds_grabado_ is what is set in this buffer (false: new buffer, sky or pass)
  EstadoEds eds_grabado_{};
  ClavePipeline eds_clave_{};      // raw key of the last state set
  uint64_t eds_dibujos_ = 0;       // draws with dynamic state
  uint64_t eds_repetidos_ = 0;     // ... with the same raw key as the previous one: nothing to set or check
  uint64_t eds_comprobables_ = 0;  // ... with something to set (what the guard counts)
  uint64_t eds_comprobados_ = 0;
  uint64_t eds_dibujos_previos_ = 0;
  uint64_t eds_repetidos_previos_ = 0;
  uint64_t eds_fotogramas_previos_ = 0;
  std::array<uint64_t, kEdsN> eds_llamadas_{};    // vkCmdSet* recorded per state since the last report
  std::array<uint64_t, 4> pipelines_por_modo_{};  // pipelines created per mode (relleno2) since start-up
  std::chrono::steady_clock::time_point eds_informe_{};
  // Dynamic state, phase 2 (VK_EXT_extended_dynamic_state3).
  static constexpr VkDynamicState kDinamicosEds3[3] = {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
  // The same mapping as kFactores in RellenarEstadoFijo (fields 2, 3 and 17-31 give ZERO).
  static constexpr VkBlendFactor kFactoresMezcla[32] = {
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  PFN_vkCmdSetColorBlendEnableEXT set_mezcla_activa_ = nullptr;
  PFN_vkCmdSetColorBlendEquationEXT set_ecuacion_ = nullptr;
  PFN_vkCmdSetColorWriteMaskEXT set_mascara_ = nullptr;
  bool eds3_disponible_ = false;  // the SDK enables the extension and its three features; the driver provides the functions
  std::unordered_map<uint64_t, VkRenderPass> pases_;
  std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
  // nfsmw_nativo_framebuffers_olvidan_vistas. The views of each cached framebuffer (by the same key), the
  // ones retired by the guard, and the counts for the warning.
  std::unordered_map<uint64_t, std::array<VkImageView, 5>> framebuffers_vistas_;
  std::vector<VkFramebuffer> fb_retirados_;
  bool fb_retirar_ = false;
  uint64_t fb_vistas_olvidadas_ = 0;
  uint64_t fb_olvidados_ = 0;
  std::unordered_map<uint64_t, Textura> texturas_;
  // Measurement only (nfsmw_nativo_diag_reutilizar): textures with a hash, by content key (content key ->
  // texture key; see AnotarContenidoTextura), and the counts for the 10 s report.
  std::unordered_multimap<uint64_t, uint64_t> vivas_por_contenido_;
  uint64_t contenidos_distintos_ = 0;
  int32_t diag_reutilizar_ = -1;  // -1 = cvar not read
  uint64_t reutilizar_nuevas_ = 0;
  uint64_t reutilizar_coinciden_ = 0;
  uint64_t reutilizar_frias_ = 0;
  uint64_t reutilizar_sin_mirar_ = 0;
  uint64_t reutilizar_bytes_nuevas_ = 0;
  uint64_t reutilizar_bytes_coinciden_ = 0;
  uint64_t reutilizar_bytes_frias_ = 0;
  uint32_t reutilizar_detalles_ = 0;
  std::chrono::steady_clock::time_point informe_reutilizar_{};
  std::unordered_map<uint64_t, Vista> vistas_;
  // The vistas_ keys of each image. InvalidarImagenes (every resolve and every copy) used to walk all of
  // vistas_: 2.7 % of the ring thread in a race. Maintained together with vistas_ in RanuraVista
  // (insertion) and in QuitarVista (every removal).
  std::unordered_map<VkImage, std::vector<uint64_t>> vistas_por_imagen_;
  std::unordered_map<uint32_t, std::pair<VkSampler, uint32_t>> samplers_;
  // Diagnostic: filter values already logged (RanuraSampler).
  uint32_t aniso_vistos_ = 0;
  bool aniso_anotado_ = false;  // nfsmw_nativo_anisotropico, one line when the first one is created
  uint32_t diag_mip_minimo_ = 0;  // nfsmw_nativo_diag_mip_minimo, read once per frame
  // nfsmw_nativo_sombras_sesgo_*, read once per frame.
  int32_t sombras_sesgo_constante_ = 0;
  int32_t sombras_sesgo_pendiente_ = 0;
  bool sombras_sesgo_anotado_ = false;
  uint32_t mip_max_vistos_ = 0;
  std::array<bool, 1024> sesgos_vistos_{};
  std::vector<Textura*> texturas_a_subir_;
  std::vector<uint8_t> temporal_;
  std::vector<uint8_t> temporal_bc_;  // reused CPU decode output; only allocated on unsupported BC GPUs
  std::vector<uint32_t, SinInicializar<uint32_t>> indices_;
  std::vector<uint32_t, SinInicializar<uint32_t>> convertidos_;
  std::vector<uint16_t, SinInicializar<uint16_t>> indices16_;  // camino rapido: 16 bits sin convertir
  // Rectangle lists (NFS Carbon): each binding's vertices with the fourth corner of every rectangle added.
  std::array<std::vector<uint8_t>, 16> rectangulos_vertices_;
  // IndicesDe16 (nfsmw_nativo_indices_neon). Ring only.
  int32_t indices_neon_ = -1;  // -1 cvar not read, 0 plain loop, 1 NEON
  uint64_t indices_neon_dibujos_ = 0;
  uint64_t indices_neon_comprobados_ = 0;
  std::vector<uint16_t, SinInicializar<uint16_t>> indices_neon_prueba_;
  // Dynamic state already recorded in the current command buffer and pass.
  bool estado_grabado_ = false;
  uint64_t push_grabado_[3] = {};
  VkViewport viewport_grabado_{};
  VkRect2D tijera_grabada_{};
  float mezcla_grabada_[4] = {};
  float sesgo_grabado_[2] = {};  // depth bias: constant and slope
  float sesgo_avisado_[2] = {};
  uint32_t avisos_sesgo_ = 0;
  std::string omitir_texto_;  // nfsmw_nativo_diag_omitir_ps ya leido
  std::unordered_set<uint32_t> omitir_ps_;
  uint32_t stencil_grabado_[2] = {};  // RB_STENCILREFMASK for front and back
  bool stencil_grabado_valido_ = false;
  VkIndexType tipo_indices_grabado_ = VK_INDEX_TYPE_MAX_ENUM;
  // Shared constants of the previous draw and where they were uploaded.
  uint32_t compartidas_previas_[kPalabrasCompartidas] = {};
  uint64_t compartidas_miradas_ = 0;    // C6 report, to decide how to make the block cheaper
  uint64_t compartidas_cambiadas_ = 0;
  VkDeviceSize compartidas_offset_ = 0;
  uint64_t compartidas_epoca_ = UINT64_MAX;

  EntradaVertices entrada_;
  const EntradaVertices* entrada_actual_ = &entrada_;  // entrada_ or a cache element
  std::unordered_map<uint64_t, EntradaVertices> entradas_cache_;
  uint64_t entradas_cache_aciertos_ = 0;
  const EntradaShader* entrada_vs_ = nullptr;
  uint64_t entrada_generacion_ = UINT64_MAX;
  bool entrada_valida_ = false;

  uint64_t constantes_vs_generacion_ = UINT64_MAX;
  uint64_t constantes_vs_epoca_ = UINT64_MAX;
  VkDeviceSize constantes_vs_offset_ = 0;
  uint64_t constantes_ps_generacion_ = UINT64_MAX;
  uint64_t constantes_ps_epoca_ = UINT64_MAX;
  VkDeviceSize constantes_ps_offset_ = 0;
  uint32_t constantes_vs_bytes_ = 0;
  uint32_t constantes_ps_bytes_ = 0;

  bool pase_activo_ = false;
  // The game's open occlusion query and the host query counting this pass's span.
  bool oclusion_abierta_ = false;
  // nfsmw_reflejo_visibilidad (see nfsmw_reflejo_demanda.h). The witness, every ~1 s at 30 FPS.
  static constexpr uint64_t kTestigoCada = 32;
  bool medir_visibilidad_ = false;
  bool testigo_pendiente_ = false;
  uint64_t testigo_siguiente_ = 0;
  uint32_t consulta_oclusion_ = UINT32_MAX;
  uint32_t categoria_pase_ = kGpuOtros;
  bool enviado_tras_sombras_ = false;        // once per frame
  // Per-frame copies of the diagnostic cvars (see the comment at the Swap).
  std::string diag_omitir_ps_texto_;
  bool diag_vertices_repetidos_ = false;
  bool diag_estadisticas_dibujo_ = false;
  uint32_t tijera_prueba_ = 0;
  std::array<VkBuffer, 16> bufers_vertices_{};
  // Height actually used by each render target, deduced from the scissor. It only grows.
  std::unordered_map<uint32_t, uint32_t> alto_util_;
  bool area_util_ = true;
  // Small per-draw savings. See their cvars.
  bool encuadre_cache_ = true;             // C1: the cvar, once per frame
  bool encuadre_cache_apagada_ = false;    // C1: the guard saw a difference
  bool encuadre_valido_ = false;
  uint64_t encuadre_generacion_ = 0;
  uint64_t encuadre_pase_serie_ = 0;
  uint64_t pase_serie_ = 0;                // incremented on every EmpezarPase
  VkViewport encuadre_viewport_{};
  VkRect2D encuadre_tijera_{};
  float encuadre_ndc_[4] = {};
  uint32_t encuadre_vacio_ = 0;
  uint64_t encuadre_aciertos_ = 0;         // since start-up (the guard)
  uint64_t encuadre_comprobados_ = 0;
  uint64_t encuadre_aciertos_informe_ = 0;
  uint64_t encuadre_calculos_informe_ = 0;
  static constexpr uint64_t kEncuadresAComprobar = 200000;
  bool alto_util_memo_activo_ = true;      // C2
  bool alto_util_memo_apagado_ = false;
  uint32_t* alto_util_memo_ = nullptr;     // alto_util_ element of the last pitch (the map never erases)
  uint32_t alto_util_memo_pitch_ = 0;
  uint64_t alto_util_memo_aciertos_ = 0;
  uint64_t alto_util_memo_aciertos_previos_ = 0;
  static constexpr uint64_t kAltoUtilAComprobar = 200000;
  bool clave_pase_rapida_ = true;          // C3
  bool clave_pase_rapida_apagada_ = false;
  uint64_t pase_claves_[5] = {};           // the render target bytes that produced pase_clave_
  bool pase_claves_validas_ = false;
  uint64_t claves_pase_rapidas_ = 0;
  uint64_t claves_pase_rapidas_previas_ = 0;
  static constexpr uint64_t kClavesPaseAComprobar = 200000;
  bool cvars_por_fotograma_ = true;        // C5
  bool ps_solo_alfa_fotograma_ = true;
  bool sin_ps_sin_color_fotograma_ = true;
  std::chrono::steady_clock::time_point minucias_informe_{};
  bool sin_vegetacion_ = false;
  // Shadow map vegetation draws discarded early (see the early discard in Dibujar).
  uint64_t dibujos_vegetacion_pronto_ = 0;
  // VeredictoVegetacion (nfsmw_d3d_vegetacion_juego).
  bool vegetacion_calculada_ = false;  // this draw carries the game's verdict and the ring's has been computed
  bool vegetacion_anillo_ = false;     // what the early discard has to decide
  uint16_t vegetacion_banderas_ = 0;
  DetalleVegetacion vegetacion_detalle_;
  /*
   * The sky draw, saved in full to replay later (nfsmw_nativo_cielo_aplazado).
   *
   * This holds everything the vkCmd* calls of that draw need, by copy: handles (which do not change
   * within the pass) and values (which depend on no guest register once copied). The only thing kept "by
   * reference" are the offsets within the upload buffer, and that buffer only moves forward until
   * UsarRanura resets it, always after the pass is closed.
   */
  enum : uint32_t {
    kCieloPorMezcla = 0,  // a blended draw arrived: that one does read the background color
    kCieloPorSinZ,        // an opaque draw without Z write arrived: it could not cover the sky
    kCieloPorFinDePase,   // the pass closes (fallback path)
    kCieloPorOtroCielo,   // a second sky in the same pass: the first one is emitted now
    kCieloMotivos
  };
  struct CieloAplazado {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer bufer = VK_NULL_HANDLE;   // the upload one: vertices and indices
    bool usa_ubo = false;
    uint64_t push[3] = {};             // only with constants through pointers
    std::array<uint32_t, 3> offsets_ubo{};
    uint32_t ranura_ubo = 0;
    VkViewport viewport{};
    VkRect2D tijera{};
    float mezcla[4] = {};
    float sesgo[2] = {};
    bool con_estencil = false;
    uint32_t stencil[2] = {};          // RB_STENCILREFMASK for front and back
    uint32_t n_enlaces = 0;
    std::array<VkDeviceSize, 16> offsets_vertices{};
    bool con_indices = false;
    bool indices_de_16 = false;
    uint32_t indices = 0;
    uint32_t primer_indice = 0;
    uint32_t vmin = 0;
    uint32_t cuenta = 0;               // sin indices
    uint32_t ps_mas_uno = 0;           // for the diagnostic per-draw query
    uint32_t categoria = 0;
    uint64_t dibujos_al_aplazar = 0;   // position in the pass when it was deferred
    uint32_t eds_modo = 0;  // phases 1 and 2 its pipeline was looked up with (eds_modo_)
    EstadoEds eds{};        // and the state that pipeline lacks
  };
  CieloAplazado cielo_{};
  bool cielo_pendiente_ = false;
  bool cielo_aplazado_ = true;  // the cvar, read once per frame
  bool cielo_anotado_ = false;
  uint64_t dibujos_en_pase_ = 0;
  uint64_t cielo_vistos_ = 0;
  uint64_t cielo_vistos_previos_ = 0;
  // The ones that pass both marks (480 indices and first of the pass). This is the number to watch: if it
  // is not 1.00 per frame, the criterion still does not isolate the dome and nothing should be deferred.
  // The fingerprint-only "detectados" once gave 7.00.
  uint64_t cielo_candidatos_ = 0;
  uint64_t cielo_candidatos_previos_ = 0;
  /*
   * The self-checking guard.
   *
   * Two earlier versions broke the image by enabling this blindly. The check that was missing ("the
   * criterion detects one draw per frame, not seven") no longer needs a diagnostic build: the game itself
   * runs it, before anything is deferred.
   *
   * Phase 0, observing: for the first kCieloFotogramasPrueba race frames nothing is deferred (the image
   *   is exactly the non-deferred one) and the candidates in each frame are counted.
   * Phase 1, deferring: only if those frames never had more than one and at least kCieloFotogramasConUno
   *   had exactly one. Then yes, and that is 3.4-4.3 ms of GPU time.
   * Phase 2, off for good: as soon as two are seen in the same frame, or if the test does not come out
   *   clean. It is not retried for the whole session.
   *
   * So the worst case of this change is that it does nothing. Breaking the image is not among the
   * possible outcomes.
   */
  enum : uint32_t { kCieloMirando = 0, kCieloAplazando = 1, kCieloDescartado = 2 };
  uint32_t cielo_guardia_ = kCieloMirando;
  uint32_t cielo_guardia_fotogramas_ = 0;
  uint32_t cielo_guardia_con_uno_ = 0;
  uint32_t cielo_guardia_en_fotograma_ = 0;
  uint32_t cielo_guardia_max_ = 0;
  uint64_t cielo_aplazados_ = 0;
  uint64_t cielo_aplazados_previos_ = 0;
  uint64_t cielo_no_aplazables_ = 0;
  uint64_t cielo_dos_en_pase_ = 0;
  uint64_t cielo_perdidos_ = 0;
  std::array<uint64_t, kCieloMotivos> cielo_emitidos_{};
  std::array<uint64_t, kCieloMotivos> cielo_emitidos_previos_{};
  uint64_t cielo_posicion_suma_ = 0;
  uint64_t cielo_posicion_suma_previa_ = 0;
  uint64_t cielo_posicion_max_ = 0;
  // The last binding made with vkCmdBindVertexBuffers, so it is not repeated.
  std::array<VkDeviceSize, 16> offsets_grabados_{};
  uint32_t enlaces_grabados_ = 0;
  uint64_t enlaces_ahorrados_ = 0;
  // nfsmw_nativo_vertices_base_cero (see the cvar) and its guard.
  bool vertices_base_cero_ = true;           // the cvar, read once per frame (UsarRanura)
  bool vertices_base_cero_apagado_ = false;  // the guard saw a difference: the rest of the session, as before
  uint64_t base_cero_comprobados_ = 0;       // draws on the base-zero path since start-up (the guard)
  uint64_t base_cero_dibujos_ = 0;           // since the last report: without binding their own offset
  uint64_t base_cero_total_ = 0;             // since the last report: draws recorded with vertices
  uint64_t base_cero_varios_enlaces_ = 0;
  uint64_t base_cero_desalineados_ = 0;      // one binding, but the dedupe gave a copy off a multiple
  uint64_t base_cero_enlaces_grabados_ = 0;  // vkCmdBindVertexBuffers recorded since the last report
  std::chrono::steady_clock::time_point base_cero_informe_{};
  static constexpr uint64_t kBaseCeroAComprobar = 200000;
  std::array<uint64_t, 19> sub_ns_{};  // C6 subetapas, measurement only
  std::array<uint64_t, 19> sub_n_{};
  uint64_t sub_muestras_ = 0;
  std::chrono::steady_clock::time_point sub_siguiente_{};
  uint64_t envios_tras_sombras_ = 0;         // for the report
  uint32_t estadisticas_pase_ = UINT32_MAX;
  uint32_t avisos_oclusion_dibujo_ = 0;  // diagnostic of the query draws
  std::chrono::steady_clock::time_point ultimo_aviso_oclusion_dibujo_{};
  VkCommandBuffer pase_comandos_ = VK_NULL_HANDLE;
  uint64_t pase_clave_ = 0;
  uint64_t pase_generacion_ = UINT64_MAX;
  uint32_t pase_ancho_ = 0;
  uint32_t pase_alto_ = 0;
  float pase_escala_ = 1.0f;  // 1 except in the scaled shadow map
  uint64_t borrados_profundidad_en_pase_ = 0;  // ZCULL
  std::chrono::steady_clock::time_point inicio_pase_{};  // pass change breakdown
  uint32_t pase_formatos_[5] = {};
  VkRenderPass pase_rp_ = VK_NULL_HANDLE;
  uint64_t grabacion_generacion_ = UINT64_MAX;
  VkPipeline pipeline_enlazado_ = VK_NULL_HANDLE;
  bool sets_enlazados_ = false;

  uint32_t diagnosticos_ = 0;
  std::unordered_set<uint32_t> avisados_vs_;
  uint32_t avisos_indices_ = 0;
  uint32_t avisos_swizzle_ = 0;
  std::unordered_set<uint64_t> diagnosticados_;
  uint64_t dibujados_ = 0;
  uint64_t dibujados_cronometrados_ = 0;  // of those, with the stage stopwatch
  uint32_t cronometro_contador_ = 0;
  bool cronometrar_ = false;  // the current draw carries the stage stopwatch
  // Rear-view mirror diagnostic: cubemap refreshes from their resolved faces (report every 10 s).
  uint64_t cubos_refrescados_ = 0;
  uint64_t cubos_refrescados_previos_ = 0;
  uint64_t fotograma_informe_cubos_ = 0;
  std::chrono::steady_clock::time_point informe_cubos_{};
  uint64_t rechazados_ = 0;
  // 8 stages. Stage 7 is the pass change, split from the pass stage to tell how much of the 3.9 us per
  // draw is the actual change (15 per frame at 162 us) and how much is what is done on every draw.
  std::array<uint64_t, kEtapasDibujo> etapas_ns_{};
  std::unordered_map<uint32_t, uint64_t> causas_;
  uint32_t ultima_causa_ = 0;
  uint32_t entrada_causa_ = 0;  // cause of the failed vertex input
  uint64_t subidas_textura_ = 0;
  uint64_t megas_bytes_ = 0;
  uint64_t bytes_texturas_ = 0;  // texture images created (C6 report)
  int32_t texturas_mb_max_ = 0;   // nfsmw_nativo_texturas_mb_max
  // Large slabs from which textures take chunks, instead of a dedicated allocation per texture (1.9 ms of
  // CPU each on Horizon). Terminar() goes in the destructor after the DestruirImagen loops, never before.
  PoolTexturas pool_texturas_;
  // Texture bind thread (nfsmw_nativo_texturas_enlace_hilo). See the BucleEnlaces block.
  struct PeticionEnlace {
    VkImage imagen = VK_NULL_HANDLE;
    VkDeviceMemory memoria = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkResult resultado = VK_NOT_READY;  // written by the thread, under enlaces_mutex_
    uint64_t ns = 0;                    // wall time inside vkBindImageMemory, on the thread
  };
  struct TexturaEnVuelo {  // ring thread only
    Textura* textura = nullptr;
    uint64_t ticket = 0;
    VkImage imagen = VK_NULL_HANDLE;  // the queued one (to remove it from imagenes_en_vuelo_ and move its views)
    VkResult resultado = VK_NOT_READY;
    VkFormat formato = VK_FORMAT_UNDEFINED;  // the fields below, to recreate it through CrearTextura if the bind fails
    uint32_t ancho = 0;
    uint32_t alto = 0;
    uint32_t capas = 1;
    uint32_t fondo = 0;
    uint32_t niveles = 1;
    bool copia = false;  // SubirTextura already left its data in the upload buffer
    VkDeviceSize copia_offset = 0;
    uint64_t copia_epoca = 0;
    VkCommandBuffer copia_comandos = VK_NULL_HANDLE;
  };
  struct VistaEnVuelo {  // ring thread only
    uint64_t clave = 0;
    VkImage imagen = VK_NULL_HANDLE;
    VkFormat formato = VK_FORMAT_UNDEFINED;
    uint32_t swizzle = 0;
    uint16_t swizzle_host = 0;
    uint32_t monton = 0;
    uint32_t ranura = 0;
  };
  static constexpr uint64_t kColaEnlaces = 256;  // potencia de 2
  static constexpr uint64_t kEnlacesAComprobar = 64;
  static constexpr int32_t kEnlacesSinDecidir = -1;
  static constexpr int32_t kEnlacesApagado = 0;
  static constexpr int32_t kEnlacesMirando = 1;
  static constexpr int32_t kEnlacesAplicando = 2;
  std::array<PeticionEnlace, kColaEnlaces> cola_enlaces_{};
  std::mutex enlaces_mutex_;
  std::condition_variable enlaces_cv_;         // there are requests or a stop request (the thread waits on it)
  std::condition_variable enlaces_hechos_cv_;  // the thread finished one (the ring waits on it)
  uint64_t enlaces_pedidos_ = 0;       // con enlaces_mutex_
  uint64_t enlaces_hechos_ = 0;        // con enlaces_mutex_
  uint64_t enlaces_recogidos_ = 0;     // con enlaces_mutex_
  bool enlaces_durmiendo_ = false;     // con enlaces_mutex_
  bool enlaces_esperando_ = false;     // con enlaces_mutex_
  bool enlaces_parar_ = false;         // con enlaces_mutex_
  bool enlaces_prioridad_ok_ = true;   // con enlaces_mutex_
  uint64_t ns_enlaces_hilo_ = 0;       // con enlaces_mutex_
  uint64_t ns_enlace_peor_ = 0;        // con enlaces_mutex_
  uint64_t enlaces_cola_llena_ = 0;    // con enlaces_mutex_
  std::thread enlaces_hilo_;
  int32_t enlaces_prioridad_ = 0x2D;  // written by the ring before creating the thread
  // Ring thread only:
  int32_t enlaces_fase_ = kEnlacesSinDecidir;
  bool enlaces_atascado_ = false;
  bool midiendo_creacion_ = false;
  bool recogiendo_enlaces_ = false;
  std::vector<TexturaEnVuelo> en_vuelo_;
  std::vector<VistaEnVuelo> vistas_en_vuelo_;
  std::unordered_set<VkImage> imagenes_en_vuelo_;
  uint64_t enlaces_comprobados_ = 0;
  uint64_t enlaces_hilo_total_ = 0;
  uint64_t enlaces_fallidos_ = 0;
  uint64_t vistas_aplazadas_ = 0;
  uint64_t copias_aplazadas_ = 0;
  uint64_t ranuras_perdidas_ = 0;
  uint64_t ns_espera_enlaces_total_ = 0;
  // The 10 s report (InformeEnlaces):
  std::chrono::steady_clock::time_point informe_enlaces_{};
  uint64_t enlaces_hilo_informe_ = 0;
  uint64_t esperas_enlaces_informe_ = 0;
  uint64_t ns_espera_enlaces_informe_ = 0;
  uint64_t ns_espera_enlaces_peor_ = 0;
  uint64_t ns_crear_informe_ = 0;
  uint64_t creadas_informe_previas_ = 0;
  uint64_t vistas_aplazadas_informe_ = 0;
  uint64_t copias_aplazadas_informe_ = 0;
  uint64_t ns_enlaces_hilo_previo_ = 0;
  uint64_t cola_llena_previa_ = 0;
  uint32_t informes_sin_compensar_ = 0;
  // Texture hash thread (nfsmw_nativo_texturas_huella_hilo). See the PlanearHuella block.
  std::array<TrabajoHuella, kColaHuellas> cola_huellas_{};  // a slot belongs to the thread while it is EnHilo
  std::mutex huellas_mutex_;
  std::condition_variable huellas_cv_;         // there are jobs or a stop request (the thread waits on it)
  std::condition_variable huellas_hechos_cv_;  // the thread finished one (the ring waits on it)
  uint64_t huellas_publicados_ = 0;   // under huellas_mutex_ (only the ring writes it)
  uint64_t huellas_tomados_ = 0;      // under huellas_mutex_: the thread has looked up to here
  uint64_t huellas_hechos_ = 0;       // con huellas_mutex_
  bool huellas_durmiendo_ = false;    // con huellas_mutex_
  bool huellas_esperando_ = false;    // con huellas_mutex_
  bool huellas_parar_ = false;        // con huellas_mutex_
  bool huellas_prioridad_ok_ = true;  // con huellas_mutex_
  bool huellas_nucleo_ok_ = true;     // con huellas_mutex_
  int huellas_nucleo_real_ = -1;      // under huellas_mutex_: where the thread ran when starting
  std::thread huellas_hilo_;
  int32_t huellas_prioridad_ = 0x2E;  // written by the ring before creating the thread
  int32_t huellas_nucleo_pedido_ = -2;
  int32_t huellas_nucleo_ = -1;       // written by the ring before creating the thread
  // The snapshots and the plans: the ring writes them before publishing each job; the thread only reads
  // them.
  std::unique_ptr<uint8_t[]> instantaneas_;
  std::unique_ptr<LecturaHuella[]> lecturas_huella_;
  uint64_t instantaneas_bytes_ = 0;
  // Ring thread only:
  int32_t huellas_fase_ = kHuellasSinDecidir;
  uint64_t huellas_recogidos_ = 0;
  uint64_t instantaneas_usado_ = 0;
  uint32_t lecturas_usadas_ = 0;
  std::array<LecturaHuella, kLecturasPorTextura> lecturas_plan_{};
  std::vector<TrabajoHuella> huellas_planeadas_;
  uint64_t huellas_planeadas_pendientes_ = 0;
  std::vector<TrabajoHuella> huellas_recogidas_;
  std::array<ComparacionHuella, kColaHuellas> comparaciones_huella_{};
  const Textura* comparacion_textura_ = nullptr;
  uint64_t comparacion_secuencia_ = 0;
  std::vector<uint8_t> huellas_datos_anillo_;
  std::vector<uint8_t> huellas_comprobacion_anillo_;
  bool huellas_rapido_apagado_anillo_ = false;
  bool huellas_sin_hilo_ = false;
  bool huellas_atascado_ = false;
  bool huellas_sospechosas_ = false;
  bool recogiendo_huellas_ = false;
  uint64_t huellas_comparaciones_planeadas_ = 0;
  uint64_t huellas_nuevas_aplicando_ = 0;
  uint64_t huellas_hilo_total_ = 0;
  uint64_t huellas_anillo_total_ = 0;
  uint64_t huellas_comparadas_ = 0;
  uint64_t huellas_sin_comparar_ = 0;
  uint64_t huellas_descartadas_ = 0;
  uint64_t huellas_resubidas_ = 0;
  // The 10 s report (InformeHuellas):
  std::chrono::steady_clock::time_point informe_huellas_{};
  uint64_t huellas_planeadas_informe_ = 0;
  uint64_t huellas_comparacion_informe_ = 0;
  uint64_t huellas_hilo_informe_ = 0;
  uint64_t huellas_anillo_informe_ = 0;
  uint64_t ns_huellas_hilo_informe_ = 0;
  uint64_t ns_huella_cruda_hilo_informe_ = 0;
  uint64_t ns_huellas_anillo_informe_ = 0;
  uint64_t ns_instantaneas_informe_ = 0;
  uint64_t bytes_instantaneas_informe_ = 0;
  uint64_t ns_espera_huellas_informe_ = 0;
  uint64_t ns_espera_huellas_peor_ = 0;
  uint64_t esperas_huellas_informe_ = 0;
  uint64_t huellas_sin_sitio_informe_ = 0;
  uint32_t informes_sin_compensar_huellas_ = 0;
  int32_t prueba_sin_memoria_cada_ = 0;  // nfsmw_nativo_prueba_sin_memoria_cada
  uint64_t reservas_de_textura_ = 0;
  bool soltando_por_falta_de_memoria_ = false;  // guard against reentry
  uint64_t soltadas_poco_a_poco_ = 0;
  uint64_t aviso_goteo_ = 0;
  uint64_t intento_expulsion_ = 0;
  uint64_t texturas_soltadas_ = 0;
  // Diagnostic of why the cache grows: creations per address and last key per shape (address, format and
  // size). Only touched when a texture is created.
  uint64_t texturas_creadas_ = 0;
  uint64_t creadas_en_direccion_vista_ = 0;
  uint64_t creadas_misma_forma_otra_clave_ = 0;
  std::unordered_map<uint32_t, uint32_t> creadas_por_direccion_;
  std::unordered_map<uint64_t, uint64_t> clave_por_forma_;
  std::unordered_map<uint64_t, std::array<uint32_t, 5>> palabras_por_forma_;
  uint64_t claves_incoherentes_ = 0;  // texture key guard (PrepararTextura)
  uint32_t avisos_otra_clave_ = 0;
  // "C6 contadores".
  uint64_t pases_empezados_ = 0;
  uint64_t envios_llenos_ = 0;
  uint64_t ns_envios_llenos_ = 0;
  uint64_t bytes_vertices_ = 0;
  uint64_t bytes_indices_subidos_ = 0;
  uint64_t samplers_preparados_ = 0;
  uint64_t samplers_cache_ = 0;
  uint64_t ns_pases_ = 0;
  uint64_t ns_vertices_ = 0;
  uint64_t entradas_calculadas_ = 0;
  uint64_t ns_entradas_ = 0;
  uint64_t entradas_reutilizadas_ = 0;
  uint64_t pases_por_generacion_ = 0;
  uint64_t pases_por_destino_ = 0;
  uint64_t pases_reanudados_ = 0;
  uint64_t texels_pases_ = 0;
  std::array<uint64_t, kGpuCategorias> texels_por_categoria_{};
  std::array<uint64_t, kGpuCategorias> dibujos_por_categoria_{};
  std::chrono::steady_clock::time_point inicio_alternancia_ps_ = std::chrono::steady_clock::now();
  bool alternancia_ps_anotada_ = false;
  bool alternancia_solo_alfa_anotada_ = false;
  bool inv_tamano_tex_ = false;
  bool pcf_barato_ = false;                    // a single shadow map sample
  bool pase_coches_sombra_ = false;            // nfsmw_nativo_sombra_minimo, car pass
  bool sin_desenfoque_fotograma_ = true;       // nfsmw_nativo_sin_desenfoque, per frame
  bool mip_puntual_prueba_ = false;            // Test: trilinear -> bilinear (measurement only)
  // Deduplication of vertex uploads within the frame.
  DedupeVertices dedupe_;
  bool dedupe_activo_ = true;
  uint32_t dedupe_sinc_vista_ = 0;  // last value seen of g_sincronizaciones_anillo
  std::set<uint64_t> avisos_invtam_resuelta_;  // one trace per size
  bool alternancia_tijera_anotada_ = false;
  std::unordered_map<const EntradaShader*, VkShaderModule> modulos_solo_alfa_;
  // The same module with OpExecutionMode EarlyFragmentTests (nfsmw_nativo_z_temprana).
  std::unordered_map<const EntradaShader*, VkShaderModule> modulos_z_temprana_;
  uint64_t dibujos_ps_inutil_ = 0;     // no color and a PS that does not discard
  uint64_t dibujos_ps_necesario_ = 0;  // no color, but the PS is needed
  uint64_t sombras_alfa_activa_ = 0;
  uint64_t escena_con_descarte_ = 0;
  uint64_t escena_sin_descarte_ = 0;   // shadow map draws with the alpha test enabled
  /*
   * nfsmw_nativo_z_temprana and nfsmw_nativo_saltar_invisibles. The first four split C6's "lo impiden"
   * in two: those fixed by testing earlier and those that cannot be fixed because they write depth or
   * stencil (the latter are the exact size of a depth pre-pass).
   */
  enum : uint32_t {
    kZPuesta = 0,       // its depth test is moved earlier
    kZEscribeZ,         // not possible: the draw writes depth
    kZEstencil,         // not possible: stencil is involved
    kZOclusion,         // left alone: a game occlusion query is open
    kZYaTemprano,       // no alpha test or kill: the GPU already tested early
    kInvisibleMezcla,   // blending copies the destination: the draw paints nothing
    kInvisibleAlfa,     // alpha test with the NEVER function
    kInvisibleSoloColor,  // invisible in color but writes Z: only its color is removed
    kZCuentas
  };
  std::array<uint64_t, kZCuentas> cuentas_z_{};
  std::array<uint64_t, kZCuentas> cuentas_z_previas_{};
  uint64_t z_temprana_sin_modulo_ = 0;
  uint64_t fotogramas_z_ = 0;
  uint64_t fotogramas_z_previos_ = 0;
  std::chrono::steady_clock::time_point ultimo_informe_z_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point inicio_alternancia_z_ = std::chrono::steady_clock::now();
  bool z_temprana_ = true;
  bool alternancia_z_anotada_ = false;
  bool saltar_invisibles_ = true;
  uint64_t sombras_alfa_apagada_ = 0;  // ... and without it (the stage would be unnecessary)
  std::array<uint64_t, kGpuCategorias> triangulos_por_categoria_{};
  std::array<uint64_t, kGpuCategorias> pases_por_categoria_{};
  uint64_t ns_render_pass_ = 0;
  // Prueba nfsmw_nativo_omitir_sombras_alternar_s.
  std::chrono::steady_clock::time_point inicio_sombras_ = std::chrono::steady_clock::now();
  bool sombras_omitidas_ = false;
  // Diagnostico nfsmw_nativo_diag_vertices_repetidos.
  struct VerticesVistos {
    uint64_t fotograma = UINT64_MAX;
    uint64_t huella = 0;
  };
  std::unordered_map<uint64_t, VerticesVistos> vertices_vistos_;
  uint64_t bytes_repetidos_fotograma_ = 0;
  uint64_t bytes_iguales_anterior_ = 0;
  uint64_t ns_hash_vertices_ = 0;
  // Last texture and sampler result of each PS sampler register.
  struct CacheSampler {
    std::array<uint32_t, 6> fetch{};
    uint64_t fotograma = UINT64_MAX;
    uint64_t generacion = 0;
    uint32_t ranura = 0;
    uint32_t monton = 0;
    uint32_t sampler = 0;
    uint64_t valido_hasta = 0;  // last frame it is valid without going back to PrepararTextura
    uint32_t ancho = 0;  // host image size, for 1/size
    uint32_t alto = 0;
  };
  bool cache_entre_fotogramas_ = true;  // nfsmw_nativo_cache_texturas_entre_fotogramas
  bool mipmaps_ = true;                 // nfsmw_nativo_mipmaps
  bool diag_mips_ = false;              // nfsmw_nativo_diag_mips
  uint64_t mips_revisadas_ = 0;
  uint64_t mips_raras_ = 0;
  uint64_t niveles_mip_subidos_ = 0;    // report: mip levels (without the base) in created textures
  std::array<CacheSampler, 16> cache_samplers_{};
  // Second cache, by the whole fetch constant: a direct-mapped table (the hash picks the slot). It went
  // from 256 to 1024 slots because in a race there are ~340 distinct textures per frame and 256 slots
  // collided (64 KB: fits in L2).
  /*
   * From 1024 to 4096 (288 KB, still fits in the 2 MB L2). At the Heritage & Omega exit there are ~3,000
   * draws per frame and ~205 misses of this table per frame (C6 counters: 97,709 in 20 s), at ~15 us
   * each through PrepararTextura (the texture stage measures 1.2-1.5 us per draw with 3.4 % misses). With
   * N distinct fetches in M slots, the fraction sharing a slot with another is 1 - e^(-N/M): with ~600
   * that is 44 % at 1024 and 14 % at 4096. The hit conditions do not change: there is just more room. The
   * misses-by-cause report (every 10 s) says how many were collisions.
   */
  std::array<CacheSampler, 4096> cache_fetch_{};
  uint64_t samplers_cache_fetch_ = 0;
  uint64_t fetch_fallos_choque_ = 0;      // The slot held another fetch constant
  uint64_t fetch_fallos_vacia_ = 0;       // casilla sin usar todavia
  uint64_t fetch_fallos_generacion_ = 0;  // same fetch, but generacion_texturas_ changed
  uint64_t fetch_fallos_caducada_ = 0;    // same fetch, but the texture is due for a check (valido_hasta)
  std::array<uint64_t, 6> fetch_informe_previos_{};
  std::chrono::steady_clock::time_point fetch_informe_{};
  uint64_t generacion_texturas_ = 0;  // changes with every C2 copy and every retired image
  // On-disk pipeline cache (CargarCachePipelines and GuardarCachePipelines).
  using FnCrearCachePipelines = VkResult(VKAPI_PTR*)(VkDevice, const VkPipelineCacheCreateInfo*,
                                                     const VkAllocationCallbacks*,
                                                     VkPipelineCache*);
  using FnDatosCachePipelines = VkResult(VKAPI_PTR*)(VkDevice, VkPipelineCache, size_t*, void*);
  using FnDestruirCachePipelines = void(VKAPI_PTR*)(VkDevice, VkPipelineCache,
                                                    const VkAllocationCallbacks*);
  FnDatosCachePipelines datos_cache_ = nullptr;
  FnDestruirCachePipelines destruir_cache_ = nullptr;
  VkPipelineCache cache_pipelines_ = VK_NULL_HANDLE;
  uint32_t pipelines_sin_guardar_ = 0;
  uint32_t guardados_cache_ = 0;
  size_t bytes_cache_guardados_ = 0;  // size of the last file read or saved
  std::chrono::steady_clock::time_point cache_guardada_{};
  // Thread that writes the pipeline cache to disk (EscritorCacheMain). It is persistent, because on
  // Horizon detach() closes the game, and wake-ups are decided with the lock held.
  std::thread escritor_cache_;
  std::mutex escritor_mutex_;
  std::condition_variable escritor_aviso_;
  std::vector<uint8_t> escritor_datos_;
  bool escritor_pendiente_ = false;
  bool escritor_parar_ = false;
  // The single pipelines file. The last written version of each part (or the one read at start-up): owned
  // by the writer thread once it is running, and by Inicializar before that. lista_leida_ goes from
  // CargarCachePipelines to CargarListaPipelines.
  std::vector<uint8_t> escrito_cache_;
  std::vector<uint8_t> escrito_lista_;
  std::vector<uint8_t> lista_leida_;
  bool ficheros_viejos_ = false;  // the two older files were read: deleted when the new one is written
  uint64_t ns_pipelines_ = 0;  // creando pipelines (informe C6)
  std::unordered_set<uint32_t> avisados_;
  // Pipeline prewarm (BuclePrecalentado). lista_archivo_ and estado_lista_ are sized before the thread
  // starts and never change size; the thread writes estado_lista_[i] before publishing
  // precalentado_hasta_ > i. Anything not atomic and not marked otherwise belongs to the ring only.
  std::vector<RegistroPipeline> lista_archivo_;         // the list read at start-up (walked by the thread)
  std::vector<uint8_t> estado_lista_;                   // kLista* of each record of lista_archivo_
  std::unordered_map<uint64_t, size_t> indice_lista_;   // XXH3 of the key -> index (SIZE_MAX: from this session)
  std::vector<RegistroPipeline> lista_sesion_;          // this session's new ones
  uint32_t lista_sin_guardar_ = 0;
  std::vector<uint8_t> escritor_lista_;                 // con escritor_mutex_
  std::thread precalentado_hilo_;
  const ShadersNativos* biblioteca_precalentado_ = nullptr;
  uint32_t precalentado_eds_ = 0;
  bool precalentado_decidido_ = false;
  bool precalentado_diferencia_ = false;
  std::chrono::steady_clock::time_point precalentado_inicio_{};
  std::chrono::steady_clock::time_point precalentado_informe_{};
  uint64_t precalentado_cambios_previos_ = 0;
  std::atomic<bool> precalentado_parar_{false};
  std::atomic<bool> precalentado_terminado_{false};
  std::atomic<size_t> precalentado_hasta_{0};
  std::atomic<uint32_t> precalentado_hechos_{0};
  std::atomic<uint32_t> precalentado_compilados_{0};
  std::atomic<uint64_t> precalentado_ns_compilados_{0};
  std::atomic<uint32_t> precalentado_sin_shader_{0};
  std::atomic<uint32_t> precalentado_otro_modo_{0};
  std::atomic<uint32_t> precalentado_fallidos_{0};
  std::atomic<int32_t> precalentado_prioridad_{-1};
  uint32_t precalentado_contados_ = 0;  // of precalentado_compilados_, already added to pipelines_sin_guardar_
  uint64_t anillo_precalentados_ = 0, anillo_precalentados_lentos_ = 0, ns_anillo_precalentados_ = 0;
  uint64_t anillo_de_lista_ = 0, ns_anillo_de_lista_ = 0;
  uint64_t anillo_nuevos_ = 0, ns_anillo_nuevos_ = 0, anillo_a_lista_ = 0;
};

}  // namespace

std::unique_ptr<DibujosVulkan> DibujosVulkan::Crear(const VulkanDevice* dispositivo,
                                                    rex::memory::Memory* memoria,
                                                    ContextoDestinos* contexto) {
  if (!dispositivo || !memoria || !contexto) {
    return nullptr;
  }
  auto dibujos = std::make_unique<DibujosVulkanImpl>(dispositivo, memoria, contexto);
  if (!dibujos->Inicializar()) {
    REXLOG_ERROR("[nativo] C6: no se pudieron preparar los dibujos nativos");
    return nullptr;
  }
  REXLOG_INFO("[nativo] C6: dibujos nativos preparados");
  return dibujos;
}

}  // namespace nfsmw::nativo
