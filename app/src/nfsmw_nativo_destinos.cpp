// nfsmw - native renderer, part C2 (see nfsmw_nativo_destinos.h).
//
// WHAT IT COVERS
//   - k_8_8_8_8 and k_8_8_8_8_GAMMA color render targets without MSAA, as
//     R8G8B8A8 images of pitch x max(720, pitch) (capped at 2048).
//   - Copy of a rectangle of the render target to the resolved texture at
//     RB_COPY_DEST_BASE (destination format k_8_8_8_8), with the rectangle and
//     the base computed like GetResolveInfo (graphics/util/draw.cpp:765-1010).
//   - Render targets with MSAA: used with 1 sample (MSAA hangs NVK on the Switch).
//   - Color clear with RB_COLOR_CLEAR, converted like the emulation does
//     (vulkan/render_target_cache.cpp:5350-5355).
//   - Presentation: the resolved texture requested by fetch constant 0 of the
//     Swap is drawn into the presenter output with the SDK's own shaders. By
//     default, with the gamma ramp the game loads, like the Xbox 360 display
//     (shaders/nfsmw_salida_rampa_gamma.frag).
//
// DRAWS (parts C3-C6, nfsmw_nativo_dibujos.cpp)
//   This class lends them the render targets (color and depth), the frame's
//   command buffer and an upload one that is submitted right before it.
//
// WHAT IT DOES NOT COVER
//   Copies from depth, 16- and 32-bit formats, 3D textures as destination,
//   clearing only the rectangle (the whole image is cleared) and the normal
//   draws (parts C3-C6, in nfsmw_nativo_dibujos.cpp).
//
// Everything is used only by the native system's PM4 ring thread: no locks.

#include "nfsmw_nativo_destinos.h"
#include "nfsmw_esperas_tiron.h"
#include "nfsmw_nativo_shaders.h"  // Samplers of the PS (nfsmw_nativo_diag_lectores_s)
#include "nfsmw_nativo_sincronizacion.h"
#include "nfsmw_gpu_compatibilidad.h"
#include "nfsmw_reflejo_demanda.h"  // road reflection only when it is read

#include "nfsmw_ajustes_graficos.h"

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

#include <algorithm>
#include <array>
#include <chrono>

#if REX_PLATFORM_SWITCH
// Counter 0 of the console profiler (game FPS), the same one IssueSwap of the emulated path uses.
extern "C" void RexSwitchPerfCount(unsigned id);
// Interval of a long frame for stack sampling ("durante los tirones" section).
extern "C" void RexSwitchPerfTiron(uint64_t inicio, uint64_t fin);
#endif
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_STRING(nfsmw_consultas_oclusion, "auto", "NFSMW",
                     "Consultas Vulkan: auto evita oclusiones en Mali Android; off las evita en todas las GPU; on las permite")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_nativo_sincronizacion_gpu, true, "NFSMW",
                    "Sincroniza subidas, reflejos y lecturas de imagenes entre pases Vulkan")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(nfsmw_nativo_resolver_sin_copia_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (prueba, build 154): con N > 0 alterna copiar e intercambiar cada N "
                     "segundos, para comparar capturas del mismo sitio con el juego en pausa");
REXCVAR_DEFINE_INT32(nfsmw_nativo_sombras_escala, 100, "NFSMW",
                     "Renderizador nativo (19/09, build 201): dibuja los dos mapas de sombras de 1600x1600 "
                     "del juego a este porcentaje y los sube de tamano al resolverlos. 100 = como la Xbox 360 "
                     "(1600, que son 2000 de sus 2048 baldosas de EDRAM); 64 = como la version de PC de este "
                     "mismo juego (1024). El valor se toma al crear el primer mapa y no cambia en marcha")
    .range(50, 100);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_resolver_sin_copia, true, "NFSMW",
                    "Renderizador nativo (17/09, build 154; por defecto desde la 161, medido en consola: -2,36 ms "
                    "de los 7,41 de copias): al resolver un destino entero a una textura del mismo "
                    "tamano y formato, intercambia las imagenes en vez de copiar los pixeles. La imagen no cambia; si "
                    "el juego vuelve a dibujar en ese destino sin borrarlo antes, se restaura la copia");
/*
 * Swap also when the command does not clear the render target.
 *
 * The game resolves the shadow map twice per frame, once per cascade. The first command clears the
 * render target and therefore swaps (free); the second does not clear, and therefore copied the whole
 * 1600x1600: 2.56 Mpixels, 2.4 ms of real GPU time measured, 44 % of all copies in the frame.
 *
 * Requiring the command to clear was a precaution, not a necessity: if the game draws into the render
 * target again without clearing it, RestaurarContenido already brings the image back from the resolved
 * texture. So the worst case is paying the same copy later (net zero), not a regression. And the game
 * starts the next frame by clearing the shadow map, so there should not be a single restore.
 *
 * How to check it in the log: "C2 resoluciones sin copia" must go from ~0.93 to ~1.9 per frame and
 * "restauraciones" must stay at 0. If the restores go up, this does not help and should be turned off:
 * we would be paying for the copy anyway, just somewhere else.
 */
/*
 * Swap the color too, not only the depth.
 *
 * Image swapping (resolving without copying) works for depth and is worth 2.36 ms measured. For color it
 * never could, not because of the mechanism but because of a silly size detail: the color render target is
 * created with `height = max(720, pitch)`, so with pitch 1280 it is 1280x1280, while the resolved texture is
 * 1280x720. Since IntercambiarConResuelta requires the same size, it always said no.
 *
 * The idea: the game never draws below row 720 in a color render target (the viewport and the scissor are
 * 1280x720 in every draw measured), so the pitch-1280 color render target was created 720 high and both
 * sizes matched.
 *
 * That is 4 copies of 1280x720 per frame = 3.69 Mpixels, 78 % of all copy traffic.
 *
 * Result (tested): it does not work, for two separate reasons.
 *   1. Without requiring a clear: 16,807 color swaps and 17,350 restores, almost one for one. The game
 *      draws on top again without clearing, so the copy is paid anyway, later and with two operations
 *      instead of one.
 *   2. Requiring a clear and creating the render target 720 high: the scene breaks (the screen fills with
 *      a yellow smear). The color render target cannot be shrunk to 720.
 * It stays off. The code is kept because the mechanism is correct; what fails is that this game reuses the
 * color render target without clearing it.
 *
 * The safety net is mandatory and did not exist for color: DestinoColor did not call RestaurarContenido
 * (DestinoProfundidad did). Without it, the first frame in which the game draws on top without clearing
 * would see the previous frame's content. It is added here.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_intercambiar_color, false, "NFSMW",
                    "Renderizador nativo (20/09): al resolver el destino de color entero a una textura del "
                    "mismo tamano, intercambiar las imagenes en vez de copiar 1280x720 pixeles. Son 4 copias "
                    "por fotograma, el 78 % del trafico de copias. La imagen no cambia; si el juego dibuja "
                    "encima sin borrar, se restaura");

REXCVAR_DEFINE_BOOL(nfsmw_nativo_intercambiar_sin_borrado, true, "NFSMW",
                    "Renderizador nativo (20/09): intercambiar la imagen tambien cuando la orden de resolve no "
                    "borra el destino. Se lleva por delante la copia de 1600x1600 del segundo mapa de sombras "
                    "(2,4 ms reales). Si el juego dibuja encima sin borrar, se restaura: peor caso, lo mismo "
                    "que ahora. Vigila 'restauraciones' en el log: tiene que quedarse en 0");
/*
 * The flickering shadows of the main menu. This was the cause.
 *
 * A resolve without a clear that is done by swapping leaves the content in the texture and the render target
 * with that texture's old image (contenido_invalido). It was only brought back when drawing on top
 * (DestinoProfundidad) and never before another resolve, which read the old image as it was.
 *
 * In a race it does not happen: between the two resolves of the shadow map the cars are drawn and that
 * brings it back. In the menu it does, every frame. The garage draws the car into the 1600x1600 map and
 * resolves twice in a row, without drawing in between: without a clear to 07CEA000 (read by the car body) and
 * with a clear to 086AE000 (read by the scenery). On the console, in the menu: 398 swaps without a clear + 398
 * with a clear and 0 restores in 10 s. With images A, B and C:
 *     draw into A -> resolve to 07CEA000: the texture keeps A (correct), the render target gets B (the old one)
 *                 -> resolve to 086AE000: the texture keeps B: the previous frame's map
 * The scenery always sampled the map from one frame earlier with the current frame's light matrix. While the
 * shadow framing does not change, it is not noticeable. As soon as it jumps (the menu camera comes in
 * rotating) the old map lands shifted and the wall to the right of the car goes dark for one frame: a
 * console capture shows it every ~4 game frames between seconds 8 and 9.
 *
 * Now, if the render target about to be resolved has its content in another texture, it is first brought
 * back by copying (without lending or the minimum trick: this is a resolve, not the car pass), and the
 * resolve continues as usual. It is the exact path: on the Xbox 360 both resolves read the same EDRAM. It
 * costs one 1600x1600 copy per frame in the menu (~1.5 ms of GPU); in a race nothing is touched because the
 * render target already arrives valid. Report line: "C2 restauraciones por fotograma", the "para
 * resolver" figure. false = previous behavior.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_resolver_contenido_valido, true, "NFSMW",
                    "Renderizador nativo (26/09, build 193): antes de resolver un destino de profundidad cuyo contenido "
                    "se fue en un intercambio, traerlo de vuelta. Arregla el parpadeo de las sombras del menu (el "
                    "escenario muestreaba el mapa del fotograma anterior). false = como la 192");
/*
 * The verdict on intercambiar_sin_borrado, with numbers measured on the console.
 *
 * Console log, three consecutive 10 s intervals in a race:
 *     swaps without clear       4679 -> 4885 -> 5145 -> 5369   (+206, +260, +224)
 *     restores                  3584 -> 3790 -> 4050 -> 4274   (+206, +260, +224)
 * One restore per swap, exactly, in all three intervals. So the pixel saving is exactly zero: the
 * 1600x1600 copy is not avoided, it is paid later and with two operations (swap + restore) instead of one.
 *
 * And it is the missing piece to make the "copias" breakdown of the C2 report add up:
 *     recorded inventory  2.67 Mpixels x 0.78 ms/Mpixel = 2.08 ms
 *     restores            0.83/frame x 2.56 Mpixels x 0.78 = 1.66 ms  <-- not recorded
 *     scaling blit 1280x720 -> 1024x576 (internal resolution)         = 0.49 ms
 *                                                            total    4.23 ms
 *     measured: 2.69 raw x 1.627 = 4.38 ms real.
 * Restores were 38 % of the frame's copies and did not show up in any bucket. They are recorded from here
 * on (AnotarCopia in RestaurarContenido), so that the inventory adds up by itself.
 *
 * The setting is not turned off: turning it off moves the copy back to the resolve and costs the same. What
 * has to go is the restore, which is what the two settings below are for.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_restaurar_area_util, true, "NFSMW",
                    "Renderizador nativo (20/09 noche): al traer de vuelta el contenido de un destino "
                    "intercambiado, copiar solo las filas que el juego resuelve de verdad en vez de la imagen "
                    "entera. Los destinos se crean con alto = max(720, pitch) (la escena es 1280x1280 para "
                    "dibujar 1280x720), y lo de debajo del area util no se dibuja ni se lee nunca");
/*
 * Restore without copying a single pixel.
 *
 * The render target lost its content in a swap: it is in the resolved texture R. Normally R -> target is
 * copied (10.24 MB for the shadow map, 1.66 ms real per frame). But the content does not have to be
 * duplicated, it only has to be in the render target: swapping the two images again is enough. Cost: zero
 * bytes, only handles.
 *
 * What it costs: R keeps the render target's old image until the game resolves to that address again. For
 * the shadow map that happens in the same frame and before anybody reads it:
 *     [cascade 1 pass]  <- restored here, R is lent out
 *     resolve cascade 1 -> another address
 *     [cascade 2 pass]
 *     resolve cascade 2 -> R          <- R gets good content back
 *     reflection, cubemap, SCENE      <- the scene is what samples R
 * So in steady state it is safe. In transitions (menu, loading, pause) it may not be.
 *
 * That is why it is off and watched: 'prestadas leidas' in the C2 report counts the times someone
 * requested a resolved texture while it was lent out. If the log says 0, this is worth 1.66 ms real and
 * gets turned on. If it says anything else, the image could show the previous frame's shadows and it stays
 * off.
 */
/*
 * It stayed on, but the watchdog reading flipped. Read this.
 *
 * The mix-up: it was turned on citing "prestadas leidas 0, limpio" from an earlier run. That zero proved
 * nothing: the cvar was false in the toml, not a single image was lent, and the watchdog counted zeros
 * because it had nothing to count. The first session in which this path actually ran says the opposite:
 *     prestadas leidas 8226   -> *** EL PRESTAMO NO ES SEGURO ***
 * One read per frame, exactly, during the five minutes of racing. The premise written above ("in steady
 * state it is safe, the risk is the transitions") is backwards: the transitions gave 9 isolated reads and
 * the steady state gives one every time.
 *
 * And even so it stayed on, because what it buys is measured and was the best variance improvement so far,
 * and variance is what the player notices (same load, without vs. with):
 *     copies         4.29 -> 1.93 ms real        frame       38.63 -> 36.11 ms
 *     deviation      8.19 -> 7.06 ms             > 50 ms     6.14 % -> 3.66 %
 *     frame median   43.0 -> 33.3 ms
 * Turning it off brings the median back to 43 ms, for a risk that in six minutes of play did not produce a
 * single visible fault. The "C2 prestadas leidas, por direccion" line reports what is read, with address
 * and size: if it is the 1600x1600 shadow map, it has to be fixed (by returning the render target, which is
 * where the good content is); anything else may be harmless.
 */
/*
 * Off. This is what left the car without a shadow (bridges, trees).
 *
 * The race shadow pass resolves the same render target twice (sub_82443B18):
 *   1. after the world, without a clear  -> texture[1] ([0x82A15374], 07CEA000)   sub_824427F8
 *   2. after the cars, with a clear      -> texture[0] ([0x82A15370], 086AE000)   sub_82442908
 * The car body (effect type 7, sub_824511E8) samples 1 (the map without cars, so as not to shadow itself)
 * and the world samples 0. Between the two resolves the game draws the cars on top without clearing, so
 * here the render target was restored by swapping: texture[1] got its old image back and stayed lent out
 * until the next frame's resolve. The scene reads it before that: in a console run, the increase of
 * "prestadas leidas" is exactly the number of reads of 07CEA000 in each race report (369/369, 385/385,
 * 336/336...). And since it always gets the same image back, the car does not read the previous frame's
 * map: it reads one frozen since the start of the race. The ground darkens under the bridge and the car
 * does not.
 *
 * It costs the 1600x1600 copy this used to save (2.56 Mpixels, ~2.00 ms real per frame in a race): that
 * saving was fake. With false, "prestadas leidas" and "por intercambio" must stay at 0.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_restaurar_por_intercambio, false, "NFSMW",
                    "Renderizador nativo (20/09 noche; APAGADO el 25/09): traer de vuelta el contenido de "
                    "un destino intercambiado volviendo a intercambiar las imagenes en vez de copiarlas. NO "
                    "ENCENDER: deja prestada la textura[1] del mapa de sombras, la que muestrea el coche, y "
                    "el coche deja de recibir sombras. Vigila 'prestadas leidas' en el log: tiene que ser 0");
/*
 * Depth that nobody samples is not copied (nfsmw_nativo_profundidad_perezosa).
 *
 * Every race frame the game resolves the scene depth to a 1024x576 texture (091F0000 in the logs: 0.59
 * Mpixels and one copy per frame, "C2 caras resueltas") and the only reader is the final composition
 * p_000139 (PS n19): it is its HEIGHTMAP, which only feeds the radial blur factor. With
 * nfsmw_nativo_sin_desenfoque (true by default) that sampling is dead (the specialization constant cuts
 * the blend and the compiler removes it), but the copy was still being paid: ~0.35 ms real per frame
 * (0.60 ms per Mpixel, which is what the 1600x1600 copy cost when it came back: copies went from 1.93 to
 * 3.47 ms).
 *
 * Now that copy is deferred, and only if in the last kPerezosaFotogramas frames that address was requested
 * by the composition without blur and no draw really sampled it. It is kept pending (source, destination,
 * rectangle):
 *   - if a draw really samples it (another shader, p_000140 with depth of field, or the composition with the
 *     blur on), it is copied right before that draw. The source has not changed: every write to it first
 *     goes through AntesDeEscribirProfundidad (pass, clear, restore, swap and destruction);
 *   - if the source is about to be written in another frame and the composition already requested it,
 *     nobody really read it: it is dropped without copying (the normal case in a race: the next frame's
 *     scene clear);
 *   - if the source is about to be written in the same frame, or without the composition having requested
 *     it, it is copied (exact);
 *   - if another resolve arrives at the same address while the copy is still deferred, it is copied first
 *     (exact).
 * GUARD: if a draw really samples an address whose copy was dropped (a late read: a previous frame's depth
 * after the next frame has started drawing), DIFERENCIA in the log and it turns off for the session.
 * Report line: "C2 profundidad perezosa". false = always copy, as before.
 */
// The first version saved nothing (the copy was recorded if the source was rewritten in the same frame, which
// is always the case); with the current AntesDeEscribirProfundidad rule it is dropped. On again.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_profundidad_perezosa, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): la profundidad que el juego resuelve para la composicion "
                    "final solo se copia si un dibujo la muestrea de verdad (sin el desenfoque, ninguno). La imagen no "
                    "cambia; ~0,35 ms de GPU por fotograma. Se comprueba sola. false = copiar siempre, como antes");
/*
 * The front buffer is drawn from its render target, without copying it (nfsmw_nativo_frontal_perezoso).
 *
 * At the end of each frame the game resolves the 1024x576 output (the pitch-1040 render target) to one of
 * its two front buffers (09430000 and 09670000) and the Swap draws it with PintarSalida. That is 0.59
 * Mpixels of copying per frame (~0.35 ms real, at 0.60 ms per Mpixel) and across the measured sessions no
 * draw has ever sampled a front buffer: 0 reads in every session ("C2 caras resueltas"). Only the Swap
 * reads them.
 *
 * If the address is a front buffer (a Swap drew it in the last kFrontalFotogramas and no draw sampled it)
 * and the copy is 1 to 1, of the whole texture and from the corner of the render target, it is deferred:
 *   - in the Swap it is drawn from the image that holds the content with the exact variant of the output
 *     (texelFetch of the pixel, output of the front buffer's size and without FXAA, which is the usual
 *     one): texel (x, y) of that image is the same byte the copy would have left at (x, y) of the texture.
 *     With FXAA, without the ramp or with another output, the copy is recorded before submitting the work
 *     and the texture is drawn, as always;
 *   - if the game clears the whole render target (which it does when the next frame starts), the render
 *     target gets a spare image of the same size and the one with the content is kept for the front
 *     buffer: the clear does not need what was there, so nothing is copied. There are at most
 *     kFrontalImagenesMax spares (~4 MB each);
 *   - if the render target is written some other way first (a pass, a restore, a swap), if a draw samples
 *     the front buffer or if another resolve arrives that does not cover it entirely, the copy is recorded
 *     first (exact);
 *   - if another resolve arrives that covers it entirely, the deferred copy is unnecessary and dropped.
 * The content is never lost: the image is the same by construction. GUARD: if something requests a front
 * buffer whose copy can no longer be made, DIFERENCIA in the log and it turns off for the session. Report
 * line: "C2 frontal perezoso". false = always copy, as before.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_frontal_perezoso, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el Swap pinta el frontal desde su destino de render (o una "
                    "imagen retenida) en vez de copiarlo antes a la textura: 0,59 Mpixeles menos por fotograma. Misma "
                    "imagen; con FXAA se copia como siempre. Se comprueba sola. false = copiar siempre, como antes");
/*
 * The shadow map without the 1600x1600 copy (nfsmw_nativo_sombra_minimo).
 *
 * The race shadow pass (sub_82443B18) draws the world into the 1600x1600 render target, resolves it without
 * a clear to texture[1] (07CEA000: the map without cars, sampled by the car body), draws the cars on top and
 * resolves it with a clear to texture[0] (086AE000: the map with cars, sampled by the world). Since we
 * resolve by swapping images, after the first resolve the render target no longer holds the world and it
 * has to be brought back before the cars: that is the 1600x1600 copy (2.56 Mpixels, ~1.5 ms real per frame,
 * the most expensive copy in the frame).
 *
 * With this setting the render target is cleared to 1.0 instead of copied and the cars are drawn alone.
 * texture[0] ends up with the cars only, and the draws that sample it through their SHADOWMAP_SAMPLER
 * (tfetch2DSombraMin in the library) get min(texture[0], texture[1]): the 3D index word of that register
 * carries texture[1] and the pipeline carries the SPEC_CONSTANT_SOMBRA_MINIMO bit (23). This is exact, not
 * an approximation: with Z writes and a LESS or LEQUAL test, what a draw leaves on a buffer is the minimum
 * of what was there and of its fragments, and min(world, min(1, cars)) = min(world, cars) because no texel
 * exceeds 1.0. The map is point-sampled and both views carry the same swizzle: the result is the same value
 * the copy would have left. The depth bias is the same on both paths (same pipelines). With the cheap PCF
 * there is one extra read per fragment that samples the world shadow; with the 9-sample PCF it would be nine
 * and it does not pay off: without nfsmw_nativo_pcf_barato it copies as always.
 *
 * It costs a clear of 2.56 Mpixels (~0.19 ms at 0.075 ms per Mpixel) and that read; it saves the copy
 * (~1.54 ms).
 *
 * GUARD, self-checking:
 *   - WATCHING: it copies as always and watches the whole cycle (resolve without a clear by swapping, car pass
 *     on the same render target, resolve with a clear by swapping), that every draw of the car pass is exact
 *     (pass without color, without stencil, without an occlusion query and, if it writes Z, a NEVER, LESS or
 *     LEQUAL test) and that every read of texture[0] comes from a shader with tfetch2DSombraMin in that
 *     register and outside the window between the two resolves. Those reads already use the pipeline with
 *     the bit (minimum with itself: the same texel), so switching to applying does not create pipelines in
 *     the middle of a race. After kSombraMinimoCiclos clean cycles in a row it moves to
 *   - APPLYING: clears instead of copying. If anything watched fails, DIFERENCIA in the log (REXLOG_ERROR) and
 *   - OFF for the session: it copies again. A library without tfetch2DSombraMin never leaves watching.
 * Report line: "C2 sombra por minimo". false = always copy, as before.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_sombra_minimo, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): el mapa de sombras del mundo ya no se restaura copiando "
                    "1600x1600; los coches se dibujan sobre el destino borrado y el mundo muestrea el minimo de las dos "
                    "texturas. Misma imagen. Necesita la biblioteca con tfetch2DSombraMin. Se comprueba sola. false = "
                    "copiar como antes");
REXCVAR_DEFINE_INT32(nfsmw_nativo_sombra_minimo_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 184, prueba): con N > 0 y la guardia ya aplicando, alterna cada N "
                     "segundos el minimo (tramos pares) y la copia de siempre (impares), para medir la ganancia neta en la "
                     "consola con 'C2: GPU por Swap'. 0 = sin alternar");
/*
 * Repeated clears.
 *
 * Clears are 0.41 raw = 0.67 ms real per frame (9 per frame: 4 color and 5 depth).
 * vkCmdClearColorImage and vkCmdClearDepthStencilImage clear the whole image, and the game requests some
 * clears on a render target that is already cleared to that same value and has not been drawn to since.
 * That clear does not change a single bit.
 *
 * The condition is deliberately conservative: it is skipped only if (a) the last clear of that render
 * target used the same value, (b) the global draw counter has not gone up since then (meaning nothing has
 * been drawn anywhere) and (c) the render target has not changed image through a swap nor received a
 * restore. With that it is impossible to skip a clear that is needed.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_saltar_borrados_repetidos, true, "NFSMW",
                    "Renderizador nativo (20/09 noche): saltarse un borrado cuando el destino ya esta borrado "
                    "con ese mismo valor y no se ha dibujado nada desde entonces. No cambia ni un pixel; "
                    "'borrados saltados' en el informe C2 dice cuantos se ahorran");
/*
 * How much of each clear is used (nfsmw_nativo_diag_borrados).
 *
 * Clears are ~0.70 ms real per frame in a race (8 color and 9 depth) and they all clear the whole image:
 * 1280x1280 for a 1280x720 scene, 1040x1040 for a 1024x576 output, 320x720 for a 256x256 cubemap face. To
 * clear less, it must first be known, per render target, which part is really used until the next clear:
 * what the passes load and store (their renderArea, the useful area), the rectangles that are resolved,
 * what is restored and what is swapped (whole). Measurement only: it does not change the image. One line
 * every 20 s: "C2 borrados por destino". false = not measured.
 */
// false by default. Measured: the clip to the useful area that needs it only saved 0.09 ms of GPU, and the
// tracking runs on the PM4 ring thread, which is now the bottleneck.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_borrados, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184, diagnostico): por destino, area borrada frente a la que se "
                    "usa hasta el borrado siguiente (pases, resolves, restauraciones e intercambios). Una linea cada "
                    "20 s ('C2 borrados por destino'). No cambia la imagen");
/*
 * Color clears only over the area in use (nfsmw_nativo_borrar_area_util).
 *
 * vkCmdClearColorImage clears the whole image: 1280x1280 for a 1280x720 scene, 1040x1040 for the 1024x576
 * output, 320x720 for a 256x256 cubemap face. With the measurement of nfsmw_nativo_diag_borrados (the
 * largest height used by passes, resolves, restores and swaps of that render target, plus one row of margin,
 * rounded up to the next multiple of 64), once a render target has kAreaUtilCiclos measured clears only that
 * top band is cleared, with a loadOp = CLEAR pass (the same thing NVK does internally, over fewer pixels).
 * The bottom band is recorded with its color and cleared before any use that reaches it: the result is the
 * same as clearing it all. Draws do not touch it: NVK clips each pass to its renderArea (SET_SURFACE_CLIP)
 * and the pass that reaches the band completes it before it opens. If a render target needs that 3 times,
 * it stops being clipped (its useful area is not stable). Color only: depth without TRANSFER_DST is cleared
 * per pass and that pass clears the whole ZCULL region (NVK), so it is not clipped. GUARD: if a band cannot
 * be completed, DIFERENCIA in the log and it turns off for the session. false = clear the whole image, as
 * before.
 */
// false by default. Measured: 0.09 ms of GPU per frame (1.18 Mpixels not cleared): not worth it with the
// PM4 ring as the bottleneck.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_borrar_area_util, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): los borrados de color borran solo las filas que se usan de "
                    "verdad; el resto se borra antes de que algo lo use. Misma imagen. Necesita "
                    "nfsmw_nativo_diag_borrados. false = la imagen entera, como antes");
// Defined in nfsmw_nativo_dibujos.cpp; here it is only read so as not to open two queries of the same type
// at once.
REXCVAR_DECLARE(int32_t, nfsmw_nativo_estadisticas_por_dibujo_s);
// nfsmw_nativo_sombra_minimo only pays off with the single-sample PCF (defined in nfsmw_nativo_dibujos.cpp).
REXCVAR_DECLARE(bool, nfsmw_nativo_pcf_barato);
REXCVAR_DEFINE_BOOL(nfsmw_nativo_estadisticas_pipeline, false, "NFSMW",
                    "Renderizador nativo (17/09, build 156): cuenta fragmentos sombreados, invocaciones de "
                    "vertice y primitivas recortadas por tipo de pasada (informe C2). La imagen no cambia, "
                    "pero las consultas cuestan tiempo de GPU: solo para medir");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_borrado, false, "NFSMW",
                    "Renderizador nativo: borrar cada destino de render con un color propio en "
                    "vez del color del juego (solo pruebas: comprueba copia, borrado y "
                    "presentacion)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_diag_resueltas, false, "NFSMW",
                    "Renderizador nativo: presentar en mosaico las texturas resueltas de cada "
                    "fotograma, en el orden de sus copias (solo pruebas)");
/*
 * Who reads each resolved texture (nfsmw_nativo_diag_lectores_s, measurement only).
 *
 * "C2 caras resueltas" counts copies and reads per address, but a read is a call to TexturaResuelta and
 * only happens when the sampler caches of DibujosVulkan miss (per frame and per fetch constant): it does not
 * say which shader reads nor after which copy. With 098B0000 (the 1024x576 scene) that leaves the question
 * open: it is written twice per frame (the blit of the 1280x720 scene and, after the composition, the
 * composited scene 1 to 1) and it has two reads per frame. If both come from the first write, nobody reads
 * the second one before the next frame covers it entirely, and it is unnecessary (0.59 Mpixels, ~0.35 ms
 * real per frame).
 *
 * Every N seconds two whole frames are examined: every logical write to a resolved texture (the color or
 * depth resolve, even if the real copy is deferred or is a swap), every draw that samples it according to
 * the fetch constants of its pixel shader, bypassing the caches, and the Swap that draws the front buffer.
 * At the end, one line per address: for each write, who reads it until the next one (PS nN, sampler, draws
 * and the render target being drawn into) and SOBRA if nobody reads it and the next one covers it entirely.
 * What is read before the first write of the window goes to the last one before it ("[antes]"). The
 * per-address lines are only printed if their pattern changes (the first three windows, all of them); the
 * summary line always is.
 * A write that comes out as SOBRA stays watched in every frame (menus, pause, rain, cutscenes...),
 * separating the reads before its source is written again (a deferred copy covers them: it would be recorded
 * right before that draw; this is the case of the raindrops on the screen with 098B0000) from the ones after
 * (with a single one, dropping the copy when the source is written would not be exact). The first of each
 * kind is reported; the counts, in every summary. It is the evidence needed before deferring or dropping a
 * copy.
 * Cost: outside the window, one if per draw and per copy (and, with something watched, comparing each
 * sampler's address with 1-4 addresses); in the two frames of the window, one lookup in the resolved map
 * per sampler (~0.2-0.3 ms per frame). 0 = off.
 */
// 0 by default. Measured: its watching only served the lazy composite copy, which saves nothing.
REXCVAR_DEFINE_INT32(nfsmw_nativo_diag_lectores_s, 0, "NFSMW",
                     "Renderizador nativo (25/09, build 184): cada estos segundos se miran dos fotogramas enteros y se "
                     "escribe, por textura resuelta, que pixel shaders leen cada escritura antes de la siguiente "
                     "(lineas C2 lectores); lo que sale como SOBRA se vigila despues en todos los fotogramas. Solo "
                     "mide. 0 = apagado")
    .range(0, 3600);
/*
 * The composited scene is only copied if someone reads it (nfsmw_nativo_compuesta_perezosa).
 * The scene texture (098B0000 at 1024x576) is written twice per frame: the blit of the scene and, after the
 * composition, the composited scene 1 to 1 from the output (sub_82442478: VT, resolve and drops). The
 * second one is only read by the raindrops on the screen (sub_82448168, PS n145), which come right after and
 * before the HUD. It is deferred: it is recorded right before the first draw that samples it without its
 * source having been written again (exact), dropped if another write covers it entirely (exact) and dropped
 * when its source (the HUD) is written if nobody has read it: 0.59 Mpixels and ~0.35 ms real of GPU per
 * frame without drops. It only applies to the (address, source) pair that the watching of
 * nfsmw_nativo_diag_lectores_s has seen kCompuestaAMirar times with no read after the source was written,
 * and it turns itself off if a draw samples the texture after its copy was dropped (DIFERENCIA in the log).
 * Without the diagnostic (nfsmw_nativo_diag_lectores_s = 0) it never applies. false = always copy, as
 * before.
 */
// false by default. Measured: 19,895 of 21,041 copies were recorded anyway because a draw reads it almost
// every frame: 0 ms saved.
REXCVAR_DEFINE_BOOL(nfsmw_nativo_compuesta_perezosa, false, "NFSMW",
                    "Renderizador nativo (25/09, build 184): la copia 1 a 1 de la escena compuesta a su textura "
                    "(098B0000, antes del HUD) se aplaza: se graba si un dibujo la muestrea antes de volver a escribir "
                    "su origen (las gotas de lluvia) y se tira si no. Se activa tras la vigilancia de "
                    "nfsmw_nativo_diag_lectores_s y se apaga sola al primer desacuerdo. false = se copia siempre");
REXCVAR_DEFINE_INT32(nfsmw_nativo_leer_resueltas_texels, 4096, "NFSMW",
                     "Renderizador nativo: las texturas resueltas de hasta estos texels se copian "
                     "tambien a la memoria del invitado, que el juego lee para su exposicion (0 = "
                     "ninguna; 4096 = 64x64, las que usa la exposicion; 57600 = 320x180)");
REXCVAR_DEFINE_BOOL(nfsmw_nativo_invalidar_texturas_cada_copia, false, "NFSMW",
                    "Renderizador nativo: tirar las caches de texturas en cada copia (lo de antes de la build 127). "
                    "Desde la 127 solo se tiran cuando una textura resuelta se crea, se rehace, se prepara o cambia "
                    "su orden de canales");
/*
 * On by default. It was written earlier and left off without being measured.
 *
 * The baseline without overclock shows the ring waiting 17.24 ms per frame for the previous presentation
 * to finish: almost exactly one 60 Hz vsync. The frame's work is ~38 ms, i.e. 2.3 vsyncs, and it should land
 * on 3 (50 ms, 20 FPS); with that wait on top it goes to almost 4 (62 ms, 16 FPS).
 *
 * And it fits the SDK, which already has a mailbox of three output images made for this:
 *     static constexpr uint32_t kGuestOutputMailboxSize = 3;
 *     // mailbox for presenting ... without long interlocking between guest output refreshing and painting
 * The SDK hands out a different image on each refresh, so painting the next one while the previous one is
 * presented is the intended use, not a shortcut. It does not change the image.
 *
 * To compare without a new NRO: set it to false in the toml and repeat the race.
 */
/*
 * How many work slots are really used (there is room for 3).
 *
 * With 2 the CPU can only be one frame ahead of the GPU, and on the console that meant 4,826 ms out of
 * every 10 seconds stopped inside Grabar() waiting for the GPU to finish the previous work. With 3 it can
 * be two ahead. Each slot costs a 64 MB upload buffer.
 *
 * It is left selectable to compare without a new NRO: with 2, exactly the earlier behavior.
 */
/*
 * Four. Breaking a console run down by regime showed that the bottleneck changes with the stretch of track,
 * and that the third slot falls short exactly where the GPU is the limit:
 *
 *   few draws (straight):    GPU 31.37 ms | gap 2.05 | fence wait inside Grabar() 4.9-9.8 ms
 *   many draws (alley):      GPU 30.30 ms | gap 5.50 | fence wait ~0
 *
 * That is: on the straight the CPU arrives first and waits for the GPU to release the slot; in the alley it
 * is the other way round. With four slots the CPU can be three frames ahead and stops stalling in the first
 * case. It fits easily: the log says "monton 0 (GPU): 482 MB usados de 1382 MB presupuestados", and each
 * slot costs a 64 MB upload buffer.
 */
/*
 * It stays at three. The fourth one breaks the image, and the reason is known.
 *
 * A build with only this change (sky deferral off) showed the same artifacts seen before: flickering and
 * odd colors. So the fourth slot shared the blame, it was not innocent.
 *
 * The cause: the work slots and the output slots are two different things and only one is configurable.
 *
 *     work slots   (nfsmw_nativo_ranuras_trabajo):  3 -> 4
 *     output slots (kRanurasSalida, :486):          3   FIXED
 *
 * `kRanurasSalida` governs the quad that draws the game image on screen: its pools, its descriptors, its
 * fences (`fences_salida_`) and `salidas_pendientes_`, and the rotation at :3305 is `% kRanurasSalida`.
 * Letting the CPU get three frames ahead in the work reuses an output slot whose image is still being
 * displayed. Hence the artifacts.
 *
 * To raise it, kRanurasSalida must be raised at the same time and the four places above reviewed.
 * Changing only this number is not enough.
 *
 * -------- what was believed before, kept so that it is not repeated --------
 * Four, and this time on its own.
 *
 * It first went in together with the sky deferral and the lazy sealing. That build broke the image and all
 * three were reverted without knowing which one it was; then the sky counter showed the culprit was the
 * sky ("7,00 detectados por fotograma" when 0.9 was expected). So this one comes back on its own, which
 * is how it should have been added from the start.
 *
 * What it buys, measured per regime:
 *   few draws (straight):    GPU 31.37 ms | gap 2.05 | waiting for the fence in Grabar() 4.9-9.8 ms
 *   many draws (alley):      GPU 30.30 ms | gap 5.50 | waiting for the fence ~0
 *
 * On the straight the CPU arrives first and stalls waiting for the GPU to release the slot. With four it is
 * three frames ahead. It costs 64 MB of upload buffer, out of the ~900 MB free in the heap.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_ranuras_trabajo, 3, "NFSMW",
                     "Renderizador nativo: ranuras de trabajo (2 a 4). Con mas, la CPU va mas fotogramas por "
                     "delante de la GPU y se para menos; cada una cuesta 64 MB. 2 = como la compilacion 80")
    .range(2, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// ZCULL (hierarchical depth culling), switchable.
//
// It is enabled by removing TRANSFER_DST from the game's depth buffers, which is what makes them eligible
// for a ZCULL plane in the driver. It measured -2.12 ms in the scene, but right after that the car body was
// observed darkening at times as if it were in shadow, and the scene and the reflection cubemap are exactly
// what ends up under ZCULL (the shadow map does not: it has TRANSFER_DST). It stayed off until a race ruled
// that out.
/*
 * On by default. It had been turned off on suspicion of darkening the car body, and that suspicion was
 * ruled out: the change of tone was the environment cubemap with caras_max=1 refreshing one face every six
 * frames, so the car's lighting lagged and then jumped. ZCULL is worth -2.12 ms measured and was already
 * enabled by hand in the test toml; the default value was misleading.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_zcull, true, "NFSMW",
                    "Descarte jerarquico de profundidad (ZCULL): quita TRANSFER_DST a las "
                    "profundidades del juego para que el driver les de plano de ZCULL");

REXCVAR_DEFINE_BOOL(nfsmw_nativo_salida_sin_espera, true, "NFSMW",
                    "Renderizador nativo: pinta la salida de cada Swap rotando 3 ranuras en vez de una, para no "
                    "esperar a que la GPU termine la salida anterior (en la consola el anillo esperaba ahi 17 ms "
                    "por Swap, casi un vsync). No cambia la imagen; false vuelve al comportamiento de antes");
// In NVK, TOP_OF_PIPE is PIPELINE_LOCATION_NONE: the timestamp is released when the GPU reads the command,
// not when the previous work finishes, and the per-category breakdown is approximate (a copy gets charged
// with the draw of the previous pass). BOTTOM_OF_PIPE is PIPELINE_LOCATION_ALL: it is released when all
// previous work finishes.
/*
 * On by default. It was only enabled through the toml, and the toml overrides the default: if the toml
 * ever ships without that line, the improvement silently disappears. Three cvars like this were found, one
 * of them worth 2 ms and dead for several builds.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_marcas_precisas, true, "NFSMW",
                    "Renderizador nativo (medida): marcas de tiempo de GPU entre categorias con BOTTOM_OF_PIPE (se "
                    "sueltan al terminar lo anterior) en vez de TOP_OF_PIPE (al leer el comando): reparto por "
                    "categorias exacto. No cambia la imagen");
REXCVAR_DEFINE_INT32(nfsmw_nativo_marcas_precisas_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (medida): con N > 0 alterna marcas normales (tramos pares) y precisas "
                     "(tramos impares) cada N segundos y anota cada cambio, para comparar en la misma ejecucion");
REXCVAR_DEFINE_INT32(nfsmw_nativo_lecturas_cada, 1, "NFSMW",
                     "Renderizador nativo: de cada destino se lee y escribe en la memoria del invitado "
                     "una de cada N copias pequenas (1 = todas); el juego las usa para su exposicion");
// The Xbox 360 passes the image through the gamma ramp the game loads, and NFSMW does not load the identity
// (measured: [64] = 273 and [128] = 539 in 10 bits, instead of 256 and 513).
REXCVAR_DEFINE_BOOL(nfsmw_nativo_rampa_gamma, true, "NFSMW",
                    "Renderizador nativo: aplica en la salida la rampa de gamma que carga el juego, como la pantalla "
                    "de la Xbox 360 (sin ella, los medios tonos y las sombras salen mas oscuros). false: la imagen tal "
                    "cual, como antes de la build 137")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard (nfsmw_guardia_30) lives in nfsmw_recorte_sombras.cpp, which owns the lever it pulls.
namespace nfsmw::guardia30 {
void Latir(double ms);
void Informe();
}  // namespace nfsmw::guardia30

namespace nfsmw::nativo {
namespace shaders {
// The same SPIR-V the SDK presenter uses to draw the game image
// (vulkan_presenter.cpp:128-141).
#include "vulkan_spirv/guest_output_bilinear_ps.h"
#include "vulkan_spirv/guest_output_triangle_strip_rect_vs.h"
// The same sampling as guest_output_bilinear_ps with the game's gamma ramp
// (shaders/nfsmw_salida_rampa_gamma.frag).
#include "shaders/nfsmw_salida_rampa_gamma_ps.h"
}  // namespace shaders

namespace {

namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanPresenter;

constexpr VkFormat kFormatoColor = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kAltoMaximoDestino = 2048;
constexpr VkImageSubresourceRange kRangoColor = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceRange kRangoProfundidad = {
    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
using FnCopiarImagen = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                                        VkImageLayout, uint32_t, const VkImageCopy*);
using FnBorrarProfundidad = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout,
                                             const VkClearDepthStencilValue*, uint32_t,
                                             const VkImageSubresourceRange*);
using FnBlit = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                                uint32_t, const VkImageBlit*, VkFilter);

// Direct3D 11 16.8 fixed point with rounding, like ui::FloatToD3D11Fixed16p8.
int32_t Fijo16p8(float valor) {
  if (!(std::abs(valor) >= 1.0f / 512.0f)) {
    return 0;
  }
  const double escalado = std::clamp(double(valor) * 256.0, -2147483392.0, 2147483392.0);
  return int32_t(std::lround(escalado));
}

// Address of a texel in a 32x32-tiled texture (pipeline/texture/util.cpp:424-436).
int32_t DesplazamientoMosaico2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// With 4-byte texels, DesplazamientoMosaico2D(x, y, pitch, 2) is
// (((x >> 5) + (y >> 5) * (pitch aligned to 32 >> 5)) << 12) + table[(y & 31) * 32 + (x & 31)]:
// the part inside the 32x32 tile does not depend on the pitch (EscribirLecturas).
const std::array<uint16_t, 1024>& TablaMosaico2DTexel4() {
  static const std::array<uint16_t, 1024> tabla = [] {
    std::array<uint16_t, 1024> t{};
    for (int32_t y = 0; y < 32; ++y) {
      for (int32_t x = 0; x < 32; ++x) {
        t[size_t(y) * 32 + size_t(x)] = uint16_t(DesplazamientoMosaico2D(x, y, 32, 2));
      }
    }
    return t;
  }();
  return tabla;
}

int32_t ExtenderSigno15(uint32_t valor) {
  return int32_t(valor << 17) >> 17;
}

// Fixed color per render target with nfsmw_nativo_diag_borrado: the
// capture shows which render target reached the screen.
VkClearColorValue ColorDiagnostico(uint32_t base, uint32_t formato, uint32_t pitch) {
  uint32_t h = base * 2654435761u ^ pitch * 2246822519u ^ formato * 3266489917u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  VkClearColorValue color{};
  for (uint32_t j = 0; j < 3; ++j) {
    color.float32[j] = float((h >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
  }
  color.float32[3] = 1.0f;
  return color;
}

using Imagen = ImagenNativa;  // preparada = already in GENERAL and cleared

struct Resuelta {
  Imagen imagen;
  uint32_t formato_guest = 0;
  bool intercambio_rb = false;
  // Times the draws have requested this address since the last report. It lives here and not in the report
  // map because TexturaResuelta is on the hot path (one call per texture and draw, ~8,000 per frame) and the
  // lookup of the resolved texture is done anyway.
  uint64_t lecturas = 0;
};

// C2 report of readbacks per render target (base and size) and cadence of nfsmw_nativo_lecturas_cada.
struct DestinoLectura {
  uint32_t base = 0;
  uint32_t ancho = 0;
  uint32_t alto = 0;
  uint64_t copias = 0;    // small copies seen in the interval
  uint64_t saltadas = 0;  // of those, the ones not read because of nfsmw_nativo_lecturas_cada
};

// Rear-view mirror diagnostic: copies to a square resolved texture (AnotarCopia).
// And of all the others too, with their size and how many times each one is read. That answers whether a
// copy is needed: a resolved texture that is copied every frame and never requested is wasted bandwidth.
struct CopiaDestino {
  uint32_t ancho = 0;
  uint32_t alto = 0;
  uint64_t copias = 0;
  uint64_t con_dibujos = 0;  // copies with some draw since the previous copy
  uint64_t dibujos = 0;
  uint64_t pixeles = 0;
};

// What is known about a render target between game commands. It serves two purposes:
//  - skipping a clear that changes nothing (nfsmw_nativo_saltar_borrados_repetidos);
//  - restoring only the rows the game really uses (nfsmw_nativo_restaurar_area_util).
// It lives in a separate map and not in ImagenNativa because that structure belongs to another file.
struct EstadoDestino {
  bool borrado_limpio = false;    // the content is exactly the last clear, with nothing on top
  uint64_t valor_borrado = 0;     // color empaquetado, o profundidad+stencil
  uint64_t dibujos_al_borrar = 0; // global draw counter at that moment
  uint32_t alto_usado = 0;        // the largest y1 the game has resolved from this render target
};

// Readback of a small resolved texture (nfsmw_nativo_leer_resueltas_texels):
// host-visible buffer it is copied to and what is needed to write it to the guest.
struct Lectura {
  VkBuffer bufer = VK_NULL_HANDLE;
  VkDeviceMemory memoria = VK_NULL_HANDLE;
  uint8_t* datos = nullptr;
  VkDeviceSize bytes = 0;
  bool coherente = true;
};
struct LecturaPendiente {
  Lectura* lectura;
  uint32_t base;          // RB_COPY_DEST_BASE
  int32_t x0;             // texel of the texture where the rectangle starts
  int32_t y0;
  uint32_t ancho;
  uint32_t alto;
  uint32_t pitch;         // RB_COPY_DEST_PITCH
  uint32_t alto_destino;
  uint32_t info;          // RB_COPY_DEST_INFO
};

// Output slots with nfsmw_nativo_salida_sin_espera (without it, only slot 0 is used).
constexpr uint32_t kRanurasSalida = 3;
// GPU timestamps per slot (the last one is kept for the final timestamp).
constexpr uint32_t kMarcasPorRanura = 128;
constexpr uint8_t kGpuFin = 0xFF;
// Host occlusion queries per work unit (draw spans of the game's queries).
constexpr uint32_t kOclusionesPorRanura = 32;
// nfsmw_reflejo_visibilidad. Own occlusion queries per work unit: the draws that sample the reflection and
// the witness (the final composition). The ones that do not fit are assumed visible.
constexpr uint32_t kVisibilidadPorRanura = 16;
constexpr uint8_t kVisibilidadAgua = 0;
constexpr uint8_t kVisibilidadTestigo = 1;
// Passes measured per work unit. In a race there are ~16 per frame including the resumed ones.
constexpr uint32_t kEstadisticasPorRanura = 64;
constexpr uint32_t kContadoresEstadistica = 3;  // vertices, primitivas recortadas y fragmentos
// Draws measured in a diagnostic frame (the scene has ~1200).
constexpr uint32_t kEstadisticasDibujoPorRanura = 2048;
constexpr uint32_t kEtiquetasShader = 512;
// Buckets of the copy breakdown by size (pixels of the copy).
constexpr uint32_t kCubetasCopia = 4;
constexpr uint32_t kPixelesCubeta[kCubetasCopia] = {64 * 64, 320 * 320, 1024 * 1024, 0xFFFFFFFFu};

// One of the work slots: while a frame is being recorded, the previous ones can still be
// on the GPU with the other one.
struct RanuraTrabajo {
  VkCommandPool pool_trabajo = VK_NULL_HANDLE;
  VkCommandPool pool_subida = VK_NULL_HANDLE;
  VkCommandBuffer trabajo = VK_NULL_HANDLE;
  VkCommandBuffer subida = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pendiente = false;
  uint64_t orden = 0;                        // numero de envio
  // Wall-clock time of the work, from vkQueueSubmit to the signaled fence. The GPU timestamps give
  // 30.5 ms per frame while the hardware's own counter says 99.7 % load on a 55 ms frame: either there is
  // work we do not mark or the timestamp scale is wrong. This bounds the truth from above (the CPU sees
  // the fence a little late) and the timestamps from below.
  std::chrono::steady_clock::time_point enviado{};
  std::vector<LecturaPendiente> lecturas;  // readbacks submitted with the work
  std::vector<uint8_t> categorias;  // GPU category of each timestamp written
  bool marcas_precisas = false;      // intermediate marks with BOTTOM_OF_PIPE
  // Occlusion queries recorded in the work, in order from the slot's first one, with the number of the game
  // query they add to.
  std::vector<std::pair<uint32_t, uint64_t>> oclusiones;
  // Statistics queries recorded, with the category of each one's pass.
  std::vector<std::pair<uint32_t, uint8_t>> estadisticas;
  // Per-draw queries, with the pixel shader number of each one.
  std::vector<std::pair<uint32_t, uint16_t>> estadisticas_dibujo;
  // Reflection visibility queries, with their type (kVisibilidadAgua or kVisibilidadTestigo).
  std::vector<std::pair<uint32_t, uint8_t>> visibilidad;
};

// A game occlusion query (from its Issue(BEGIN) to its Issue(END)) while its spans are being counted on the
// GPU.
struct ConsultaOclusionJuego {
  uint32_t base = 0;              // D3D counter structure
  uint64_t muestras = 0;          // sum of the spans read
  uint32_t tramos_pendientes = 0;  // recorded and not yet read
  bool terminada = false;         // ya llego su Issue(END)
  bool fallida = false;           // some span without room or not read: not published
};

class DestinosVulkan final : public DestinosNativos, public ContextoDestinos {
 public:
  DestinosVulkan(const VulkanDevice* dispositivo, rex::memory::Memory* memoria)
      : dispositivo_(dispositivo),
        dfn_(dispositivo->functions()),
        device_(dispositivo->device()),
        memoria_(memoria),
        familia_(dispositivo->queue_family_graphics_compute()) {}

  ~DestinosVulkan() override {
    EsperarGpu();
    dibujos_.reset();  // their framebuffers and views point to these images
    for (auto& [clave, imagen] : profundidades_) {
      Destruir(imagen);
    }
    for (auto& [clave, imagen] : destinos_) {
      Destruir(imagen);
    }
    for (auto& [clave, resuelta] : resueltas_) {
      Destruir(resuelta.imagen);
    }
    Destruir(mosaico_);
    for (ImagenFrontal& repuesto : frontales_imagenes_) {  // nfsmw_nativo_frontal_perezoso
      Destruir(repuesto.imagen);
    }
    lecturas_pendientes_.clear();
    for (RanuraTrabajo& ranura : ranuras_) {
      ranura.lecturas.clear();
    }
    for (auto& [clave, lectura] : lecturas_) {
      DestruirLectura(lectura);
    }
    for (auto& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
      }
    }
    if (pipeline_ != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, pipeline_, nullptr);
    for (VkPipeline p : pipelines_rampa_) {
      if (p != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, p, nullptr);
    }
    if (fs_rampa_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, fs_rampa_, nullptr);
    for (RampaSalida& rampa : rampas_salida_) {
      if (rampa.bufer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, rampa.bufer, nullptr);
      if (rampa.memoria != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, rampa.memoria, nullptr);
    }
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    if (pool_descriptores_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorPool(device_, pool_descriptores_, nullptr);
    if (layout_descriptores_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorSetLayout(device_, layout_descriptores_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, sampler_, nullptr);
    if (vs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, vs_, nullptr);
    if (fs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, fs_, nullptr);
    if (render_pass_salida_ != VK_NULL_HANDLE)
      dfn_.vkDestroyRenderPass(device_, render_pass_salida_, nullptr);
    for (RanuraTrabajo& ranura : ranuras_) {
      if (ranura.fence != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, ranura.fence, nullptr);
      if (ranura.pool_trabajo != VK_NULL_HANDLE)
        dfn_.vkDestroyCommandPool(device_, ranura.pool_trabajo, nullptr);
      if (ranura.pool_subida != VK_NULL_HANDLE)
        dfn_.vkDestroyCommandPool(device_, ranura.pool_subida, nullptr);
    }
    if (consultas_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, consultas_, nullptr);
    if (oclusiones_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, oclusiones_, nullptr);
    if (visibilidad_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, visibilidad_, nullptr);
    for (uint32_t i = 0; i < kRanurasSalida; ++i) {
      if (fences_salida_[i] != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, fences_salida_[i], nullptr);
      if (pools_salida_[i] != VK_NULL_HANDLE) dfn_.vkDestroyCommandPool(device_, pools_salida_[i], nullptr);
    }
  }

  bool Inicializar() {
    REXLOG_INFO("[nativo] sincronizacion de imagenes Vulkan = {} ({})",
                REXCVAR_GET(nfsmw_nativo_sincronizacion_gpu) ? "SI" : "no",
                dispositivo_->properties().deviceName);
    // vkCmdCopyImage is not in the SDK's function table: it is requested from the driver.
    copiar_imagen_ = reinterpret_cast<FnCopiarImagen>(
        dispositivo_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkCmdCopyImage"));
    if (!copiar_imagen_) {
      REXLOG_ERROR("[nativo] C2: el driver no da vkCmdCopyImage");
      return false;
    }
    borrar_profundidad_ = reinterpret_cast<FnBorrarProfundidad>(
        dispositivo_->vulkan_instance()->functions().vkGetDeviceProcAddr(
            device_, "vkCmdClearDepthStencilImage"));
    blit_ = reinterpret_cast<FnBlit>(
        dispositivo_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkCmdBlitImage"));
    // Attachment, copy and clear source and destination, and sampling of the resolved ones (shadows).
    const VkFormatFeatureFlags kUsosProfundidad =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    // To draw the shadow map smaller it has to be scaled up when resolving it, and that is a vkCmdBlitImage on
    // depth. If the driver does not provide it, nothing is scaled.
    const VkFormatFeatureFlags kUsosEscalado =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkFormatFeatureFlags usos_elegido = 0;
    for (const VkFormat candidato : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
      VkFormatProperties propiedades{};
      dispositivo_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
          dispositivo_->physical_device(), candidato, &propiedades);
      const VkFormatFeatureFlags usos = propiedades.optimalTilingFeatures & kUsosProfundidad;
      if (!(usos & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        continue;
      }
      if (formato_profundidad_ == VK_FORMAT_UNDEFINED || usos == kUsosProfundidad) {
        formato_profundidad_ = candidato;
        usos_elegido = usos;
        profundidad_escalable_ =
            (propiedades.optimalTilingFeatures & kUsosEscalado) == kUsosEscalado;
      }
      if (usos == kUsosProfundidad) {
        break;
      }
    }
    if (formato_profundidad_ != VK_FORMAT_UNDEFINED) {
      const VkFormatFeatureFlags faltan = kUsosProfundidad & ~usos_elegido;
      REXLOG_INFO("[nativo] C2: formato de profundidad {}{}{}{}",
                  formato_profundidad_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
                  (faltan & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? ", sin muestreo" : "",
                  (faltan & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ? ", sin origen de copias" : "",
                  (faltan & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ? ", sin destino de copias" : "");
      if (!profundidad_escalable_) {
        REXLOG_INFO("[nativo] C2: el driver no escala profundidad: el mapa de sombras se queda a su tamano");
      }
    }
    // One pool per command buffer: the SDK table does not include
    // vkResetCommandBuffer either, so the whole pool is reset.
    VkCommandPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info_pool.queueFamilyIndex = familia_;
    VkCommandBufferAllocateInfo reserva{};
    reserva.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    reserva.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    reserva.commandBufferCount = 1;
    VkFenceCreateInfo info_fence{};
    info_fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (RanuraTrabajo& ranura : ranuras_) {
      if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &ranura.pool_trabajo) !=
              VK_SUCCESS ||
          dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &ranura.pool_subida) !=
              VK_SUCCESS ||
          dfn_.vkCreateFence(device_, &info_fence, nullptr, &ranura.fence) != VK_SUCCESS) {
        return false;
      }
      reserva.commandPool = ranura.pool_trabajo;
      if (dfn_.vkAllocateCommandBuffers(device_, &reserva, &ranura.trabajo) != VK_SUCCESS) {
        return false;
      }
      reserva.commandPool = ranura.pool_subida;
      if (dfn_.vkAllocateCommandBuffers(device_, &reserva, &ranura.subida) != VK_SUCCESS) {
        return false;
      }
    }
    for (uint32_t i = 0; i < kRanurasSalida; ++i) {
      if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &pools_salida_[i]) != VK_SUCCESS) {
        return false;
      }
      reserva.commandPool = pools_salida_[i];
      if (dfn_.vkAllocateCommandBuffers(device_, &reserva, &comandos_salida_[i]) != VK_SUCCESS) {
        return false;
      }
      if (dfn_.vkCreateFence(device_, &info_fence, nullptr, &fences_salida_[i]) != VK_SUCCESS) {
        return false;
      }
    }
    ranuras_usadas_ = uint32_t(std::clamp(REXCVAR_GET(nfsmw_nativo_ranuras_trabajo), 2,
                                          int32_t(ranuras_.size())));
    REXLOG_INFO("[nativo] C2: ranuras de trabajo (nfsmw_nativo_ranuras_trabajo) = {} de {}", ranuras_usadas_,
                ranuras_.size());
    salida_sin_espera_ = REXCVAR_GET(nfsmw_nativo_salida_sin_espera);
    invalidar_cada_copia_ = REXCVAR_GET(nfsmw_nativo_invalidar_texturas_cada_copia);
    REXLOG_INFO("[nativo] C2: caches de texturas tiradas en cada copia (nfsmw_nativo_invalidar_texturas_cada_copia) = {}",
                invalidar_cada_copia_ ? "SI" : "no");
    REXLOG_INFO("[nativo] C2: salida sin esperar a la anterior (nfsmw_nativo_salida_sin_espera) = {}",
                salida_sin_espera_ ? "SI" : "no");
    marcas_precisas_ = REXCVAR_GET(nfsmw_nativo_marcas_precisas);
    alternar_marcas_s_ = REXCVAR_GET(nfsmw_nativo_marcas_precisas_alternar_s);
    inicio_marcas_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[nativo] C2: marcas de GPU precisas (nfsmw_nativo_marcas_precisas) = {}; alternar cada {} s",
                marcas_precisas_ ? "SI" : "no", alternar_marcas_s_);
    // GPU time per work unit: two timestamps per slot. Without timestamp bits on the queue, it is not measured.
    {
      const auto& ifn = dispositivo_->vulkan_instance()->functions();
      uint32_t familias = 0;
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(dispositivo_->physical_device(), &familias,
                                                   nullptr);
      std::vector<VkQueueFamilyProperties> colas(familias);
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(dispositivo_->physical_device(), &familias,
                                                   colas.data());
      VkPhysicalDeviceProperties fisico{};
      ifn.vkGetPhysicalDeviceProperties(dispositivo_->physical_device(), &fisico);
      escribir_marca_ = reinterpret_cast<FnEscribirMarca>(
          ifn.vkGetDeviceProcAddr(device_, "vkCmdWriteTimestamp"));
      leer_consultas_ = reinterpret_cast<FnLeerConsultas>(
          ifn.vkGetDeviceProcAddr(device_, "vkGetQueryPoolResults"));
      if (familia_ < familias && colas[familia_].timestampValidBits && escribir_marca_ &&
          leer_consultas_ && fisico.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo info_consultas{};
        info_consultas.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_consultas.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info_consultas.queryCount = uint32_t(ranuras_.size() * kMarcasPorRanura);
        if (dfn_.vkCreateQueryPool(device_, &info_consultas, nullptr, &consultas_) != VK_SUCCESS) {
          consultas_ = VK_NULL_HANDLE;
        }
        periodo_marca_ns_ = fisico.limits.timestampPeriod;
      }
      REXLOG_INFO("[nativo] C2: tiempo de GPU por Swap {}",
                  consultas_ != VK_NULL_HANDLE ? "medido con marcas de tiempo"
                                               : "no disponible (la cola no tiene marcas)");
      // The game's occlusion queries (the sun flare), counted on the GPU.
      oclusion_gpu_ = nfsmw::compatibilidad::OclusionGpu(
          REXCVAR_GET(nfsmw_consultas_oclusion), REX_PLATFORM_ANDROID, fisico.vendorID);
      // Avoid the entire lifecycle including pool creation/reset. Callers
      // fall back without changing or persisting the user's other settings.
      REXLOG_INFO("[compatibilidad] consultas de oclusion: {} (opcion {}, GPU {})",
                  oclusion_gpu_ ? "activadas" : "desactivadas; reflejos por lecturas, sin destello solar",
                  REXCVAR_GET(nfsmw_consultas_oclusion), fisico.deviceName);
      VkQueryPoolCreateInfo info_oclusiones{};
      info_oclusiones.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      info_oclusiones.queryType = VK_QUERY_TYPE_OCCLUSION;
      info_oclusiones.queryCount = uint32_t(ranuras_.size() * kOclusionesPorRanura);
      if (!oclusion_gpu_ || !leer_consultas_ ||
          dfn_.vkCreateQueryPool(device_, &info_oclusiones, nullptr, &oclusiones_) != VK_SUCCESS) {
        oclusiones_ = VK_NULL_HANDLE;
      }
      oclusion_precisa_ = dispositivo_->properties().occlusionQueryPrecise;
      REXLOG_INFO("[nativo] C2: consultas de oclusion del host {} ({})",
                  oclusiones_ != VK_NULL_HANDLE ? "disponibles" : "no disponibles: cuenta fingida",
                  oclusion_precisa_ ? "precisas" : "sin precision: cuentan si hubo alguna muestra");
      // nfsmw_reflejo_visibilidad, in a separate pool so as not to touch the count of the game's queries.
      if (oclusion_gpu_ && nfsmw::reflejo_demanda::MedirVisibilidad() && leer_consultas_) {
        VkQueryPoolCreateInfo info_visibilidad{};
        info_visibilidad.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_visibilidad.queryType = VK_QUERY_TYPE_OCCLUSION;
        info_visibilidad.queryCount = uint32_t(ranuras_.size() * kVisibilidadPorRanura);
        if (dfn_.vkCreateQueryPool(device_, &info_visibilidad, nullptr, &visibilidad_) != VK_SUCCESS) {
          visibilidad_ = VK_NULL_HANDLE;
        }
        REXLOG_INFO("[nativo] C2: visibilidad del reflejo (build 192): {}",
                    visibilidad_ != VK_NULL_HANDLE ? "consultas disponibles"
                                                   : "SIN consultas: el reflejo se decide por lecturas (como la 191)");
      }
      // Pipeline statistics per pass. The order of the counters is the order of the bits,
      // not the order of this list: vertices, clipped primitives and fragments.
      if (dispositivo_->properties().pipelineStatisticsQuery && leer_consultas_) {
        VkQueryPoolCreateInfo info_estadisticas{};
        info_estadisticas.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_estadisticas.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info_estadisticas.queryCount = uint32_t(ranuras_.size() * kEstadisticasPorRanura);
        info_estadisticas.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (dfn_.vkCreateQueryPool(device_, &info_estadisticas, nullptr, &estadisticas_) != VK_SUCCESS) {
          estadisticas_ = VK_NULL_HANDLE;
        }
      }
      if (estadisticas_ != VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo info_dibujo{};
        info_dibujo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_dibujo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info_dibujo.queryCount = uint32_t(ranuras_.size() * kEstadisticasDibujoPorRanura);
        info_dibujo.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (dfn_.vkCreateQueryPool(device_, &info_dibujo, nullptr, &estadisticas_dibujo_) != VK_SUCCESS) {
          estadisticas_dibujo_ = VK_NULL_HANDLE;
        }
        fragmentos_por_shader_.assign(size_t(kEtiquetasShader) * kGpuCategorias, 0);
        dibujos_por_shader_.assign(size_t(kEtiquetasShader) * kGpuCategorias, 0);
      }
      REXLOG_INFO("[nativo] C2: estadisticas de tuberia por pasada {}",
                  estadisticas_ != VK_NULL_HANDLE ? "disponibles (nfsmw_nativo_estadisticas_pipeline)"
                                                  : "no disponibles");
    }

    VkSamplerCreateInfo info_sampler{};
    info_sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info_sampler.magFilter = VK_FILTER_LINEAR;
    info_sampler.minFilter = VK_FILTER_LINEAR;
    info_sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info_sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info_sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info_sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (dfn_.vkCreateSampler(device_, &info_sampler, nullptr, &sampler_) != VK_SUCCESS) {
      return false;
    }

    // Same layout as the presenter: image at 0 and sampler at 1, and the gamma ramp at 2 (the pipeline without
    // the ramp does not use it).
    VkDescriptorSetLayoutBinding enlaces[3]{};
    enlaces[0].binding = 0;
    enlaces[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    enlaces[0].descriptorCount = 1;
    enlaces[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    enlaces[1].binding = 1;
    enlaces[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    enlaces[1].descriptorCount = 1;
    enlaces[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    enlaces[1].pImmutableSamplers = &sampler_;
    enlaces[2].binding = 2;
    enlaces[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    enlaces[2].descriptorCount = 1;
    enlaces[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_layout.bindingCount = 3;
    info_layout.pBindings = enlaces;
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_layout, nullptr, &layout_descriptores_) !=
        VK_SUCCESS) {
      return false;
    }
    VkPushConstantRange rangos[2]{};
    rangos[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    rangos[0].offset = 0;
    rangos[0].size = 16;  // GuestOutputPaintRectangleConstants
    rangos[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    rangos[1].offset = 16;
    rangos[1].size = 16;  // Presenter::BilinearConstants
    VkPipelineLayoutCreateInfo info_pipeline_layout{};
    info_pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info_pipeline_layout.setLayoutCount = 1;
    info_pipeline_layout.pSetLayouts = &layout_descriptores_;
    info_pipeline_layout.pushConstantRangeCount = 2;
    info_pipeline_layout.pPushConstantRanges = rangos;
    if (dfn_.vkCreatePipelineLayout(device_, &info_pipeline_layout, nullptr, &layout_pipeline_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorPoolSize tamanos[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kRanurasSalida},
                                       {VK_DESCRIPTOR_TYPE_SAMPLER, kRanurasSalida},
                                       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kRanurasSalida}};
    VkDescriptorPoolCreateInfo info_pool_desc{};
    info_pool_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_desc.maxSets = kRanurasSalida;
    info_pool_desc.poolSizeCount = 3;
    info_pool_desc.pPoolSizes = tamanos;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_desc, nullptr, &pool_descriptores_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo reserva_desc{};
    reserva_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserva_desc.descriptorPool = pool_descriptores_;
    reserva_desc.descriptorSetCount = 1;
    reserva_desc.pSetLayouts = &layout_descriptores_;
    for (uint32_t i = 0; i < kRanurasSalida; ++i) {
      if (dfn_.vkAllocateDescriptorSets(device_, &reserva_desc, &descriptores_salida_[i]) != VK_SUCCESS) {
        return false;
      }
    }
    // One ramp buffer per output slot, so it is only changed in a slot the GPU is no longer using. Without
    // them, the output has no ramp (as before).
    rampa_gamma_ = REXCVAR_GET(nfsmw_nativo_rampa_gamma);
    for (uint32_t i = 0; i < kRanurasSalida && rampa_gamma_; ++i) {
      RampaSalida& rampa = rampas_salida_[i];
      uint32_t tipo = 0;
      void* mapeado = nullptr;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              dispositivo_, sizeof(rampa_valores_), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kUpload, rampa.bufer, rampa.memoria, &tipo) ||
          dfn_.vkMapMemory(device_, rampa.memoria, 0, VK_WHOLE_SIZE, 0, &mapeado) != VK_SUCCESS) {
        REXLOG_WARN("[nativo] C2: no se pudo crear el bufer de la rampa de gamma: la salida va sin ella");
        rampa_gamma_ = false;
        break;
      }
      rampa.datos = static_cast<uint8_t*>(mapeado);
      rampa.tipo = tipo;
      VkDescriptorBufferInfo info_bufer{rampa.bufer, 0, sizeof(rampa_valores_)};
      VkWriteDescriptorSet escritura{};
      escritura.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      escritura.dstSet = descriptores_salida_[i];
      escritura.dstBinding = 2;
      escritura.descriptorCount = 1;
      escritura.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      escritura.pBufferInfo = &info_bufer;
      dfn_.vkUpdateDescriptorSets(device_, 1, &escritura, 0, nullptr);
    }
    if (!rampa_gamma_) {
      // Without a ramp, binding 2 stays unwritten: the pipeline without the ramp does not read it.
      for (RampaSalida& rampa : rampas_salida_) {
        if (rampa.bufer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, rampa.bufer, nullptr);
        if (rampa.memoria != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, rampa.memoria, nullptr);
        rampa = RampaSalida{};
      }
    }

    VkShaderModuleCreateInfo info_modulo{};
    info_modulo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info_modulo.codeSize = sizeof(shaders::guest_output_triangle_strip_rect_vs);
    info_modulo.pCode = shaders::guest_output_triangle_strip_rect_vs;
    if (dfn_.vkCreateShaderModule(device_, &info_modulo, nullptr, &vs_) != VK_SUCCESS) {
      return false;
    }
    info_modulo.codeSize = sizeof(shaders::guest_output_bilinear_ps);
    info_modulo.pCode = shaders::guest_output_bilinear_ps;
    if (dfn_.vkCreateShaderModule(device_, &info_modulo, nullptr, &fs_) != VK_SUCCESS) {
      return false;
    }
    if (rampa_gamma_) {
      info_modulo.codeSize = sizeof(shaders::nfsmw_salida_rampa_gamma_ps);
      info_modulo.pCode = shaders::nfsmw_salida_rampa_gamma_ps;
      if (dfn_.vkCreateShaderModule(device_, &info_modulo, nullptr, &fs_rampa_) != VK_SUCCESS) {
        return false;
      }
    }

    VkAttachmentDescription adjunto{};
    adjunto.format = VulkanPresenter::kGuestOutputFormat;
    adjunto.samples = VK_SAMPLE_COUNT_1_BIT;
    adjunto.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    adjunto.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    adjunto.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    adjunto.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    adjunto.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    adjunto.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
    VkAttachmentReference referencia{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpase{};
    subpase.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpase.colorAttachmentCount = 1;
    subpase.pColorAttachments = &referencia;
    VkRenderPassCreateInfo info_rp{};
    info_rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info_rp.attachmentCount = 1;
    info_rp.pAttachments = &adjunto;
    info_rp.subpassCount = 1;
    info_rp.pSubpasses = &subpase;
    if (dfn_.vkCreateRenderPass(device_, &info_rp, nullptr, &render_pass_salida_) != VK_SUCCESS) {
      return false;
    }

    pipeline_ = CrearPipelineSalida(fs_, nullptr);
    if (pipeline_ == VK_NULL_HANDLE) {
      return false;
    }
    // The ramp variants without extras (bilinear and exact texel) are created now; the post-processing ones
    // with per-pixel work and FXAA, the first time they are requested (PipelineRampa).
    if (rampa_gamma_ && (PipelineRampa(0) == VK_NULL_HANDLE || PipelineRampa(1) == VK_NULL_HANDLE)) {
      return false;
    }
    REXLOG_INFO("[nativo] C2: rampa de gamma del juego en la salida (nfsmw_nativo_rampa_gamma) = {}",
                rampa_gamma_ ? "SI" : "no");
    // Parts C3-C6: without the required capabilities only copies and presentation remain.
    dibujos_ = DibujosVulkan::Crear(dispositivo_, memoria_, this);
    return true;
  }

  // Pipeline of the output pass with the fragment shader fs (and its specialization constants, if any).
  VkPipeline CrearPipelineSalida(VkShaderModule fs, const VkSpecializationInfo* especial) {
    VkPipelineShaderStageCreateInfo etapas[2]{};
    etapas[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    etapas[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    etapas[0].module = vs_;
    etapas[0].pName = "main";
    etapas[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    etapas[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    etapas[1].module = fs;
    etapas[1].pName = "main";
    etapas[1].pSpecializationInfo = especial;
    VkPipelineVertexInputStateCreateInfo entrada{};
    entrada.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ensamblado{};
    ensamblado.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ensamblado.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vista{};
    vista.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vista.viewportCount = 1;
    vista.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizado{};
    rasterizado.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizado.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizado.cullMode = VK_CULL_MODE_NONE;
    rasterizado.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizado.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo muestreo{};
    muestreo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    muestreo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState mezcla_adjunto{};
    mezcla_adjunto.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo mezcla{};
    mezcla.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    mezcla.attachmentCount = 1;
    mezcla.pAttachments = &mezcla_adjunto;
    const VkDynamicState dinamicos[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dinamico{};
    dinamico.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dinamico.dynamicStateCount = 2;
    dinamico.pDynamicStates = dinamicos;
    VkGraphicsPipelineCreateInfo info_pipeline{};
    info_pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info_pipeline.stageCount = 2;
    info_pipeline.pStages = etapas;
    info_pipeline.pVertexInputState = &entrada;
    info_pipeline.pInputAssemblyState = &ensamblado;
    info_pipeline.pViewportState = &vista;
    info_pipeline.pRasterizationState = &rasterizado;
    info_pipeline.pMultisampleState = &muestreo;
    info_pipeline.pColorBlendState = &mezcla;
    info_pipeline.pDynamicState = &dinamico;
    info_pipeline.layout = layout_pipeline_;
    info_pipeline.renderPass = render_pass_salida_;
    info_pipeline.basePipelineIndex = -1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (dfn_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info_pipeline, nullptr, &pipeline) !=
        VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pipeline;
  }

  // Variant of the output pass with the ramp, by index: 4 * fxaa + 2 * grading + exact (specialization
  // constants 2, 1 and 0 of the shader). Created the first time it is requested; VK_NULL_HANDLE if it fails
  // (and it is logged).
  VkPipeline PipelineRampa(uint32_t indice) {
    VkPipeline& pipeline = pipelines_rampa_[indice];
    if (pipeline == VK_NULL_HANDLE && !pipelines_rampa_fallidos_[indice]) {
      const VkSpecializationMapEntry entradas[3] = {{0, 0, sizeof(VkBool32)},
                                                    {1, sizeof(VkBool32), sizeof(VkBool32)},
                                                    {2, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
      const VkBool32 valores[3] = {(indice & 1) ? VK_TRUE : VK_FALSE, (indice & 2) ? VK_TRUE : VK_FALSE,
                                   (indice & 4) ? VK_TRUE : VK_FALSE};
      const VkSpecializationInfo especial{3, entradas, sizeof(valores), valores};
      const auto antes = std::chrono::steady_clock::now();
      pipeline = CrearPipelineSalida(fs_rampa_, &especial);
      if (pipeline == VK_NULL_HANDLE) {
        pipelines_rampa_fallidos_[indice] = true;
        REXLOG_ERROR("[nativo] C2: no se pudo crear la variante {} de la pasada de salida", indice);
      } else {
        REXLOG_INFO("[nativo] C2: variante {} de la pasada de salida (exacta {}, graduacion {}, FXAA {}) creada en {:.1f} "
                    "ms",
                    indice, (indice & 1) ? "si" : "no", (indice & 2) ? "si" : "no", (indice & 4) ? "si" : "no",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - antes).count());
      }
    }
    return pipeline;
  }

  bool Copiar(const RegistrosCopia& reg) override {
    // Draws since the previous copy: those of the render target being resolved now (rear-view mirror
    // diagnostic).
    const uint64_t dibujados = dibujos_ ? dibujos_->Dibujados() : 0;
    const uint64_t dibujos_antes = dibujados - dibujados_ultima_copia_;
    dibujados_ultima_copia_ = dibujados;
    if (dibujos_) {
      dibujos_->TerminarPase();  // copying and clearing are not allowed inside a pass
      // No longer on every copy; see ObtenerResuelta and Preparar.
      if (invalidar_cada_copia_) {
        dibujos_->InvalidarTexturas();
      }
    }
    if (Grabar()) {
      MarcarGpu(kGpuCopias);  // GPU time of copies and clears (C2 report)
    }
    const uint32_t control = reg.rb_copy_control;
    const uint32_t origen = control & 0x7;
    const bool borrar_color = (control >> 8) & 0x1;
    const uint32_t comando = (control >> 20) & 0x3;
    const bool copiar = comando == uint32_t(xenos::CopyCommand::kRaw) ||
                        comando == uint32_t(xenos::CopyCommand::kConvert);
    const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
    const uint32_t msaa = (reg.rb_surface_info >> 16) & 0x3;
    if (msaa != uint32_t(xenos::MsaaSamples::k1X) && avisados_.insert(1).second) {
      REXLOG_INFO("[nativo] C2: destino con MSAA: se usa con 1 muestra");
    }
    if (origen >= xenos::kMaxColorRenderTargets) {
      // From depth: the copy goes to a resolved texture with the host depth
      // format, which the draws sample as k_24_8. With a depth source no color
      // is cleared (IsClearingColor, graphics/util/draw.h:534-538).
      const bool borrara = ((control >> 9) & 0x1) && borrar_profundidad_;
      if (copiar) {
        CopiarProfundidad(reg, pitch, borrara);
      }
      if (borrara) {
        BorrarProfundidad(reg, pitch);
      }
      return true;
    }
    const uint32_t info_color = reg.rb_color_info[origen];
    const uint32_t formato_color = (info_color >> 16) & 0xF;
    if (FormatoHostDestinoColor(formato_color) == VK_FORMAT_UNDEFINED) {
      return Rechazar(100 + formato_color, "formato de destino de render todavia no soportado");
    }
    int32_t x0, y0, x1, y1;
    if (!Rectangulo(reg, pitch, x0, y0, x1, y1)) {
      return false;
    }

    // First the objects (creating or recreating can submit work), then recording.
    Imagen* destino_render = ObtenerDestino(info_color & 0xFFF, formato_color, pitch);
    if (!destino_render) {
      return false;
    }
    // The game has just said which area of this render target matters to it. It is the data RestaurarContenido
    // uses to stop copying the bottom rows that nobody draws or reads (the scene render target is created
    // 1280x1280 to draw 1280x720).
    AnotarAreaUtil(*destino_render, y1);
    AnotarUsoBorrado(*destino_render, uint32_t(std::max(x1, 0)), uint32_t(std::max(y1, 0)));
    uint32_t base_resuelta = 0;
    Resuelta* resuelta = nullptr;
    uint32_t dx = 0, dy = 0;
    if (copiar) {
      const uint32_t info_destino = reg.rb_copy_dest_info;
      const uint32_t formato_destino = (info_destino >> 7) & 0x3F;
      {
        // Exponent biases (RB_COLOR_INFO 20-25: the shader's output; RB_COPY_DEST_INFO 16-21: the copy). Not
        // applied yet; Most Wanted leaves them at 0. Logged once per combination.
        const int32_t sesgo_color = int32_t(info_color << 6) >> 26;
        const int32_t sesgo_copia = int32_t(info_destino << 10) >> 26;
        const uint32_t combinacion = 0x40000000u | (formato_color << 24) | (formato_destino << 16) |
                                     (uint32_t(sesgo_color & 0xFF) << 8) | uint32_t(sesgo_copia & 0xFF);
        if ((sesgo_color || sesgo_copia) && avisados_.size() < 256 && avisados_.insert(combinacion).second) {
          REXLOG_INFO("[nativo] C2 sesgo de exponente: destino formato {} sesgo {}, copia a formato {} sesgo {}",
                      formato_color, sesgo_color, formato_destino, sesgo_copia);
        }
      }
      if ((info_destino >> 3) & 0x1) {
        Rechazar(3, "copia a textura 3D o array: todavia no");
      } else if (formato_destino != uint32_t(xenos::ColorFormat::k_8_8_8_8) &&
                 formato_destino != uint32_t(xenos::ColorFormat::k_8_8_8_8_A) &&
                 formato_destino != uint32_t(xenos::ColorFormat::k_8_8_8_8_AS_16_16_16_16)) {
        Rechazar(200 + formato_destino, "formato de copia todavia no soportado");
      } else {
        const uint32_t pitch_destino = reg.rb_copy_dest_pitch & 0x3FFF;
        const uint32_t alto_destino = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
        // 4 bytes per texel: base in multiples of 32 texels (GetResolveInfo).
        const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
        const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
        const uint32_t base =
            reg.rb_copy_dest_base +
            uint32_t(DesplazamientoMosaico2D(int32_t(base_x), int32_t(base_y), pitch_destino, 2));
        dx = uint32_t(x0) - base_x;
        dy = uint32_t(y0) - base_y;
        resuelta = ObtenerResuelta(base & 0x1FFFFFFF, pitch_destino, alto_destino, formato_destino,
                                   (info_destino >> 24) & 0x1);
        base_resuelta = base & 0x1FFFFFFF;
        if (resuelta) {
          AnotarCopia(base & 0x1FFFFFFF, pitch_destino, alto_destino, dibujos_antes);
          if (compuesta_hay_ || compuesta_caducada_ != 0) {  // nfsmw_nativo_compuesta_perezosa
            CompuestaAntesDeEscribirTextura(base & 0x1FFFFFFF, x0 == 0 && y0 == 0 &&
                                                                   uint32_t(x1 - x0) >= resuelta->imagen.ancho &&
                                                                   uint32_t(y1 - y0) >= resuelta->imagen.alto);
          }
          if (diag_ventana_ || !diag_vigiladas_.empty()) {  // nfsmw_nativo_diag_lectores_s
            DiagEscritura(base & 0x1FFFFFFF, info_color & 0xFFF, pitch, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                          resuelta->imagen.ancho, resuelta->imagen.alto, dibujos_antes, false);
          }
        }
        if (resuelta && REXCVAR_GET(nfsmw_nativo_diag_resueltas) &&
            resueltas_fotograma_.size() < 64 &&
            std::find(resueltas_fotograma_.begin(), resueltas_fotograma_.end(),
                      base & 0x1FFFFFFF) == resueltas_fotograma_.end()) {
          resueltas_fotograma_.push_back(base & 0x1FFFFFFF);
        }
      }
    }

    if (!Grabar()) {
      return false;
    }
    Preparar(*destino_render);
    if (resuelta) {
      Preparar(resuelta->imagen);
      // What the game asks for and what fits in the render target.
      const uint32_t pedido_ancho =
          std::min(uint32_t(x1 - x0), destino_render->ancho - uint32_t(x0));
      const uint32_t pedido_alto = std::min(uint32_t(y1 - y0), destino_render->alto - uint32_t(y0));
      const uint32_t cabe_ancho = resuelta->imagen.ancho > dx ? resuelta->imagen.ancho - dx : 0;
      const uint32_t cabe_alto = resuelta->imagen.alto > dy ? resuelta->imagen.alto - dy : 0;
      const uint32_t ancho = std::min(pedido_ancho, cabe_ancho);
      const uint32_t alto = std::min(pedido_alto, cabe_alto);
      // With the scene at a higher resolution than the render target (nfsmw_resolucion_interna = 1920x1080
      // and a 1280x720 front buffer), copying 1 to 1 takes only a piece: the image comes out cropped, with the
      // car and the HUD out of frame. When it does not fit, it is shrunk with a linear blit, which is exactly
      // the scaling wanted. If it fits, it is copied as always, bit for bit.
      // It only shrinks when the scene really has to be reduced (by a factor of 1.25 or more) and both images
      // are color: vkCmdBlitImage with a linear filter on a depth target is not valid and brings the process
      // down. Mismatches of a few pixels (320x184 -> 320x180) are still cropped as always.
      const bool encoger = blit_ != nullptr && cabe_ancho && cabe_alto &&
                           destino_render->formato == kFormatoColor &&
                           resuelta->imagen.formato == kFormatoColor &&
                           (pedido_ancho * 4 >= cabe_ancho * 5 || pedido_alto * 4 >= cabe_alto * 5);
      if (encoger && uint32_t(x0) < destino_render->ancho && uint32_t(y0) < destino_render->alto) {
        // nfsmw_nativo_frontal_perezoso. If this texture had a deferred copy, it is dropped if this resolve
        // covers it entirely, and recorded first otherwise.
        ResolverFrontalAnterior(base_resuelta, dx == 0 && dy == 0 && cabe_ancho == resuelta->imagen.ancho &&
                                                   cabe_alto == resuelta->imagen.alto);
        VkImageBlit reduccion{};
        reduccion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reduccion.srcOffsets[0] = {x0, y0, 0};
        reduccion.srcOffsets[1] = {x0 + int32_t(pedido_ancho), y0 + int32_t(pedido_alto), 1};
        reduccion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reduccion.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
        reduccion.dstOffsets[1] = {int32_t(dx + cabe_ancho), int32_t(dy + cabe_alto), 1};
        blit_(comandos_trabajo_, destino_render->imagen, VK_IMAGE_LAYOUT_GENERAL,
              resuelta->imagen.imagen, VK_IMAGE_LAYOUT_GENERAL, 1, &reduccion, VK_FILTER_LINEAR);
        ++copias_;
        ++reducciones_;
        if (reducciones_ <= 4) {
          REXLOG_INFO("[resolucion] la escena de {}x{} se encoge a {}x{} al resolverla (superescalado)",
                      pedido_ancho, pedido_alto, cabe_ancho, cabe_alto);
        }
        AnotarCopia(cabe_ancho, cabe_alto);
        // A linear-filter blit reads the large rectangle and writes the small one, so it costs more than a copy
        // of the output size. The pixels read are recorded, since they are what dominates.
        ResueltaEscrita(base_resuelta, uint64_t(pedido_ancho) * pedido_alto);
        LeerResuelta(reg, *resuelta, x0, y0, dx, dy, cabe_ancho, cabe_alto);
      } else if (ancho && alto && uint32_t(x0) < destino_render->ancho &&
                 uint32_t(y0) < destino_render->alto && dx < resuelta->imagen.ancho &&
                 dy < resuelta->imagen.alto) {
        /*
         * The same as is done with depth. If the whole render target is resolved to a texture of the same size,
         * the two images are swapped and not a single pixel is copied. That is 4 copies of 1280x720 per frame,
         * 78 % of the traffic.
         */
        const bool entero_color = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                                  ancho == destino_render->ancho && alto == destino_render->alto;
        /*
         * For color the clear is a requirement, unlike for depth. Tested without it: 16,807 color swaps and
         * 17,350 restores, almost one for one; the game draws on top again without clearing and the copy is
         * paid anyway, only later and with two operations instead of one.
         */
        if (entero_color && borrar_color && ResolverSinCopia() &&
            REXCVAR_GET(nfsmw_nativo_intercambiar_color)) {
          // With x0 = y0 = 0 the tiling offset is 0, so the address is the base as is.
          if (IntercambiarConResuelta(*destino_render, *resuelta, reg.rb_copy_dest_base & 0x1FFFFFFF)) {
            ++intercambios_color_;
            LeerResuelta(reg, *resuelta, x0, y0, dx, dy, ancho, alto);
            return true;
          }
        }
        VkImageCopy copia{};
        copia.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copia.srcOffset = {x0, y0, 0};
        copia.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copia.dstOffset = {int32_t(dx), int32_t(dy), 0};
        copia.extent = {ancho, alto, 1};
        // nfsmw_nativo_frontal_perezoso. The previous copy to this texture, if still deferred, is dropped (this
        // one covers it entirely) or recorded first; and if this texture is a front buffer that only the Swap
        // reads, it is deferred.
        ResolverFrontalAnterior(base_resuelta,
                                dx == 0 && dy == 0 && ancho == resuelta->imagen.ancho && alto == resuelta->imagen.alto);
        if (destino_render->formato != resuelta->imagen.formato) {
          // A render target in another host format than its texture (NFS Carbon's half float scene resolved to
          // 8_8_8_8): vkCmdCopyImage needs compatible formats, a 1:1 blit converts.
          if (blit_ != nullptr) {
            VkImageBlit conversion{};
            conversion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            conversion.srcOffsets[0] = {x0, y0, 0};
            conversion.srcOffsets[1] = {x0 + int32_t(ancho), y0 + int32_t(alto), 1};
            conversion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            conversion.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
            conversion.dstOffsets[1] = {int32_t(dx + ancho), int32_t(dy + alto), 1};
            blit_(comandos_trabajo_, destino_render->imagen, VK_IMAGE_LAYOUT_GENERAL, resuelta->imagen.imagen,
                  VK_IMAGE_LAYOUT_GENERAL, 1, &conversion, VK_FILTER_NEAREST);
            ++copias_;
            AnotarCopia(ancho, alto);
            ResueltaEscrita(base_resuelta, uint64_t(ancho) * alto);
            LeerResuelta(reg, *resuelta, x0, y0, dx, dy, ancho, alto);
          } else {
            Rechazar(9, "copia entre formatos distintos sin vkCmdBlitImage");
          }
        } else if (!AplazarCopiaFrontal(base_resuelta, *destino_render, *resuelta, copia) &&
            !AplazarCompuesta(base_resuelta, *destino_render, *resuelta, copia,
                              (info_color & 0xFFF) | (pitch << 12))) {
          copiar_imagen_(comandos_trabajo_, destino_render->imagen, VK_IMAGE_LAYOUT_GENERAL,
                         resuelta->imagen.imagen, VK_IMAGE_LAYOUT_GENERAL, 1, &copia);
          ++copias_;
          AnotarCopia(ancho, alto);
          ResueltaEscrita(base_resuelta, uint64_t(ancho) * alto);
          LeerResuelta(reg, *resuelta, x0, y0, dx, dy, ancho, alto);
        }
      }
    }
    if (borrar_color) {
      const uint64_t valor_crudo =
          uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);
      /*
       * Two things that were missing here.
       *  1. The clear leaves the whole render target with a known color, so if its content had gone away
       *     in a swap it no longer has to be brought back. Depth already did this (BorrarProfundidad);
       *     color did not, and that is why, when nfsmw_nativo_intercambiar_color was turned on, there were
       *     17,350 restores against 16,807 swaps: almost all of them restored only to clear right after.
       *  2. If the render target is already cleared to that same color and nothing has been drawn since
       *     then, the vkCmdClearColorImage does not change a single bit and is skipped.
       */
      if (compuesta_hay_ && compuesta_.origen == ((info_color & 0xFFF) | (pitch << 12))) {
        CompuestaAntesDeEscribirOrigen();  // nfsmw_nativo_compuesta_perezosa (its source is cleared)
      }
      if (!diag_vigiladas_.empty()) {  // nfsmw_nativo_diag_lectores_s (a source is cleared)
        DiagOrigenEscrito((info_color & 0xFFF) | (pitch << 12));
      }
      destino_render->contenido_invalido = false;
      const uint64_t valor_estado =
          REXCVAR_GET(nfsmw_nativo_diag_borrado) ? ~uint64_t(0) : valor_crudo;
      if (BorradoRedundante(*destino_render, valor_estado)) {
        ++borrados_saltados_;
      } else {
        // nfsmw_nativo_frontal_perezoso. If this render target is the source of a deferred front buffer, the
        // clear goes to a spare image and the one with the content is kept for the front buffer (no copy).
        RotarFrontalAntesDeBorrar(*destino_render);
        MarcarGpu(kGpuBorrados);  // clears, separate from copies (C2 report)
        VkClearColorValue color{};
        if (REXCVAR_GET(nfsmw_nativo_diag_borrado)) {
          color = ColorDiagnostico(info_color & 0xFFF, formato_color, pitch);
        } else {
          if (formato_color == 3 || formato_color == 12) {
            // k_2_10_10_10_FLOAT: three 7e3 channels (3-bit exponent with bias 3, 7-bit mantissa) and a 2-bit
            // alpha.
            const auto siete_e3 = [](uint32_t v) {
              const uint32_t e = (v >> 7) & 0x7, m = v & 0x7F;
              return e ? std::ldexp(1.0f + float(m) / 128.0f, int(e) - 3) : std::ldexp(float(m) / 128.0f, -2);
            };
            const uint32_t w = uint32_t(valor_crudo);
            color.float32[0] = siete_e3(w & 0x3FF);
            color.float32[1] = siete_e3((w >> 10) & 0x3FF);
            color.float32[2] = siete_e3((w >> 20) & 0x3FF);
            color.float32[3] = float(w >> 30) * (1.0f / 3.0f);
          } else {
            for (uint32_t j = 0; j < 4; ++j) {
              color.float32[j] = float((valor_crudo >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
            }
          }
        }
        // nfsmw_nativo_borrar_area_util. Only the rows in use; the bottom band, if needed.
        if (!BorrarColorAreaUtil(*destino_render, color)) {
          dfn_.vkCmdClearColorImage(comandos_trabajo_, destino_render->imagen,
                                    VK_IMAGE_LAYOUT_GENERAL, &color, 1, &kRangoColor);
          QuitarBanda(*destino_render);
        }
        ++borrados_;
        AnotarBorradoDiag(*destino_render, info_color & 0xFFF, formato_color, pitch, false, false);
      }
    }
    if (((control >> 9) & 0x1) && borrar_profundidad_) {
      BorrarProfundidad(reg, pitch);
    }
    return true;
  }

  void BorrarProfundidad(const RegistrosCopia& reg, uint32_t pitch) {
    const uint32_t info = reg.rb_depth_info;
    Imagen* profundidad = ObtenerProfundidad(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!profundidad || !Grabar()) {
      return;
    }
    Preparar(*profundidad);
    AntesDeEscribirProfundidad(*profundidad);  // nfsmw_nativo_profundidad_perezosa
    SombraMinimoAntesDeBorrar(*profundidad);   // nfsmw_nativo_sombra_minimo
    profundidad->contenido_invalido = false;  // The clear gives it contents
    const VkClearDepthStencilValue valor{float(reg.rb_depth_clear >> 8) / 16777215.0f,
                                         reg.rb_depth_clear & 0xFF};
    // If it is already cleared with this same value and nobody has drawn anything since then, the clear does
    // not change a single bit. Mind the path below: the per-pass clear also resets the ZCULL plane, so that
    // one is never skipped (only the vkCmdClearDepthStencilImage one).
    if (profundidad->admite_destino_de_copia &&
        BorradoRedundante(*profundidad, uint64_t(reg.rb_depth_clear))) {
      ++borrados_saltados_profundidad_;
      return;
    }
    MarcarGpu(kGpuBorrados);  // clears, separate from copies (C2 report)
    ++borrados_profundidad_;
    // nfsmw_nativo_diag_borrados. Without TRANSFER_DST the clear is done by opening a pass (ZCULL).
    AnotarBorradoDiag(*profundidad, info & 0xFFF, (info >> 16) & 0x1, pitch, true,
                      !profundidad->admite_destino_de_copia);
    // ZCULL: see Preparar. Without TRANSFER_DST the clear has to be done by opening a pass.
    if (!profundidad->admite_destino_de_copia && dibujos_) {
      // This bool cannot be dropped. If the clear pass cannot be opened, the depth keeps the previous frame's
      // content and the ZCULL hi-Z is not reset either (only a loadOp = CLEAR resets it), so the whole frame
      // culls against stale data. Without TRANSFER_DST there is no alternative path, so at least it is
      // counted and reported.
      if (!dibujos_->BorrarProfundidadEnPase(comandos_trabajo_, *profundidad, valor.depth,
                                             valor.stencil)) {
        if (++borrados_en_pase_fallidos_ <= 8) {
          REXLOG_WARN("[nativo] C2: NO se pudo borrar la profundidad {}x{} abriendo un pase; se queda "
                      "con el contenido anterior (fallo {})",
                      profundidad->ancho, profundidad->alto, borrados_en_pase_fallidos_);
        }
      }
      return;
    }
    borrar_profundidad_(comandos_trabajo_, profundidad->imagen, VK_IMAGE_LAYOUT_GENERAL, &valor, 1,
                        &kRangoProfundidad);
  }

  // Depth copy: the rectangle of the depth render target to a resolved texture
  // of the same host format, at the base GetResolveInfo computes
  // (graphics/util/draw.cpp:945-1010; 4 bytes per texel, like k_24_8).
  // borrara: this same game command clears the render target right after resolving it. Only then is it worth
  // swapping the images: if the game kept drawing on top, the content would have to be brought back and the
  // copy would be paid anyway (measured: one restore per frame on the shadow map).
  void CopiarProfundidad(const RegistrosCopia& reg, uint32_t pitch, bool borrara) {
    const uint32_t info_destino = reg.rb_copy_dest_info;
    if ((info_destino >> 3) & 0x1) {
      Rechazar(3, "copia a textura 3D o array: todavia no");
      return;
    }
    int32_t x0, y0, x1, y1;
    if (!copiar_imagen_ || !Rectangulo(reg, pitch, x0, y0, x1, y1)) {
      return;
    }
    const uint32_t info = reg.rb_depth_info;
    ++copias_profundidad_;  // the game asks to resolve the depth to a texture
    Imagen* profundidad = ObtenerProfundidad(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!profundidad) {
      return;
    }
    const uint32_t pitch_destino = reg.rb_copy_dest_pitch & 0x3FFF;
    const uint32_t alto_destino = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
    const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
    const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
    const uint32_t base =
        reg.rb_copy_dest_base +
        uint32_t(DesplazamientoMosaico2D(int32_t(base_x), int32_t(base_y), pitch_destino, 2));
    const uint32_t dx = uint32_t(x0) - base_x;
    const uint32_t dy = uint32_t(y0) - base_y;
    const uint32_t formato_textura = ((info >> 16) & 0x1) ? 23 : 22;  // k_24_8_FLOAT : k_24_8
    // With the scaled shadow map, the resolved texture stays at that same size.
    // The scene picks it by address, without looking at the size, and samples it with normalized coordinates
    // and point sampling, so 1024 read with the UVs of 1600 gives the same texel as 1024 upscaled to 1600
    // with NEAREST: the image does not change. What is saved is the upscaling blit (1.78 ms on the console)
    // and the larger half of the copy (10.24 MB -> 4.2 MB, twice per frame).
    uint32_t ancho_resuelta = pitch_destino, alto_resuelta = alto_destino;
    if (profundidad->ancho_guest && profundidad->ancho_guest != profundidad->ancho) {
      ancho_resuelta = uint32_t(uint64_t(pitch_destino) * profundidad->ancho / profundidad->ancho_guest);
      alto_resuelta = uint32_t(uint64_t(alto_destino) * profundidad->alto / profundidad->alto_guest);
    }
    // If the previous copy to this address is still deferred, it is recorded first (exact; it does not happen
    // in a race: the scene clear resolves it earlier). See nfsmw_nativo_profundidad_perezosa.
    if (!pendientes_.empty() && pendientes_.count(base & 0x1FFFFFFF)) {
      GrabarCopiaPendiente(base & 0x1FFFFFFF);
      ++perezosa_copiadas_escritura_;
    }
    Resuelta* resuelta = ObtenerResuelta(base & 0x1FFFFFFF, ancho_resuelta, alto_resuelta,
                                         formato_textura, false, formato_profundidad_);
    if (!resuelta || !Grabar()) {
      return;
    }
    // Depth is also included in the per-render-target inventory. Previously only the square cubemap faces were
    // recorded, so the shadow map (the most expensive copy in the frame) did not show up.
    AnotarCopia(base & 0x1FFFFFFF, ancho_resuelta, alto_resuelta, 0);
    if (diag_ventana_ || !diag_vigiladas_.empty()) {  // nfsmw_nativo_diag_lectores_s
      DiagEscritura(base & 0x1FFFFFFF, info & 0xFFF, pitch, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                    resuelta->imagen.ancho, resuelta->imagen.alto, 0, true);
    }
    Preparar(*profundidad);
    Preparar(resuelta->imagen);
    AnotarAreaUtil(*profundidad, y1);  // the area the game really resolves
    AnotarUsoBorrado(*profundidad, uint32_t(std::max(x1, 0)), uint32_t(std::max(y1, 0)));
    // With the scaled shadow map, the rectangle the guest sends is in 1600-pixel units even though the image
    // is smaller. Everything below reasons in guest pixels.
    const uint32_t ancho_guest = profundidad->ancho_guest ? profundidad->ancho_guest : profundidad->ancho;
    const uint32_t alto_guest = profundidad->alto_guest ? profundidad->alto_guest : profundidad->alto;
    const bool escalado = ancho_guest != profundidad->ancho || alto_guest != profundidad->alto;
    const uint32_t ancho = std::min({uint32_t(x1 - x0), ancho_guest - uint32_t(x0),
                                     resuelta->imagen.ancho - dx});
    const uint32_t alto = std::min({uint32_t(y1 - y0), alto_guest - uint32_t(y0),
                                    resuelta->imagen.alto - dy});
    if (!ancho || !alto || uint32_t(x0) >= ancho_guest ||
        uint32_t(y0) >= alto_guest || dx >= resuelta->imagen.ancho ||
        dy >= resuelta->imagen.alto) {
      return;
    }
    // nfsmw_nativo_resolver_contenido_valido. If the content of this render target went away in an earlier
    // swap and nobody brought it back, it is in another texture: it is brought back before reading it (the
    // menu flicker).
    if (profundidad->contenido_invalido) {
      if (REXCVAR_GET(nfsmw_nativo_resolver_contenido_valido)) {
        RestaurarContenido(*profundidad, true);
      } else {
        ++resolver_contenido_viejo_;  // previous behavior: the old image is resolved
      }
    }
    if (escalado) {
      // Source and destination have the same reduced size, so it is a normal 1 to 1 copy
      // with the rectangle converted to image pixels. No blit, no scaling.
      const auto aX = [&](int32_t v) {
        return int32_t(int64_t(v) * profundidad->ancho / ancho_guest);
      };
      const auto aY = [&](int32_t v) {
        return int32_t(int64_t(v) * profundidad->alto / alto_guest);
      };
      const uint32_t ancho_img = std::min(
          {uint32_t(std::max(1, aX(x0 + int32_t(ancho)) - aX(x0))),
           profundidad->ancho - uint32_t(aX(x0)), resuelta->imagen.ancho - uint32_t(aX(int32_t(dx)))});
      const uint32_t alto_img = std::min(
          {uint32_t(std::max(1, aY(y0 + int32_t(alto)) - aY(y0))),
           profundidad->alto - uint32_t(aY(y0)), resuelta->imagen.alto - uint32_t(aY(int32_t(dy)))});
      if (!ancho_img || !alto_img) {
        return;
      }
      VkImageCopy copia{};
      copia.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copia.srcOffset = {aX(x0), aY(y0), 0};
      copia.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copia.dstOffset = {aX(int32_t(dx)), aY(int32_t(dy)), 0};
      copia.extent = {ancho_img, alto_img, 1};
      copiar_imagen_(comandos_trabajo_, profundidad->imagen, VK_IMAGE_LAYOUT_GENERAL,
                     resuelta->imagen.imagen, VK_IMAGE_LAYOUT_GENERAL, 1, &copia);
      ++copias_;
      ++sombras_subidas_;
      AnotarCopia(ancho_img, alto_img);
      ResueltaEscrita(base & 0x1FFFFFFF, uint64_t(ancho_img) * alto_img);
      return;
    }
    // If the whole render target is resolved to a texture of the same size, the images are swapped and
    // nothing is copied (the shadow map is 1600x1600: 10 MB per copy).
    if (ResolverSinCopia()) {  // and if it is not swapped, why not
      const bool entero = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                          ancho == profundidad->ancho && alto == profundidad->alto;
      // The clear is no longer a requirement (see nfsmw_nativo_intercambiar_sin_borrado).
      const bool puede = entero && (borrara || REXCVAR_GET(nfsmw_nativo_intercambiar_sin_borrado));
      if (puede) {
        if (IntercambiarConResuelta(*profundidad, *resuelta, base & 0x1FFFFFFF)) {
          if (!borrara) {
            ++intercambios_sin_borrado_;
          }
          SombraMinimoTrasIntercambio(*profundidad, base & 0x1FFFFFFF, borrara);
          return;
        }
        ++sin_intercambio_[2];  // the swap itself could not be done
      } else {
        /*
         * The label was backwards ever since nfsmw_nativo_intercambiar_sin_borrado existed. With that setting
         * on, `puede` = whole, so everything that lands here is "not resolved whole" and the log counted it as
         * "the command does not clear the render target". It is now split by the real cause: if the resolve
         * is whole, the cause is the clear; otherwise, it is the size.
         */
        ++sin_intercambio_[entero ? 0 : 1];  // 0: the command does not clear the render target; 1: not the whole render target
        if (avisos_sin_intercambio_ < 8) {
          ++avisos_sin_intercambio_;
          REXLOG_INFO("[nativo] C2 sin intercambio: {}x{} de {}x{} en ({},{})->({},{}), borra {} (base {:03X})",
                      ancho, alto, profundidad->ancho, profundidad->alto, x0, y0, dx, dy, borrara,
                      base & 0xFFF);
        }
      }
    }
    VkImageCopy copia{};
    copia.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copia.srcOffset = {x0, y0, 0};
    copia.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copia.dstOffset = {int32_t(dx), int32_t(dy), 0};
    copia.extent = {ancho, alto, 1};
    // If this address is only requested by the composition without blur, the copy is deferred
    // (nfsmw_nativo_profundidad_perezosa): it is done as soon as a draw really samples it; if nobody does,
    // it is not.
    SombraMinimoCopiaDesde(*profundidad);  // nfsmw_nativo_sombra_minimo
    if (AplazarCopiaProfundidad(base & 0x1FFFFFFF, *profundidad, *resuelta, copia)) {
      return;
    }
    copiar_imagen_(comandos_trabajo_, profundidad->imagen, VK_IMAGE_LAYOUT_GENERAL,
                   resuelta->imagen.imagen, VK_IMAGE_LAYOUT_GENERAL, 1, &copia);
    ++copias_;
    AnotarCopia(ancho, alto);
    ResueltaEscrita(base & 0x1FFFFFFF, uint64_t(ancho) * alto);
  }

  void RampaGamma(const std::array<std::array<uint16_t, 3>, 256>& rampa) override {
    rampa_juego_ = rampa;
    RecalcularRampa();
  }

  // The output table = the game's ramp followed by the post-processing, with the formula and order of
  // GoldenEye-Recomp (ge_grade.cs.hlsl): temperature, brightness, contrast, tint, saturation, vibrance and
  // gamma. Everything that is single-channel is computed here for the 256 entries, so without saturation,
  // vibrance, vignette or scanlines there is no per-pixel work (and when off, the table is the ramp as
  // is). With them (graduacion_), the table stops before the gamma and without clamping, and the shader
  // does the rest.
  void RecalcularRampa() {
    const nfsmw::ajustes::Posproceso& p = posproceso_;
    graduacion_ = p.activo && (p.saturacion != 1.0f || p.vibracion != 0.0f || p.vineta > 0.0f || p.lineas > 0.0f);
    const float tinte[3] = {p.tinte_r, p.tinte_g, p.tinte_b};
    const float inversa_gamma = p.gamma > 0.0f ? 1.0f / p.gamma : 1.0f;
    for (uint32_t i = 0; i < 256; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        float x = float(rampa_juego_[i][c] & 0x3FF) / 1023.0f;
        if (p.activo) {
          x += c == 0 ? p.temperatura * 0.10f : c == 2 ? -p.temperatura * 0.10f : 0.0f;
          x += p.brillo;
          x = (x - 0.5f) * p.contraste + 0.5f;
          x = x + (x * tinte[c] - x) * p.tinte;
          if (!graduacion_) {
            x = std::clamp(std::pow(std::max(x, 0.0001f), inversa_gamma), 0.0f, 1.0f);
          }
        }
        rampa_valores_[i * 4 + c] = x;
      }
      rampa_valores_[i * 4 + 3] = 1.0f;
    }
    const size_t extra = 256 * 4;
    rampa_valores_[extra] = p.saturacion;
    rampa_valores_[extra + 1] = p.vibracion;
    rampa_valores_[extra + 2] = inversa_gamma;
    rampa_valores_[extra + 3] = 0.0f;
    rampa_valores_[extra + 4] = p.vineta;
    rampa_valores_[extra + 5] = p.lineas;
    rampa_valores_[extra + 6] = 0.0f;
    rampa_valores_[extra + 7] = 0.0f;
    ++version_rampa_;
  }

  bool Presentar(rex::ui::Presenter* presentador, const TexturaSwap& swap, uint32_t ancho,
                 uint32_t alto) override {
    nfsmw::reflejo_demanda::AnotarSwap();  // nfsmw_reflejo_bajo_demanda
    // The per-draw diagnostic window opens here, on the PM4 ring thread, which is the one that records the
    // draws. Inside the paint call it would be another thread and the window would catch an arbitrary piece
    // of the frame (33 of 500 shadow draws were measured).
    AbrirVentanaDiagnostico();
#if REX_PLATFORM_SWITCH
    RexSwitchPerfCount(0);  // the profiler's "juego N fps", which read 0.0 with the native renderer
#endif
    // Histogram of intervals between Swaps (C2 report): whether they come in vsync steps or spread out.
    {
      const auto ahora = std::chrono::steady_clock::now();
      if (ultimo_swap_ != std::chrono::steady_clock::time_point{}) {
        const double ms = std::chrono::duration<double, std::milli>(ahora - ultimo_swap_).count();
#if REX_PLATFORM_SWITCH
        // Frames over 45 ms go to stack sampling. The window starts one normal frame (33 ms) earlier, because
        // the game thread prepares the frame ahead of the ring. Ticks at 19.2 MHz.
        if (ms > 45.0) {
          uint64_t fin;
          asm volatile("mrs %0, cntpct_el0" : "=r"(fin));  // libnx's armGetSystemTick
          const uint64_t largo = uint64_t((ms + 33.3) * 19200.0);
          RexSwitchPerfTiron(fin > largo ? fin - largo : 0, fin);
        }
#endif
        const uint32_t cubeta = ms < 15.0    ? 0
                                : ms < 18.0  ? 1
                                : ms < 25.0  ? 2
                                : ms < 30.0  ? 3
                                : ms < 36.0  ? 4
                                : ms < 50.0  ? 5
                                : ms < 60.0  ? 6
                                : ms < 75.0  ? 7
                                : ms < 100.0 ? 8
                                : ms < 150.0 ? 9
                                             : 10;
        ++cubetas_swap_[cubeta];
        // The worst frame of the interval. The average is not felt; the peak is.
        if (ms > peor_swap_ms_) {
          peor_swap_ms_ = ms;
        }
        nfsmw::guardia30::Latir(ms);  // the 30 FPS guard decides with this
        /*
         * What happens in a frame that runs long.
         *
         * Measured on the console: 7.3 % of frames exceed 50 ms and there are peaks of 2.1 seconds. Pipelines
         * are stable (116, 11 ms) and the texture cache no longer evicts, so the cause is something else.
         *
         * When a frame runs long, what that frame did (not the running total) is dumped. If the culprit is
         * loading textures, or creating pipelines, or a big copy, or simply that the game sent three times as
         * many draws, it shows here. It only writes when there is a stutter, so it costs nothing in the normal
         * case.
         */
        /*
         * The previous dump was not enough, and this is why.
         *
         * The stutters are 60-70 ms frames (median 67, against an average of 39.34) with an exactly normal
         * render load: 14 copies, 4 clears, 2 swaps, 1 restore, the same as any other frame. And they are not
         * quantized to the vblank (34 of them fall between 60.0 and 62.5 ms, which is not a multiple of
         * 16.67). So there are ~26 extra ms that are not in the drawing and the old dump did not see them.
         *
         * So now the time breakdown is dumped, not only the work count:
         *   - GPU work of that frame, with the timestamps we already measure. If it goes up to 60 ms it is
         *     scene load; if it stays at 33 the time went to the CPU or to waiting.
         *   - how long the thread took to record and how long to present.
         *   - new textures and pipelines, the usual suspects of an isolated peak.
         * With that, the log says where the 26 ms come from without guessing.
         */
        /*
         * The cap of 200 was blinding us for half a race.
         *
         * The 200 warnings ran out at t=135 s of a 234 s session, and the following intervals had 96 frames
         * over 50 ms without a single line. So exactly the most played stretch was a black hole. With 1000 a
         * whole session fits, and since it only writes when there is a stutter it costs nothing in the normal
         * case.
         */
        if (ms > 60.0 && avisos_tiron_ < 1000) {
          ++avisos_tiron_;
          /* gpu_ns_ are raw GPU timestamps: multiply by 1.627 to get real milliseconds (see docs/measuring.md). */
          const double gpu_ms = double(gpu_ns_ - tiron_gpu_ns_) / 1e6 * 1.627;
          // The three [tiron] lines go to the report thread (NFSMW_INFORME_ANILLO).
          NFSMW_INFORME_ANILLO(
              "[tiron] fotograma de {:.1f} ms (GPU {:.1f} reales, grabar {:.1f}): {} copias, "
              "{} borrados, {} intercambios, {} restauraciones, {} esperas a la GPU",
              ms, gpu_ms, double(ns_grabar_ - tiron_ns_grabar_) / 1e6, copias_ - tiron_copias_,
              borrados_ - tiron_borrados_, intercambios_ - tiron_resolves_,
              restauraciones_ - tiron_restaura_, esperas_gpu_ - tiron_esperas_);
          /*
           * Who waits for whom in this frame, in ms (see nfsmw_esperas_tiron.h).
           * The game's and the ring's waits overlap: they do not add up to the frame, they say where it went.
           */
          namespace e = nfsmw::esperas;
          const auto delta = [&](e::Tipo t) {
            return double(e::g_ns[t].load(std::memory_order_relaxed) - tiron_esperas_ns_[t]) / 1e6;
          };
          const auto veces = [&](e::Tipo t) {
            return e::g_veces[t].load(std::memory_order_relaxed) - tiron_esperas_veces_[t];
          };
          NFSMW_INFORME_ANILLO(
              "[tiron] esperas (ms): juego: relevo del ejecutor {:.1f} ({}), relevo del preparador {:.1f} ({}), "
              "sitio en el anillo {:.1f} ({}), dentro de sub_826E8EE8 {:.1f} ({}; vtabla {:08X}, llamante {:08X}) | "
              "anillo: sin trabajo {:.1f} ({}), WAIT_REG_MEM {:.1f} ({}), valla de la GPU {:.1f}, salida {:.1f} | "
              "texturas comprobadas {:.1f} MB, aplazadas {}",
              delta(e::kRelevoEjecutor), veces(e::kRelevoEjecutor), delta(e::kRelevoPreparador),
              veces(e::kRelevoPreparador), delta(e::kSitioAnillo), veces(e::kSitioAnillo),
              delta(e::kJuegoMedio), veces(e::kJuegoMedio), e::g_vtabla_medio.load(std::memory_order_relaxed),
              e::g_llamante_medio.load(std::memory_order_relaxed), delta(e::kAnilloSinTrabajo),
              veces(e::kAnilloSinTrabajo), delta(e::kAnilloRegMem), veces(e::kAnilloRegMem),
              double(ns_esperas_gpu_ - tiron_ns_esperas_gpu_) / 1e6,
              double(ns_espera_salida_ - tiron_ns_espera_salida_) / 1e6,
              double(e::g_bytes_huella.load(std::memory_order_relaxed) - tiron_bytes_huella_) / 1048576.0,
              e::g_huellas_aplazadas.load(std::memory_order_relaxed) - tiron_huellas_aplazadas_);
          // What the ring spent this frame on.
          const auto dif = [](const std::atomic<uint64_t>& a, uint64_t antes) {
            return a.load(std::memory_order_relaxed) - antes;
          };
          // And how long the ring waited for the vertex copy thread (EsperarSubidas). The wait before each
          // submit is inside "trabajando" and the one before returning the read pointer is outside: it is read
          // separately, not added.
          // And what the ring spent creating textures (image, memory and view; "texturas" starts afterwards),
          // how many the binding thread bound and how long the ring waited for it
          // (nfsmw_nativo_texturas_enlace_hilo).
          NFSMW_INFORME_ANILLO("[tiron] anillo: {} dibujos, trabajando {:.1f} ms, texturas {:.1f} ms ({} subidas, {:.1f} MB; "
                      "{} creadas; huellas {:.1f} + {:.1f} ms); esperando al hilo de copias de vertices {:.1f} ms "
                      "({} veces), ayudandolo {:.1f} ms ({} copias); "
                      "crear texturas {:.1f} ms en el anillo, {} enlazadas en el hilo, esperandolo {:.1f} ms; "
                      "huellas en el hilo: {} texturas en {:.1f} ms del hilo, copias en el anillo {:.1f} ms, {} "
                      "hechas por el anillo en {:.1f} ms, esperandolo {:.1f} ms",
                      dif(e::g_dibujos, tiron_dibujos_), double(dif(e::g_ns_anillo_trabajando, tiron_ns_anillo_)) / 1e6,
                      double(dif(e::g_ns_texturas, tiron_ns_texturas_)) / 1e6,
                      dif(e::g_texturas_subidas, tiron_texturas_subidas_),
                      double(dif(e::g_bytes_subidos, tiron_bytes_subidos_)) / 1048576.0,
                      dif(e::g_texturas_creadas, tiron_texturas_creadas_),
                      double(dif(e::g_ns_huella_cruda, tiron_ns_huella_cruda_)) / 1e6,
                      double(dif(e::g_ns_huella_datos, tiron_ns_huella_datos_)) / 1e6,
                      double(dif(e::g_ns_esperando_copias, tiron_ns_esperando_copias_)) / 1e6,
                      dif(e::g_esperas_copias, tiron_esperas_copias_),
                      double(dif(e::g_ns_ayudando_copias, tiron_ns_ayudando_copias_)) / 1e6,
                      dif(e::g_copias_ayudadas, tiron_copias_ayudadas_),
                      double(dif(e::g_ns_crear_texturas, tiron_ns_crear_texturas_)) / 1e6,
                      dif(e::g_texturas_enlazadas_hilo, tiron_texturas_enlazadas_hilo_),
                      double(dif(e::g_ns_esperando_enlaces, tiron_ns_esperando_enlaces_)) / 1e6,
                      // the texture fingerprint thread (nfsmw_nativo_texturas_huella_hilo)
                      dif(e::g_texturas_huella_hilo, tiron_texturas_huella_hilo_),
                      double(dif(e::g_ns_huella_hilo, tiron_ns_huella_hilo_)) / 1e6,
                      double(dif(e::g_ns_huella_instantanea, tiron_ns_huella_instantanea_)) / 1e6,
                      dif(e::g_texturas_huella_anillo, tiron_texturas_huella_anillo_),
                      double(dif(e::g_ns_huella_anillo, tiron_ns_huella_anillo_)) / 1e6,
                      double(dif(e::g_ns_esperando_huellas, tiron_ns_esperando_huellas_)) / 1e6);
          /*
           * The game's side. Four of the seven race stutters in one run come from the preparer (the executor
           * waiting for the handoff, or waiting for commands inside the list, with the ring idle), and the wait
           * without commands did not show up in any line. "Fuera de la lista" is the preparer's simulation plus
           * its handoff wait (the one in the line above): the difference is what the game itself takes.
           */
          NFSMW_INFORME_ANILLO("[tiron] juego: el ejecutor sin ordenes {:.1f} ms ({}) | el preparador: llenando la "
                               "lista {:.1f} ms ({}), de ello TreeCull {:.1f} ms ({}); fuera de la lista {:.1f} ms ({})",
                               delta(e::kEjecutorSinOrdenes), veces(e::kEjecutorSinOrdenes),
                               delta(e::kPreparadorLista), veces(e::kPreparadorLista),
                               delta(e::kPreparadorEscenario), veces(e::kPreparadorEscenario),
                               delta(e::kPreparadorFuera), veces(e::kPreparadorFuera));
        }
        for (uint32_t t = 0; t < nfsmw::esperas::kNumTipos; ++t) {
          tiron_esperas_ns_[t] = nfsmw::esperas::g_ns[t].load(std::memory_order_relaxed);
          tiron_esperas_veces_[t] = nfsmw::esperas::g_veces[t].load(std::memory_order_relaxed);
        }
        tiron_ns_esperas_gpu_ = ns_esperas_gpu_;
        tiron_dibujos_ = nfsmw::esperas::g_dibujos.load(std::memory_order_relaxed);
        tiron_ns_anillo_ = nfsmw::esperas::g_ns_anillo_trabajando.load(std::memory_order_relaxed);
        tiron_ns_texturas_ = nfsmw::esperas::g_ns_texturas.load(std::memory_order_relaxed);
        tiron_texturas_subidas_ = nfsmw::esperas::g_texturas_subidas.load(std::memory_order_relaxed);
        tiron_bytes_subidos_ = nfsmw::esperas::g_bytes_subidos.load(std::memory_order_relaxed);
        tiron_texturas_creadas_ = nfsmw::esperas::g_texturas_creadas.load(std::memory_order_relaxed);
        tiron_ns_huella_cruda_ = nfsmw::esperas::g_ns_huella_cruda.load(std::memory_order_relaxed);
        tiron_ns_huella_datos_ = nfsmw::esperas::g_ns_huella_datos.load(std::memory_order_relaxed);
        tiron_bytes_huella_ = nfsmw::esperas::g_bytes_huella.load(std::memory_order_relaxed);
        tiron_huellas_aplazadas_ = nfsmw::esperas::g_huellas_aplazadas.load(std::memory_order_relaxed);
        tiron_ns_esperando_copias_ = nfsmw::esperas::g_ns_esperando_copias.load(std::memory_order_relaxed);  // 170
        tiron_esperas_copias_ = nfsmw::esperas::g_esperas_copias.load(std::memory_order_relaxed);
        tiron_ns_ayudando_copias_ = nfsmw::esperas::g_ns_ayudando_copias.load(std::memory_order_relaxed);  // 185
        tiron_copias_ayudadas_ = nfsmw::esperas::g_copias_ayudadas.load(std::memory_order_relaxed);
        // Creating textures and the binding thread (nfsmw_nativo_texturas_enlace_hilo).
        tiron_ns_crear_texturas_ = nfsmw::esperas::g_ns_crear_texturas.load(std::memory_order_relaxed);
        tiron_texturas_enlazadas_hilo_ = nfsmw::esperas::g_texturas_enlazadas_hilo.load(std::memory_order_relaxed);
        tiron_ns_esperando_enlaces_ = nfsmw::esperas::g_ns_esperando_enlaces.load(std::memory_order_relaxed);
        // The texture fingerprint thread (nfsmw_nativo_texturas_huella_hilo).
        tiron_texturas_huella_hilo_ = nfsmw::esperas::g_texturas_huella_hilo.load(std::memory_order_relaxed);
        tiron_ns_huella_hilo_ = nfsmw::esperas::g_ns_huella_hilo.load(std::memory_order_relaxed);
        tiron_ns_huella_instantanea_ = nfsmw::esperas::g_ns_huella_instantanea.load(std::memory_order_relaxed);
        tiron_texturas_huella_anillo_ = nfsmw::esperas::g_texturas_huella_anillo.load(std::memory_order_relaxed);
        tiron_ns_huella_anillo_ = nfsmw::esperas::g_ns_huella_anillo.load(std::memory_order_relaxed);
        tiron_ns_esperando_huellas_ = nfsmw::esperas::g_ns_esperando_huellas.load(std::memory_order_relaxed);
        tiron_ns_espera_salida_ = ns_espera_salida_;
        tiron_gpu_ns_ = gpu_ns_;
        tiron_ns_grabar_ = ns_grabar_;
        tiron_copias_ = copias_;
        tiron_restaura_ = restauraciones_;
        tiron_esperas_ = esperas_gpu_;
        tiron_borrados_ = borrados_;
        tiron_resolves_ = intercambios_;
      }
      ultimo_swap_ = ahora;
    }
    const uint32_t base = (swap.dword[1] & 0xFFFFF000) & 0x1FFFFFFF;
    DiagLectoresAlPresentar(base);  // nfsmw_nativo_diag_lectores_s
    // nfsmw_nativo_frontal_perezoso. Before submitting the work: either it is drawn from the image that
    // holds the content (no copy) or the deferred copy is recorded now, ahead of the output.
    Imagen* const origen_frontal = FrontalAlPresentar(base, ancho, alto);
    const bool mosaico =
        presentador && REXCVAR_GET(nfsmw_nativo_diag_resueltas) && ComponerMosaico(base);
    // Without waiting for the GPU: the output goes after the work on the same queue.
    if (!presentador || !EnviarTrabajo(false)) {
      return false;
    }
    if (mosaico) {
      bool pintado_mosaico = false;
      presentador->RefreshGuestOutput(
          1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_contexto) {
            auto& contexto =
                static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_contexto);
            pintado_mosaico = PintarSalida(contexto, mosaico_, 1280, 720);
            return pintado_mosaico;
          });
      if (pintado_mosaico) {
        ++presentados_;
      }
      return pintado_mosaico;
    }
    auto it = resueltas_.find(base);
    if (it == resueltas_.end() || !it->second.imagen.preparada) {
      Rechazar(4, "Swap sin textura resuelta: se pinta el color de prueba");
      return false;
    }
    Resuelta& resuelta = it->second;
    const uint32_t w = std::min(ancho ? ancho : resuelta.imagen.ancho, resuelta.imagen.ancho);
    const uint32_t h = std::min(alto ? alto : resuelta.imagen.alto, resuelta.imagen.alto);
    bool pintado = false;
    // How much of RefreshGuestOutput belongs to the SDK (before and after the callback) and how much to
    // PintarSalida.
    const auto antes_refresco = std::chrono::steady_clock::now();
    auto entrada_llamada = antes_refresco;
    auto salida_llamada = antes_refresco;
    bool llamada = false;
    presentador->RefreshGuestOutput(
        w, h, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_contexto) {
          entrada_llamada = std::chrono::steady_clock::now();
          llamada = true;
          auto& contexto =
              static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_contexto);
          pintado = PintarSalida(contexto, origen_frontal ? *origen_frontal : resuelta.imagen, w, h,
                                 origen_frontal != nullptr);  // nfsmw_nativo_frontal_perezoso
          salida_llamada = std::chrono::steady_clock::now();
          return pintado;
        });
    if (llamada) {
      ns_antes_llamada_ += Ns(antes_refresco, entrada_llamada);
      ns_en_llamada_ += Ns(entrada_llamada, salida_llamada);
      ns_tras_llamada_ += Ns(salida_llamada, std::chrono::steady_clock::now());
      ++refrescos_;
    }
    if (pintado) {
      ++presentados_;
    }
    return pintado;
  }

  // nfsmw_nativo_compuesta_perezosa (see the cvar's comment). PM4 ring thread only.
  // It applies to the (address, source) pair that the diagnostic's watching has seen for kCompuestaAMirar
  // writes without any read after the source was written again. Until then, it copies as always (WATCHING).
  bool CompuestaAplicable(uint32_t direccion, uint32_t origen) {
    if (compuesta_apagada_ || !REXCVAR_GET(nfsmw_nativo_compuesta_perezosa)) {
      return false;
    }
    for (const VigiladaDiag& v : diag_vigiladas_) {
      if (v.direccion != direccion || v.origen != origen) {
        continue;
      }
      if (v.tardias != 0 || v.escrituras < kCompuestaAMirar) {
        return false;
      }
      if (compuesta_aplicando_ != direccion) {
        compuesta_aplicando_ = direccion;
        NFSMW_INFORME_ANILLO("[nativo] C2 compuesta perezosa: {:08X} desde {:03X}/{}: {} escrituras vigiladas, {} "
                             "leidas antes de volver a escribir su origen y 0 despues: APLICANDO (la copia se aplaza; "
                             "se graba antes del primer dibujo que la lea y se tira al escribir su origen)",
                             direccion, origen & 0xFFF, origen >> 12, v.escrituras, v.antes);
      }
      return true;
    }
    return false;
  }

  // From Copiar's 1 to 1 path: true if the copy is deferred (not recorded now).
  bool AplazarCompuesta(uint32_t direccion, Imagen& destino, const Resuelta& resuelta, const VkImageCopy& copia,
                        uint32_t origen) {
    if (compuesta_hay_ || REXCVAR_GET(nfsmw_nativo_diag_resueltas) || REXCVAR_GET(nfsmw_nativo_intercambiar_color)) {
      return false;
    }
    if (copia.srcOffset.x != 0 || copia.srcOffset.y != 0 || copia.dstOffset.x != 0 || copia.dstOffset.y != 0 ||
        copia.extent.width != resuelta.imagen.ancho || copia.extent.height != resuelta.imagen.alto ||
        destino.formato != kFormatoColor || resuelta.imagen.formato != kFormatoColor) {
      return false;
    }
    // The small ones are read back for the guest (LeerResuelta): never those.
    const int32_t texels_lectura = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
    if (uint64_t(copia.extent.width) * copia.extent.height <= uint64_t(std::max<int32_t>(texels_lectura, 0))) {
      return false;
    }
    if (!CompuestaAplicable(direccion, origen)) {
      return false;
    }
    compuesta_.direccion = direccion;
    compuesta_.origen = origen;
    compuesta_.destino = &destino;
    compuesta_.origen_vk = destino.imagen;
    compuesta_.textura_vk = resuelta.imagen.imagen;
    compuesta_.copia = copia;
    compuesta_hay_ = true;
    ResueltaEscrita(direccion);  // new contents (deferred)
    ++compuesta_aplazadas_;
    return true;
  }

  // Records the deferred copy now (outside a pass). Exact: its source has not been touched since it was
  // deferred.
  void GrabarCompuesta() {
    if (!compuesta_hay_) {
      return;
    }
    compuesta_hay_ = false;
    const CompuestaPendiente p = compuesta_;
    const auto r = resueltas_.find(p.direccion);
    if (r == resueltas_.end() || r->second.imagen.imagen != p.textura_vk || !p.destino ||
        p.destino->imagen != p.origen_vk || !copiar_imagen_ || !Grabar()) {
      compuesta_caducada_ = p.direccion;
      CompuestaDiferencia(p.direccion, "la copia aplazada ya no se puede grabar (cambio la imagen del origen o de la "
                                       "textura)");
      return;
    }
    if (dibujos_) {
      dibujos_->TerminarPase();  // it can arrive from a draw: the copy goes outside the pass
    }
    MarcarGpu(kGpuCopias);
    copiar_imagen_(comandos_trabajo_, p.origen_vk, VK_IMAGE_LAYOUT_GENERAL, p.textura_vk, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &p.copia);
    ++copias_;
    AnotarCopia(p.copia.extent.width, p.copia.extent.height);
    ResueltaEscrita(p.direccion, uint64_t(p.copia.extent.width) * p.copia.extent.height);
  }

  // Its source is about to be written and nobody has read it: it is dropped (or recorded, if the guard
  // turned it off). The texture is left without it until the next whole write; if someone samples it
  // before that, the guard fires.
  void CompuestaAntesDeEscribirOrigen() {
    if (!compuesta_hay_) {
      return;
    }
    if (compuesta_apagada_) {
      GrabarCompuesta();
      ++compuesta_grabadas_otras_;
      return;
    }
    compuesta_hay_ = false;
    compuesta_caducada_ = compuesta_.direccion;
    compuesta_pixeles_ahorrados_ += uint64_t(compuesta_.copia.extent.width) * compuesta_.copia.extent.height;
    ++compuesta_tiradas_origen_;
  }

  // Before each draw with a deferred copy or a stale texture: if the draw samples it, the deferred copy is
  // recorded first (exact) and the stale one trips the guard; if it draws into its source, the deferred
  // copy is dropped.
  void CompuestaAntesDeDibujar(const PeticionDibujo& p) {
    if (!p.ps || !p.registros) {
      return;  // without a PS, no color is sampled or written
    }
    const uint32_t* r = p.registros;
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.registro >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.registro) * 6;
      if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture) ||
          ((f[5] >> 9) & 0x3) != uint32_t(xenos::DataDimension::k2DOrStacked)) {
        continue;
      }
      const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
      if (compuesta_hay_ && base == compuesta_.direccion) {
        GrabarCompuesta();
        ++compuesta_grabadas_lectura_;
      } else if (compuesta_caducada_ != 0 && base == compuesta_caducada_) {
        ++compuesta_lecturas_tardias_;
        CompuestaDiferencia(base, "un dibujo la muestrea despues de tirar su copia (ve la escena de antes de "
                                  "componer)");
        compuesta_caducada_ = 0;
      }
    }
    if (compuesta_hay_ && DibujoEscribeEn(r, compuesta_.origen)) {
      CompuestaAntesDeEscribirOrigen();
    }
  }

  // Before each resolve to a texture (from Copiar, before copying anything): another write to the deferred
  // address. If it covers it entirely, the deferred copy is unnecessary (nobody has read it: exact);
  // otherwise it is recorded first.
  void CompuestaAntesDeEscribirTextura(uint32_t direccion, bool entera) {
    if (compuesta_hay_ && compuesta_.direccion == direccion) {
      if (entera) {
        compuesta_hay_ = false;
        compuesta_pixeles_ahorrados_ += uint64_t(compuesta_.copia.extent.width) * compuesta_.copia.extent.height;
        ++compuesta_sustituidas_;
      } else {
        GrabarCompuesta();
        ++compuesta_grabadas_otras_;
      }
    }
    if (entera && compuesta_caducada_ == direccion) {
      compuesta_caducada_ = 0;  // whole new content: it no longer misses the dropped copy
    }
  }

  // An image that is destroyed (Destruir) cannot stay in the deferred copy.
  void CompuestaAlDestruir(const Imagen& imagen) {
    if (!compuesta_hay_ || imagen.imagen == VK_NULL_HANDLE) {
      return;
    }
    if (imagen.imagen == compuesta_.origen_vk) {
      compuesta_hay_ = false;  // without its source there is no copy to make: the texture becomes stale
      compuesta_caducada_ = compuesta_.direccion;
    } else if (imagen.imagen == compuesta_.textura_vk) {
      compuesta_hay_ = false;  // the texture is rebuilt: its content is lost just as before
    }
  }

  void CompuestaDiferencia(uint32_t direccion, const char* motivo) {
    ++compuesta_diferencias_;
    if (compuesta_apagada_) {
      return;
    }
    compuesta_apagada_ = true;
    REXLOG_ERROR("[nativo] C2 compuesta perezosa: DIFERENCIA en {:08X}: {}. Apagada para el resto de la sesion: se "
                 "copia siempre, como antes de la 184",
                 direccion, motivo);
  }

  // With the diagnostic's summary (every nfsmw_nativo_diag_lectores_s seconds).
  void CompuestaInforme() {
    if (!compuesta_aplazadas_ && !compuesta_diferencias_) {
      return;
    }
    const uint64_t fotogramas = presentados_ - compuesta_presentados_previos_;
    const double mp = double(compuesta_pixeles_ahorrados_ - compuesta_pixeles_previos_) / 1e6;
    const double por_fotograma = fotogramas ? mp / double(fotogramas) : 0.0;
    NFSMW_INFORME_ANILLO("[nativo] C2 compuesta perezosa (build 184): {} copias aplazadas desde el arranque; {} "
                         "grabadas antes de un dibujo que la lee (exacto) y {} antes de otra escritura; {} tiradas al "
                         "escribir su origen y {} al taparla otra entera: {:.2f} Mpixeles por fotograma sin copiar "
                         "(~{:.2f} ms reales); lecturas tardias {}{}",
                         compuesta_aplazadas_, compuesta_grabadas_lectura_, compuesta_grabadas_otras_,
                         compuesta_tiradas_origen_, compuesta_sustituidas_, por_fotograma, por_fotograma * 0.60,
                         compuesta_lecturas_tardias_,
                         compuesta_apagada_ ? " *** APAGADA POR LA GUARDIA ***" : " (0 = la imagen es la misma)");
    compuesta_pixeles_previos_ = compuesta_pixeles_ahorrados_;
    compuesta_presentados_previos_ = presentados_;
  }

  // nfsmw_nativo_diag_lectores_s (see the cvar's comment). PM4 ring thread only.
  // On each Swap, with the front buffer it draws: opens the window (two whole frames, Swap to Swap) or closes
  // it.
  void DiagLectoresAlPresentar(uint32_t frontal) {
    if (diag_ventana_) {
      if (resueltas_.count(frontal)) {
        DiagLectura(frontal, kLectorSwap, 0, 0);
      }
      if (++diag_fotograma_ >= 2) {
        DiagLectoresInforme();
        diag_ventana_ = false;
        diag_escrituras_.clear();
      }
      return;
    }
    const int32_t cada = REXCVAR_GET(nfsmw_nativo_diag_lectores_s);
    if (cada <= 0) {
      diag_vigiladas_.clear();  // turned off at run time: nothing is watched either
      return;
    }
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - diag_lectores_ultima_ < std::chrono::seconds(cada)) {
      return;
    }
    diag_lectores_ultima_ = ahora;
    diag_ventana_ = true;
    diag_fotograma_ = 0;
    diag_escrituras_.clear();
  }

  // A logical write to the resolved texture at `direccion`: the resolve the game requests (color or depth).
  void DiagEscritura(uint32_t direccion, uint32_t origen, uint32_t pitch, int32_t x0, int32_t y0,
                     uint32_t pedido_ancho, uint32_t pedido_alto, uint32_t ancho, uint32_t alto, uint64_t dibujos,
                     bool profundidad) {
    const uint32_t de = (origen & 0xFFF) | (pitch << 12);
    for (VigiladaDiag& v : diag_vigiladas_) {
      if (v.direccion == direccion) {
        v.pendiente = de == v.origen;  // from here on, whoever reads it needs the watched copy
        v.origen_escrito = false;
        v.escrituras += v.pendiente ? 1 : 0;
      }
    }
    if (!diag_ventana_) {
      return;
    }
    std::vector<EscrituraDiag>& lista = diag_escrituras_[direccion];
    if (lista.size() >= kDiagMaxEscrituras) {
      ++diag_escrituras_perdidas_;
      return;
    }
    EscrituraDiag e;
    e.fotograma = int32_t(diag_fotograma_);
    e.orden = 1;
    for (const EscrituraDiag& otra : lista) {
      e.orden += otra.fotograma == e.fotograma ? 1 : 0;
    }
    e.origen = de;
    e.pedido_ancho = pedido_ancho;
    e.pedido_alto = pedido_alto;
    e.ancho = ancho;
    e.alto = alto;
    e.dibujos = dibujos;
    e.profundidad = profundidad;
    e.entera = x0 == 0 && y0 == 0 && pedido_ancho >= ancho && pedido_alto >= alto;
    lista.push_back(std::move(e));
  }

  // A read of `direccion` in the window: it goes to its last write (or to the one before the window).
  void DiagLectura(uint32_t direccion, uint32_t ps, uint32_t registro, uint32_t destino) {
    std::vector<EscrituraDiag>& lista = diag_escrituras_[direccion];
    if (lista.empty()) {
      lista.emplace_back();  // what it already held before the window (frame -1)
    }
    EscrituraDiag& e = lista.back();
    for (LectorDiag& l : e.lectores) {
      if (l.ps == ps && l.registro == registro && l.destino == destino) {
        ++l.veces;
        return;
      }
    }
    if (e.lectores.size() >= kDiagMaxLectores) {
      ++e.otros;
      return;
    }
    e.lectores.push_back(LectorDiag{ps, registro, destino, 1});
  }

  // The resolved textures the PS of this draw samples, read from its fetch constants the way DibujosVulkan
  // reads them (sampler registers 0-15; 2D by their address and cubemaps by their 6 faces), bypassing the
  // caches.
  void DiagLecturasDibujo(const PeticionDibujo& p) {
    if (!p.ps || !p.registros) {
      return;
    }
    const uint32_t* r = p.registros;
    const uint32_t destino = (r[kDiagRegColorInfo] & 0xFFF) | ((r[kDiagRegSurfaceInfo] & 0x3FFF) << 12);
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.registro >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.registro) * 6;
      if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
        continue;
      }
      const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
      const uint32_t dimension = (f[5] >> 9) & 0x3;
      if (dimension == uint32_t(xenos::DataDimension::k2DOrStacked)) {
        if (resueltas_.count(base)) {
          DiagLectura(base, p.ps->numero, s.registro, destino);
          VigilarLectura(base, p.ps->numero);
        }
      } else if (dimension == uint32_t(xenos::DataDimension::kCube)) {
        // The faces of a resolved cubemap are consecutive, as DibujosVulkan looks for them (8888: 4 bytes per
        // texel, rows and columns aligned to 32 and each face to 4 KB).
        const uint32_t alto = ((f[2] >> 13) & 0x1FFF) + 1;
        const uint32_t pitch = std::max<uint32_t>(((f[0] >> 22) & 0x1FF) << 5, 1);
        const uint64_t zancada =
            (uint64_t((pitch + 31) & ~uint32_t(31)) * 4 * ((alto + 31) & ~uint32_t(31)) + 4095) & ~uint64_t(4095);
        for (uint32_t c = 0; c < 6; ++c) {
          const uint32_t cara = uint32_t((uint64_t(base) + c * zancada) & 0x1FFFFFFF);
          if (resueltas_.count(cara)) {
            DiagLectura(cara, p.ps->numero, s.registro, destino);
          }
        }
      }
    }
    DiagOrigenEscritoPorDibujo(r);  // after its reads: the draw reads its textures before writing
  }

  // Outside the window, with something watched: only the 2D address of each sampler is compared with the
  // watched ones.
  void DiagVigilar(const PeticionDibujo& p) {
    if (!p.ps || !p.registros) {
      return;
    }
    const uint32_t* r = p.registros;
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.registro >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.registro) * 6;
      if ((f[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          ((f[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked)) {
        VigilarLectura(((f[1] >> 12) << 12) & 0x1FFFFFFF, p.ps->numero);
      }
    }
    DiagOrigenEscritoPorDibujo(r);
  }

  // A read of a watched address after its watched write. Before the source is written again, a deferred
  // copy would cover it (it would be recorded right before that draw); afterwards, dropping the copy when
  // the source is written would not be exact.
  void VigilarLectura(uint32_t direccion, uint32_t ps) {
    for (VigiladaDiag& v : diag_vigiladas_) {
      if (v.direccion != direccion || !v.pendiente) {
        continue;
      }
      if (!v.origen_escrito) {
        if (v.antes++ == 0) {
          v.ps_antes = ps;
          NFSMW_INFORME_ANILLO("[nativo] C2 lectores (build 184): {:08X}, escrita desde {:03X}/{}, la lee el PS n{} "
                               "ANTES de volver a escribir su origen (tras {} escrituras vigiladas): una copia "
                               "aplazada la cubre",
                               direccion, v.origen & 0xFFF, v.origen >> 12, ps, v.escrituras);
        }
      } else if (v.tardias++ == 0) {
        v.ps_tardio = ps;
        NFSMW_INFORME_ANILLO("[nativo] C2 lectores (build 184): {:08X}, escrita desde {:03X}/{}, la lee el PS n{} "
                             "DESPUES de escribir su origen (tras {} escrituras vigiladas): tirar esa copia al "
                             "escribir el origen NO seria exacto",
                             direccion, v.origen & 0xFFF, v.origen >> 12, ps, v.escrituras);
      }
    }
  }

  // A draw (or a clear) on render target `destino` (EDRAM base | pitch << 12): if it is the source of a
  // pending watched write, its content is no longer in the source.
  void DiagOrigenEscrito(uint32_t destino) {
    for (VigiladaDiag& v : diag_vigiladas_) {
      if (v.pendiente && !v.origen_escrito && v.origen == destino) {
        v.origen_escrito = true;
      }
    }
  }

  // Whether a draw writes to `destino`: its four color render targets with their write mask. Over-reporting
  // is harmless (it is treated as written earlier); under-reporting is not.
  static bool DibujoEscribeEn(const uint32_t* r, uint32_t destino) {
    const uint32_t mascara = r[kDiagRegColorMask];
    const uint32_t pitch = (r[kDiagRegSurfaceInfo] & 0x3FFF) << 12;
    for (uint32_t i = 0; i < 4; ++i) {
      if (((mascara >> (4 * i)) & 0xF) != 0 && ((r[kDiagRegsColorInfo[i]] & 0xFFF) | pitch) == destino) {
        return true;
      }
    }
    return false;
  }

  void DiagOrigenEscritoPorDibujo(const uint32_t* r) {
    for (VigiladaDiag& v : diag_vigiladas_) {
      if (v.pendiente && !v.origen_escrito && DibujoEscribeEn(r, v.origen)) {
        v.origen_escrito = true;
      }
    }
  }

  // When the window closes: one line per address (if its pattern has changed) and the summary line.
  void DiagLectoresInforme() {
    ++diag_ventanas_;
    std::vector<uint32_t> direcciones;
    direcciones.reserve(diag_escrituras_.size());
    for (const auto& [direccion, lista] : diag_escrituras_) {
      direcciones.push_back(direccion);
    }
    std::sort(direcciones.begin(), direcciones.end());
    const uint64_t texels_cpu = uint64_t(std::max<int32_t>(REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels), 0));
    std::string sobran;
    uint32_t escrituras = 0, nuevas = 0, iguales = 0;
    for (const uint32_t direccion : direcciones) {
      const std::vector<EscrituraDiag>& lista = diag_escrituras_[direccion];
      const auto r = resueltas_.find(direccion);
      const uint32_t ancho = r != resueltas_.end() ? r->second.imagen.ancho : 0;
      const uint32_t alto = r != resueltas_.end() ? r->second.imagen.alto : 0;
      // The small ones are also copied to guest memory and read by the CPU (the exposure): they are never
      // unnecessary.
      const bool cpu = ancho && uint64_t(ancho) * alto <= texels_cpu;
      std::string linea;
      uint64_t firma = direccion;
      for (size_t i = 0; i < lista.size(); ++i) {
        const EscrituraDiag& e = lista[i];
        if (e.fotograma < 0) {
          linea += " [antes]";
        } else {
          ++escrituras;
          const bool blit = e.pedido_ancho * 4 >= e.ancho * 5 || e.pedido_alto * 4 >= e.alto * 5;
          linea += fmt::format(" [{}.{}] {} {}x{} desde {:03X}/{}{} ({} dibujos antes)", e.fotograma, e.orden,
                               e.profundidad ? "profundidad" : (blit ? "blit" : "1:1"), e.pedido_ancho,
                               e.pedido_alto, e.origen & 0xFFF, e.origen >> 12, e.entera ? "" : ", parcial",
                               e.dibujos);
        }
        firma = (firma ^ (uint64_t(uint32_t(e.fotograma + 1)) << 48 | uint64_t(e.orden) << 40 | e.origen)) *
                0x100000001B3ull;
        if (e.lectores.empty()) {
          const bool siguiente = i + 1 < lista.size();
          if (cpu) {
            linea += " -> ningun dibujo (la lee la CPU)";
          } else if (e.fotograma >= 0 && siguiente && lista[i + 1].entera) {
            linea += " -> NADIE antes de la siguiente, que la tapa entera: SOBRA";
            sobran += fmt::format(" {:08X} [{}.{}] desde {:03X}/{}", direccion, e.fotograma, e.orden,
                                  e.origen & 0xFFF, e.origen >> 12);
            firma ^= 0x5A5Au;
            AnadirVigilada(direccion, e.origen);
          } else {
            linea += siguiente ? " -> nadie antes de la siguiente (que no la tapa entera)"
                               : " -> nadie hasta el final de la ventana";
          }
        } else {
          linea += " ->";
          for (const LectorDiag& l : e.lectores) {
            if (l.ps == kLectorSwap) {
              linea += fmt::format(" Swap x{}", l.veces);
            } else {
              linea += fmt::format(" PS n{} s{} x{} en {:03X}/{}", l.ps, l.registro, l.veces, l.destino & 0xFFF,
                                   l.destino >> 12);
            }
            firma = (firma ^ (uint64_t(l.ps) << 32 | uint64_t(l.registro) << 26 | (l.destino & 0x3FFFFFF))) *
                    0x100000001B3ull;
          }
          if (e.otros) {
            linea += fmt::format(" (y {} lectores mas)", e.otros);
          }
        }
        linea += ";";
      }
      uint64_t& previa = diag_firmas_[direccion];
      if (previa == firma && diag_ventanas_ > 3) {
        ++iguales;  // the same pattern as the last time it was written: not repeated
        continue;
      }
      previa = firma;
      ++nuevas;
      NFSMW_INFORME_ANILLO("[nativo] C2 lectores (build 184, ventana {}): {:08X} {}x{}:{}", diag_ventanas_, direccion,
                           ancho, alto, linea);
    }
    std::string vigiladas;
    for (const VigiladaDiag& v : diag_vigiladas_) {
      vigiladas += fmt::format(" {:08X} desde {:03X}/{}: {} escrituras; {} lecturas antes de volver a escribir el "
                               "origen{} y {} despues{};",
                               v.direccion, v.origen & 0xFFF, v.origen >> 12, v.escrituras, v.antes,
                               v.antes ? fmt::format(" (la primera, PS n{})", v.ps_antes) : std::string(),
                               v.tardias,
                               v.tardias ? fmt::format(" (la primera, PS n{}): tirarla NO seria exacto", v.ps_tardio)
                                         : std::string(v.escrituras ? " (tirarla al escribir el origen seria exacto)"
                                                                    : ""));
    }
    NFSMW_INFORME_ANILLO("[nativo] C2 lectores de las resueltas (build 184, ventana {}, 2 fotogramas cada {} s): {} "
                         "direcciones y {} escrituras; {} lineas escritas y {} iguales a la ultima escrita (no se "
                         "repiten){}; escrituras que nadie lee antes de otra que las tapa entera:{} | vigiladas en "
                         "todos los fotogramas:{}",
                         diag_ventanas_, REXCVAR_GET(nfsmw_nativo_diag_lectores_s), direcciones.size(), escrituras,
                         nuevas, iguales,
                         diag_escrituras_perdidas_
                             ? fmt::format(" ({} escrituras sin apuntar por el tope de {} por direccion)",
                                           diag_escrituras_perdidas_, kDiagMaxEscrituras)
                             : std::string(),
                         sobran.empty() ? std::string(" ninguna") : sobran,
                         vigiladas.empty() ? std::string(" ninguna") : vigiladas);
    CompuestaInforme();  // nfsmw_nativo_compuesta_perezosa
  }

  // A write that is unnecessary in a window becomes watched in every frame (at most kDiagMaxVigiladas).
  void AnadirVigilada(uint32_t direccion, uint32_t origen) {
    for (const VigiladaDiag& v : diag_vigiladas_) {
      if (v.direccion == direccion && v.origen == origen) {
        return;
      }
    }
    if (diag_vigiladas_.size() < kDiagMaxVigiladas) {
      VigiladaDiag v;
      v.direccion = direccion;
      v.origen = origen;
      diag_vigiladas_.push_back(v);
    }
  }

  // Per-draw diagnostic window, one whole frame every N seconds. It is decided
  // in the Swap because the upload buffer rotates several times per frame.
  void AbrirVentanaDiagnostico() {
    const int32_t cada = REXCVAR_GET(nfsmw_nativo_estadisticas_por_dibujo_s);
    if (cada <= 0) {
      ventana_diagnostico_ = false;
      return;
    }
    const auto ahora = std::chrono::steady_clock::now();
    if (ventana_diagnostico_) {
      ventana_diagnostico_ = false;  // that frame was already measured
      return;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(ahora - ultima_ventana_).count() >= cada) {
      ultima_ventana_ = ahora;
      ventana_diagnostico_ = true;
      ventana_leida_ = false;
    }
  }

  void Estadisticas(uint64_t& copias, uint64_t& borrados, uint64_t& presentados,
                    uint64_t& rechazos) const override {
    copias = copias_;
    borrados = borrados_;
    presentados = presentados_;
    rechazos = rechazos_;
  }

  // Depth copies (the only ones that force the depth tile to be stored).
  uint64_t CopiasProfundidad() const override { return copias_profundidad_; }
  uint64_t BorradosProfundidad() const override { return borrados_profundidad_; }

  void TiempoGpu(uint64_t& nanosegundos, uint64_t& trabajos) const override {
    nanosegundos = gpu_ns_;
    trabajos = gpu_trabajos_;
  }

  uint64_t MarcaGpuFinalNs() const override { return uint64_t(double(ultima_marca_gpu_) * periodo_marca_ns_); }
  uint64_t TrabajosMarcasPrecisas() const override { return gpu_trabajos_precisos_; }

  void EsperasGpu(uint64_t& veces, uint64_t& nanosegundos) const override {
    veces = esperas_gpu_;
    nanosegundos = ns_esperas_gpu_;
  }

  void DuracionTrabajosGpu(uint64_t& veces, uint64_t& nanosegundos) const override {
    veces = trabajos_gpu_;
    nanosegundos = ns_trabajo_gpu_;
  }

  void CosteGrabar(uint64_t coste[6]) const override {
    coste[0] = grabaciones_;
    coste[1] = ns_grabar_;
    coste[2] = ns_reiniciar_pools_;
    coste[3] = lecturas_escritas_;
    coste[4] = texels_escritos_;
    coste[5] = ns_escribir_lecturas_;
  }

  // C2 report: waits of the ring for the GPU in CompletarUna.
  uint64_t esperas_gpu_ = 0;
  uint64_t ns_esperas_gpu_ = 0;
  uint64_t trabajos_gpu_ = 0;     // work units timed with the wall clock
  uint64_t ns_trabajo_gpu_ = 0;   // and how long they took from submit to fence
  // C2 report: cost of Grabar and of the readbacks.
  uint64_t grabaciones_ = 0;
  uint64_t ns_grabar_ = 0;
  uint64_t ns_reiniciar_pools_ = 0;
  uint64_t lecturas_escritas_ = 0;
  uint64_t texels_escritos_ = 0;
  uint64_t ns_escribir_lecturas_ = 0;

  void TiempoGpuPorCategoria(
      std::array<uint64_t, kGpuCategorias>& nanosegundos) const override {
    nanosegundos = gpu_categorias_ns_;
  }

  void EstadisticasPipeline(std::array<uint64_t, kGpuCategorias>& fragmentos,
                            std::array<uint64_t, kGpuCategorias>& vertices,
                            std::array<uint64_t, kGpuCategorias>& primitivas) const override {
    fragmentos = fragmentos_categoria_;
    vertices = vertices_categoria_;
    primitivas = primitivas_categoria_;
  }

  void CopiasPorTamano(std::array<uint64_t, 4>& copias, std::array<uint64_t, 4>& pixeles) const override {
    copias = copias_cubeta_;
    pixeles = pixeles_cubeta_;
  }

  void EstadisticasPorShader(std::vector<uint64_t>& fragmentos, std::vector<uint64_t>& dibujos,
                             uint64_t& fotogramas) const override {
    fragmentos = fragmentos_por_shader_;
    dibujos = dibujos_por_shader_;
    fotogramas = fotogramas_diagnostico_;
  }

  void CostePresentar(uint64_t coste[12]) const override {
    const uint64_t valores[12] = {esperas_salida_,   ns_espera_salida_,  envios_trabajo_,  ns_candado_trabajo_,
                                  ns_submit_trabajo_, envios_salida_,     ns_candado_salida_, ns_submit_salida_,
                                  refrescos_,        ns_antes_llamada_, ns_en_llamada_,     ns_tras_llamada_};
    std::copy(std::begin(valores), std::end(valores), coste);
  }

  void IntervalosEntreSwaps(std::array<uint64_t, kCubetasSwap>& cubetas, uint64_t& solapes,
                            double& peor_ms) const override {
    cubetas = cubetas_swap_;
    peor_ms = peor_swap_ms_;
    peor_swap_ms_ = 0.0;  // reset on every report: what matters is the worst of this interval
    solapes = solapes_gpu_;
  }

  bool Dibujar(const PeticionDibujo& peticion) override {
    if (compuesta_hay_ || compuesta_caducada_ != 0) {
      CompuestaAntesDeDibujar(peticion);  // nfsmw_nativo_compuesta_perezosa
    }
    if (diag_ventana_) {
      DiagLecturasDibujo(peticion);  // nfsmw_nativo_diag_lectores_s
    } else if (!diag_vigiladas_.empty()) {
      DiagVigilar(peticion);
    }
    return dibujos_ && dibujos_->Dibujar(peticion);
  }

  EstadisticasDibujos EstadisticasDeDibujos() const override {
    return dibujos_ ? dibujos_->Estadisticas() : EstadisticasDibujos{};
  }

  void EsperarSubidas() override {
    if (dibujos_) {
      dibujos_->EsperarSubidas();
    }
  }

  size_t CopiasPendientes() const override {  // fence measurement only
    return dibujos_ ? dibujos_->CopiasPendientes() : 0;
  }

  // --- ContextoDestinos (piezas C3-C6) ---------------------------------------

  VkCommandBuffer ComandosTrabajo() override {
    return Grabar() ? comandos_trabajo_ : VK_NULL_HANDLE;
  }

  VkCommandBuffer ComandosSubida() override {
    if (!Grabar()) {
      return VK_NULL_HANDLE;
    }
    if (!grabando_subida_) {
      VkCommandBufferBeginInfo inicio{};
      inicio.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      inicio.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (dfn_.vkBeginCommandBuffer(comandos_subida_, &inicio) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
      }
      grabando_subida_ = true;
      if (REXCVAR_GET(nfsmw_nativo_sincronizacion_gpu)) {
        // Previous submissions may still sample these textures or draw the
        // reflection faces. Submission order alone does not make writes visible.
        VkMemoryBarrier barrera{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrera.srcAccessMask = kAccesosImagenes;
        barrera.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dfn_.vkCmdPipelineBarrier(comandos_subida_, kEtapasImagenes,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrera,
                                  0, nullptr, 0, nullptr);
      }
    }
    return comandos_subida_;
  }

  uint64_t GeneracionComandos() const override { return generacion_comandos_; }

  ImagenNativa* DestinoColor(uint32_t base, uint32_t formato, uint32_t pitch) override {
    Imagen* imagen = ObtenerDestino(base, formato, pitch);
    if (!imagen || !Grabar()) {
      return nullptr;
    }
    Preparar(*imagen);
    AntesDeEscribirColor(*imagen);  // nfsmw_nativo_frontal_perezoso
    // Mandatory since color is also swapped. Here a pass starts that draws on top; if the content went away
    // in a swap and nobody has cleared it, it has to be brought back. Without this, the first frame that
    // draws without clearing shows the previous frame.
    RestaurarContenido(*imagen);
    return imagen;
  }

  ImagenNativa* DestinoProfundidad(uint32_t base, uint32_t formato, uint32_t pitch) override {
    Imagen* imagen = ObtenerProfundidad(base, formato, pitch);
    if (!imagen || !Grabar()) {
      return nullptr;
    }
    Preparar(*imagen);
    AntesDeEscribirProfundidad(*imagen);  // nfsmw_nativo_profundidad_perezosa
    // A pass that draws on top starts here, so if the content went away in a swap and nobody has cleared it
    // since then, it has to be brought back.
    RestaurarContenido(*imagen);
    return imagen;
  }

  const ImagenNativa* TexturaResuelta(uint32_t direccion) override {
    const auto it = resueltas_.find(direccion);
    // Watchdog of nfsmw_nativo_restaurar_por_intercambio. This is the only door through which draws reach a
    // resolved texture, so if someone requests one that is lent out (its content was given back to the render
    // target and it has not been resolved again) the lending is not safe and the previous frame's image would
    // be sampled. It must stay at 0 in the log.
    if (!prestadas_.empty() && prestadas_.count(direccion)) {
      ++prestadas_leidas_;
    }
    if (it == resueltas_.end()) {
      return nullptr;
    }
    // And how many times each address is requested, for the per-render-target copy report. A resolved
    // texture that is copied every frame and never requested is a copy that is not needed. It is a ++ on the
    // entry that has already been looked up: it costs nothing even though this is called thousands of times
    // per frame.
    ++it->second.lecturas;
    if (direccion == nfsmw::reflejo_demanda::kDireccion) {
      nfsmw::reflejo_demanda::AnotarLectura();  // nfsmw_reflejo_bajo_demanda
    }
    // nfsmw_nativo_profundidad_perezosa. Who requests each depth texture; if its copy is deferred and this is
    // a real sampling, it is recorded now, before this draw.
    if (it->second.imagen.formato == formato_profundidad_) {
      AnotarLecturaProfundidad(direccion);
    } else if (!frontales_presentados_.empty() && frontales_presentados_.count(direccion)) {
      AnotarLecturaFrontal(direccion);  // nfsmw_nativo_frontal_perezoso
    }
    return it->second.imagen.preparada ? &it->second.imagen : nullptr;
  }

  // nfsmw_nativo_profundidad_perezosa. DibujosVulkan sets it around the textures of the final composition
  // when its depth sampling is dead (no blur).
  void LecturasDeProfundidadMuertas(bool muertas) override { lecturas_profundidad_muertas_ = muertas; }

  // nfsmw_nativo_sombra_minimo, what the draws ask (see the cvar's comment).
  // When a pass is opened on this depth: if it is the car pass of the current cycle, each draw is validated.
  bool PaseDeCochesSombra(const ImagenNativa* profundidad, bool solo_profundidad) override {
    if (!profundidad || profundidad != sm_destino_ciclo_ || sm_estado_ != kSmCoches) {
      return false;
    }
    if (!solo_profundidad) {
      SombraMinimoFalloCiclo("el pase de los coches tiene destino de color", 0, true);
    }
    return true;
  }

  // A draw of the car pass. `exacto`: it leaves in the depth the minimum of what was there and of its
  // fragments.
  void DibujoDeCochesSombra(bool exacto, uint32_t control_z) override {
    ++sm_dibujos_coches_;
    if (!exacto) {
      ++sm_dibujos_no_exactos_;
      SombraMinimoFalloCiclo("un dibujo de los coches no deja el minimo (el dato es su control de Z)", control_z, true);
    }
  }

  // The texture with cars that the draws must look at: whenever it holds the cars only, and otherwise while
  // watching or applying. 0 = none.
  uint32_t DireccionSombraCoches() const override {
    return (sm_virtual_ || (sm_encendido_ && sm_fase_ != kSmApagada)) ? sm_coches_ : 0;
  }

  // A draw samples the texture with cars. `capaz`: its pixel shader has tfetch2DSombraMin in that register
  // (`ps` is its number, for the log). Returns the partner to take the minimum with, or nullptr to sample
  // it as is.
  const ImagenNativa* CompaneraSombraCoches(bool capaz, uint32_t ps) override {
    if (sm_virtual_) {
      // It holds the cars only: without the minimum, the world would miss its shadows.
      const auto it = resueltas_.find(sm_mundo_);
      const bool mundo = it != resueltas_.end() && it->second.imagen.preparada;
      if (!sm_virtual_valida_ || !capaz || !mundo) {
        ++sm_lecturas_incapaces_;
        SombraMinimoDiferencia(!sm_virtual_valida_ ? "se muestrea la textura de los coches solos y la del mundo ya es de "
                                                     "otro ciclo (el dato es el pixel shader)"
                               : !capaz ? "un pixel shader sin tfetch2DSombraMin muestrea la textura de los coches solos "
                                          "(el dato es su numero)"
                                        : "no esta la textura del mundo (el dato es el pixel shader)",
                               ps);
        return nullptr;
      }
      ++sm_lecturas_minimo_;
      return &it->second.imagen;
    }
    // It is the usual one (world and cars): who reads it and when is watched, so the minimum can be applied
    // without surprises.
    if (sm_estado_ != kSmLibre) {
      ++sm_lecturas_a_destiempo_;
      SombraMinimoNoApto("la textura de los coches se muestrea entre las dos resoluciones (el dato es el pixel shader)",
                         ps);
    }
    if (!capaz) {
      ++sm_lecturas_incapaces_;
      SombraMinimoNoApto("un pixel shader sin tfetch2DSombraMin muestrea la textura de los coches: biblioteca sin el "
                         "minimo? (el dato es su numero)",
                         ps);
      return nullptr;
    }
    const auto it = resueltas_.find(sm_coches_);
    if (!sm_encendido_ || sm_fase_ == kSmApagada || it == resueltas_.end() || !it->second.imagen.preparada) {
      ++sm_lecturas_normales_;
      return nullptr;
    }
    // Minimum with itself (the same texel): the pipeline with the bit already exists when applying starts.
    ++sm_lecturas_si_misma_;
    return &it->second.imagen;
  }

  // nfsmw_nativo_diag_borrados. What is known about the clears of a render target.
  struct UsoBorrado {
    uint32_t base = 0;             // EDRAM base, guest format and pitch (for the report)
    uint32_t formato = 0;
    uint32_t pitch = 0;
    bool profundidad = false;
    bool en_pase = false;          // cleared by opening a pass (depth without TRANSFER_DST: ZCULL)
    bool abierto = false;          // there is a clear whose use is being measured
    uint32_t usado_ancho = 0;      // since the last clear: the largest x1 and y1 used
    uint32_t usado_alto = 0;
    uint32_t max_ancho = 0;        // the same since startup (only grows)
    uint32_t max_alto = 0;
    uint64_t ciclos = 0;           // clears with their use already measured, since startup
    uint64_t borrados = 0;         // in the report interval
    uint64_t pixeles_borrados = 0;
    uint64_t pixeles_usados = 0;   // of the cycles closed in the interval
    // nfsmw_nativo_borrar_area_util. The bottom band the last clear left uncleared.
    bool banda = false;
    uint32_t banda_desde = 0;
    VkImage banda_imagen = VK_NULL_HANDLE;
    VkClearColorValue banda_color{};
    uint32_t bandas_completadas = 0;
    bool area_util_apagada = false;  // its useful area is not stable: cleared entirely
  };

  // What is drawn in a pass on that render target (its renderArea). Called by DibujosVulkan in EmpezarPase.
  void AnotarAreaDePase(const ImagenNativa* imagen, uint32_t ancho, uint32_t alto) override {
    if (imagen) {
      AnotarUsoBorrado(*imagen, ancho, alto);
    }
  }

  // A whole clear of that image. Closes the previous cycle (what has been used since the previous clear) and
  // opens another.
  void AnotarBorradoDiag(const Imagen& imagen, uint32_t base, uint32_t formato, uint32_t pitch, bool profundidad,
                         bool en_pase) {
    if (!diag_borrados_) {
      return;
    }
    UsoBorrado& u = uso_borrados_[&imagen];
    if (u.abierto) {
      u.pixeles_usados += uint64_t(u.usado_ancho) * u.usado_alto;
      ++u.ciclos;
    }
    u.base = base;
    u.formato = formato;
    u.pitch = pitch;
    u.profundidad = profundidad;
    u.en_pase = en_pase;
    ++u.borrados;
    u.pixeles_borrados += uint64_t(imagen.ancho) * imagen.alto;
    u.usado_ancho = 0;
    u.usado_alto = 0;
    u.abierto = true;
  }

  // The rectangle from 0 to width and from 0 to height of that image is used (a pass, a resolve, a restore
  // or a swap). It only counts on render targets that have been cleared at least once.
  void AnotarUsoBorrado(const Imagen& imagen, uint32_t ancho, uint32_t alto) {
    // nfsmw_nativo_borrar_area_util. While any band remains uncleared it is checked even if the diagnostic
    // was turned off at run time: a pending band is always completed before it is used.
    if (uso_borrados_.empty() || (!diag_borrados_ && !bandas_pendientes_)) {
      return;
    }
    const auto it = uso_borrados_.find(&imagen);
    if (it == uso_borrados_.end()) {
      return;
    }
    UsoBorrado& u = it->second;
    ancho = std::min(ancho, imagen.ancho);
    alto = std::min(alto, imagen.alto);
    // nfsmw_nativo_borrar_area_util. If this use reaches the band the last clear left uncleared (or its
    // first row, which a linear blit can read), it is cleared now, before the use: the result is the same
    // as if it had been cleared entirely.
    if (u.banda && (u.banda_imagen != imagen.imagen || alto >= u.banda_desde)) {
      CompletarBanda(imagen, u);
    }
    if (!diag_borrados_) {
      return;  // it was only checked because of the band
    }
    u.usado_ancho = std::max(u.usado_ancho, ancho);
    u.usado_alto = std::max(u.usado_alto, alto);
    u.max_ancho = std::max(u.max_ancho, ancho);
    u.max_alto = std::max(u.max_alto, alto);
  }

  // nfsmw_nativo_borrar_area_util. Clears only the rows that render target uses, if they are already known.
  // Returns false if it has to be cleared entirely, as always.
  bool BorrarColorAreaUtil(Imagen& destino, const VkClearColorValue& color) {
    if (!borrar_area_util_ || !diag_borrados_ || !dibujos_ || destino.formato != kFormatoColor) {
      return false;
    }
    const auto it = uso_borrados_.find(&destino);
    if (it == uso_borrados_.end()) {
      return false;
    }
    UsoBorrado& u = it->second;
    if (u.area_util_apagada || u.ciclos < kAreaUtilCiclos || u.max_alto == 0) {
      return false;
    }
    // Up to the multiple of 64 that leaves at least one row of margin below what is used: a linear blit (the
    // resolve that shrinks the scene) can read the row below its rectangle, and that row must be cleared.
    const uint32_t alto = std::min(destino.alto, (u.max_alto + 64u) & ~63u);
    if (alto >= destino.alto) {
      return false;
    }
    if (!dibujos_->BorrarColorEnPase(comandos_trabajo_, destino, color, VkRect2D{{0, 0}, {destino.ancho, alto}})) {
      return false;
    }
    if (!u.banda) {
      ++bandas_pendientes_;
    }
    u.banda = true;
    u.banda_desde = alto;
    u.banda_imagen = destino.imagen;
    u.banda_color = color;
    ++area_util_borrados_;
    area_util_pixeles_ += uint64_t(destino.ancho) * (destino.alto - alto);
    return true;
  }

  // The render target has been cleared entirely: no band is left.
  void QuitarBanda(const Imagen& destino) {
    if (uso_borrados_.empty()) {
      return;
    }
    const auto it = uso_borrados_.find(&destino);
    if (it != uso_borrados_.end() && it->second.banda) {
      it->second.banda = false;
      if (bandas_pendientes_) {
        --bandas_pendientes_;
      }
    }
  }

  // Clears the band that was left uncleared, before a use that reaches it (outside a pass: called by Copiar,
  // RestaurarContenido, IntercambiarConResuelta and EmpezarPase before opening its own).
  void CompletarBanda(const Imagen& imagen, UsoBorrado& u) {
    u.banda = false;
    if (bandas_pendientes_) {
      --bandas_pendientes_;
    }
    ++u.bandas_completadas;
    ++area_util_completadas_;
    if (u.bandas_completadas >= 3) {
      u.area_util_apagada = true;  // its useful area is not stable: from now on it is cleared entirely
    }
    const bool misma_imagen = u.banda_imagen == imagen.imagen;
    const uint32_t alto_banda = imagen.alto > u.banda_desde ? imagen.alto - u.banda_desde : 0;
    if (misma_imagen && (!alto_banda || (dibujos_ && Grabar() &&
                                         dibujos_->BorrarColorEnPase(
                                             comandos_trabajo_, imagen, u.banda_color,
                                             VkRect2D{{0, int32_t(u.banda_desde)}, {imagen.ancho, alto_banda}})))) {
      return;
    }
    // The image changed without going through a clear (should not happen) or the band could not be cleared.
    ++area_util_fallos_;
    if (!area_util_global_apagada_) {
      area_util_global_apagada_ = true;
      borrar_area_util_ = false;
      REXLOG_ERROR("[nativo] C2 borrar area util: DIFERENCIA, no se pudo completar la banda de {}x{} desde la fila {} "
                   "({}). Apagado para el resto de la sesion: se borra la imagen entera",
                   imagen.ancho, imagen.alto, u.banda_desde, misma_imagen ? "fallo el pase" : "la imagen cambio");
    }
  }

  // Every 20 s, per render target, what was cleared against what was used (C2 borrados por destino).
  // Clears cost ~0.075 ms real per Mpixel (measured, see "C2 borrados saltados").
  void InformeBorrados() {
    diag_borrados_ = REXCVAR_GET(nfsmw_nativo_diag_borrados);
    borrar_area_util_ = REXCVAR_GET(nfsmw_nativo_borrar_area_util) && !area_util_global_apagada_;
    const auto ahora = std::chrono::steady_clock::now();
    if (informe_borrados_ == std::chrono::steady_clock::time_point{}) {
      informe_borrados_ = ahora;
      presentados_informe_borrados_ = presentados_;
      return;
    }
    if (ahora - informe_borrados_ < std::chrono::seconds(20)) {
      return;
    }
    informe_borrados_ = ahora;
    const uint64_t fotogramas = presentados_ - presentados_informe_borrados_;
    presentados_informe_borrados_ = presentados_;
    if (diag_borrados_ && fotogramas > 0 && !uso_borrados_.empty()) {
      struct Fila {
        const UsoBorrado* uso;
        uint32_t ancho;
        uint32_t alto;
        double borrado;  // Mpixels per frame
        double usado;
      };
      std::vector<Fila> filas;
      double total_borrado = 0.0, total_usado = 0.0;
      for (const auto& [imagen, u] : uso_borrados_) {
        if (!u.borrados) {
          continue;
        }
        const double borrado = double(u.pixeles_borrados) / 1e6 / double(fotogramas);
        // Each clear closes the previous one's cycle: in steady state, as many closed cycles as clears.
        const double usado = std::min(borrado, double(u.pixeles_usados) / 1e6 / double(fotogramas));
        filas.push_back({&u, imagen->ancho, imagen->alto, borrado, usado});
        total_borrado += borrado;
        total_usado += usado;
      }
      std::sort(filas.begin(), filas.end(),
                [](const Fila& a, const Fila& b) { return a.borrado - a.usado > b.borrado - b.usado; });
      std::string lista;
      for (const Fila& fila : filas) {
        const UsoBorrado& u = *fila.uso;
        lista += fmt::format(" | {:03X}/{} {} {}x{}{}: {:.2f} por fotograma, {:.2f} Mpixeles borrados, {:.2f} usados "
                             "(hasta {}x{})",
                             u.base, u.pitch, u.profundidad ? "prof" : "color", fila.ancho, fila.alto,
                             u.en_pase ? " (por pase, ZCULL)" : "", double(u.borrados) / double(fotogramas),
                             fila.borrado, fila.usado, u.max_ancho, u.max_alto);
      }
      NFSMW_INFORME_ANILLO("[nativo] C2 borrados por destino (build 184, {} fotogramas): {:.2f} Mpixeles borrados y {:.2f} "
                           "usados por fotograma (sobran {:.2f}: ~{:.2f} ms reales){}",
                           fotogramas, total_borrado, total_usado, total_borrado - total_usado,
                           (total_borrado - total_usado) * 0.075, lista);
    }
    if (area_util_borrados_ || area_util_fallos_) {  // nfsmw_nativo_borrar_area_util
      const double mp =
          double(area_util_pixeles_ - area_util_pixeles_previos_) / 1e6 / double(std::max<uint64_t>(fotogramas, 1));
      NFSMW_INFORME_ANILLO("[nativo] C2 borrar area util (build 184): {} borrados de color recortados desde el arranque; "
                           "{:.2f} Mpixeles por fotograma sin borrar (~{:.2f} ms reales); {} bandas completadas antes de "
                           "un uso; {} fallos{}",
                           area_util_borrados_, mp, mp * 0.075, area_util_completadas_, area_util_fallos_,
                           area_util_global_apagada_ ? " *** APAGADO POR LA GUARDIA ***" : " (0 = la imagen es la misma)");
      area_util_pixeles_previos_ = area_util_pixeles_;
    }
    for (auto& [imagen, u] : uso_borrados_) {
      u.borrados = 0;
      u.pixeles_borrados = 0;
      u.pixeles_usados = 0;
    }
  }

  // A request for a resolved depth texture (see nfsmw_nativo_profundidad_perezosa).
  void AnotarLecturaProfundidad(uint32_t direccion) {
    if (lecturas_profundidad_muertas_) {
      ultima_lectura_muerta_[direccion] = presentados_;
      if (!pendientes_.empty()) {
        const auto p = pendientes_.find(direccion);
        if (p != pendientes_.end()) {
          p->second.leida_muerta = true;
        }
      }
      return;
    }
    ultima_lectura_viva_[direccion] = presentados_;
    if (!pendientes_.empty() && pendientes_.count(direccion)) {
      GrabarCopiaPendiente(direccion);
      ++perezosa_copiadas_lectura_;
    }
    if (!caducadas_.empty() && caducadas_.erase(direccion)) {
      ++perezosa_lecturas_tardias_;
      if (!perezosa_apagada_) {
        perezosa_apagada_ = true;
        REXLOG_ERROR("[nativo] C2 profundidad perezosa: DIFERENCIA, un dibujo muestrea {:08X} despues de tirar su "
                     "copia (lectura tardia: lee una profundidad vieja). Apagada para el resto de la sesion: se "
                     "copia siempre",
                     direccion);
      }
    }
  }

  // Records the deferred copy of that address, outside a pass, and removes it from the list.
  void GrabarCopiaPendiente(uint32_t direccion) {
    const auto p = pendientes_.find(direccion);
    if (p == pendientes_.end()) {
      return;
    }
    const CopiaPendiente pendiente = p->second;
    pendientes_.erase(p);
    const auto r = resueltas_.find(direccion);
    if (r == resueltas_.end() || r->second.imagen.imagen != pendiente.destino_vk || !pendiente.origen ||
        pendiente.origen->imagen != pendiente.origen_vk || !copiar_imagen_ || !Grabar()) {
      caducadas_.insert(direccion);  // should not happen: every image change goes through the hooks first
      return;
    }
    if (dibujos_) {
      dibujos_->TerminarPase();  // it can arrive from a draw (TexturaResuelta): the copy goes outside the pass
    }
    MarcarGpu(kGpuCopias);
    copiar_imagen_(comandos_trabajo_, pendiente.origen_vk, VK_IMAGE_LAYOUT_GENERAL, pendiente.destino_vk,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &pendiente.copia);
    ++copias_;
    AnotarCopia(pendiente.copia.extent.width, pendiente.copia.extent.height);
    ResueltaEscrita(direccion, uint64_t(pendiente.copia.extent.width) * pendiente.copia.extent.height);
  }

  // Before any write to a depth render target. The deferred copies that come from it are recorded (same
  // frame, or the composition has not requested them yet) or dropped (nobody has sampled them).
  void AntesDeEscribirProfundidad(const Imagen& imagen) {
    if (pendientes_.empty() || imagen.imagen == VK_NULL_HANDLE) {
      return;
    }
    for (auto it = pendientes_.begin(); it != pendientes_.end();) {
      if (it->second.origen_vk != imagen.imagen) {
        ++it;
        continue;
      }
      const uint32_t direccion = it->first;
      // Dropped even within the same frame if the composite without blur already requested it (leida_muerta).
      // Keeping same-frame copies saved nothing: the game always rewrites the source within the same frame,
      // and the reader watch saw 0 reads after the rewrite. The guard catches a late read.
      if (!it->second.leida_muerta) {
        ++it;  // GrabarCopiaPendiente erases that entry: the iterator is already on the next one
        GrabarCopiaPendiente(direccion);
        ++perezosa_copiadas_escritura_;
      } else {
        perezosa_pixeles_ahorrados_ += uint64_t(it->second.copia.extent.width) * it->second.copia.extent.height;
        it = pendientes_.erase(it);
        caducadas_.insert(direccion);
        ++perezosa_tiradas_;
      }
    }
  }

  // Defers the copy if, lately, the only reader of that address is the composite without blur.
  bool AplazarCopiaProfundidad(uint32_t direccion, const Imagen& origen, const Resuelta& resuelta,
                               const VkImageCopy& copia) {
    if (perezosa_apagada_ || !REXCVAR_GET(nfsmw_nativo_profundidad_perezosa)) {
      return false;
    }
    const auto muerta = ultima_lectura_muerta_.find(direccion);
    if (muerta == ultima_lectura_muerta_.end() || presentados_ - muerta->second > kPerezosaFotogramas) {
      return false;  // the composite without blur has not requested it lately
    }
    const auto viva = ultima_lectura_viva_.find(direccion);
    if (viva != ultima_lectura_viva_.end() && presentados_ - viva->second <= kPerezosaFotogramas) {
      return false;  // something really samples it: copy as usual
    }
    CopiaPendiente& pendiente = pendientes_[direccion];
    pendiente.origen = &origen;
    pendiente.origen_vk = origen.imagen;
    pendiente.destino_vk = resuelta.imagen.imagen;
    pendiente.copia = copia;
    pendiente.fotograma = presentados_;
    pendiente.leida_muerta = false;
    ResueltaEscrita(direccion);  // new contents (deferred): no longer lent or stale
    ++perezosa_aplazadas_;
    return true;
  }

  // nfsmw_nativo_frontal_perezoso. Deferred copy to a front buffer, and the render target's spare images.
  struct FrontalPendiente {
    Imagen* destino = nullptr;           // source render target (destinos_ never erases: the pointer stays valid)
    VkImage origen_vk = VK_NULL_HANDLE;  // image holding the content: the target's own or a retained one
    int32_t retenida = -1;               // index in frontales_imagenes_ if it is no longer in the target
    VkImage textura_vk = VK_NULL_HANDLE;
    VkImageCopy copia{};
  };
  struct ImagenFrontal {
    Imagen imagen;              // same size, format and usage as the target: swapped with it
    uint32_t retenida_por = 0;  // address of the front buffer that retains it; 0 = free
  };

  // Is this address a front buffer read only by the Swap? A Swap presented it recently and no draw has
  // sampled it in that time.
  bool EsFrontalSoloDelSwap(uint32_t direccion) const {
    const auto presentado = frontales_presentados_.find(direccion);
    if (presentado == frontales_presentados_.end() || presentados_ - presentado->second > kFrontalFotogramas) {
      return false;
    }
    const auto leido = frontales_leidos_.find(direccion);
    return leido == frontales_leidos_.end() || presentados_ - leido->second > kFrontalFotogramas;
  }

  // The retained image of a deferred copy becomes free again.
  void LiberarRetenida(const FrontalPendiente& pendiente) {
    if (pendiente.retenida >= 0 && size_t(pendiente.retenida) < frontales_imagenes_.size()) {
      frontales_imagenes_[size_t(pendiente.retenida)].retenida_por = 0;
    }
  }

  // The image holding the deferred copy's content, if it is still the same one; otherwise nullptr.
  Imagen* OrigenFrontal(const FrontalPendiente& pendiente) {
    Imagen* origen = nullptr;
    if (pendiente.retenida >= 0) {
      if (size_t(pendiente.retenida) < frontales_imagenes_.size()) {
        origen = &frontales_imagenes_[size_t(pendiente.retenida)].imagen;
      }
    } else {
      origen = pendiente.destino;
    }
    return origen && origen->imagen == pendiente.origen_vk && origen->preparada ? origen : nullptr;
  }

  // Records the deferred copy to that front buffer (outside a pass) and removes it from the list.
  void GrabarCopiaFrontal(uint32_t direccion) {
    const auto p = frontales_pendientes_.find(direccion);
    if (p == frontales_pendientes_.end()) {
      return;
    }
    const FrontalPendiente pendiente = p->second;
    frontales_pendientes_.erase(p);
    const auto r = resueltas_.find(direccion);
    Imagen* const origen = OrigenFrontal(pendiente);
    if (r == resueltas_.end() || r->second.imagen.imagen != pendiente.textura_vk || !origen || !copiar_imagen_ ||
        !Grabar()) {
      LiberarRetenida(pendiente);
      frontales_caducados_.insert(direccion);  // should not happen: every image change goes through the hooks first
      return;
    }
    if (dibujos_) {
      dibujos_->TerminarPase();  // may come from a draw (TexturaResuelta): the copy goes outside the pass
    }
    MarcarGpu(kGpuCopias);
    copiar_imagen_(comandos_trabajo_, pendiente.origen_vk, VK_IMAGE_LAYOUT_GENERAL, pendiente.textura_vk,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &pendiente.copia);
    ++copias_;
    AnotarCopia(pendiente.copia.extent.width, pendiente.copia.extent.height);
    ResueltaEscrita(direccion, uint64_t(pendiente.copia.extent.width) * pendiente.copia.extent.height);
    frontales_caducados_.erase(direccion);
    LiberarRetenida(pendiente);
  }

  // Another resolve (or an image swap) reaches that texture. If it covers the whole texture, the deferred
  // copy is unnecessary and is dropped: nobody will see its content. Otherwise it is recorded first.
  void ResolverFrontalAnterior(uint32_t direccion, bool entera) {
    if (entera && !frontales_caducados_.empty()) {
      frontales_caducados_.erase(direccion);  // fully covered: it is no longer missing any copy
    }
    if (frontales_pendientes_.empty()) {
      return;
    }
    const auto p = frontales_pendientes_.find(direccion);
    if (p == frontales_pendientes_.end()) {
      return;
    }
    if (!entera) {
      GrabarCopiaFrontal(direccion);
      ++frontal_copiadas_escritura_;
      return;
    }
    frontal_pixeles_ahorrados_ += uint64_t(p->second.copia.extent.width) * p->second.copia.extent.height;
    LiberarRetenida(p->second);
    frontales_pendientes_.erase(p);
    ++frontal_sustituidas_;
  }

  // Defers the copy if it is 1:1, covers the whole texture from the target's corner, and that address is
  // read only by the Swap.
  bool AplazarCopiaFrontal(uint32_t direccion, Imagen& destino, const Resuelta& resuelta, const VkImageCopy& copia) {
    if (frontal_apagado_ || !REXCVAR_GET(nfsmw_nativo_frontal_perezoso) || REXCVAR_GET(nfsmw_nativo_diag_resueltas)) {
      return false;
    }
    if (copia.srcOffset.x != 0 || copia.srcOffset.y != 0 || copia.dstOffset.x != 0 || copia.dstOffset.y != 0 ||
        copia.extent.width != resuelta.imagen.ancho || copia.extent.height != resuelta.imagen.alto ||
        destino.formato != kFormatoColor || resuelta.imagen.formato != kFormatoColor) {
      return false;
    }
    // Small ones are read back for the guest (LeerResuelta): those are never deferred.
    const int32_t texels_lectura = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
    if (uint64_t(copia.extent.width) * copia.extent.height <= uint64_t(std::max<int32_t>(texels_lectura, 0))) {
      return false;
    }
    if (!EsFrontalSoloDelSwap(direccion)) {
      return false;
    }
    FrontalPendiente& pendiente = frontales_pendientes_[direccion];
    pendiente.destino = &destino;
    pendiente.origen_vk = destino.imagen;
    pendiente.retenida = -1;
    pendiente.textura_vk = resuelta.imagen.imagen;
    pendiente.copia = copia;
    ResueltaEscrita(direccion);  // new contents (deferred)
    frontales_caducados_.erase(direccion);
    ++frontal_aplazadas_;
    return true;
  }

  // Before writing to a color target in any way other than a full clear (a pass, a restore, an image
  // swap): deferred copies whose source is its image are recorded now (exact).
  void AntesDeEscribirColor(const Imagen& destino) {
    if (frontales_pendientes_.empty() || destino.imagen == VK_NULL_HANDLE) {
      return;
    }
    for (auto it = frontales_pendientes_.begin(); it != frontales_pendientes_.end();) {
      if (it->second.retenida >= 0 || it->second.origen_vk != destino.imagen) {
        ++it;
        continue;
      }
      const uint32_t direccion = it->first;
      ++it;  // GrabarCopiaFrontal erases that entry: the iterator is already on the next one
      GrabarCopiaFrontal(direccion);
      ++frontal_copiadas_escritura_;
    }
  }

  // Before a full clear of a color target. If its image is the source of a deferred front buffer, the
  // target takes a free spare image of the same size and the one holding the content is retained for the
  // front buffer: the clear does not need the old content, so nothing is copied. Without a spare (or with
  // two front buffers from the same image), the copy is recorded first, as usual.
  void RotarFrontalAntesDeBorrar(Imagen& destino) {
    if (frontales_pendientes_.empty() || destino.imagen == VK_NULL_HANDLE) {
      return;
    }
    uint32_t base = 0;
    uint32_t cuantas = 0;
    for (const auto& [direccion, pendiente] : frontales_pendientes_) {
      if (pendiente.retenida < 0 && pendiente.origen_vk == destino.imagen) {
        base = direccion;
        ++cuantas;
      }
    }
    if (cuantas == 0) {
      return;
    }
    int32_t libre = -1;
    if (cuantas == 1) {
      for (size_t i = 0; i < frontales_imagenes_.size(); ++i) {
        const Imagen& candidata = frontales_imagenes_[i].imagen;
        if (frontales_imagenes_[i].retenida_por == 0 && candidata.imagen != VK_NULL_HANDLE &&
            candidata.ancho == destino.ancho && candidata.alto == destino.alto &&
            candidata.formato == destino.formato) {
          libre = int32_t(i);
          break;
        }
      }
      if (libre < 0 && frontales_imagenes_.size() < kFrontalImagenesMax) {
        ImagenFrontal nueva;
        // Same usage flags as ObtenerDestino: the image becomes the render target.
        if (Crear(nueva.imagen, destino.ancho, destino.alto,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  destino.formato)) {
          Preparar(nueva.imagen);
          frontales_imagenes_.push_back(nueva);
          libre = int32_t(frontales_imagenes_.size()) - 1;
          REXLOG_INFO("[nativo] C2 frontal perezoso: imagen de repuesto {} de {}x{} para el destino del frontal",
                      frontales_imagenes_.size(), destino.ancho, destino.alto);
        }
      }
    }
    if (libre < 0) {
      ++frontal_sin_repuesto_;
      AntesDeEscribirColor(destino);  // the copy is recorded before the clear, as usual
      return;
    }
    Imagen& repuesto = frontales_imagenes_[size_t(libre)].imagen;
    std::swap(destino.imagen, repuesto.imagen);
    std::swap(destino.memoria, repuesto.memoria);
    std::swap(destino.vista, repuesto.vista);
    std::swap(destino.preparada, repuesto.preparada);
    frontales_imagenes_[size_t(libre)].retenida_por = base;
    frontales_pendientes_[base].retenida = libre;  // origen_vk is still the content image, now retained
    if (dibujos_) {
      dibujos_->InvalidarImagenes(destino.imagen, repuesto.imagen);
    }
    ++frontal_rotaciones_;
  }

  // A draw samples a front buffer. If its copy is deferred, it is recorded now (before the draw).
  void AnotarLecturaFrontal(uint32_t direccion) {
    frontales_leidos_[direccion] = presentados_;
    if (!frontales_pendientes_.empty() && frontales_pendientes_.count(direccion)) {
      GrabarCopiaFrontal(direccion);
      ++frontal_copiadas_lectura_;
    }
    if (!frontales_caducados_.empty() && frontales_caducados_.erase(direccion)) {
      ++frontal_lecturas_tardias_;
      if (!frontal_apagado_) {
        frontal_apagado_ = true;
        REXLOG_ERROR("[nativo] C2 frontal perezoso: DIFERENCIA, un dibujo muestrea el frontal {:08X} sin su ultima "
                     "copia. Apagado para el resto de la sesion: se copia siempre",
                     direccion);
      }
    }
  }

  // The Swap of that front buffer, before the submission. Returns the image that can be presented without
  // a copy (the target's or the retained one, with the front buffer in its corner) or nullptr: in that
  // case any deferred copy is recorded here (in the same submission, ahead of the output) and the output
  // reads the texture as usual.
  Imagen* FrontalAlPresentar(uint32_t direccion, uint32_t ancho, uint32_t alto) {
    frontales_presentados_[direccion] = presentados_;
    if (!frontales_caducados_.empty() && frontales_caducados_.erase(direccion)) {
      ++frontal_lecturas_tardias_;
      if (!frontal_apagado_) {
        frontal_apagado_ = true;
        REXLOG_ERROR("[nativo] C2 frontal perezoso: DIFERENCIA, el Swap pinta el frontal {:08X} sin su ultima copia. "
                     "Apagado para el resto de la sesion: se copia siempre",
                     direccion);
      }
    }
    if (frontales_pendientes_.empty()) {
      return nullptr;
    }
    if (REXCVAR_GET(nfsmw_nativo_diag_resueltas)) {
      // The diagnostic grid shows every resolved texture: if it is enabled at runtime, no copy stays deferred.
      while (!frontales_pendientes_.empty()) {
        GrabarCopiaFrontal(frontales_pendientes_.begin()->first);  // removes it from the list, whether it succeeds or not
        ++frontal_copiadas_swap_;
      }
      return nullptr;
    }
    const auto p = frontales_pendientes_.find(direccion);
    if (p == frontales_pendientes_.end()) {
      return nullptr;
    }
    const auto r = resueltas_.find(direccion);
    Imagen* const origen = OrigenFrontal(p->second);
    bool puede = origen && r != resueltas_.end() && r->second.imagen.imagen == p->second.textura_vk &&
                 r->second.imagen.preparada && rampa_gamma_ && !nfsmw::ajustes::AntialiasingFxaa() &&
                 !frontal_apagado_;
    if (puede) {
      // Same computation as Presentar: the output has the texture's size (exact variant) and the copy
      // covers it.
      const Imagen& textura = r->second.imagen;
      const uint32_t w = std::min(ancho ? ancho : textura.ancho, textura.ancho);
      const uint32_t h = std::min(alto ? alto : textura.alto, textura.alto);
      puede = w == textura.ancho && h == textura.alto && p->second.copia.extent.width == w &&
              p->second.copia.extent.height == h;
    }
    if (!puede) {
      GrabarCopiaFrontal(direccion);
      ++frontal_copiadas_swap_;
      return nullptr;
    }
    if (p->second.retenida >= 0) {
      ++frontal_pintadas_retenida_;
    } else {
      ++frontal_pintadas_destino_;
    }
    return origen;
  }

  // Upload buffer full: submit what has been recorded and continue in the other slot, with its empty
  // upload buffer (Grabar only waits if that slot's last submission is still on the GPU).
  bool EnviarYEsperar() override { return EnviarTrabajo(false) && Grabar(); }

  // The function above submits and continues; this one waits. Same order as every other place that must
  // destroy something the GPU might still be reading (see the resolved texture that changes size).
  bool EsperarGpuDelTodo() override {
    EsperarGpu();
    return Grabar();
  }

  void MarcarGpu(uint32_t categoria) override {
    if (consultas_ == VK_NULL_HANDLE || !grabando_) {
      return;
    }
    RanuraTrabajo& ranura = ranuras_[ranura_];
    if (ranura.categorias.size() + 1 >= kMarcasPorRanura ||
        (!ranura.categorias.empty() && ranura.categorias.back() == categoria)) {
      return;  // no room (the last one is for the end mark) or same category continues
    }
    escribir_marca_(comandos_trabajo_,
                    ranura.marcas_precisas ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    consultas_, ranura_ * kMarcasPorRanura + uint32_t(ranura.categorias.size()));
    ranura.categorias.push_back(uint8_t(categoria));
  }

  // The game's occlusion queries (see nfsmw_nativo_destinos.h).
  void EmpezarOclusion(uint32_t base) override {
    if (oclusion_abierta_ != 0) {
      uint64_t descartadas = 0;
      TerminarOclusion(0, descartadas);
    }
    if (!dibujos_ || oclusiones_ == VK_NULL_HANDLE) {
      return;
    }
    if (oclusiones_juego_.size() >= 256) {
      oclusiones_juego_.clear();  // spans that were never read (should not happen): leftovers are ignored
    }
    oclusion_abierta_ = siguiente_oclusion_++;
    oclusiones_juego_[oclusion_abierta_].base = base;
    dibujos_->OclusionAbierta(true);
  }

  bool TerminarOclusion(uint32_t base, uint64_t& muestras) override {
    if (oclusion_abierta_ != 0) {
      if (dibujos_) {
        dibujos_->OclusionAbierta(false);
      }
      const auto it = oclusiones_juego_.find(oclusion_abierta_);
      oclusion_abierta_ = 0;
      if (it != oclusiones_juego_.end()) {
        it->second.terminada = true;
        if (it->second.tramos_pendientes == 0) {
          PublicarOclusion(it);  // no draws counted: 0 samples
        }
      }
    }
    const auto medida = oclusion_por_base_.find(base);
    if (medida == oclusion_por_base_.end()) {
      return false;
    }
    muestras = medida->second;
    return true;
  }

  void EstadisticasOclusion(uint64_t valores[5]) const override {
    std::copy(std::begin(estadisticas_oclusion_), std::end(estadisticas_oclusion_), valores);
  }

  bool OclusionGpuPermitida() const override { return oclusion_gpu_; }

  uint32_t EmpezarConsultaOclusion() override {
    if (oclusiones_ == VK_NULL_HANDLE || oclusion_abierta_ == 0 || !grabando_) {
      return UINT32_MAX;
    }
    const auto it = oclusiones_juego_.find(oclusion_abierta_);
    if (it == oclusiones_juego_.end()) {
      return UINT32_MAX;
    }
    RanuraTrabajo& ranura = ranuras_[ranura_];
    if (ranura.oclusiones.size() >= kOclusionesPorRanura) {
      ++estadisticas_oclusion_[1];
      it->second.fallida = true;
      return UINT32_MAX;
    }
    const uint32_t indice = ranura_ * kOclusionesPorRanura + uint32_t(ranura.oclusiones.size());
    dfn_.vkCmdBeginQuery(comandos_trabajo_, oclusiones_, indice,
                         oclusion_precisa_ ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    ranura.oclusiones.emplace_back(indice, oclusion_abierta_);
    ++it->second.tramos_pendientes;
    ++estadisticas_oclusion_[0];
    return indice;
  }

  void TerminarConsultaOclusion(uint32_t indice) override {
    if (oclusiones_ != VK_NULL_HANDLE && indice != UINT32_MAX && grabando_) {
      dfn_.vkCmdEndQuery(comandos_trabajo_, oclusiones_, indice);
    }
  }

  // nfsmw_reflejo_visibilidad. Our own query around a single draw (the one that samples the reflection,
  // or the witness), inside the open pass. UINT32_MAX if there is no pool, nothing is being recorded or
  // there is no room left.
  uint32_t EmpezarConsultaVisibilidad(bool testigo) override {
    if (visibilidad_ == VK_NULL_HANDLE || !grabando_) {
      return UINT32_MAX;
    }
    RanuraTrabajo& ranura = ranuras_[ranura_];
    if (ranura.visibilidad.size() >= kVisibilidadPorRanura) {
      ++visibilidad_sin_sitio_;
      return UINT32_MAX;
    }
    const uint32_t indice = ranura_ * kVisibilidadPorRanura + uint32_t(ranura.visibilidad.size());
    dfn_.vkCmdBeginQuery(comandos_trabajo_, visibilidad_, indice, 0);
    ranura.visibilidad.emplace_back(indice, testigo ? kVisibilidadTestigo : kVisibilidadAgua);
    return indice;
  }

  void TerminarConsultaVisibilidad(uint32_t indice) override {
    if (visibilidad_ != VK_NULL_HANDLE && indice != UINT32_MAX && grabando_) {
      dfn_.vkCmdEndQuery(comandos_trabajo_, visibilidad_, indice);
    }
  }

  // One query per pass, from before vkCmdBeginRenderPass to after EndRenderPass.
  uint32_t EmpezarEstadisticas(uint32_t categoria) override {
    // With the per-draw diagnostic enabled this one is not opened: two queries of the same type cannot be
    // active at once, and the per-pass query would enclose the per-draw ones.
    if (estadisticas_ == VK_NULL_HANDLE || !grabando_ ||
        !REXCVAR_GET(nfsmw_nativo_estadisticas_pipeline) ||
        REXCVAR_GET(nfsmw_nativo_estadisticas_por_dibujo_s) > 0) {
      return UINT32_MAX;
    }
    RanuraTrabajo& ranura = ranuras_[ranura_];
    if (ranura.estadisticas.size() >= kEstadisticasPorRanura) {
      ++estadisticas_sin_sitio_;
      if (estadisticas_sin_sitio_ % 200 == 1) {
        REXLOG_WARN("[nativo] C2: sin sitio para estadisticas de pasada ({} veces): el trabajo lleva mas de {} "
                    "pases", estadisticas_sin_sitio_, kEstadisticasPorRanura);
      }
      return UINT32_MAX;
    }
    const uint32_t indice =
        ranura_ * kEstadisticasPorRanura + uint32_t(ranura.estadisticas.size());
    dfn_.vkCmdBeginQuery(comandos_trabajo_, estadisticas_, indice, 0);
    ranura.estadisticas.emplace_back(indice, uint8_t(categoria));
    return indice;
  }

  void TerminarEstadisticas(uint32_t indice) override {
    if (estadisticas_ != VK_NULL_HANDLE && indice != UINT32_MAX && grabando_) {
      dfn_.vkCmdEndQuery(comandos_trabajo_, estadisticas_, indice);
    }
  }

  // One query per draw, tagged with its pixel shader.
  uint32_t EmpezarEstadisticasDibujo(uint32_t etiqueta, uint32_t categoria) override {
    if (estadisticas_dibujo_ == VK_NULL_HANDLE || !grabando_ || !ventana_diagnostico_) {
      return UINT32_MAX;
    }
    RanuraTrabajo& ranura = ranuras_[ranura_];
    if (ranura.estadisticas_dibujo.size() >= kEstadisticasDibujoPorRanura) {
      ++estadisticas_dibujo_sin_sitio_;
      if (estadisticas_dibujo_sin_sitio_ % 2000 == 1) {
        REXLOG_WARN("[nativo] C2: sin sitio para estadisticas por dibujo ({} veces)",
                    estadisticas_dibujo_sin_sitio_);
      }
      return UINT32_MAX;
    }
    const uint32_t indice = ranura_ * kEstadisticasDibujoPorRanura +
                            uint32_t(ranura.estadisticas_dibujo.size());
    dfn_.vkCmdBeginQuery(comandos_trabajo_, estadisticas_dibujo_, indice, 0);
    ranura.estadisticas_dibujo.emplace_back(
        indice, uint16_t((categoria % kGpuCategorias) * kEtiquetasShader + etiqueta % kEtiquetasShader));
    return indice;
  }

  void TerminarEstadisticasDibujo(uint32_t indice) override {
    if (estadisticas_dibujo_ != VK_NULL_HANDLE && indice != UINT32_MAX && grabando_) {
      dfn_.vkCmdEndQuery(comandos_trabajo_, estadisticas_dibujo_, indice);
    }
  }

 private:
  bool Rechazar(uint32_t causa, const char* texto) {
    ++rechazos_;
    if (avisados_.insert(causa).second) {
      REXLOG_WARN("[nativo] C2: {} (causa {})", texto, causa);
    }
    return false;
  }

  // Rectangle covered by the copy, as in GetResolveInfo (draw.cpp:787-889).
  bool Rectangulo(const RegistrosCopia& reg, uint32_t pitch, int32_t& x0, int32_t& y0,
                  int32_t& x1, int32_t& y1) {
    const uint32_t tipo = reg.fetch_vertices[0] & 0x3;
    const uint32_t direccion = reg.fetch_vertices[0] >> 2;
    const auto orden = static_cast<xenos::Endian>(reg.fetch_vertices[1] & 0x3);
    const uint32_t tamano = (reg.fetch_vertices[1] >> 2) & 0xFFFFFF;
    if (tipo != uint32_t(xenos::FetchConstantType::kVertex) || tamano != 3 * 2) {
      return Rechazar(5, "vertices de la copia en un formato no soportado");
    }
    const uint8_t* vertices = memoria_->TranslatePhysical(direccion * 4);
    const float medio_pixel =
        (reg.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
    int32_t fijo[6];
    for (int i = 0; i < 6; ++i) {
      float valor;
      std::memcpy(&valor, vertices + i * 4, sizeof(valor));
      fijo[i] = Fijo16p8(xenos::GpuSwap(valor, orden) + medio_pixel);
    }
    x0 = (std::min({fijo[0], fijo[2], fijo[4]}) + 127) >> 8;
    y0 = (std::min({fijo[1], fijo[3], fijo[5]}) + 127) >> 8;
    x1 = (std::max({fijo[0], fijo[2], fijo[4]}) + 127) >> 8;
    y1 = (std::max({fijo[1], fijo[3], fijo[5]}) + 127) >> 8;

    const int32_t desplazamiento_x = ExtenderSigno15(reg.pa_sc_window_offset & 0x7FFF);
    const int32_t desplazamiento_y = ExtenderSigno15((reg.pa_sc_window_offset >> 16) & 0x7FFF);
    if ((reg.pa_su_sc_mode_cntl >> 16) & 0x1) {  // vtx_window_offset_enable
      x0 += desplazamiento_x;
      y0 += desplazamiento_y;
      x1 += desplazamiento_x;
      y1 += desplazamiento_y;
    }
    // Window scissor (GetScissor without clamping to the pitch).
    int32_t izquierda = int32_t(reg.pa_sc_window_scissor_tl & 0x3FFF);
    int32_t arriba = int32_t((reg.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
    int32_t derecha = int32_t(reg.pa_sc_window_scissor_br & 0x3FFF);
    int32_t abajo = int32_t((reg.pa_sc_window_scissor_br >> 16) & 0x3FFF);
    if (!((reg.pa_sc_window_scissor_tl >> 31) & 0x1)) {  // window_offset_disable
      izquierda += desplazamiento_x;
      arriba += desplazamiento_y;
      derecha += desplazamiento_x;
      abajo += desplazamiento_y;
    }
    izquierda = std::max(izquierda, 0);
    arriba = std::max(arriba, 0);
    derecha = std::max(derecha, izquierda);
    abajo = std::max(abajo, arriba);
    x0 = std::clamp(x0, izquierda, derecha);
    y0 = std::clamp(y0, arriba, abajo);
    x1 = std::clamp(x1, izquierda, derecha);
    y1 = std::clamp(y1, arriba, abajo);
    // D3D9 alinea a 8 (kResolveAlignmentPixels).
    x0 &= ~int32_t(7);
    y0 &= ~int32_t(7);
    x1 = (x1 + 7) & ~int32_t(7);
    y1 = (y1 + 7) & ~int32_t(7);
    const int32_t pitch_alineado = int32_t(pitch & ~uint32_t(7));
    x0 = std::min(x0, pitch_alineado);
    x1 = std::min(x1, pitch_alineado);
    if (x0 >= x1 || y0 >= y1) {
      return Rechazar(6, "rectangulo de copia vacio");
    }
    return true;
  }

  Imagen* ObtenerDestino(uint32_t base, uint32_t formato, uint32_t pitch) {
    if (!pitch) {
      Rechazar(7, "destino de render con pitch 0");
      return nullptr;
    }
    // The "AS" variants are the same EDRAM contents seen with another precision: NFS Carbon draws its scene in
    // 2_10_10_10_FLOAT and, in the same render target, in 2_10_10_10_FLOAT_AS_16_16_16_16. Both views are one
    // image (they already share the host format).
    if (formato == 12) {
      formato = 3;
    } else if (formato == 10) {
      formato = 2;
    }
    const uint64_t clave = (uint64_t(base) << 20) | (uint64_t(formato) << 16) | pitch;
    auto it = destinos_.find(clave);
    if (it != destinos_.end()) {
      return &it->second;
    }
    /*
     * The height comes from the pitch, so the scene's color target measures 1280x1280 to draw 1280x720
     * and can never be swapped with its resolved texture. With a pitch of 1280 or less the game never
     * draws below 720 (measured viewport and scissor), so 720 rows would be enough and the sizes would
     * match. Above 1280 (the internal 1080p mode) the larger height is needed.
     */
    /*
     * Creating the color target 720 rows high (instead of deriving the height from the pitch), so that it
     * matches the resolved texture and the images can be swapped instead of copied, was tried: it breaks
     * the scene (the screen fills with a yellow smear). Reverted.
     */
    const uint32_t alto = std::min(kAltoMaximoDestino, std::max<uint32_t>(720, (pitch + 15) & ~15u));
    Imagen imagen;
    const VkFormat formato_host = FormatoHostDestinoColor(formato);
    if (formato_host == VK_FORMAT_UNDEFINED) {
      Rechazar(100 + formato, "formato de destino de render todavia no soportado");
      return nullptr;
    }
    if (!Crear(imagen, pitch, alto,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               formato_host)) {
      Rechazar(8, "no se pudo crear un destino de render");
      return nullptr;
    }
    const VkClearColorValue diag = ColorDiagnostico(base, formato, pitch);
    REXLOG_INFO("[nativo] C2: destino de render base {:03X}, formato {}, {}x{} (color de "
                "diagnostico {:02X}{:02X}{:02X})",
                base, formato, pitch, alto, uint32_t(std::lround(diag.float32[0] * 255.0f)),
                uint32_t(std::lround(diag.float32[1] * 255.0f)),
                uint32_t(std::lround(diag.float32[2] * 255.0f)));
    return &destinos_.emplace(clave, imagen).first->second;
  }

  Imagen* ObtenerProfundidad(uint32_t base, uint32_t formato, uint32_t pitch) {
    if (!pitch || formato_profundidad_ == VK_FORMAT_UNDEFINED) {
      Rechazar(11, "sin destino de profundidad (pitch 0 o formato no disponible)");
      return nullptr;
    }
    const uint64_t clave = (uint64_t(base) << 20) | (uint64_t(formato) << 16) | pitch;
    auto it = profundidades_.find(clave);
    if (it != profundidades_.end()) {
      return &it->second;
    }
    const uint32_t alto = std::min(kAltoMaximoDestino, std::max<uint32_t>(720, (pitch + 15) & ~15u));
    // Smaller shadow map.
    //
    // The game requests two 1600x1600 maps per frame, and 1600 is not a quality choice: it is 2000 of
    // the 2048 80x16 tiles of the Xbox 360 EDRAM. The PC version of this same game uses 1024x1024. Here
    // the map is drawn at nfsmw_nativo_sombras_escala percent and upscaled when resolved, so the texture
    // the scene samples is the usual one and only the detail goes down.
    //
    // It is recognized by its 1600x1600 size, which no other render target uses. The scale is read only
    // once (when the first map is created) so that changing it at runtime does not leave old images unused.
    uint32_t ancho_imagen = pitch, alto_imagen = alto;
    uint32_t ancho_guest = 0, alto_guest = 0;
    if (pitch == kLadoSombras && alto == kLadoSombras) {
      if (escala_sombras_ == 0) {
        escala_sombras_ = uint32_t(std::clamp(REXCVAR_GET(nfsmw_nativo_sombras_escala), 50, 100));
        if (!blit_ || !profundidad_escalable_) {
          escala_sombras_ = 100;
        }
      }
      if (escala_sombras_ < 100) {
        ancho_imagen = std::max<uint32_t>(64, ((pitch * escala_sombras_ / 100) + 15) & ~15u);
        alto_imagen = std::max<uint32_t>(64, ((alto * escala_sombras_ / 100) + 15) & ~15u);
        ancho_guest = pitch;
        alto_guest = alto;
        REXLOG_INFO("[nativo] C2: mapa de sombras a {} %: se dibuja y se resuelve a {}x{} (el juego pide {}x{})",
                    escala_sombras_, ancho_imagen, alto_imagen, pitch, alto);
      }
    }
    // The usage flag that decides whether there is hierarchical depth culling.
    //
    // The driver only assigns a ZCULL plane if the usage flags fit in
    //   DEPTH_STENCIL_ATTACHMENT | TRANSFER_SRC | SAMPLED | INPUT_ATTACHMENT
    // (nvk_image.c). TRANSFER_DST disqualifies the image, and we only request it for
    // vkCmdClearDepthStencilImage, which the specification requires with that usage.
    //
    // TRANSFER_DST is dropped from the scene depth, which is 50 % of the frame, and kept for the shadow
    // map. Reason: the scene resolves 1280x720 out of a 1280x1280 image, so it is never "whole" and
    // never swaps with its resolved texture (console logs: thousands of "sin intercambio ... porque la
    // orden no borra el destino", 0 restores). The shadow map does swap, ~0.83 times per frame, and that
    // swap is worth 2.36 ms measured. So the scene gains ZCULL without losing a single swap.
    //
    // In exchange, that image can no longer be cleared with vkCmdClearDepthStencilImage: it is cleared
    // by opening a pass with loadOp = CLEAR (DibujosVulkan::BorrarProfundidadEnPase).
    // With nfsmw_nativo_zcull off, every image gets TRANSFER_DST: none receives a ZCULL plane and clears
    // go back to vkCmdClearDepthStencilImage, the path used before ZCULL.
    //
    // =========================================================================================
    // Do not remove TRANSFER_DST from the shadow map. Examined in depth and rejected.
    //
    // 1. It still needs it. The reasoning was that the map is no longer copied but swapped, so the flag
    //    would be unnecessary. Wrong: each swap is paid for with a restore, and a restore is exactly a
    //    copy into this image (RestaurarContenido -> copiar_imagen_ with destino.imagen as dst, which
    //    requires TRANSFER_DST). Steady race, three consecutive 10 s intervals:
    //        swaps without clear  4679 -> 4885 -> 5145 -> 5369   (+206, +260, +224)
    //        restores             3584 -> 3790 -> 4050 -> 4274   (+206, +260, +224)
    //    Exactly one restore per swap. The 1600x1600 copy did not go away: it is paid later, in
    //    DestinoProfundidad. Without TRANSFER_DST that is an illegal write.
    //    Also, IntercambiarConResuelta requires admite_destino_de_copia, so removing the flag would also
    //    disable the 2 swaps per frame that do pay off.
    //
    // 2. Even if it were possible, it would not help. Fit over 17 race intervals with a constant open
    //    area (5.12 Mtexels): raw ms = 0.0342 x thousands of triangles + 0.023, r2 = 0.982. The
    //    intercept is 0.037 real ms: with both maps open and zero triangles the pass costs nothing.
    //    The whole pass is geometry, and ZCULL discards fragments. The same fit against draws gives
    //    r2 = 0.404. ZCULL ceiling here: 0.037 ms; with the most generous bound (the scale test: -44 %
    //    of area gave -9 % of time), 0.1-0.25 ms. Against the 2.36 ms of the swap that would be lost:
    //    a net loss of 10 to 1.
    //
    // 3. ZCULL for shadows only, without touching the scene depth: in NVK (nvk_cmd_draw.c,
    //    `use_zcull`) a pass opened with loadOp = LOAD_OP_CLEAR enables ZCULL even when the image has
    //    no plane. It is ephemeral, without LOAD_ZCULL/STORE_ZCULL between passes, which a shadow map
    //    drawn whole every time does not need. The shadow pass opens with LOAD, or with DONT_CARE under
    //    nfsmw_nativo_pase_sombras_sin_load, so it gets no ephemeral ZCULL either. Given point 2, that
    //    does not pay off while the pass is pure geometry.
    // =========================================================================================
    const bool zcull = REXCVAR_GET(nfsmw_nativo_zcull);
    const bool es_mapa_de_sombras = (pitch == kLadoSombras && alto == kLadoSombras);
    const bool con_transfer_dst = es_mapa_de_sombras || !zcull;
    VkImageUsageFlags uso_profundidad = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
    if (con_transfer_dst) {
      uso_profundidad |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    Imagen imagen;
    // Source of the depth copies (CopiarProfundidad): TRANSFER_SRC is required.
    // SAMPLED: with nfsmw_nativo_resolver_sin_copia this image can end up being the resolved texture.
    if (!Crear(imagen, ancho_imagen, alto_imagen, uso_profundidad, formato_profundidad_)) {
      Rechazar(12, "no se pudo crear un destino de profundidad");
      return nullptr;
    }
    imagen.ancho_guest = ancho_guest;
    imagen.alto_guest = alto_guest;
    imagen.admite_destino_de_copia = con_transfer_dst;
    REXLOG_INFO("[nativo] C2: destino de profundidad base {:03X}, formato {}, {}x{}", base,
                formato, pitch, alto);
    return &profundidades_.emplace(clave, imagen).first->second;
  }

  Resuelta* ObtenerResuelta(uint32_t base, uint32_t ancho, uint32_t alto, uint32_t formato,
                            bool intercambio_rb, VkFormat formato_host = kFormatoColor) {
    if (!ancho || !alto) {
      Rechazar(9, "copia a una textura de tamano 0");
      return nullptr;
    }
    auto it = resueltas_.find(base);
    if (it != resueltas_.end()) {
      if (it->second.imagen.ancho == ancho && it->second.imagen.alto == alto &&
          it->second.imagen.formato == formato_host) {
        if (dibujos_ && (it->second.formato_guest != formato || it->second.intercambio_rb != intercambio_rb)) {
          dibujos_->InvalidarTexturas();  // changes the swizzle it is sampled with
        }
        it->second.formato_guest = formato;
        it->second.intercambio_rb = intercambio_rb;
        it->second.imagen.intercambio_rb = intercambio_rb;
        return &it->second;
      }
      // Another size or format at the same address: the old image may still be in use.
      EnviarTrabajo(true);
      EsperarGpu();
      if (dibujos_) {
        dibujos_->OlvidarImagen(it->second.imagen.imagen);
      }
      Destruir(it->second.imagen);
      resueltas_.erase(it);
      // The image at that address no longer exists, so no render target owes it anything.
      if (!prestadas_.empty()) {
        prestadas_.erase(base);
      }
      SombraMinimoResueltaEscrita(base);  // the new image does not have its content
    }
    Resuelta resuelta;
    // The attachment usage (color or depth) is needed for nfsmw_nativo_resolver_sin_copia: this image can
    // end up being the render target it is swapped with.
    const VkImageUsageFlags uso_destino = EsProfundidadFormato(formato_host)
                                              ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                              : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!Crear(resuelta.imagen, ancho, alto,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | uso_destino,
               formato_host)) {
      Rechazar(10, "no se pudo crear una textura resuelta");
      return nullptr;
    }
    resuelta.formato_guest = formato;
    resuelta.intercambio_rb = intercambio_rb;
    resuelta.imagen.intercambio_rb = intercambio_rb;
    REXLOG_INFO("[nativo] C2: textura resuelta en {:08X}, {}x{}, formato {}", base, ancho, alto,
                formato);
    if (dibujos_) {
      dibujos_->InvalidarTexturas();  // that address is now sampled from the resolved texture
    }
    return &resueltas_.emplace(base, resuelta).first->second;
  }

  // One more copy in its size bucket, to tell whether the copy milliseconds come from pixels or from a
  // fixed cost per copy.
  void AnotarCopia(uint32_t ancho, uint32_t alto) {
    const uint64_t pixeles = uint64_t(ancho) * alto;
    for (uint32_t c = 0; c < kCubetasCopia; ++c) {
      if (pixeles <= kPixelesCubeta[c]) {
        ++copias_cubeta_[c];
        pixeles_cubeta_[c] += pixeles;
        return;
      }
    }
  }

  // The setting, or its alternating test.
  bool ResolverSinCopia() {
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_resolver_sin_copia_alternar_s);
    if (alternar <= 0) {
      return REXCVAR_GET(nfsmw_nativo_resolver_sin_copia);
    }
    const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - inicio_alternancia_)
                              .count();
    const bool sin_copia = (segundos / alternar) % 2 == 1;
    if (sin_copia != alternancia_anotada_) {
      alternancia_anotada_ = sin_copia;
      REXLOG_INFO("[nativo] C2 resolucion: {}", sin_copia ? "sin copia (intercambio)" : "copiando");
    }
    return sin_copia;
  }

  static bool EsProfundidadFormato(VkFormat formato) {
    return formato == VK_FORMAT_D24_UNORM_S8_UINT || formato == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           formato == VK_FORMAT_D16_UNORM || formato == VK_FORMAT_D32_SFLOAT ||
           formato == VK_FORMAT_X8_D24_UNORM_PACK32;
  }

  // Resolve without copying. The whole render target becomes the resolved texture and that texture's
  // old image stays as the render target. What the game sees is the same; what is saved is moving the
  // pixels (the two 1600x1600 shadow map copies are 41 MB per frame). false if it is not possible.
  bool IntercambiarConResuelta(Imagen& destino, Resuelta& resuelta, uint32_t base) {
    if (destino.ancho != resuelta.imagen.ancho || destino.alto != resuelta.imagen.alto ||
        destino.formato != resuelta.imagen.formato) {
      return false;
    }
    // ZCULL safety net. If the render target was created without TRANSFER_DST (so it can have a ZCULL
    // plane), swapping it would put it in the resolved texture's place, which does receive copies: an
    // illegal write. And the other way round, the resolved texture (with TRANSFER_DST and no plane) would
    // become the depth target and ZCULL would switch off on alternate frames. Fall back to the usual copy
    // path instead.
    if (!destino.admite_destino_de_copia) {
      return false;
    }
    // nfsmw_nativo_diag_borrados. The whole image becomes a sampled texture, so all of it counts as used
    // (noted before the images change places).
    AnotarUsoBorrado(destino, destino.ancho, destino.alto);
    // Careful: OlvidarImagen must not be called here. It frees the descriptor slot and reuses it in the
    // same frame, so draws already recorded with it end up reading another image (flicker). Bumping the
    // generation is enough: later draws resolve their view again, and existing views stay valid because
    // they go with their image.
    // nfsmw_nativo_profundidad_perezosa. The render target changes image: first, whatever it has deferred
    // as a source is resolved; and the texture receives the whole target, so its deferred copy is no
    // longer needed.
    if (!pendientes_.empty()) {
      AntesDeEscribirProfundidad(destino);
      if (pendientes_.erase(base)) {
        ++perezosa_sustituidas_;
      }
    }
    // nfsmw_nativo_frontal_perezoso. Same for front buffers: deferred copies whose source is this target
    // are recorded first, and the texture receives the whole target (its deferred copy is unnecessary).
    if (!frontales_pendientes_.empty()) {
      AntesDeEscribirColor(destino);
      ResolverFrontalAnterior(base, true);
    }
    std::swap(destino.imagen, resuelta.imagen.imagen);
    std::swap(destino.memoria, resuelta.imagen.memoria);
    std::swap(destino.vista, resuelta.imagen.vista);
    std::swap(destino.preparada, resuelta.imagen.preparada);
    resuelta.imagen.intercambio_rb = resuelta.intercambio_rb;  // belongs to the content, not the image
    resuelta.imagen.contenido_invalido = false;
    destino.contenido_invalido = true;  // its content is now in the resolved texture
    destino.resuelta_base = base;
    ++intercambios_;
    ResueltaEscrita(base);   // if it was lent, it has valid content again
    OlvidarBorrado(destino);  // and the target keeps another image: its clear no longer holds
    if (dibujos_) {
      // Only what points to these two images: invalidating the whole cache here costs more than the copy
      // it saves (measured on PC: +0.18 ms of scene per frame).
      dibujos_->InvalidarImagenes(destino.imagen, resuelta.imagen.imagen);
    }
    return true;
  }

  // If the render target lost its content in an image swap and the game is about to draw on top, the
  // content is brought back from the resolved texture.
  //
  // In a race this is not rare: it happens once for every swap without a clear (measured: +206/+260/+224
  // swaps and +206/+260/+224 restores in three consecutive intervals). That is 1.66 real ms per frame
  // that no bucket of the copy inventory accounted for. Here they are counted, trimmed to the useful
  // area and, if requested, done without copying a single pixel.
  //
  // para_resolver = requested by a resolve (nfsmw_nativo_resolver_contenido_valido), not by a pass that
  // will draw on top. Then it is only copied: no lending (it would leave the source texture without
  // content just when the car body is about to read it) and no nfsmw_nativo_sombra_minimo clear (that
  // is for the car pass).
  void RestaurarContenido(Imagen& destino, bool para_resolver = false) {
    if (!destino.contenido_invalido) {
      return;
    }
    destino.contenido_invalido = false;
    // nfsmw_nativo_profundidad_perezosa. The resolved texture is about to be read (if its copy was
    // deferred, it is recorded first) and the render target written (first, whatever it has deferred as
    // a source).
    if (!pendientes_.empty()) {
      if (pendientes_.count(destino.resuelta_base)) {
        GrabarCopiaPendiente(destino.resuelta_base);
        ++perezosa_copiadas_lectura_;
      }
      AntesDeEscribirProfundidad(destino);
    }
    // nfsmw_nativo_frontal_perezoso. Same for front buffers: if the texture about to be read has a
    // deferred copy, it is recorded first; and so are deferred copies whose source is this render target.
    if (!frontales_pendientes_.empty()) {
      if (frontales_pendientes_.count(destino.resuelta_base)) {
        GrabarCopiaFrontal(destino.resuelta_base);
        ++frontal_copiadas_lectura_;
      }
      AntesDeEscribirColor(destino);
    }
    auto it = resueltas_.find(destino.resuelta_base);
    if (it == resueltas_.end() || it->second.imagen.ancho != destino.ancho ||
        it->second.imagen.alto != destino.alto || it->second.imagen.formato != destino.formato ||
        !copiar_imagen_ || !Grabar()) {
      if (para_resolver) {
        ++resolver_sin_origen_;  // unknown where to bring it from; resolve whatever is there
      }
      return;
    }
    if (para_resolver) {
      // nfsmw_nativo_resolver_contenido_valido. Only the copy, with its barriers.
      CopiarDeVueltaParaResolver(destino, it->second.imagen);
      return;
    }
    // The content does not need duplicating, only to be in the render target: swapping the two images
    // back puts it where it belongs without moving a byte. The resolved texture stays lent until the next
    // resolve to that address; 'prestadas leidas' checks that nobody looks at it in the meantime.
    // Both must already be in GENERAL: the swap moves the image, not its layout, and preparing after the
    // swap would put the barrier on the wrong image.
    /*
     * Why the restore does not swap.
     *
     * Restores copy 2.56 Mpixels per frame = 2.00 real ms, and 17 log reports in a row show a saving of
     * 0.00. This path exists precisely to avoid the copy, and the counter says "0 por intercambio": it
     * never activates. It has three conditions, so each one is counted instead of guessing which one
     * fails. Three uint64 increments on a path that already copies 10 MB: zero cost.
     */
    if (REXCVAR_GET(nfsmw_nativo_restaurar_por_intercambio)) {
      if (!destino.admite_destino_de_copia) {
        ++no_swap_sin_transfer_dst_;
      } else if (!destino.preparada) {
        ++no_swap_destino_sin_preparar_;
      } else if (!it->second.imagen.preparada) {
        ++no_swap_resuelta_sin_preparar_;
      } else if (destino.ancho != it->second.imagen.ancho ||
                 destino.alto != it->second.imagen.alto) {
        // Not a condition of the if below, but it does matter: the swap moves the whole image, and if the
        // sizes do not match it would leave the render target with the wrong dimensions. If this count is
        // high, the lever cannot work as written.
        ++no_swap_tamanos_distintos_;
      }
    }
    if (REXCVAR_GET(nfsmw_nativo_restaurar_por_intercambio) && destino.admite_destino_de_copia &&
        destino.preparada && it->second.imagen.preparada) {
      const uint32_t base = destino.resuelta_base;
      std::swap(destino.imagen, it->second.imagen.imagen);
      std::swap(destino.memoria, it->second.imagen.memoria);
      std::swap(destino.vista, it->second.imagen.vista);
      std::swap(destino.preparada, it->second.imagen.preparada);
      it->second.imagen.contenido_invalido = false;
      if (dibujos_) {
        dibujos_->InvalidarImagenes(destino.imagen, it->second.imagen.imagen);
      }
      prestadas_.insert(base);
      ++prestamos_;
      ++restauraciones_;
      pixeles_restaurar_ahorrados_ += uint64_t(destino.ancho) * destino.alto;
      OlvidarBorrado(destino);
      return;
    }
    // nfsmw_nativo_sombra_minimo. The car pass of the shadow map: while it is being applied, the render
    // target is cleared to 1.0 instead of copying the world back (the world draws take the minimum).
    if (SombraMinimoEnVezDeRestaurar(destino)) {
      return;
    }
    Preparar(it->second.imagen);
    Preparar(destino);
    const VkImageAspectFlags aspecto =
        EsProfundidadFormato(destino.formato) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    // Render targets are created with height = max(720, pitch): the scene measures 1280x1280 to draw
    // 1280x720 and the blur targets 320x720 to draw 320x180. What lies below the useful area is never
    // drawn or resolved, so copying it wastes bandwidth. The useful area is the largest y1 the game has
    // asked to resolve from this target; until there is data, the whole target is copied.
    uint32_t alto = destino.alto;
    if (REXCVAR_GET(nfsmw_nativo_restaurar_area_util)) {
      const auto e = estado_destino_.find(&destino);
      if (e != estado_destino_.end() && e->second.alto_usado &&
          e->second.alto_usado < destino.alto) {
        alto = e->second.alto_usado;
        ++restauraciones_recortadas_;
        pixeles_restaurar_ahorrados_ += uint64_t(destino.ancho) * (destino.alto - alto);
      }
    }
    VkImageCopy copia{};
    copia.srcSubresource = {aspecto, 0, 0, 1};
    copia.dstSubresource = {aspecto, 0, 0, 1};
    copia.extent = {destino.ancho, alto, 1};
    AnotarUsoBorrado(destino, destino.ancho, alto);  // nfsmw_nativo_diag_borrados
    MarcarGpu(kGpuCopias);
    copiar_imagen_(comandos_trabajo_, it->second.imagen.imagen, VK_IMAGE_LAYOUT_GENERAL,
                   destino.imagen, VK_IMAGE_LAYOUT_GENERAL, 1, &copia);
    ++restauraciones_;
    pixeles_restaurados_ += uint64_t(destino.ancho) * alto;
    AnotarCopia(destino.ancho, alto);  // so the copy inventory adds up to the measured ms
    OlvidarBorrado(destino);
  }

  /*
   * nfsmw_nativo_resolver_contenido_valido. The render target gets its content back from the resolved
   * texture where the image swap left it, just before a resolve reads it.
   *
   * The copy goes between two barriers, and here they are needed: the source is the image that was just
   * drawn as the depth target (the previous resolve moved it into the texture by swapping), and what is
   * copied will be sampled as soon as the current resolve swaps it. NVK only waits for the GPU and
   * flushes the texture cache at a barrier (in a race the restore still goes without them, as always:
   * this does not touch it). Each one costs a pipeline drain, twice per frame in the menu. The whole
   * target is copied: that is what the EDRAM held and what the resolve reads.
   */
  void CopiarDeVueltaParaResolver(Imagen& destino, Imagen& origen) {
    Preparar(origen);
    Preparar(destino);
    const VkImageAspectFlags aspecto =
        EsProfundidadFormato(destino.formato) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkMemoryBarrier barrera{};
    barrera.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrera.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    barrera.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    constexpr VkPipelineStageFlags kEscrituras =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dfn_.vkCmdPipelineBarrier(comandos_trabajo_, kEscrituras, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrera, 0,
                              nullptr, 0, nullptr);
    VkImageCopy copia{};
    copia.srcSubresource = {aspecto, 0, 0, 1};
    copia.dstSubresource = {aspecto, 0, 0, 1};
    copia.extent = {destino.ancho, destino.alto, 1};
    AnotarUsoBorrado(destino, destino.ancho, destino.alto);  // nfsmw_nativo_diag_borrados, like the restore
    MarcarGpu(kGpuCopias);
    copiar_imagen_(comandos_trabajo_, origen.imagen, VK_IMAGE_LAYOUT_GENERAL, destino.imagen, VK_IMAGE_LAYOUT_GENERAL,
                   1, &copia);
    // And the copied data made visible to what follows: the current resolve (copy or swap), the draws that
    // sample the texture that receives it, and the pass that draws to the render target again.
    barrera.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrera.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(comandos_trabajo_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              0, 1, &barrera, 0, nullptr, 0, nullptr);
    ++restauraciones_para_resolver_;
    pixeles_restaurados_ += uint64_t(destino.ancho) * destino.alto;
    AnotarCopia(destino.ancho, destino.alto);
    OlvidarBorrado(destino);
  }

  // --- nfsmw_nativo_sombra_minimo (see the cvar comment) --------------------------------------------------

  // A failure while applying may change the image of this very frame: DIFERENCIA in the log and it
  // switches off for the session. Textures that already hold only the cars keep being served with the
  // minimum until they are written again.
  void SombraMinimoDiferencia(const char* motivo, uint32_t dato) {
    ++sm_diferencias_;
    if (sm_fase_ != kSmApagada) {
      sm_fase_ = kSmApagada;
      REXLOG_ERROR("[nativo] C2 sombra por minimo: DIFERENCIA ({}; dato {:08X}). Se apaga para la sesion: el mapa de "
                   "sombras se vuelve a copiar como antes",
                   motivo, dato);
    }
  }

  // Something the minimum would not reproduce, seen before any image changed: not applied this session.
  void SombraMinimoNoApto(const char* motivo, uint32_t dato) {
    if (sm_fase_ != kSmApagada) {
      sm_fase_ = kSmApagada;
      REXLOG_WARN("[nativo] C2 sombra por minimo: no se aplica en esta sesion ({}; dato {:08X}); el mapa de sombras se "
                  "sigue copiando como siempre",
                  motivo, dato);
    }
  }

  // A failure within the current cycle. If the render target was already cleared (applying) it is a
  // DIFERENCIA; otherwise either the minimum does not work for this game (permanent) or the cycle simply
  // does not count (transient: menus, loading).
  void SombraMinimoFalloCiclo(const char* motivo, uint32_t dato, bool permanente) {
    sm_ciclo_limpio_ = false;
    if (sm_ciclo_borrado_) {
      SombraMinimoDiferencia(motivo, dato);
    } else if (permanente) {
      SombraMinimoNoApto(motivo, dato);
    }
  }

  // The setting for the cycle that starts: the cvar, its alternating test and the cheap PCF.
  bool SombraMinimoEncendido() {
    if (!REXCVAR_GET(nfsmw_nativo_sombra_minimo) || !REXCVAR_GET(nfsmw_nativo_pcf_barato)) {
      return false;
    }
    const int32_t alternar = REXCVAR_GET(nfsmw_nativo_sombra_minimo_alternar_s);
    if (alternar <= 0 || sm_fase_ != kSmAplicando) {
      return true;
    }
    const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                           sm_inicio_alternancia_)
                              .count();
    const bool con_minimo = (segundos / alternar) % 2 == 0;
    if (con_minimo != sm_tramo_anotado_) {
      sm_tramo_anotado_ = con_minimo;
      REXLOG_INFO("[nativo] C2 sombra por minimo (alternancia): {}", con_minimo ? "con el minimo" : "copiando");
    }
    return con_minimo;
  }

  // After resolving a whole depth target by swapping images (CopiarProfundidad). Without a clear it
  // starts a cycle (the texture receives the world); with a clear, after the car pass, it ends it (the
  // texture receives the cars).
  void SombraMinimoTrasIntercambio(const Imagen& destino, uint32_t base, bool borrara) {
    if (destino.ancho != kLadoSombras || destino.alto != kLadoSombras) {
      return;
    }
    if (!borrara) {
      if (sm_estado_ == kSmCoches && sm_ciclo_borrado_) {
        SombraMinimoDiferencia("el destino con los coches solos se resuelve sin borrar", base);
      }
      if (sm_estado_ != kSmLibre) {
        sm_limpios_seguidos_ = 0;  // the previous cycle did not close: count from scratch
      }
      sm_estado_ = kSmTrasMundo;
      sm_destino_ciclo_ = &destino;
      sm_mundo_ciclo_ = base;
      sm_ciclo_limpio_ = true;
      sm_ciclo_borrado_ = false;
      sm_mundo_reescrito_ = false;
      sm_encendido_ = SombraMinimoEncendido();
      return;
    }
    if (sm_estado_ != kSmCoches || &destino != sm_destino_ciclo_) {
      if (sm_estado_ != kSmLibre) {
        sm_limpios_seguidos_ = 0;
      }
      sm_estado_ = kSmLibre;
      return;
    }
    sm_estado_ = kSmLibre;
    ++sm_ciclos_;
    if (sm_ciclo_borrado_) {
      // The texture has just received only the cars: from here on it is sampled paired with the world.
      if (base != sm_coches_) {
        SombraMinimoDiferencia("la resolucion de los coches va a otra textura", base);
        sm_coches_ = base;  // it holds only the cars: serve that one
      }
      sm_virtual_ = true;
      sm_virtual_valida_ = !sm_mundo_reescrito_;
      if (sm_mundo_reescrito_) {
        SombraMinimoDiferencia("la textura del mundo se volvio a escribir dentro del ciclo", sm_mundo_);
      }
      ++sm_ciclos_aplicados_;
      return;
    }
    // Observing (or applying with the setting off in this cycle): the texture is the usual one, with the
    // world and the cars.
    if (base != sm_coches_ || sm_mundo_ciclo_ != sm_mundo_ || sm_destino_ciclo_ != sm_destino_) {
      if (sm_fase_ == kSmAplicando) {
        // A cycle different from the learned one (split screen, another mode): it cannot be served.
        // Stop applying.
        SombraMinimoNoApto("aparece un ciclo de mapa de sombras distinto del aprendido (el dato es su textura)", base);
        return;
      }
      sm_coches_ = base;  // learned (or changed): count again from zero
      sm_mundo_ = sm_mundo_ciclo_;
      sm_destino_ = sm_destino_ciclo_;
      sm_limpios_seguidos_ = 0;
    }
    sm_limpios_seguidos_ = (sm_ciclo_limpio_ && !sm_mundo_reescrito_) ? sm_limpios_seguidos_ + 1 : 0;
    if (sm_fase_ == kSmMirando && sm_encendido_ && sm_limpios_seguidos_ >= kSombraMinimoCiclos) {
      sm_fase_ = kSmAplicando;
      REXLOG_INFO("[nativo] C2 sombra por minimo: APLICANDO tras {} ciclos limpios seguidos (destino {}x{}, mundo en "
                  "{:08X}, coches en {:08X}): el pase de los coches se dibuja sobre el destino borrado y no se copia",
                  sm_limpios_seguidos_, destino.ancho, destino.alto, sm_mundo_, sm_coches_);
    }
  }

  // From RestaurarContenido: the render target lost its content in a swap and is about to be drawn on.
  // If this is the car pass of the current cycle it is noted, and while applying, the target is cleared
  // to 1.0 instead of bringing the world back (true: no copy needed).
  bool SombraMinimoEnVezDeRestaurar(Imagen& destino) {
    if (&destino != sm_destino_ciclo_ || sm_estado_ != kSmTrasMundo) {
      if (&destino == sm_destino_ciclo_ && sm_estado_ == kSmCoches) {
        SombraMinimoFalloCiclo("el destino de los coches se restaura otra vez", destino.resuelta_base, false);
      }
      return false;
    }
    if (destino.resuelta_base != sm_mundo_ciclo_) {
      SombraMinimoFalloCiclo("el destino se restaura desde otra textura", destino.resuelta_base, false);
      return false;
    }
    sm_estado_ = kSmCoches;
    // Only with the learned cycle: the same render target and the same world texture as in the observing
    // phase.
    if (sm_fase_ != kSmAplicando || !sm_encendido_ || &destino != sm_destino_ || sm_mundo_ciclo_ != sm_mundo_ ||
        !sm_coches_ || !borrar_profundidad_ || !destino.admite_destino_de_copia) {
      return false;
    }
    Preparar(destino);
    MarcarGpu(kGpuBorrados);
    const VkClearDepthStencilValue valor{1.0f, 0};
    borrar_profundidad_(comandos_trabajo_, destino.imagen, VK_IMAGE_LAYOUT_GENERAL, &valor, 1, &kRangoProfundidad);
    AnotarUsoBorrado(destino, destino.ancho, destino.alto);  // nfsmw_nativo_diag_borrados: like the restore
    OlvidarBorrado(destino);  // not a game clear: the game's next clear cannot be skipped
    sm_ciclo_borrado_ = true;
    sm_pixeles_ahorrados_ += uint64_t(destino.ancho) * destino.alto;
    return true;
  }

  // A copy (not a swap) from a depth target: if it is the current cycle's target, something reads its
  // content mid-cycle. With the target already cleared, what gets copied is only the cars.
  void SombraMinimoCopiaDesde(const Imagen& origen) {
    if (&origen == sm_destino_ciclo_ && sm_estado_ != kSmLibre) {
      SombraMinimoFalloCiclo("se copia del destino del mapa de sombras a mitad del ciclo", 0, false);
    }
  }

  // The game clears the current cycle's target without the second resolve: its content is lost the same
  // way on both paths, but in this cycle the texture with the cars is not renewed while the world one
  // is. The cycle does not count.
  void SombraMinimoAntesDeBorrar(const Imagen& destino) {
    if (&destino == sm_destino_ciclo_ && sm_estado_ != kSmLibre) {
      sm_limpios_seguidos_ = 0;
      sm_estado_ = kSmLibre;
    }
  }

  // A resolved texture receives new content (ResueltaEscrita) or is recreated (ObtenerResuelta).
  void SombraMinimoResueltaEscrita(uint32_t base) {
    if (sm_virtual_) {
      if (base == sm_coches_) {
        sm_virtual_ = false;  // new content: no longer only the cars
      } else if (base == sm_mundo_) {
        sm_virtual_valida_ = false;  // its pair changes: reading it now would not give its cycle's minimum
      }
    }
    if (sm_estado_ != kSmLibre && base == sm_mundo_ciclo_) {
      sm_mundo_reescrito_ = true;
    }
  }

  // C2 report line (every 10 s, with the other copy lines). The ms use 0.60 per Mpixel copied and 0.075
  // per Mpixel cleared.
  void InformeSombraMinimo() {
    if (!sm_ciclos_ && !sm_diferencias_ && !sm_lecturas_incapaces_ && !sm_lecturas_a_destiempo_) {
      return;
    }
    const double fotogramas = double(presentados_ - presentados_informe_copias_);
    const double porFot = fotogramas > 0.0 ? 1.0 / fotogramas : 0.0;
    const double mp = double(sm_pixeles_ahorrados_ - sm_pixeles_ahorrados_previos_) / 1e6 * porFot;
    static constexpr const char* kFases[3] = {"mirando", "APLICANDO", "apagada"};
    NFSMW_INFORME_ANILLO(
        "[nativo] C2 sombra por minimo (build 184): fase {}; {} ciclos desde el arranque ({} limpios seguidos, {} "
        "aplicados); {:.2f} Mpixeles por fotograma borrados en vez de copiados (~{:.2f} ms reales de copia menos y ~{:.2f} "
        "de borrado mas); dibujos de los coches {} ({} no exactos); lecturas de {:08X} desde el arranque: {} con el mundo "
        "de pareja, {} consigo misma, {} sin minimo, {} entre las dos resoluciones, {} sin tfetch2DSombraMin; "
        "DIFERENCIAS {}{}",
        kFases[sm_fase_ < 3 ? sm_fase_ : 2], sm_ciclos_, sm_limpios_seguidos_, sm_ciclos_aplicados_, mp, mp * 0.60,
        mp * 0.075, sm_dibujos_coches_, sm_dibujos_no_exactos_, sm_coches_, sm_lecturas_minimo_, sm_lecturas_si_misma_,
        sm_lecturas_normales_, sm_lecturas_a_destiempo_, sm_lecturas_incapaces_, sm_diferencias_,
        sm_diferencias_ ? " *** LA IMAGEN PUEDE HABER CAMBIADO: ver el ERROR del log ***" : " (0 = la imagen es la misma)");
    sm_pixeles_ahorrados_previos_ = sm_pixeles_ahorrados_;
  }

  // The resolved texture has just received new content, so if it was lent it no longer is. An empty set
  // (the normal case) touches nothing. `pixeles` = the pixels actually copied (0 if the resolve swapped
  // images), so that the per-target inventory shows where the traffic goes.
  void ResueltaEscrita(uint32_t base, uint64_t pixeles = 0) {
    SombraMinimoResueltaEscrita(base);  // nfsmw_nativo_sombra_minimo
    if (!prestadas_.empty()) {
      prestadas_.erase(base);
    }
    if (!caducadas_.empty()) {
      caducadas_.erase(base);  // New contents: no dropped copy is missing any more
    }
    if (pixeles) {
      const auto c = copias_por_destino_.find(base);
      if (c != copias_por_destino_.end()) {
        c->second.pixeles += pixeles;
      }
    }
  }

  // The render target's content is no longer that of its last clear (something was copied over it or
  // it changed image). The game's next clear cannot be skipped.
  //
  // It also answers the other question about clears: if the target's content changes without anything
  // drawn since it was cleared, that clear was wiped out by a swap or a restore and served no purpose.
  // It is the equivalent of a loadOp = CLEAR that gets thrown away: these are DONT_CARE candidates.
  void OlvidarBorrado(const Imagen& destino) {
    const auto e = estado_destino_.find(&destino);
    if (e == estado_destino_.end()) {
      return;
    }
    if (e->second.borrado_limpio &&
        e->second.dibujos_al_borrar == (dibujos_ ? dibujos_->Dibujados() : 0)) {
      ++borrados_inutiles_;
      pixeles_borrados_inutiles_ += uint64_t(destino.ancho) * destino.alto;
    }
    e->second.borrado_limpio = false;
  }

  // The largest rectangle the game resolves from this render target: the area it actually uses.
  void AnotarAreaUtil(const Imagen& destino, int32_t y1) {
    if (y1 > 0) {
      auto& e = estado_destino_[&destino];
      e.alto_usado = std::max(e.alto_usado, std::min(uint32_t(y1), destino.alto));
    }
  }

  // A clear that does not change a single bit. It is skipped if the render target is already cleared
  // to that same value and nothing has been drawn, anywhere, since then. Returns true if it must be
  // skipped.
  bool BorradoRedundante(const Imagen& destino, uint64_t valor) {
    auto& e = estado_destino_[&destino];
    const uint64_t dibujados = dibujos_ ? dibujos_->Dibujados() : 0;
    if (REXCVAR_GET(nfsmw_nativo_saltar_borrados_repetidos) && e.borrado_limpio &&
        e.valor_borrado == valor && e.dibujos_al_borrar == dibujados) {
      pixeles_borrados_saltados_ += uint64_t(destino.ancho) * destino.alto;
      return true;
    }
    e.borrado_limpio = true;
    e.valor_borrado = valor;
    e.dibujos_al_borrar = dibujados;
    return false;
  }

  bool Crear(Imagen& imagen, uint32_t ancho, uint32_t alto, VkImageUsageFlags uso,
             VkFormat formato = kFormatoColor) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = formato;
    info.extent = {ancho, alto, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = uso;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
            dispositivo_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, imagen.imagen,
            imagen.memoria)) {
      // The GPU can run out of memory here too. Render targets are few and large, so one that does not fit
      // shows up at once: the texture cache is asked to release memory and the allocation is retried once.
      if (!dibujos_ || !dibujos_->SoltarTexturasPorFaltaDeMemoria() ||
          !rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              dispositivo_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, imagen.imagen,
              imagen.memoria)) {
        REXLOG_ERROR("[nativo] C2: sin memoria en la GPU para un destino de {}x{} (formato {}) y soltar la cache "
                     "no ha bastado",
                     ancho, alto, uint32_t(formato));
        return false;
      }
    }
    VkImageViewCreateInfo info_vista{};
    info_vista.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info_vista.image = imagen.imagen;
    info_vista.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info_vista.format = formato;
    info_vista.subresourceRange = EsProfundidadFormato(formato) ? kRangoProfundidad : kRangoColor;
    if (dfn_.vkCreateImageView(device_, &info_vista, nullptr, &imagen.vista) != VK_SUCCESS) {
      Destruir(imagen);
      return false;
    }
    imagen.ancho = ancho;
    imagen.alto = alto;
    imagen.formato = formato;
    imagen.preparada = false;
    return true;
  }

  void Destruir(Imagen& imagen) {
    CompuestaAlDestruir(imagen);  // nfsmw_nativo_compuesta_perezosa
    // nfsmw_nativo_frontal_perezoso. A deferred copy cannot keep a destroyed image. If the texture is
    // what gets destroyed (ObtenerResuelta recreates it with another size or format), its content is lost
    // just as before: the copy is simply dropped. If it is the source (only at shutdown: render targets
    // are not destroyed, and whatever leaves a target through a swap is recorded first), the texture lacks
    // that copy: it becomes stale and, if something requests it before another full resolve, the guard
    // trips.
    if (!frontales_pendientes_.empty() && imagen.imagen != VK_NULL_HANDLE) {
      for (auto it = frontales_pendientes_.begin(); it != frontales_pendientes_.end();) {
        if (it->second.textura_vk == imagen.imagen) {
          LiberarRetenida(it->second);
          it = frontales_pendientes_.erase(it);
        } else if (it->second.origen_vk == imagen.imagen) {
          frontales_caducados_.insert(it->first);
          LiberarRetenida(it->second);
          it = frontales_pendientes_.erase(it);
        } else {
          ++it;
        }
      }
    }
    // nfsmw_nativo_profundidad_perezosa. Without the image there is no copy to do: its texture becomes
    // stale (if something samples it before the next resolve, the guard trips).
    if (!pendientes_.empty() && imagen.imagen != VK_NULL_HANDLE) {
      for (auto it = pendientes_.begin(); it != pendientes_.end();) {
        if (it->second.origen_vk == imagen.imagen || it->second.destino_vk == imagen.imagen) {
          caducadas_.insert(it->first);
          it = pendientes_.erase(it);
        } else {
          ++it;
        }
      }
    }
    // The DibujosVulkan framebuffers that use this view are destroyed before it: Vulkan may give the same
    // handle to a new view and FramebufferDe would return a stale one (see
    // nfsmw_nativo_framebuffers_olvidan_vistas). At shutdown, dibujos_ is already gone and so are its
    // framebuffers.
    if (dibujos_ && imagen.vista != VK_NULL_HANDLE) {
      dibujos_->OlvidarVista(imagen.vista);
    }
    if (imagen.vista != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, imagen.vista, nullptr);
    if (imagen.imagen != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, imagen.imagen, nullptr);
    if (imagen.memoria != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, imagen.memoria, nullptr);
    imagen = Imagen{};
  }

  // First time: to GENERAL (valid for copy, clear and sampling) and cleared to zero.
  void Preparar(Imagen& imagen) {
    if (imagen.preparada) {
      return;
    }
    // In the upload command buffer, which runs before the work one (the rear-view mirror cubemap copies
    // its resolved faces there and, on its first frame, read them unprepared).
    const VkCommandBuffer comandos = ComandosSubida();
    if (comandos == VK_NULL_HANDLE) {
      return;
    }
    VkImageMemoryBarrier barrera{};
    barrera.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrera.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrera.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrera.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrera.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    const bool profundidad = imagen.formato == VK_FORMAT_D24_UNORM_S8_UINT ||
                             imagen.formato == VK_FORMAT_D32_SFLOAT_S8_UINT;
    barrera.image = imagen.imagen;
    barrera.subresourceRange = profundidad ? kRangoProfundidad : kRangoColor;
    barrera.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(comandos, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrera);
    if (profundidad) {
      // ZCULL: images created without TRANSFER_DST (the ones eligible for a ZCULL plane) cannot be
      // cleared with vkCmdClearDepthStencilImage. They are cleared by opening a pass with
      // loadOp = CLEAR. The barrier above (UNDEFINED -> GENERAL) stays as it is: it is what makes the
      // driver zero the ZCULL plane, and without it the hardware kills the context.
      if (!imagen.admite_destino_de_copia && dibujos_) {
        if (!dibujos_->BorrarProfundidadEnPase(comandos, imagen, 1.0f, 0) &&
            ++borrados_en_pase_fallidos_ <= 8) {
          REXLOG_WARN("[nativo] C2: NO se pudo preparar la profundidad {}x{} abriendo un pase de "
                      "borrado (fallo {})",
                      imagen.ancho, imagen.alto, borrados_en_pase_fallidos_);
        }
      } else if (borrar_profundidad_) {
        const VkClearDepthStencilValue lejos{1.0f, 0};
        borrar_profundidad_(comandos, imagen.imagen, VK_IMAGE_LAYOUT_GENERAL, &lejos, 1,
                            &kRangoProfundidad);
      }
    } else {
      const VkClearColorValue cero{};
      dfn_.vkCmdClearColorImage(comandos, imagen.imagen, VK_IMAGE_LAYOUT_GENERAL, &cero,
                                1, &kRangoColor);
    }
    imagen.preparada = true;
    if (dibujos_) {
      dibujos_->InvalidarTexturas();  // TexturaResuelta only returns prepared ones
    }
  }

  bool Grabar() {
    if (grabando_) {
      return true;
    }
    const auto antes_grabar = std::chrono::steady_clock::now();
    const bool grabando = EmpezarGrabacion();
    ns_grabar_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - antes_grabar).count());
    ++grabaciones_;
    return grabando;
  }

  bool EmpezarGrabacion() {
    // Slots rotate: while one frame is recorded, the previous ones can still be on the GPU. It only waits
    // if this slot's last submission has not finished yet, so the more slots there are, the further ahead
    // the CPU can get before it has to stop.
    ranura_ = (ranura_ + 1) % ranuras_usadas_;
    RanuraTrabajo& ranura = ranuras_[ranura_];
    Completar(ranura);
    const auto antes_pools = std::chrono::steady_clock::now();
    dfn_.vkResetCommandPool(device_, ranura.pool_trabajo, 0);
    dfn_.vkResetCommandPool(device_, ranura.pool_subida, 0);
    ns_reiniciar_pools_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - antes_pools).count());
    comandos_trabajo_ = ranura.trabajo;
    comandos_subida_ = ranura.subida;
    grabando_subida_ = false;
    VkCommandBufferBeginInfo inicio{};
    inicio.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    inicio.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(comandos_trabajo_, &inicio) != VK_SUCCESS) {
      return false;
    }
    ranura.categorias.clear();
    if (consultas_ != VK_NULL_HANDLE) {
      dfn_.vkCmdResetQueryPool(comandos_trabajo_, consultas_, ranura_ * kMarcasPorRanura,
                               kMarcasPorRanura);
    }
    if (oclusiones_ != VK_NULL_HANDLE) {
      DescartarOclusiones(ranura);  // Completar already read the last submission's; these were never submitted
      dfn_.vkCmdResetQueryPool(comandos_trabajo_, oclusiones_, ranura_ * kOclusionesPorRanura,
                               kOclusionesPorRanura);
    }
    // nfsmw_reflejo_visibilidad. Any left over belong to work that was never submitted: without a
    // measurement, they count as visible. The reset always happens when there is a pool, even if the
    // guard trips mid-frame.
    if (visibilidad_ != VK_NULL_HANDLE) {
      if (!ranura.visibilidad.empty()) {
        nfsmw::reflejo_demanda::AnotarVisible(false);
        ranura.visibilidad.clear();
      }
      dfn_.vkCmdResetQueryPool(comandos_trabajo_, visibilidad_, ranura_ * kVisibilidadPorRanura,
                               kVisibilidadPorRanura);
    }
    /*
     * Do not reset pools that this frame will not use.
     *
     * This used to happen always, with any toml: 64 statistics queries + 2048 per-draw statistics
     * queries = 2,144 resets per frame. And in our NVK on Tegra the bulk path is excluded for the layout
     * of these pools (nvk_query_pool.c:351: everything that is not a timestamp falls into
     * ALIGNED_INTERLEAVED), so each reset is a 5-dword SET_REPORT_SEMAPHORE with
     * RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE and PIPELINE_LOCATION_ALL: 2,144 chained writes that
     * wait on each other, 42.9 KB of command stream, at the start of every frame.
     *
     * And they are almost never used: the per-pass statistics are effectively disabled (see the `> 0`
     * check in EmpezarEstadisticas: with estadisticas_por_dibujo_s set, the per-pass query is never
     * opened), and the per-draw ones are only used one frame every 20 seconds.
     *
     * The reset comes before the first MarcarGpu, so its cost does not show up in any category of the
     * breakdown: it is swallowed by the "hueco entre trabajos" (gap between submissions). Estimated at
     * 0.3-1.1 real ms per frame; the measured upper bound (the minimum gap over a whole session) is
     * 2.81 ms.
     *
     * With both off, 32 resets remain instead of 2,144: -98.5 %. The CPU vectors are cleared anyway,
     * which costs nothing.
     */
    ranura.estadisticas.clear();  // those never submitted are not read
    ranura.estadisticas_dibujo.clear();
    if (estadisticas_ != VK_NULL_HANDLE && REXCVAR_GET(nfsmw_nativo_estadisticas_pipeline) &&
        REXCVAR_GET(nfsmw_nativo_estadisticas_por_dibujo_s) <= 0) {
      dfn_.vkCmdResetQueryPool(comandos_trabajo_, estadisticas_,
                               ranura_ * kEstadisticasPorRanura, kEstadisticasPorRanura);
    }
    if (estadisticas_dibujo_ != VK_NULL_HANDLE && ventana_diagnostico_) {
      dfn_.vkCmdResetQueryPool(comandos_trabajo_, estadisticas_dibujo_,
                               ranura_ * kEstadisticasDibujoPorRanura,
                               kEstadisticasDibujoPorRanura);
    }
    // The timestamp mode is decided per whole submission.
    if (alternar_marcas_s_ > 0) {
      const int64_t segundos = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - inicio_marcas_)
                                   .count();
      const bool precisas = (segundos / alternar_marcas_s_) % 2 == 1;
      if (precisas != marcas_precisas_) {
        marcas_precisas_ = precisas;
        REXLOG_INFO("[nativo] prueba de marcas: {} (trabajo {})", precisas ? "precisas" : "normales",
                    generacion_comandos_);
      }
    }
    ranura.marcas_precisas = marcas_precisas_;
    grabando_ = true;
    MarcarGpu(kGpuOtros);
    ++generacion_comandos_;
    if (dibujos_) {
      dibujos_->UsarRanura(ranura_);
    }
    return true;
  }

  // Waits for that slot's submission. Earlier submissions that have already finished are collected
  // first, so read-backs are written in frame order.
  /*
   * This was what prevented CPU and GPU from overlapping.
   *
   * It used to wait as well for every earlier slot still pending, so each frame drained the whole GPU
   * before recording continued. That is why the report always said "trabajos solapados en la GPU 0",
   * why adding a third slot changed nothing (it waited for all of them, however many there were) and
   * why the wait moved elsewhere instead of disappearing.
   *
   * Now it only blocks on the slot about to be reused, the only one whose resources are needed. Earlier
   * ones are collected only if they have already finished (polling without waiting), so their
   * timestamps are read in order when possible; one still running is collected when its turn comes.
   */
  void Completar(RanuraTrabajo& ranura) {
    for (RanuraTrabajo& otra : ranuras_) {
      if (&otra != &ranura && otra.pendiente && otra.orden < ranura.orden &&
          dfn_.vkGetFenceStatus(device_, otra.fence) == VK_SUCCESS) {
        CompletarUna(otra);
      }
    }
    CompletarUna(ranura);
  }

  void CompletarUna(RanuraTrabajo& ranura) {
    if (!ranura.pendiente) {
      return;
    }
    const auto antes_espera = std::chrono::steady_clock::now();
    dfn_.vkWaitForFences(device_, 1, &ranura.fence, VK_TRUE, UINT64_MAX);
    ns_esperas_gpu_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - antes_espera)
                                    .count());
    ++esperas_gpu_;
    if (ranura.enviado.time_since_epoch().count()) {
      ns_trabajo_gpu_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - ranura.enviado)
                                      .count());
      ++trabajos_gpu_;
    }
    dfn_.vkResetFences(device_, 1, &ranura.fence);
    ranura.pendiente = false;
    const uint32_t marcadas = uint32_t(ranura.categorias.size());
    if (consultas_ != VK_NULL_HANDLE && marcadas >= 2) {
      std::array<uint64_t, kMarcasPorRanura> marcas{};
      const uint32_t indice = uint32_t(&ranura - ranuras_.data());
      if (leer_consultas_(device_, consultas_, indice * kMarcasPorRanura, marcadas,
                          sizeof(uint64_t) * marcadas, marcas.data(), sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          marcas[marcadas - 1] >= marcas[0]) {
        for (uint32_t i = 0; i + 1 < marcadas; ++i) {
          if (marcas[i + 1] >= marcas[i] && ranura.categorias[i] < kGpuCategorias) {
            gpu_categorias_ns_[ranura.categorias[i]] +=
                uint64_t(double(marcas[i + 1] - marcas[i]) * periodo_marca_ns_);
          }
        }
        // GPU gap since the end of the previous submission (C2 report). Submissions are read in the order
        // they were sent; if one could not be read, marca_fin_previa_ is 0 and that gap is not counted.
        if (marca_fin_previa_ != 0) {
          if (marcas[0] >= marca_fin_previa_) {
            gpu_categorias_ns_[kGpuHuecoEntreTrabajos] +=
                uint64_t(double(marcas[0] - marca_fin_previa_) * periodo_marca_ns_);
          } else {
            ++solapes_gpu_;
          }
        }
        marca_fin_previa_ = marcas[marcadas - 1];
        ultima_marca_gpu_ = marcas[marcadas - 1];
        gpu_ns_ += uint64_t(double(marcas[marcadas - 1] - marcas[0]) * periodo_marca_ns_);
        ++gpu_trabajos_;
        if (ranura.marcas_precisas) {
          ++gpu_trabajos_precisos_;
        }
      } else {
        marca_fin_previa_ = 0;  // without this submission's timestamps, the next gap is not counted
      }
    } else {
      marca_fin_previa_ = 0;
    }
    ranura.categorias.clear();
    LeerOclusiones(ranura);
    LeerVisibilidad(ranura);
    LeerEstadisticas(ranura);
    LeerEstadisticasDibujo(ranura);
    EscribirLecturas(ranura.lecturas);
  }

  // Per-draw statistics of a finished submission, added to its pixel shader.
  void LeerEstadisticasDibujo(RanuraTrabajo& ranura) {
    if (ranura.estadisticas_dibujo.empty()) {
      return;
    }
    const uint32_t n = uint32_t(ranura.estadisticas_dibujo.size());
    std::vector<uint64_t> valores(size_t(n) * kContadoresEstadistica, 0);
    if (estadisticas_dibujo_ != VK_NULL_HANDLE && leer_consultas_ &&
        leer_consultas_(device_, estadisticas_dibujo_, ranura.estadisticas_dibujo.front().first, n,
                        sizeof(uint64_t) * valores.size(), valores.data(),
                        sizeof(uint64_t) * kContadoresEstadistica,
                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint16_t etiqueta = ranura.estadisticas_dibujo[i].second;
        fragmentos_por_shader_[etiqueta] += valores[i * kContadoresEstadistica + 2];
        ++dibujos_por_shader_[etiqueta];
      }
      // A diagnostic frame may span several submissions: the first one of each window counts.
      if (!ventana_leida_) {
        ventana_leida_ = true;
        ++fotogramas_diagnostico_;
      }
    }
    ranura.estadisticas_dibujo.clear();
  }

  // Pass statistics of a finished submission, added to their category.
  void LeerEstadisticas(RanuraTrabajo& ranura) {
    if (ranura.estadisticas.empty()) {
      return;
    }
    const uint32_t n = uint32_t(ranura.estadisticas.size());
    std::array<uint64_t, kEstadisticasPorRanura * kContadoresEstadistica> valores{};
    if (estadisticas_ != VK_NULL_HANDLE && leer_consultas_ &&
        leer_consultas_(device_, estadisticas_, ranura.estadisticas.front().first, n,
                        sizeof(uint64_t) * kContadoresEstadistica * n, valores.data(),
                        sizeof(uint64_t) * kContadoresEstadistica,
                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint8_t c = ranura.estadisticas[i].second;
        if (c < kGpuCategorias) {
          vertices_categoria_[c] += valores[i * kContadoresEstadistica + 0];
          primitivas_categoria_[c] += valores[i * kContadoresEstadistica + 1];
          fragmentos_categoria_[c] += valores[i * kContadoresEstadistica + 2];
        }
      }
    }
    ranura.estadisticas.clear();
  }

  // Occlusion queries of a finished submission. They add to their game query, which is published once
  // it has its Issue(END) and all its spans have been read.
  void LeerOclusiones(RanuraTrabajo& ranura) {
    if (ranura.oclusiones.empty()) {
      return;
    }
    const uint32_t n = uint32_t(ranura.oclusiones.size());
    std::array<uint64_t, kOclusionesPorRanura> cuentas{};
    const bool leidas = oclusiones_ != VK_NULL_HANDLE && leer_consultas_ &&
                        leer_consultas_(device_, oclusiones_, ranura.oclusiones.front().first, n,
                                        sizeof(uint64_t) * n, cuentas.data(), sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = oclusiones_juego_.find(ranura.oclusiones[i].second);
      if (it == oclusiones_juego_.end()) {
        continue;
      }
      if (leidas) {
        it->second.muestras += cuentas[i];
      } else {
        it->second.fallida = true;
      }
      if (it->second.tramos_pendientes) {
        --it->second.tramos_pendientes;
      }
      if (it->second.terminada && it->second.tramos_pendientes == 0) {
        PublicarOclusion(it);
      }
    }
    ranura.oclusiones.clear();
  }

  // nfsmw_reflejo_visibilidad. Our own queries of a finished submission: for each water draw, whether
  // it left any sample; the witness must always leave some. If they cannot be read, the water counts as
  // visible and the witness counts neither for nor against.
  void LeerVisibilidad(RanuraTrabajo& ranura) {
    if (ranura.visibilidad.empty()) {
      return;
    }
    const uint32_t n = uint32_t(ranura.visibilidad.size());
    std::array<uint64_t, kVisibilidadPorRanura> cuentas{};
    const bool leidas = visibilidad_ != VK_NULL_HANDLE && leer_consultas_ &&
                        leer_consultas_(device_, visibilidad_, ranura.visibilidad.front().first, n,
                                        sizeof(uint64_t) * n, cuentas.data(), sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    for (uint32_t i = 0; i < n; ++i) {
      if (ranura.visibilidad[i].second == kVisibilidadTestigo) {
        if (leidas) {
          nfsmw::reflejo_demanda::AnotarTestigo(cuentas[i] != 0);
        }
      } else if (!leidas) {
        nfsmw::reflejo_demanda::AnotarVisible(false);
      } else if (cuentas[i] != 0) {
        nfsmw::reflejo_demanda::AnotarVisible(true);
      } else {
        nfsmw::reflejo_demanda::AnotarOculto();
      }
    }
    ranura.visibilidad.clear();
  }

  // Spans recorded in a submission that was never sent: their game query is not published.
  void DescartarOclusiones(RanuraTrabajo& ranura) {
    for (const auto& [indice, id] : ranura.oclusiones) {
      auto it = oclusiones_juego_.find(id);
      if (it != oclusiones_juego_.end()) {
        it->second.fallida = true;
        if (it->second.tramos_pendientes) {
          --it->second.tramos_pendientes;
        }
        if (it->second.terminada && it->second.tramos_pendientes == 0) {
          PublicarOclusion(it);
        }
      }
    }
    ranura.oclusiones.clear();
  }

  void PublicarOclusion(std::unordered_map<uint64_t, ConsultaOclusionJuego>::iterator it) {
    if (!it->second.fallida) {
      if (oclusion_por_base_.size() >= 64 && !oclusion_por_base_.count(it->second.base)) {
        oclusion_por_base_.clear();
      }
      oclusion_por_base_[it->second.base] = it->second.muestras;
      ++estadisticas_oclusion_[2];
      estadisticas_oclusion_[3] += it->second.muestras;
      estadisticas_oclusion_[4] = std::max(estadisticas_oclusion_[4], it->second.muestras);
    }
    oclusiones_juego_.erase(it);
  }

  bool EnviarTrabajo(bool esperar) {
    if (grabando_) {
      if (dibujos_) {
        dibujos_->AntesDeEnviar();  // closes the pass and publishes the upload buffer
      }
      grabando_ = false;
      std::array<VkCommandBuffer, 2> bufers{};
      uint32_t n = 0;
      if (grabando_subida_) {
        grabando_subida_ = false;
        if (REXCVAR_GET(nfsmw_nativo_sincronizacion_gpu)) {
          // The following work buffer samples the newly uploaded textures and
          // cubemap layers. Keep the dependency even when their layout is GENERAL.
          VkMemoryBarrier barrera{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
          barrera.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          barrera.dstAccessMask = kAccesosImagenes;
          dfn_.vkCmdPipelineBarrier(comandos_subida_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    kEtapasImagenes, 0, 1, &barrera, 0, nullptr, 0, nullptr);
        }
        if (dfn_.vkEndCommandBuffer(comandos_subida_) != VK_SUCCESS) {
          return false;
        }
        bufers[n++] = comandos_subida_;
      }
      if (consultas_ != VK_NULL_HANDLE) {
        // Final mark: closes the last span (MarcarGpu leaves room for it).
        RanuraTrabajo& marcada = ranuras_[ranura_];
        escribir_marca_(comandos_trabajo_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, consultas_,
                        ranura_ * kMarcasPorRanura + uint32_t(marcada.categorias.size()));
        marcada.categorias.push_back(kGpuFin);
      }
      if (dfn_.vkEndCommandBuffer(comandos_trabajo_) != VK_SUCCESS) {
        return false;
      }
      bufers[n++] = comandos_trabajo_;
      VkSubmitInfo envio{};
      envio.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      envio.commandBufferCount = n;
      envio.pCommandBuffers = bufers.data();
      {
        const auto antes_candado = std::chrono::steady_clock::now();  // "C2: presentar" report
        const auto cola = dispositivo_->AcquireQueue(familia_, 0);
        const auto antes_envio = std::chrono::steady_clock::now();
        if (dfn_.vkQueueSubmit(cola.queue(), 1, &envio, ranuras_[ranura_].fence) != VK_SUCCESS) {
          return false;
        }
        ns_candado_trabajo_ += Ns(antes_candado, antes_envio);
        ns_submit_trabajo_ += Ns(antes_envio, std::chrono::steady_clock::now());
        ++envios_trabajo_;
      }
      RanuraTrabajo& ranura = ranuras_[ranura_];
      ranura.pendiente = true;
      ranura.orden = ++envios_;
      ranura.enviado = std::chrono::steady_clock::now();
      // Read-backs recorded in this submission are written when it finishes.
      ranura.lecturas.insert(ranura.lecturas.end(), lecturas_pendientes_.begin(),
                             lecturas_pendientes_.end());
      lecturas_pendientes_.clear();
    }
    if (esperar) {
      Completar(ranuras_[ranura_]);
    }
    return true;
  }

  void EsperarGpu() {
    if (grabando_) {
      EnviarTrabajo(false);
    }
    for (RanuraTrabajo& ranura : ranuras_) {
      Completar(ranura);
    }
    EsperarSalidas();
  }

  // All pending outputs, before destroying or reusing what they use.
  void EsperarSalidas() {
    for (uint32_t i = 0; i < kRanurasSalida; ++i) {
      if (salidas_pendientes_[i]) {
        dfn_.vkWaitForFences(device_, 1, &fences_salida_[i], VK_TRUE, UINT64_MAX);
        dfn_.vkResetFences(device_, 1, &fences_salida_[i]);
        salidas_pendientes_[i] = false;
      }
    }
  }

  // Diagnostic (nfsmw_nativo_diag_resueltas): the frame's resolved textures in a 4x4 grid over
  // 1280x720, in the order of their first copy. Cells without a texture stay dark purple.
  bool ComponerMosaico(uint32_t base_swap) {
    std::vector<uint32_t> bases;
    bases.swap(resueltas_fotograma_);
    if (bases.empty() || !blit_) {
      return false;
    }
    if (mosaico_.imagen == VK_NULL_HANDLE &&
        !Crear(mosaico_, 1280, 720, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      blit_ = nullptr;
      return false;
    }
    if (dibujos_) {
      dibujos_->TerminarPase();
    }
    if (!Grabar()) {
      return false;
    }
    Preparar(mosaico_);
    const VkClearColorValue fondo{{0.12f, 0.0f, 0.12f, 1.0f}};
    dfn_.vkCmdClearColorImage(comandos_trabajo_, mosaico_.imagen, VK_IMAGE_LAYOUT_GENERAL, &fondo,
                              1, &kRangoColor);
    std::string lista;
    for (size_t i = 0; i < bases.size() && i < 16; ++i) {
      const auto it = resueltas_.find(bases[i]);
      if (it == resueltas_.end() || !it->second.imagen.preparada ||
          it->second.imagen.formato != kFormatoColor) {
        continue;  // depth ones do not support blits to color
      }
      const Imagen& imagen = it->second.imagen;
      const int32_t x = int32_t(i % 4) * 320;
      const int32_t y = int32_t(i / 4) * 180;
      VkImageBlit copia{};
      copia.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copia.srcOffsets[1] = {int32_t(imagen.ancho), int32_t(imagen.alto), 1};
      copia.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copia.dstOffsets[0] = {x + 2, y + 2, 0};
      copia.dstOffsets[1] = {x + 318, y + 178, 1};
      blit_(comandos_trabajo_, imagen.imagen, VK_IMAGE_LAYOUT_GENERAL, mosaico_.imagen,
            VK_IMAGE_LAYOUT_GENERAL, 1, &copia, VK_FILTER_LINEAR);
      lista += fmt::format(" {}{}:{:08X} {}x{}", i, bases[i] == base_swap ? "*" : "", bases[i],
                           imagen.ancho, imagen.alto);
    }
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - ultimo_aviso_mosaico_ >= std::chrono::seconds(10)) {
      ultimo_aviso_mosaico_ = ahora;
      REXLOG_INFO("[nativo] diag resueltas: {} en el fotograma (* = la del Swap):{}", bases.size(),
                  lista);
    }
    return true;
  }

  // Rear-view mirror diagnostic: for each resolved texture, how many copies there are and how many had
  // draws since the previous copy (the car's cubemap faces are 256x256). A face resolved without draws
  // copies again what the render target already had.
  // It is no longer limited to square textures up to 512. The copy inventory has to be complete
  // (address, size, Mpixels and reads) because it is the only thing that tells whether a copy is
  // needed. There are ~20 addresses in a race, which fit easily in one line.
  void AnotarCopia(uint32_t base, uint32_t ancho, uint32_t alto, uint64_t dibujos) {
    if (base == nfsmw::reflejo_demanda::kDireccion) {
      nfsmw::reflejo_demanda::AnotarCopia();  // nfsmw_reflejo_bajo_demanda
    }
    CopiaDestino& d = copias_por_destino_[base];
    d.ancho = ancho;
    d.alto = alto;
    ++d.copias;
    d.dibujos += dibujos;
    d.con_dibujos += dibujos != 0 ? 1 : 0;
    const auto ahora = std::chrono::steady_clock::now();
    if (ahora - informe_copias_ < std::chrono::seconds(10)) {
      return;
    }
    informe_copias_ = ahora;
    std::vector<std::pair<uint32_t, CopiaDestino>> orden(copias_por_destino_.begin(),
                                                         copias_por_destino_.end());
    std::sort(orden.begin(), orden.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string lista;
    uint32_t escritas = 0;
    for (const auto& [direccion, c] : orden) {
      if (++escritas > 48) {  // defensive cap: ~20 in a race, but a load can touch many more
        lista += fmt::format(" (y {} direcciones mas)", orden.size() - 48);
        break;
      }
      const auto r = resueltas_.find(direccion);
      const uint64_t lecturas = r != resueltas_.end() ? r->second.lecturas : 0;
      if (r != resueltas_.end()) {
        r->second.lecturas = 0;
      }
      lista += fmt::format(" {:08X} {}x{}: {} copias, {} con dibujos ({:.1f} dibujos por copia), "
                           "{:.2f} Mpixeles, {} lecturas{};",
                           direccion, c.ancho, c.alto, c.copias, c.con_dibujos,
                           c.copias ? double(c.dibujos) / double(c.copias) : 0.0,
                           double(c.pixeles) / 1e6, lecturas,
                           c.copias && !lecturas ? " *** NADIE LA LEE ***" : "");
    }
    copias_por_destino_.clear();
    NFSMW_INFORME_ANILLO("[nativo] C2 caras resueltas desde el informe anterior:{}", lista);
    if (intercambios_ || restauraciones_) {
      // Restores are the verdict. If they go up, the swap without a clear saves nothing: the same copy is
      // paid, just later.
      NFSMW_INFORME_ANILLO("[nativo] C2 resoluciones sin copia: {} intercambios ({} sin borrado, {} de COLOR) y {} "
                  "restauraciones desde el arranque -> {}",
                  intercambios_, intercambios_sin_borrado_, intercambios_color_, restauraciones_,
                  restauraciones_ == 0 ? "ni una restauracion: el ahorro es limpio"
                                       : "*** hay restauraciones: el ahorro NO es limpio ***");
      // nfsmw_nativo_resolver_contenido_valido. In the menu, one per frame with shadows; in a race, 0. The
      // "sin origen" count is resolves of a render target whose content is no longer anywhere (must be
      // 0); the other count is the resolves that, with the setting off, would read the stale image.
      if (restauraciones_para_resolver_ || resolver_sin_origen_ || resolver_contenido_viejo_) {
        NFSMW_INFORME_ANILLO("[nativo] C2 resolver con contenido valido (build 193): {} veces se trajo de vuelta el "
                             "contenido antes de resolver (antes el resolve leia la imagen del fotograma anterior: el "
                             "parpadeo del menu); {} sin origen; {} leidas como la 192 (ajuste apagado)",
                             restauraciones_para_resolver_, resolver_sin_origen_, resolver_contenido_viejo_);
      }
      // And what they cost per frame, which is the only thing that decides. 0.78 real ms per Mpixel
      // copied and 0.075 per Mpixel cleared, measured here.
      const double fotogramas = double(presentados_ - presentados_informe_copias_);
      const double porFot = fotogramas > 0.0 ? 1.0 / fotogramas : 0.0;
      const double mp_rest = double(pixeles_restaurados_ - pixeles_restaurados_previos_) / 1e6;
      const double mp_ahorro =
          double(pixeles_restaurar_ahorrados_ - pixeles_restaurar_ahorrados_previos_) / 1e6;
      NFSMW_INFORME_ANILLO("[nativo] C2 restauraciones por fotograma: {:.2f} Mpixeles copiados ({:.2f} ms "
                  "reales) y {:.2f} Mpixeles ahorrados ({:.2f} ms); desde el arranque: {} recortadas "
                  "al area util y {} por intercambio (prestadas leidas {}{})",
                  mp_rest * porFot, mp_rest * porFot * 0.78, mp_ahorro * porFot,
                  mp_ahorro * porFot * 0.78, restauraciones_recortadas_, prestamos_,
                  prestadas_leidas_,
                  prestadas_leidas_ ? " *** EL PRESTAMO NO ES SEGURO ***" : ", limpio");
      // And why the restore does not swap, which is what has to be known to fix it.
      if (no_swap_sin_transfer_dst_ || no_swap_destino_sin_preparar_ ||
          no_swap_resuelta_sin_preparar_ || no_swap_tamanos_distintos_) {
        NFSMW_INFORME_ANILLO(
            "[nativo] C2 restaurar por intercambio, veces que NO se pudo: {} sin TRANSFER_DST, "
            "{} con el destino sin preparar, {} con la resuelta sin preparar; y de las que si "
            "cumplian, {} tenian tamanos distintos (esas romperian la imagen)",
            no_swap_sin_transfer_dst_, no_swap_destino_sin_preparar_,
            no_swap_resuelta_sin_preparar_, no_swap_tamanos_distintos_);
      }
      pixeles_restaurados_previos_ = pixeles_restaurados_;
      pixeles_restaurar_ahorrados_previos_ = pixeles_restaurar_ahorrados_;
    }
    if (borrados_saltados_ || borrados_saltados_profundidad_) {
      // Clears that did not change a single bit (nfsmw_nativo_saltar_borrados_repetidos). A clear costs
      // 0.075 real ms per Mpixel: ten times less than a copy, but it adds up.
      const double fotogramas = double(presentados_ - presentados_informe_copias_);
      const double porFot = fotogramas > 0.0 ? 1.0 / fotogramas : 0.0;
      const double mp = double(pixeles_borrados_saltados_ - pixeles_borrados_saltados_previos_) / 1e6;
      NFSMW_INFORME_ANILLO("[nativo] C2 borrados saltados: {} de color y {} de profundidad desde el arranque; "
                  "{:.2f} Mpixeles por fotograma sin borrar ({:.2f} ms reales)",
                  borrados_saltados_, borrados_saltados_profundidad_, mp * porFot,
                  mp * porFot * 0.075);
      pixeles_borrados_saltados_previos_ = pixeles_borrados_saltados_;
    }
    if (borrados_inutiles_) {
      // Clears wiped out by a swap or a restore before anything was drawn. These are the real candidates
      // for removal (the equivalent of a loadOp = DONT_CARE).
      const double fotogramas = double(presentados_ - presentados_informe_copias_);
      const double porFot = fotogramas > 0.0 ? 1.0 / fotogramas : 0.0;
      const double mp = double(pixeles_borrados_inutiles_ - pixeles_borrados_inutiles_previos_) / 1e6;
      NFSMW_INFORME_ANILLO("[nativo] C2 borrados que no sirvieron para nada (el destino cambio de contenido sin "
                  "que se dibujara nada): {} desde el arranque; {:.2f} Mpixeles por fotograma "
                  "({:.2f} ms reales)",
                  borrados_inutiles_, mp * porFot, mp * porFot * 0.075);
      pixeles_borrados_inutiles_previos_ = pixeles_borrados_inutiles_;
    }
    if (perezosa_aplazadas_ || perezosa_lecturas_tardias_) {  // nfsmw_nativo_profundidad_perezosa
      // 0.60 real ms per Mpixel copied: the 1600x1600 shadow map copy raised copies from 1.93 to 3.47 ms.
      const double fotogramas = double(presentados_ - presentados_informe_copias_);
      const double porFot = fotogramas > 0.0 ? 1.0 / fotogramas : 0.0;
      const double mp = double(perezosa_pixeles_ahorrados_ - perezosa_pixeles_ahorrados_previos_) / 1e6;
      NFSMW_INFORME_ANILLO("[nativo] C2 profundidad perezosa (build 184): {} resoluciones de profundidad aplazadas "
                           "desde el arranque; {} grabadas al muestrearlas, {} antes de volver a escribir su origen o "
                           "de otro resolve, {} sustituidas por un intercambio y {} tiradas sin copiar (nadie las "
                           "muestreo): {:.2f} Mpixeles por fotograma sin copiar (~{:.2f} ms reales); lecturas "
                           "tardias {}{}",
                           perezosa_aplazadas_, perezosa_copiadas_lectura_, perezosa_copiadas_escritura_,
                           perezosa_sustituidas_, perezosa_tiradas_, mp * porFot, mp * porFot * 0.60,
                           perezosa_lecturas_tardias_,
                           perezosa_apagada_ ? " *** APAGADA POR LA GUARDIA ***" : " (0 = la imagen es la misma)");
      perezosa_pixeles_ahorrados_previos_ = perezosa_pixeles_ahorrados_;
    }
    if (frontal_aplazadas_ || frontal_lecturas_tardias_) {  // nfsmw_nativo_frontal_perezoso
      const double fotogramas = double(presentados_ - presentados_informe_copias_);
      const double porFot = fotogramas > 0.0 ? 1.0 / fotogramas : 0.0;
      const double mp = double(frontal_pixeles_ahorrados_ - frontal_pixeles_ahorrados_previos_) / 1e6;
      NFSMW_INFORME_ANILLO("[nativo] C2 frontal perezoso (build 184): {} copias a frontales aplazadas desde el arranque; "
                           "Swaps pintados desde el destino {} y desde una imagen retenida {}; {} borrados sobre una "
                           "imagen de repuesto ({} sin repuesto); grabadas: {} al muestrearlas, {} antes de volver a "
                           "escribir el destino u otro resolve, {} en el Swap (FXAA, sin rampa u otra salida); {} tiradas "
                           "sin copiar (otro resolve las tapa enteras): {:.2f} Mpixeles por fotograma sin copiar (~{:.2f} "
                           "ms reales); {} imagenes de repuesto; lecturas tardias {}{}",
                           frontal_aplazadas_, frontal_pintadas_destino_, frontal_pintadas_retenida_,
                           frontal_rotaciones_, frontal_sin_repuesto_, frontal_copiadas_lectura_,
                           frontal_copiadas_escritura_, frontal_copiadas_swap_, frontal_sustituidas_, mp * porFot,
                           mp * porFot * 0.60, frontales_imagenes_.size(), frontal_lecturas_tardias_,
                           frontal_apagado_ ? " *** APAGADO POR LA GUARDIA ***" : " (0 = la imagen es la misma)");
      frontal_pixeles_ahorrados_previos_ = frontal_pixeles_ahorrados_;
    }
    InformeSombraMinimo();  // nfsmw_nativo_sombra_minimo
    InformeBorrados();  // nfsmw_nativo_diag_borrados, every 20 s
    presentados_informe_copias_ = presentados_;
    if (sin_intercambio_[0] || sin_intercambio_[1] || sin_intercambio_[2]) {
      NFSMW_INFORME_ANILLO("[nativo] C2 copias de profundidad sin intercambiar desde el informe anterior: {} porque la "
                  "orden no borra el destino, {} porque no se resuelve entero, {} porque no se pudo",
                  sin_intercambio_[0], sin_intercambio_[1], sin_intercambio_[2]);
      sin_intercambio_ = {};
    }
  }

  // Read-back of a small resolved texture: the copied rectangle goes to a host-visible buffer in the
  // same submission; EscribirLecturas moves it into guest memory.
  void LeerResuelta(const RegistrosCopia& reg, const Resuelta& resuelta, int32_t x0, int32_t y0,
                    uint32_t dx, uint32_t dy, uint32_t ancho, uint32_t alto) {
    const int32_t maximo = REXCVAR_GET(nfsmw_nativo_leer_resueltas_texels);
    if (maximo <= 0 || uint64_t(ancho) * alto > uint64_t(maximo) ||
        resuelta.imagen.formato != kFormatoColor) {
      return;
    }
    const uint64_t clave_destino =
        (uint64_t(reg.rb_copy_dest_base) << 28) ^ (uint64_t(ancho) << 14) ^ uint64_t(alto);
    DestinoLectura& destino = lecturas_por_destino_[clave_destino];
    destino.base = reg.rb_copy_dest_base;
    destino.ancho = ancho;
    destino.alto = alto;
    const uint64_t cada = uint64_t(std::max(REXCVAR_GET(nfsmw_nativo_lecturas_cada), 1));
    if (destino.copias++ % cada != 0) {
      ++destino.saltadas;  // exposure changes slowly: the guest keeps the previous one
      return;
    }
    const VkDeviceSize bytes = VkDeviceSize(ancho) * alto * 4;
    const uint64_t clave = (uint64_t(reg.rb_copy_dest_base) << 32) ^ (uint64_t(uint32_t(x0)) << 16) ^
                           uint64_t(uint32_t(y0));
    // One buffer per slot: the previous submission may still be copying into the other slot's.
    Lectura* puntero = &lecturas_[clave ^ (uint64_t(ranura_) << 63)];
    if (puntero->bytes < bytes && puntero->bufer != VK_NULL_HANDLE) {
      // Larger: the old buffer may have recorded or submitted copies.
      EnviarTrabajo(true);
      EsperarGpu();
      DestruirLectura(*puntero);
      if (!Grabar()) {
        return;
      }
      puntero = &lecturas_[clave ^ (uint64_t(ranura_) << 63)];
      if (puntero->bytes < bytes && puntero->bufer != VK_NULL_HANDLE) {
        DestruirLectura(*puntero);  // the GPU has nothing pending any more
      }
    }
    Lectura& lectura = *puntero;
    if (lectura.bytes < bytes) {
      uint32_t tipo = 0;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              dispositivo_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kReadback, lectura.bufer, lectura.memoria, &tipo)) {
        lectura = Lectura{};
        Rechazar(16, "no se pudo crear un bufer de lectura de una textura resuelta");
        return;
      }
      void* mapeado = nullptr;
      if (dfn_.vkMapMemory(device_, lectura.memoria, 0, VK_WHOLE_SIZE, 0, &mapeado) != VK_SUCCESS) {
        DestruirLectura(lectura);
        Rechazar(16, "no se pudo crear un bufer de lectura de una textura resuelta");
        return;
      }
      lectura.datos = static_cast<uint8_t*>(mapeado);
      lectura.bytes = bytes;
      lectura.coherente = (dispositivo_->memory_types().host_coherent >> tipo) & 0x1;
    }
    VkBufferImageCopy copia{};
    copia.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copia.imageOffset = {int32_t(dx), int32_t(dy), 0};
    copia.imageExtent = {ancho, alto, 1};
    dfn_.vkCmdCopyImageToBuffer(comandos_trabajo_, resuelta.imagen.imagen, VK_IMAGE_LAYOUT_GENERAL,
                                lectura.bufer, 1, &copia);
    lecturas_pendientes_.push_back({&lectura, reg.rb_copy_dest_base, x0, y0, ancho, alto,
                                    reg.rb_copy_dest_pitch & 0x3FFF,
                                    (reg.rb_copy_dest_pitch >> 16) & 0x3FFF, reg.rb_copy_dest_info});
    if (lecturas_hechas_++ == 0) {
      REXLOG_INFO("[nativo] C2: lectura de vuelta de texturas resueltas de hasta {} texels (la "
                  "primera: {:08X}, {}x{})",
                  maximo, reg.rb_copy_dest_base, ancho, alto);
    }
  }

  // The submission finished: the submitted read-backs go to guest memory. Loaded with its fetch
  // constant (GpuSwap per word and R8G8B8A8), each texel must give the same channels as the resolved
  // texture on the GPU: (B, G, R, A) with copy_dest_swap and (R, G, B, A) without it.
  void EscribirLecturas(std::vector<LecturaPendiente>& lecturas) {
    const auto antes_lecturas = std::chrono::steady_clock::now();
    if (antes_lecturas - informe_lecturas_ >= std::chrono::seconds(10)) {
      std::string lista;
      for (auto it = lecturas_por_destino_.begin(); it != lecturas_por_destino_.end();) {
        DestinoLectura& d = it->second;
        if (!d.copias) {
          it = lecturas_por_destino_.erase(it);  // no ha vuelto a aparecer
          continue;
        }
        lista += fmt::format(" {:08X} {}x{} {}/{};", d.base, d.ancho, d.alto, d.copias - d.saltadas,
                             d.copias);
        d.copias = 0;
        d.saltadas = 0;
        ++it;
      }
      if (!lista.empty()) {
        NFSMW_INFORME_ANILLO("[nativo] C2 lecturas de vuelta por destino (hechas/copias):{}", lista);
      }
      informe_lecturas_ = antes_lecturas;
    }
    for (const LecturaPendiente& p : lecturas) {
      const Lectura& lectura = *p.lectura;
      if (!lectura.datos) {
        continue;
      }
      ++lecturas_escritas_;
      texels_escritos_ += uint64_t(p.ancho) * p.alto;
      if (!lectura.coherente) {
        VkMappedMemoryRange rango{};
        rango.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        rango.memory = lectura.memoria;
        rango.size = VK_WHOLE_SIZE;
        dfn_.vkInvalidateMappedMemoryRanges(device_, 1, &rango);
      }
      const bool intercambio = (p.info >> 24) & 0x1;
      const uint32_t orden_copia = p.info & 0x7;  // Endian128: 0-3 as xenos::Endian
      const auto orden =
          orden_copia <= 3 ? static_cast<xenos::Endian>(orden_copia) : xenos::Endian::k8in32;
      // Each texel is a permutation of its 4 bytes (R/B swap plus GpuSwap): it is computed once with
      // marked bytes. Addresses come from the tile and the tiling table, and the guest's physical memory
      // is contiguous.
      const uint32_t permutacion =
          xenos::GpuSwap(intercambio ? uint32_t(0x03000102) : uint32_t(0x03020100), orden);
      const uint8_t b0 = uint8_t(permutacion), b1 = uint8_t(permutacion >> 8),
                    b2 = uint8_t(permutacion >> 16), b3 = uint8_t(permutacion >> 24);
      uint8_t* const fisica = memoria_->TranslatePhysical(0);
      const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);
      const auto& tabla = TablaMosaico2DTexel4();
      const uint64_t baldosas_por_fila = ((p.pitch + 31) & ~uint32_t(31)) >> 5;
      for (uint32_t j = 0; j < p.alto; ++j) {
        const uint32_t ty = uint32_t(p.y0) + j;
        if (p.alto_destino && ty >= p.alto_destino) {
          break;
        }
        const uint64_t fila = base + ((uint64_t(ty >> 5) * baldosas_por_fila) << 12);
        const uint16_t* const local = tabla.data() + size_t(ty & 31) * 32;
        const uint8_t* s = lectura.datos + size_t(j) * p.ancho * 4;
        for (uint32_t i = 0; i < p.ancho; ++i, s += 4) {
          const uint32_t tx = uint32_t(p.x0) + i;
          if (tx >= p.pitch) {
            break;
          }
          const uint64_t direccion = fila + (uint64_t(tx >> 5) << 12) + local[tx & 31];
          if (direccion + 4 > 0x20000000) {
            continue;
          }
          const uint8_t t0 = s[b0], t1 = s[b1], t2 = s[b2], t3 = s[b3];
          uint8_t* const d = fisica + direccion;
          d[0] = t0;
          d[1] = t1;
          d[2] = t2;
          d[3] = t3;
        }
      }
    }
    lecturas.clear();
    ns_escribir_lecturas_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - antes_lecturas).count());
  }

  void DestruirLectura(Lectura& lectura) {
    if (lectura.memoria != VK_NULL_HANDLE) {
      if (lectura.datos) {
        dfn_.vkUnmapMemory(device_, lectura.memoria);
      }
      dfn_.vkFreeMemory(device_, lectura.memoria, nullptr);
    }
    if (lectura.bufer != VK_NULL_HANDLE) {
      dfn_.vkDestroyBuffer(device_, lectura.bufer, nullptr);
    }
    lectura = Lectura{};
  }

  // desde_destino = the source is a render target's image (or a retained one) with the front buffer in its
  // corner and larger than the output (nfsmw_nativo_frontal_perezoso). Only requested with the exact variant
  // and without FXAA.
  bool PintarSalida(VulkanPresenter::VulkanGuestOutputRefreshContext& contexto, Imagen& origen,
                    uint32_t ancho, uint32_t alto, bool desde_destino = false) {
    // With nfsmw_nativo_salida_sin_espera, the next of 3 slots, and it only waits if that one is still pending.
    // Without it, always slot 0: it waits for the previous output.
    const uint32_t s = salida_sin_espera_ ? (salida_actual_ + 1) % kRanurasSalida : 0;
    if (salidas_pendientes_[s]) {
      const auto antes_espera = std::chrono::steady_clock::now();  // "C2: presentar" report
      dfn_.vkWaitForFences(device_, 1, &fences_salida_[s], VK_TRUE, UINT64_MAX);
      ns_espera_salida_ += Ns(antes_espera, std::chrono::steady_clock::now());
      ++esperas_salida_;
      dfn_.vkResetFences(device_, 1, &fences_salida_[s]);
      salidas_pendientes_[s] = false;
    }
    salida_actual_ = s;
    // The menu post-processing settings, if they changed (an atomic counter; the cvars are only read then).
    if (rampa_gamma_) {
      const uint64_t version_posproceso = nfsmw::ajustes::VersionPosproceso();
      if (version_posproceso != version_posproceso_) {
        version_posproceso_ = version_posproceso;
        posproceso_ = nfsmw::ajustes::LeerPosproceso();
        RecalcularRampa();
        const auto& p = posproceso_;
        REXLOG_INFO("[nativo] C2: posproceso {}: brillo {:.2f}, contraste {:.2f}, saturacion {:.2f}, vibracion {:.2f}, "
                    "temperatura {:.2f}, gamma {:.2f}, tinte {:.2f}/{:.2f}/{:.2f} al {:.2f}, vineta {:.2f}, lineas "
                    "{:.2f}; {}",
                    p.activo ? "encendido" : "apagado", p.brillo, p.contraste, p.saturacion, p.vibracion,
                    p.temperatura, p.gamma, p.tinte_r, p.tinte_g, p.tinte_b, p.tinte, p.vineta, p.lineas,
                    graduacion_ ? "con trabajo por pixel (saturacion, vibracion, vineta o lineas)"
                                : "todo en la tabla, sin trabajo por pixel");
      }
    }
    // The gamma ramp, if it changed since this slot's last output (the GPU is no longer reading it).
    if (rampa_gamma_ && rampas_salida_[s].version != version_rampa_) {
      RampaSalida& rampa = rampas_salida_[s];
      std::memcpy(rampa.datos, rampa_valores_.data(), sizeof(rampa_valores_));
      rex::ui::vulkan::util::FlushMappedMemoryRange(dispositivo_, rampa.memoria, rampa.tipo);
      rampa.version = version_rampa_;
    }
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const auto& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == contexto.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      auto& f = framebuffers_[siguiente_framebuffer_];
      siguiente_framebuffer_ = (siguiente_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        EsperarSalidas();  // a pending output could still use this framebuffer
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView vista = contexto.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_salida_;
      info.attachmentCount = 1;
      info.pAttachments = &vista;
      info.width = ancho;
      info.height = alto;
      info.layers = 1;
      if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = contexto.image_version();
      framebuffer = f.framebuffer;
    }

    VkDescriptorImageInfo info_imagen{};
    info_imagen.imageView = origen.vista;
    info_imagen.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet escritura{};
    escritura.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    escritura.dstSet = descriptores_salida_[s];
    escritura.dstBinding = 0;
    escritura.descriptorCount = 1;
    escritura.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    escritura.pImageInfo = &info_imagen;
    dfn_.vkUpdateDescriptorSets(device_, 1, &escritura, 0, nullptr);

    dfn_.vkResetCommandPool(device_, pools_salida_[s], 0);
    VkCommandBufferBeginInfo inicio{};
    inicio.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    inicio.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(comandos_salida_[s], &inicio) != VK_SUCCESS) {
      return false;
    }
    VkRenderPassBeginInfo pase{};
    pase.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pase.renderPass = render_pass_salida_;
    pase.framebuffer = framebuffer;
    pase.renderArea.extent = {ancho, alto};
    if (desde_destino) {
      // That image was just written as a render target (or copy destination) in the frame's work, which goes in
      // another submission: make the output read it fully written.
      VkMemoryBarrier barrera{};
      barrera.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      barrera.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      barrera.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      dfn_.vkCmdPipelineBarrier(comandos_salida_[s],
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrera, 0, nullptr, 0, nullptr);
    }
    dfn_.vkCmdBeginRenderPass(comandos_salida_[s], &pase, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, float(ancho), float(alto), 0.0f, 1.0f};
    dfn_.vkCmdSetViewport(comandos_salida_[s], 0, 1, &viewport);
    const VkRect2D tijera{{0, 0}, {ancho, alto}};
    dfn_.vkCmdSetScissor(comandos_salida_[s], 0, 1, &tijera);
    VkPipeline pipeline = pipeline_;
    if (rampa_gamma_) {
      // With an output the size of the source (the normal case), the exact texel, unfiltered.
      // From a render target, the output has the front buffer's size, not the image's, and has no FXAA
      // (FrontalAlPresentar only requests it that way): texelFetch of the pixel, the same texel the texture
      // would hold.
      const bool exacta = desde_destino || (ancho == origen.ancho && alto == origen.alto);
      const bool fxaa = !desde_destino && nfsmw::ajustes::AntialiasingFxaa();
      if (fxaa != fxaa_anotado_) {
        fxaa_anotado_ = fxaa;
        REXLOG_INFO("[nativo] C2: antialiasing en la salida: {}", fxaa ? "FXAA" : "ninguno");
      }
      pipeline = PipelineRampa((fxaa ? 4 : 0) + (graduacion_ ? 2 : 0) + (exacta ? 1 : 0));
      if (pipeline == VK_NULL_HANDLE) {
        pipeline = pipelines_rampa_[exacta ? 1 : 0];  // requested variant missing: the usual one
      }
      if (!exacta && !avisado_rampa_bilineal_) {
        avisado_rampa_bilineal_ = true;
        REXLOG_INFO("[nativo] C2: salida de {}x{} desde un origen de {}x{}: rampa con muestreo bilineal", ancho,
                    alto, origen.ancho, origen.alto);
      }
    }
    dfn_.vkCmdBindPipeline(comandos_salida_[s], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    dfn_.vkCmdBindDescriptorSets(comandos_salida_[s], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 layout_pipeline_, 0, 1, &descriptores_salida_[s], 0, nullptr);
    const float rectangulo[4] = {-1.0f, -1.0f, 2.0f, 2.0f};  // x, y, width, height in NDC
    dfn_.vkCmdPushConstants(comandos_salida_[s], layout_pipeline_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                            sizeof(rectangulo), rectangulo);
    struct {
      int32_t desplazamiento[2];
      float inverso[2];
    } bilineal = {{0, 0}, {1.0f / float(ancho), 1.0f / float(alto)}};
    dfn_.vkCmdPushConstants(comandos_salida_[s], layout_pipeline_, VK_SHADER_STAGE_FRAGMENT_BIT, 16,
                            sizeof(bilineal), &bilineal);
    dfn_.vkCmdDraw(comandos_salida_[s], 4, 1, 0, 0);
    dfn_.vkCmdEndRenderPass(comandos_salida_[s]);
    if (desde_destino) {
      // And nothing submitted later (the next frame draws and clears that image again) writes it before the
      // output has read it.
      dfn_.vkCmdPipelineBarrier(comandos_salida_[s], VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                nullptr, 0, nullptr, 0, nullptr);
    }
    if (dfn_.vkEndCommandBuffer(comandos_salida_[s]) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo envio{};
    envio.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    envio.commandBufferCount = 1;
    envio.pCommandBuffers = &comandos_salida_[s];
    {
      const auto antes_candado = std::chrono::steady_clock::now();  // "C2: presentar" report
      const auto cola = dispositivo_->AcquireQueue(familia_, 0);
      const auto antes_envio = std::chrono::steady_clock::now();
      if (dfn_.vkQueueSubmit(cola.queue(), 1, &envio, fences_salida_[s]) != VK_SUCCESS) {
        return false;
      }
      ns_candado_salida_ += Ns(antes_candado, antes_envio);
      ns_submit_salida_ += Ns(antes_envio, std::chrono::steady_clock::now());
      ++envios_salida_;
    }
    salidas_pendientes_[s] = true;
    contexto.SetIs8bpc(true);
    return true;
  }

  struct Framebuffer {
    uint64_t version = UINT64_MAX;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  const VulkanDevice* dispositivo_;
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* memoria_;
  uint32_t familia_;

  std::array<VkCommandPool, kRanurasSalida> pools_salida_{};  // output slots
  FnCopiarImagen copiar_imagen_ = nullptr;
  uint32_t reducciones_ = 0;  // resolves that had to shrink
  // Those of the slot being recorded (Grabar switches them).
  VkCommandBuffer comandos_trabajo_ = VK_NULL_HANDLE;
  VkCommandBuffer comandos_subida_ = VK_NULL_HANDLE;
  // Three slots by default, up to four. With two, the CPU is only one frame ahead and waits for the GPU inside
  // Grabar() half the time (4,826 ms out of every 10 s on the console). How many are actually used is set by
  // nfsmw_nativo_ranuras_trabajo, so configurations can be compared without another NRO.
  // The fourth: measured by regime, with few draws (open road, GPU-bound) the fence wait inside Grabar() is
  // 4.9-9.8 ms, while with many (alleys, CPU-bound) it is ~0. So the third slot falls short exactly where the
  // GPU is the bottleneck. It fits easily: the log says "monton 0 (GPU): 482 MB usados de 1382 MB
  // presupuestados".
  std::array<RanuraTrabajo, 4> ranuras_{};
  uint32_t ranuras_usadas_ = 3;  // set by the cvar (nfsmw_nativo_ranuras_trabajo)
  uint32_t ranura_ = 1;  // Grabar starts with slot 0
  uint64_t envios_ = 0;
  std::array<VkCommandBuffer, kRanurasSalida> comandos_salida_{};
  std::array<VkFence, kRanurasSalida> fences_salida_{};
  bool grabando_ = false;
  std::array<bool, kRanurasSalida> salidas_pendientes_{};
  uint32_t salida_actual_ = 0;
  bool salida_sin_espera_ = false;
  bool marcas_precisas_ = false;
  int32_t alternar_marcas_s_ = 0;
  std::chrono::steady_clock::time_point inicio_marcas_{};
  bool invalidar_cada_copia_ = false;
  bool grabando_subida_ = false;
  uint64_t generacion_comandos_ = 0;
  FnBorrarProfundidad borrar_profundidad_ = nullptr;
  // GPU time of the submissions (timestamps, if the queue supports them).
  using FnEscribirMarca = void(VKAPI_PTR*)(VkCommandBuffer, VkPipelineStageFlags, VkQueryPool,
                                          uint32_t);
  using FnLeerConsultas = VkResult(VKAPI_PTR*)(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t,
                                              void*, VkDeviceSize, VkQueryResultFlags);
  FnEscribirMarca escribir_marca_ = nullptr;
  FnLeerConsultas leer_consultas_ = nullptr;
  VkQueryPool consultas_ = VK_NULL_HANDLE;
  // The game's occlusion queries. The open one (0 = none), those waiting for their spans by number, the last
  // complete count of each D3D structure and, accumulated, spans, spans without room, published queries,
  // published samples and the maximum of a single query.
  VkQueryPool oclusiones_ = VK_NULL_HANDLE;
  bool oclusion_gpu_ = true;
  // Pipeline statistics per pass category.
  VkQueryPool estadisticas_ = VK_NULL_HANDLE;
  std::array<uint64_t, kGpuCategorias> fragmentos_categoria_{};
  std::array<uint64_t, kGpuCategorias> vertices_categoria_{};
  std::array<uint64_t, kGpuCategorias> primitivas_categoria_{};
  uint64_t estadisticas_sin_sitio_ = 0;
  VkQueryPool estadisticas_dibujo_ = VK_NULL_HANDLE;
  std::vector<uint64_t> fragmentos_por_shader_;
  std::vector<uint64_t> dibujos_por_shader_;
  uint64_t fotogramas_diagnostico_ = 0;
  uint64_t estadisticas_dibujo_sin_sitio_ = 0;
  std::array<uint64_t, 3> sin_intercambio_{};          // no clear, not whole, not possible
  uint32_t avisos_sin_intercambio_ = 0;
  std::array<uint64_t, kCubetasCopia> copias_cubeta_{};
  std::array<uint64_t, kCubetasCopia> pixeles_cubeta_{};
  bool ventana_diagnostico_ = false;
  bool ventana_leida_ = false;
  std::chrono::steady_clock::time_point ultima_ventana_ = std::chrono::steady_clock::now();
  bool oclusion_precisa_ = false;
  VkQueryPool visibilidad_ = VK_NULL_HANDLE;  // nfsmw_reflejo_visibilidad
  uint64_t visibilidad_sin_sitio_ = 0;
  uint64_t oclusion_abierta_ = 0;
  uint64_t siguiente_oclusion_ = 1;
  std::unordered_map<uint64_t, ConsultaOclusionJuego> oclusiones_juego_;
  std::unordered_map<uint32_t, uint64_t> oclusion_por_base_;
  uint64_t estadisticas_oclusion_[5] = {};
  double periodo_marca_ns_ = 0.0;
  uint64_t gpu_ns_ = 0;
  uint64_t gpu_trabajos_ = 0;
  uint64_t gpu_trabajos_precisos_ = 0;  // measured with precise intermediate marks
  std::array<uint64_t, kGpuCategorias> gpu_categorias_ns_{};
  uint64_t marca_fin_previa_ = 0;  // end timestamp of the last submission read (gap between submissions)
  uint64_t ultima_marca_gpu_ = 0;  // same, but kept when a read fails (timestamp scale)
  // Breakdown of Presentar's time (CostePresentar).
  uint64_t esperas_salida_ = 0, ns_espera_salida_ = 0;
  uint64_t envios_trabajo_ = 0, ns_candado_trabajo_ = 0, ns_submit_trabajo_ = 0;
  uint64_t envios_salida_ = 0, ns_candado_salida_ = 0, ns_submit_salida_ = 0;
  uint64_t refrescos_ = 0, ns_antes_llamada_ = 0, ns_en_llamada_ = 0, ns_tras_llamada_ = 0;
  static uint64_t Ns(std::chrono::steady_clock::time_point desde, std::chrono::steady_clock::time_point hasta) {
    return hasta > desde ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(hasta - desde).count()) : 0;
  }
  uint64_t solapes_gpu_ = 0;
  std::array<uint64_t, kCubetasSwap> cubetas_swap_{};
  mutable double peor_swap_ms_ = 0.0;  // worst frame since the previous report
  std::chrono::steady_clock::time_point ultimo_swap_{};
  // Resolved texture grid diagnostic (nfsmw_nativo_diag_resueltas).
  FnBlit blit_ = nullptr;
  // The game's shadow map, and the scale it is being drawn at (0 = not decided yet).
  static constexpr uint32_t kLadoSombras = 1600;
  bool profundidad_escalable_ = false;
  uint32_t escala_sombras_ = 0;
  uint64_t sombras_subidas_ = 0;
  std::vector<uint32_t> resueltas_fotograma_;
  Imagen mosaico_;
  std::chrono::steady_clock::time_point ultimo_aviso_mosaico_{};
  // Read-back of small resolved textures (nfsmw_nativo_leer_resueltas_texels).
  std::unordered_map<uint64_t, Lectura> lecturas_;
  std::unordered_map<uint64_t, DestinoLectura> lecturas_por_destino_;  // informe C2 y cadencia
  std::chrono::steady_clock::time_point informe_lecturas_{};
  std::vector<LecturaPendiente> lecturas_pendientes_;  // recorded in the current submission
  uint64_t lecturas_hechas_ = 0;
  // Rear-view mirror diagnostic (AnotarCopia).
  uint64_t dibujados_ultima_copia_ = 0;
  std::unordered_map<uint32_t, CopiaDestino> copias_por_destino_;
  std::chrono::steady_clock::time_point informe_copias_{};
  VkFormat formato_profundidad_ = VK_FORMAT_UNDEFINED;
  // Parts C3-C6: draws with the library shaders (nullptr without the required capabilities).
  std::unique_ptr<DibujosVulkan> dibujos_;

  std::unordered_map<uint64_t, Imagen> destinos_;
  std::unordered_map<uint32_t, Resuelta> resueltas_;
  // nfsmw_nativo_diag_lectores_s (DiagLectoresAlPresentar). Ring thread only.
  struct LectorDiag {
    uint32_t ps = 0;        // PS number in the library (kLectorSwap: the Swap presenting the front buffer)
    uint32_t registro = 0;  // sampler (fetch constant)
    uint32_t destino = 0;   // EDRAM base of color 0 | pitch << 12: where it draws
    uint32_t veces = 0;     // draws
  };
  struct EscrituraDiag {
    int32_t fotograma = -1;  // 0 or 1 within the window; -1: the last one before the window
    uint32_t orden = 0;      // n-th write to that address in its frame
    uint32_t origen = 0;     // EDRAM base | pitch << 12 of the target being resolved
    uint32_t pedido_ancho = 0;
    uint32_t pedido_alto = 0;
    uint32_t ancho = 0;  // the texture's
    uint32_t alto = 0;
    uint64_t dibujos = 0;    // draws since the previous copy (any)
    bool profundidad = false;
    bool entera = false;  // covers it fully: from (0,0) and at least its size
    uint32_t otros = 0;   // readers that do not fit in the list
    std::vector<LectorDiag> lectores;
  };
  // A write classified as SOBRA in a window, watched in every frame since then.
  struct VigiladaDiag {
    uint32_t direccion = 0;
    uint32_t origen = 0;          // EDRAM base | pitch << 12 of the watched writes
    bool pendiente = false;       // the texture's last write comes from that source
    bool origen_escrito = false;  // and the source was drawn to or cleared since then
    uint64_t escrituras = 0;      // from that source since watching began
    uint64_t antes = 0;           // reads before the source is rewritten (a deferred copy covers them)
    uint64_t tardias = 0;         // reads after the source is written (dropping the copy there would fail)
    uint32_t ps_antes = 0;        // PS of the first one of each kind
    uint32_t ps_tardio = 0;
  };
  static constexpr uint32_t kLectorSwap = UINT32_MAX;
  static constexpr size_t kDiagMaxEscrituras = 32;
  static constexpr size_t kDiagMaxLectores = 12;
  static constexpr size_t kDiagMaxVigiladas = 4;
  static constexpr uint32_t kDiagRegSurfaceInfo = 0x2000;  // RB_SURFACE_INFO
  static constexpr uint32_t kDiagRegColorInfo = 0x2001;    // RB_COLOR_INFO
  static constexpr uint32_t kDiagRegFetch = 0x4800;        // SHADER_CONSTANT_FETCH_00_0
  static constexpr uint32_t kDiagRegColorMask = 0x2104;    // RB_COLOR_MASK: 4 bits per color target
  static constexpr uint32_t kDiagRegsColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};  // RB_COLOR_INFO, RB_COLORn_INFO
  bool diag_ventana_ = false;
  uint32_t diag_fotograma_ = 0;
  uint64_t diag_ventanas_ = 0;
  uint64_t diag_escrituras_perdidas_ = 0;
  std::chrono::steady_clock::time_point diag_lectores_ultima_ = std::chrono::steady_clock::now();
  std::unordered_map<uint32_t, std::vector<EscrituraDiag>> diag_escrituras_;
  std::unordered_map<uint32_t, uint64_t> diag_firmas_;  // pattern of the last line written, per address
  std::vector<VigiladaDiag> diag_vigiladas_;
  // nfsmw_nativo_compuesta_perezosa (AplazarCompuesta). A single deferred copy at a time.
  struct CompuestaPendiente {
    uint32_t direccion = 0;
    uint32_t origen = 0;                 // EDRAM base | pitch << 12 of the source target
    Imagen* destino = nullptr;           // that target (destinos_ never erases: the pointer stays valid)
    VkImage origen_vk = VK_NULL_HANDLE;  // its image when deferred: if it changes, it can no longer be recorded
    VkImage textura_vk = VK_NULL_HANDLE;
    VkImageCopy copia{};
  };
  static constexpr uint64_t kCompuestaAMirar = 900;  // watched writes without late reads (~30 s of racing)
  CompuestaPendiente compuesta_;
  bool compuesta_hay_ = false;
  bool compuesta_apagada_ = false;
  uint32_t compuesta_caducada_ = 0;   // address whose last copy was dropped; 0 = none
  uint32_t compuesta_aplicando_ = 0;  // address of the last APLICANDO notice
  uint64_t compuesta_aplazadas_ = 0;
  uint64_t compuesta_grabadas_lectura_ = 0;
  uint64_t compuesta_grabadas_otras_ = 0;
  uint64_t compuesta_tiradas_origen_ = 0;
  uint64_t compuesta_sustituidas_ = 0;
  uint64_t compuesta_lecturas_tardias_ = 0;
  uint64_t compuesta_diferencias_ = 0;
  uint64_t compuesta_pixeles_ahorrados_ = 0;
  uint64_t compuesta_pixeles_previos_ = 0;
  uint64_t compuesta_presentados_previos_ = 0;
  std::unordered_map<uint64_t, Imagen> profundidades_;

  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_descriptores_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_descriptores_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kRanurasSalida> descriptores_salida_{};
  VkShaderModule vs_ = VK_NULL_HANDLE;
  VkShaderModule fs_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_salida_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  // Output with the game's gamma ramp (nfsmw_nativo_rampa_gamma).
  struct RampaSalida {
    VkBuffer bufer = VK_NULL_HANDLE;
    VkDeviceMemory memoria = VK_NULL_HANDLE;
    uint8_t* datos = nullptr;
    uint32_t tipo = 0;
    uint64_t version = UINT64_MAX;  // the rampa_valores_ version the buffer holds
  };
  bool rampa_gamma_ = false;
  std::array<RampaSalida, kRanurasSalida> rampas_salida_{};
  VkShaderModule fs_rampa_ = VK_NULL_HANDLE;
  // Index 4 * fxaa + 2 * graduacion + exacta: 0 bilinear (like guest_output_bilinear_ps), 1 exact texel
  // (output the same size as the source); +2 with the saturation, vibrance, vignette or scanline
  // post-processing; +4 with FXAA. Those other than 0 and 1 are created on request (PipelineRampa).
  std::array<VkPipeline, 8> pipelines_rampa_{};
  std::array<bool, 8> pipelines_rampa_fallidos_{};
  bool avisado_rampa_bilineal_ = false;
  // The game's ramp in 10 bits (red, green, blue). Until the game loads its own, the identity the SDK starts
  // with (i * 1023 / 255).
  std::array<std::array<uint16_t, 3>, 256> rampa_juego_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();
  // The output UBO in std140: one vec4 per entry (red, green and blue in 0-1, with the single-channel
  // post-processing applied), followed by the mix (saturation, vibrance, 1 / gamma) and effects (vignette,
  // scanlines). Filled by RecalcularRampa.
  static constexpr size_t kFlotantesRampa = 256 * 4 + 8;
  std::array<float, kFlotantesRampa> rampa_valores_{};
  uint64_t version_rampa_ = 0;
  // Post-processing in effect and the settings version it was computed from.
  nfsmw::ajustes::Posproceso posproceso_{};
  uint64_t version_posproceso_ = 0;
  bool graduacion_ = false;  // shader variant with saturation, vibrance, vignette or scanlines
  bool fxaa_anotado_ = false;  // nfsmw_antialiasing at the last output (to log changes)
  std::array<Framebuffer, VulkanPresenter::kMaxActiveGuestOutputImageVersions> framebuffers_{};
  size_t siguiente_framebuffer_ = 0;

  std::chrono::steady_clock::time_point inicio_alternancia_ = std::chrono::steady_clock::now();
  bool alternancia_anotada_ = false;
  uint64_t intercambios_ = 0;    // resolves without a copy
  uint64_t restauraciones_ = 0;
  uint64_t restauraciones_para_resolver_ = 0;  // nfsmw_nativo_resolver_contenido_valido
  uint64_t resolver_sin_origen_ = 0;           // the content was in no texture
  uint64_t resolver_contenido_viejo_ = 0;      // with the setting off, resolves of the stale image
  uint64_t intercambios_sin_borrado_ = 0;  // the ones that used to copy 1600x1600
  uint64_t intercambios_color_ = 0;  // color targets resolved without a copy
  // Copies and clears removed, and what they cost.
  std::unordered_map<const Imagen*, EstadoDestino> estado_destino_;
  uint64_t borrados_saltados_ = 0;             // de color
  uint64_t borrados_saltados_profundidad_ = 0;
  uint64_t pixeles_borrados_saltados_ = 0;
  uint64_t borrados_inutiles_ = 0;             // clears wiped out by a swap with no draw in between
  uint64_t pixeles_borrados_inutiles_ = 0;
  uint64_t restauraciones_recortadas_ = 0;     // restores that did not copy the whole image
  uint64_t pixeles_restaurados_ = 0;           // the ones actually copied (so the report adds up)
  uint64_t pixeles_restaurar_ahorrados_ = 0;   // the ones not copied, due to useful area or swap
  uint64_t prestamos_ = 0;                     // restauraciones hechas volviendo a intercambiar
  uint64_t prestadas_leidas_ = 0;              // times a lent resolved texture was requested (must be 0)
  std::unordered_set<uint32_t> prestadas_;     // bases whose content is lent to the render target
  // nfsmw_nativo_sombra_minimo. State of the shadow map cycle and phase of the guard.
  enum : uint32_t { kSmLibre = 0, kSmTrasMundo = 1, kSmCoches = 2 };
  enum : uint32_t { kSmMirando = 0, kSmAplicando = 1, kSmApagada = 2 };
  static constexpr uint64_t kSombraMinimoCiclos = 300;  // consecutive clean cycles before applying (~10 s of racing)
  uint32_t sm_fase_ = kSmMirando;
  uint32_t sm_estado_ = kSmLibre;
  bool sm_encendido_ = false;                  // cvar, alternation and cheap PCF, read at the start of each cycle
  bool sm_tramo_anotado_ = true;               // nfsmw_nativo_sombra_minimo_alternar_s: last interval written to the log
  std::chrono::steady_clock::time_point sm_inicio_alternancia_ = std::chrono::steady_clock::now();
  const Imagen* sm_destino_ciclo_ = nullptr;   // the current cycle: 1600x1600 target...
  uint32_t sm_mundo_ciclo_ = 0;                // ...and the texture that received the world (no clear)
  bool sm_ciclo_limpio_ = false;               // nothing watched has failed in this cycle
  bool sm_ciclo_borrado_ = false;              // this cycle's car pass draws on the cleared target
  bool sm_mundo_reescrito_ = false;            // the world texture was rewritten within the cycle
  const Imagen* sm_destino_ = nullptr;         // learned while observing: the target...
  uint32_t sm_mundo_ = 0;                      // ...the texture without cars (textura[1], 07CEA000)...
  uint32_t sm_coches_ = 0;                     // ...and the texture with cars (textura[0], 086AE000)
  bool sm_virtual_ = false;                    // sm_coches_ holds only the cars: sampled with the minimum
  bool sm_virtual_valida_ = false;             // and sm_mundo_ still holds the world of that same cycle
  uint64_t sm_limpios_seguidos_ = 0;
  uint64_t sm_ciclos_ = 0;
  uint64_t sm_ciclos_aplicados_ = 0;
  uint64_t sm_pixeles_ahorrados_ = 0;
  uint64_t sm_pixeles_ahorrados_previos_ = 0;
  uint64_t sm_dibujos_coches_ = 0;
  uint64_t sm_dibujos_no_exactos_ = 0;
  uint64_t sm_lecturas_minimo_ = 0;            // of the cars-only texture, paired with the world
  uint64_t sm_lecturas_si_misma_ = 0;          // of the usual texture, paired with itself (observing)
  uint64_t sm_lecturas_normales_ = 0;          // of the usual texture, without the minimum
  uint64_t sm_lecturas_a_destiempo_ = 0;       // between the two resolves of a cycle
  uint64_t sm_lecturas_incapaces_ = 0;         // from a shader without tfetch2DSombraMin on that register
  uint64_t sm_diferencias_ = 0;
  // nfsmw_nativo_diag_borrados (see UsoBorrado).
  std::unordered_map<const Imagen*, UsoBorrado> uso_borrados_;  // destinos_ and profundidades_ never erase: the key stays valid
  bool diag_borrados_ = REXCVAR_GET(nfsmw_nativo_diag_borrados);
  std::chrono::steady_clock::time_point informe_borrados_{};
  uint64_t presentados_informe_borrados_ = 0;
  // nfsmw_nativo_borrar_area_util.
  bool borrar_area_util_ = REXCVAR_GET(nfsmw_nativo_borrar_area_util);
  bool area_util_global_apagada_ = false;
  uint32_t bandas_pendientes_ = 0;  // targets with an uncleared band: AnotarUsoBorrado checks them even when not measuring
  uint64_t area_util_borrados_ = 0;
  uint64_t area_util_completadas_ = 0;
  uint64_t area_util_fallos_ = 0;
  uint64_t area_util_pixeles_ = 0;
  uint64_t area_util_pixeles_previos_ = 0;
  static constexpr uint64_t kAreaUtilCiclos = 120;  // measured clears of a target before trimming its own
  // nfsmw_nativo_profundidad_perezosa. Deferred depth copies, by texture address.
  struct CopiaPendiente {
    const Imagen* origen = nullptr;  // depth target (profundidades_ never erases: the pointer stays valid)
    VkImage origen_vk = VK_NULL_HANDLE;
    VkImage destino_vk = VK_NULL_HANDLE;
    VkImageCopy copia{};
    uint64_t fotograma = 0;     // presentados_ al aplazarla
    bool leida_muerta = false;  // the composite without blur already requested it
  };
  std::unordered_map<uint32_t, CopiaPendiente> pendientes_;
  std::unordered_map<uint32_t, uint64_t> ultima_lectura_viva_;    // per address, in presentados_
  std::unordered_map<uint32_t, uint64_t> ultima_lectura_muerta_;
  std::unordered_set<uint32_t> caducadas_;     // dropped copies: their texture lacks the last resolve
  bool lecturas_profundidad_muertas_ = false;  // lo pone DibujosVulkan (LecturasDeProfundidadMuertas)
  bool perezosa_apagada_ = false;              // the guard saw a late read
  uint64_t perezosa_aplazadas_ = 0;
  uint64_t perezosa_copiadas_lectura_ = 0;
  uint64_t perezosa_copiadas_escritura_ = 0;
  uint64_t perezosa_sustituidas_ = 0;
  uint64_t perezosa_tiradas_ = 0;
  uint64_t perezosa_lecturas_tardias_ = 0;
  uint64_t perezosa_pixeles_ahorrados_ = 0;
  uint64_t perezosa_pixeles_ahorrados_previos_ = 0;
  static constexpr uint64_t kPerezosaFotogramas = 30;  // ~1 s: window for "requested" and "nobody samples it"
  // nfsmw_nativo_frontal_perezoso (see FrontalPendiente).
  std::unordered_map<uint32_t, FrontalPendiente> frontales_pendientes_;  // per texture address
  std::vector<ImagenFrontal> frontales_imagenes_;                         // repuestos y retenidas
  std::unordered_map<uint32_t, uint64_t> frontales_presentados_;  // address -> presentados_ at its last Swap
  std::unordered_map<uint32_t, uint64_t> frontales_leidos_;       // address -> last sampling by a draw
  std::unordered_set<uint32_t> frontales_caducados_;              // missing a copy that can no longer be made
  bool frontal_apagado_ = false;
  uint64_t frontal_aplazadas_ = 0;
  uint64_t frontal_pintadas_destino_ = 0;
  uint64_t frontal_pintadas_retenida_ = 0;
  uint64_t frontal_rotaciones_ = 0;
  uint64_t frontal_sin_repuesto_ = 0;
  uint64_t frontal_copiadas_lectura_ = 0;
  uint64_t frontal_copiadas_escritura_ = 0;
  uint64_t frontal_copiadas_swap_ = 0;
  uint64_t frontal_sustituidas_ = 0;
  uint64_t frontal_lecturas_tardias_ = 0;
  uint64_t frontal_pixeles_ahorrados_ = 0;
  uint64_t frontal_pixeles_ahorrados_previos_ = 0;
  static constexpr uint64_t kFrontalFotogramas = 8;    // window for "the Swap presents it" and "nobody samples it"
  static constexpr size_t kFrontalImagenesMax = 3;     // spares of the front buffer's target (~4 MB each at 1040)
  // The values above as they were at the previous report: the new lines are per frame.
  uint64_t pixeles_restaurados_previos_ = 0;
  uint64_t pixeles_restaurar_ahorrados_previos_ = 0;
  uint64_t pixeles_borrados_saltados_previos_ = 0;
  uint64_t pixeles_borrados_inutiles_previos_ = 0;
  uint64_t presentados_informe_copias_ = 0;
  // State of the previous frame, for the stutter dump.
  uint64_t tiron_copias_ = 0, tiron_borrados_ = 0, tiron_resolves_ = 0;
  uint64_t tiron_restaura_ = 0, tiron_esperas_ = 0;
  // Snapshot of the waits at the previous Swap, for the "[tiron] esperas" line.
  uint64_t tiron_esperas_ns_[nfsmw::esperas::kNumTipos] = {};
  uint64_t tiron_esperas_veces_[nfsmw::esperas::kNumTipos] = {};
  uint64_t tiron_ns_esperas_gpu_ = 0, tiron_ns_espera_salida_ = 0;
  uint64_t tiron_bytes_huella_ = 0, tiron_huellas_aplazadas_ = 0;
  uint64_t tiron_dibujos_ = 0, tiron_ns_anillo_ = 0, tiron_ns_texturas_ = 0;
  uint64_t tiron_texturas_subidas_ = 0, tiron_bytes_subidos_ = 0, tiron_texturas_creadas_ = 0;
  uint64_t tiron_ns_huella_cruda_ = 0, tiron_ns_huella_datos_ = 0;
  uint64_t tiron_ns_esperando_copias_ = 0, tiron_esperas_copias_ = 0;
  uint64_t tiron_ns_ayudando_copias_ = 0, tiron_copias_ayudadas_ = 0;
  uint64_t tiron_ns_crear_texturas_ = 0, tiron_texturas_enlazadas_hilo_ = 0;
  uint64_t tiron_ns_esperando_enlaces_ = 0;
  uint64_t tiron_texturas_huella_hilo_ = 0, tiron_ns_huella_hilo_ = 0, tiron_ns_huella_instantanea_ = 0;
  uint64_t tiron_texturas_huella_anillo_ = 0, tiron_ns_huella_anillo_ = 0, tiron_ns_esperando_huellas_ = 0;
  // Why the restore does not swap. See the block in RestaurarContenido.
  uint64_t no_swap_sin_transfer_dst_ = 0;
  uint64_t no_swap_destino_sin_preparar_ = 0;
  uint64_t no_swap_resuelta_sin_preparar_ = 0;
  uint64_t no_swap_tamanos_distintos_ = 0;
  uint64_t tiron_gpu_ns_ = 0;      // GPU timestamps when the previous frame closed
  uint64_t tiron_ns_grabar_ = 0;   // ns recording when the previous frame closed
  uint32_t avisos_tiron_ = 0;
  uint64_t copias_ = 0;
  uint64_t copias_profundidad_ = 0;  // those that force keeping the depth tile
  uint64_t borrados_profundidad_ = 0;  // depth clears (borrados_ only counts color ones)
  uint64_t borrados_en_pase_fallidos_ = 0;  // clears through a pass that could not be opened
  uint64_t borrados_ = 0;
  uint64_t presentados_ = 0;
  uint64_t rechazos_ = 0;
  std::unordered_set<uint32_t> avisados_;
};

}  // namespace

std::unique_ptr<DestinosNativos> DestinosNativos::Crear(const VulkanDevice* dispositivo,
                                                        rex::memory::Memory* memoria) {
  if (!dispositivo || !memoria) {
    return nullptr;
  }
  auto destinos = std::make_unique<DestinosVulkan>(dispositivo, memoria);
  if (!destinos->Inicializar()) {
    REXLOG_ERROR("[nativo] C2: no se pudo preparar la presentacion de destinos");
    return nullptr;
  }
  return destinos;
}

}  // namespace nfsmw::nativo
