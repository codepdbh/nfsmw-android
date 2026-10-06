// nfsmw - native renderer, part C1: the app's own graphics system.
//
// What it does
//   Replaces the Xenos emulation plugin when nfsmw_renderizador is
//   "nativo" (OnPreSetup in nfsmw_app.h decides it):
//     - Presentation: the SDK's VulkanProvider + VulkanPresenter, same as
//       the emulation. ReXApp sees presenter() and sets up F3, settings and
//       the console the usual way (rex_app.cpp:386-398).
//     - The game's D3D keeps running unchanged and fills its command ring. A
//       "sink" consumes it right away: it answers what the game expects from
//       the GPU and hands the draws, copies and Swaps to the rest of the
//       native renderer.
//
// What the game expects from the GPU, and why each thing is needed
//   1. MMIO registers at 0x7FC80000. Without a registered range REX_MM_LOAD_U32
//      reads uninitialized garbage (mmio_handler.cpp:105-124). The same fixed
//      values as the emulation are returned (graphics_system.cpp:232-262).
//   2. The ring read pointer: the game spins in sub_82597690 watching that
//      word to know how much room it has left.
//   3. Interrupts: vblank (sub_82597960 counts vblanks) and PM4_INTERRUPT
//      packets, which signal that the GPU reached that point of the ring.
//   4. GPU writes to memory (MEM_WRITE, COND_WRITE, REG_TO_MEM and
//      EVENT_WRITE_*), with the semantics of graphics/command_processor.cpp.
//      Occlusion queries return 1000 samples, like the emulation.
//
// Where the rest is
//   nfsmw_nativo_destinos.cpp does the copies (resolve and clear) and presents
//   the resolved texture of each Swap, nfsmw_nativo_shaders.cpp identifies the
//   shaders that reach the ring, and nfsmw_nativo_dibujos.cpp draws the
//   geometry. See docs/native-renderer.md.

#include "nfsmw_nativo_sistema.h"

#include "nfsmw_esperas_tiron.h"
#include "nfsmw_nativo_destinos.h"
#include "nfsmw_nativo_ganchos.h"
#include "nfsmw_nativo_shaders.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the store of the data being
 * hashed (strict aliasing). With that, the texture key read claves[4] before writing it and the same texture was
 * created several times (see docs/toolchain.md). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h ya se ha incluido con su implementacion antes de este punto: XXH_FORCE_MEMORY_ACCESS 0 llegaria tarde"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

// The summary of the scenery LOD hook (nfsmw_escenario_lod.cpp).
// Declared here instead of creating a header for a single function.
namespace nfsmw::escenario_lod {
std::string Resumen();
}  // namespace nfsmw::escenario_lod

#if REX_PLATFORM_SWITCH
// Only for RexSwitchSetCurrentThreadPriority. That header deliberately does not include switch.h.
#include "../../sdk/src/core/threading_switch.h"
#endif

REXCVAR_DEFINE_STRING(nfsmw_renderizador, "xenos", "NFSMW",
                      "xenos = emulacion de la GPU; nativo = renderizador nativo en desarrollo "
                      "(pieza C1: el juego corre sin emulacion y la pantalla muestra un color de "
                      "prueba)")
    .allowed({"xenos", "nativo"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_nativo_registros_vectoriales, true, "NFSMW",
                    "Renderizador nativo (24/09, build 164): los bloques de registros del anillo se copian de 4 en 4 "
                    "con NEON. Los primeros 200.000 bloques se comprueban contra el camino de siempre y, si uno "
                    "difiere, se apaga solo. false = el camino de siempre")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_nativo_registros_en_bloque, true, "NFSMW",
                    "Renderizador nativo: los bloques de registros sin efectos (casi siempre constantes de VS y PS) "
                    "se escriben de una vez en vez de registro a registro (build 130). false: como antes");

/*
 * The report is written from the ring thread, and that causes real stutters.
 *
 * Measured over 170 race stutters: the periodic dump explains 33 of them (19.4 %) and 1,267 ms of the 5,707
 * lost. It is not a coincidence: the report runs on a timer, not on what happens in the game, and still 13 of
 * the 30 reports of the race land inside a stuttering frame, when chance would give 1.53 %. That is 28 times the
 * background probability. Each block is 31-37 lines and 5.9-7.4 KB, and takes 14-40 ms to write.
 *
 * Interval: 20 s. A longer interval means fewer dumps, but each one costs twice as much and the damage
 * concentrates in clusters of 5-6 stutters in a row; even so, measured side by side, 10 s loses twice the time:
 *         20 s:  33 stutters from this cause, 211 ms of stutter per minute of racing
 *         10 s:  58 stutters,                 428 ms/min
 *     Half a millisecond per frame thrown away, of the same order as what the query pools gained.
 *
 * So 20. The clusters are ugly, but the total rules, and with 20 s there are still ~15 reports per session,
 * plenty to measure.
 *
 * The real fix is not spacing the dump out but keeping its cost off the ring thread, so that no frame pays for
 * the whole block: see the next comment.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_informe_s, 20, "NFSMW",
                     "Renderizador nativo: segundos entre volcados del informe de diagnostico. Cada volcado "
                     "lo escribe el hilo que alimenta a la GPU y cuesta tirones: 10 da el doble de detalle y "
                     "el doble de tirones por esta causa")
    .range(5, 120);

/*
 * The dump is written on another thread.
 *
 * What the comment above asked for, with a new measurement: the block takes 29-32 ms on the ring thread. Of the
 * 6 stutters of 60 ms or more in a race, 3 end 40-70 ms after a dump starts and 1 within 0.12 s of the
 * "C6 subetapas" line, when chance would give 0.5; and 3 of the 7 dumps of the race produced a stutter of 60 ms
 * or more. It is not the formatting: the log is synchronous (log_async = false) and the 2-9 ms gaps between
 * lines fall every 2-4 lines, when the FILE is flushed to the SD.
 *
 * Lines are still formatted on the ring (they read its state, which is only coherent on its thread) and
 * enqueued; the "NFSMW informes" thread writes them, sleeping on a condition variable. Both lessons of the
 * failed asynchronous logger (see log_async in sdk/src/core/logging.cpp) are covered: the queue never blocks
 * the producer (full = the line is dropped and counted) and the thread runs at 0x3B, the only time-sliced
 * priority on Horizon. One point of contention remains: if the ring writes a line of its own right while this
 * thread writes another, it waits for that line to finish (the log lock), not for the whole block.
 * false = as before: the ring writes every line itself.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_informes_diferidos, true, "NFSMW",
                    "Renderizador nativo (25/09, build 179): el informe periodico y las lineas [tiron] del hilo del "
                    "anillo se escriben en la SD desde un hilo aparte; el anillo solo las formatea y las encola. "
                    "false = como antes, las escribe el propio anillo (29-32 ms cada 20 s en la 176)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * IM_LOAD without memcmp. On every IM_LOAD the ring compared the whole microcode with guest memory (~0.6 us,
 * ~41,000 per second in a race). Now the D3D constructors and the fetch patcher report what they write
 * (nfsmw_microcodigo_versiones.h, with the cross-thread protocol) and the ring only compares if someone has
 * written that microcode since the last check. The self-checking guard is in CargarShaderCacheado.
 */
#include "nfsmw_microcodigo_versiones.h"

REXCVAR_DEFINE_BOOL(nfsmw_nativo_im_load_sin_memcmp, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): un IM_LOAD cuyo microcodigo no ha escrito nadie desde la "
                    "ultima comprobacion (lo avisan los constructores y el parcheador de los fetch del D3D) se toma de "
                    "la cache sin compararlo entero con memcmp. Empieza comprobando cada carga contra el memcmp y se "
                    "apaga solo al primer desacuerdo. false = memcmp en cada IM_LOAD, como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * IM_LOAD_IMMEDIATE with an exact cache. sub_825A37D8 (D3D) copies the VS into the ring itself with the
 * fetches patched and, if needed, with the outputs the PS does not read nulled: op 2B, 4,000-8,000 per second
 * in stretches of a race, at 1.5-2.8 us each ("tiempos por paquete"). Now a packet whose microcode is
 * byte-identical to one already seen (memcmp against the stored raw copy) reuses its swapped copy, its
 * identification, its fingerprint and its generation. The self-checking guard is in CargarInmediato.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_im_inmediato_cache, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): un IM_LOAD_IMMEDIATE (el VS que el D3D copia en el anillo "
                    "con los fetch parcheados) identico byte a byte a uno ya visto reutiliza su copia girada, su "
                    "identificacion, su huella y su generacion, sin girar ni identificar otra vez. Empieza comprobando "
                    "cada acierto contra el camino de siempre y se apaga solo al primer desacuerdo. false = sin cache, "
                    "como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Game frames, one per Swap (nfsmw_d3d_trace.cpp). Only for the AnotarJuegoPorDelante measurement.
extern std::atomic<uint64_t> g_nfsmw_fotogramas_juego;

namespace nfsmw::nativo {
namespace {

// The line queue and its thread. Created with the first line; it lives until exit.
class ColaInformes {
 public:
  ~ColaInformes() {
    {
      std::lock_guard<std::mutex> cerrojo(mutex_);
      parar_ = true;
    }
    aviso_.notify_one();
    if (hilo_.joinable()) {
      hilo_.join();  // writes whatever is left before exiting
    }
  }

  // false if there is no thread (it could not be created, or shutdown is under way): then the caller writes
  // the line.
  bool Encolar(std::string&& linea) {
    bool avisar = false;
    {
      std::lock_guard<std::mutex> cerrojo(mutex_);
      if (parar_ || sin_hilo_) {
        return false;
      }
      if (!hilo_.joinable()) {
        try {
          hilo_ = std::thread([this] { Bucle(); });  // persistent: on Horizon detach() closes the game
        } catch (...) {
          sin_hilo_ = true;  // no thread (Horizon thread limit): everything as before, nothing breaks
          return false;
        }
      }
      if (cola_.size() >= kMaxLineas) {
        ++perdidas_;  // never block the producer: the line is dropped and counted
        return true;
      }
      avisar = cola_.empty();  // the thread only sleeps with the queue empty: one wake-up per burst, not per line
      cola_.push_back(std::move(linea));
    }
    if (avisar) {
      aviso_.notify_one();
    }
    return true;
  }

 private:
  static constexpr size_t kMaxLineas = 4096;

  void Bucle() {
    rex::thread::set_current_thread_name("NFSMW informes");
#if REX_PLATFORM_SWITCH
    const bool prioridad = RexSwitchSetCurrentThreadPriorityOk(REX_SWITCH_PRIO_GUEST);
#else
    const bool prioridad = true;
#endif
    REXLOG_INFO("[nativo] informes diferidos (build 179): hilo en marcha{}",
                prioridad ? "" : " (el kernel NO acepto la prioridad 0x3B)");
    std::deque<std::string> lote;
    for (;;) {
      uint64_t perdidas = 0;
      {
        std::unique_lock<std::mutex> cerrojo(mutex_);
        aviso_.wait(cerrojo, [this] { return parar_ || !cola_.empty(); });
        if (cola_.empty()) {
          return;  // parar_ and nothing pending
        }
        lote.swap(cola_);
        perdidas = perdidas_;
        perdidas_ = 0;
      }
      for (const std::string& linea : lote) {
        REXLOG_INFO("{}", linea);
      }
      lote.clear();
      if (perdidas) {
        REXLOG_WARN("[nativo] informes diferidos: {} lineas perdidas (la cola de {} estaba llena)", perdidas,
                    kMaxLineas);
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable aviso_;
  std::deque<std::string> cola_;  // con mutex_
  std::thread hilo_;              // created with mutex_ held
  bool parar_ = false;            // con mutex_
  bool sin_hilo_ = false;         // with mutex_: the thread could not be created
  uint64_t perdidas_ = 0;         // con mutex_
};

}  // namespace

void InformeDiferido(std::string linea) {
  static const bool diferir = REXCVAR_GET(nfsmw_nativo_informes_diferidos);
  if (!diferir) {
    REXLOG_INFO("{}", linea);
    return;
  }
  static ColaInformes cola;
  if (!cola.Encolar(std::move(linea))) {
    REXLOG_INFO("{}", linea);  // no report thread: as before (Encolar has not moved it)
  }
}

}  // namespace nfsmw::nativo

/*
 * See BucleAnillo. Lowered so the presentation thread (0x2C) can preempt it; at the same priority the
 * presenter went without a core 70 % of the time.
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_anillo_prioridad, 0x2D, "NFSMW",
                     "Switch: prioridad de Horizon del hilo del anillo (0x1C-0x3B). 0x2D por "
                     "defecto, un escalon por debajo del presentador (0x2C) y dos por debajo del "
                     "audio (0x2B). Subirlo a 0x2C devuelve el reparto de la compilacion 113, en el "
                     "que solo llegaba a la pantalla la mitad de los fotogramas");
/*
 * Which core the ring starts on.
 *
 * Pinning the Vulkan thread to a separate CPU core was suggested, and the data support looking at it: the ring
 * uses 94.7 % of a core and the game's main thread 88.2 %, and both run with "preferred core -1", which on
 * Horizon is the process default core: the same one for both. The kernel has to keep moving them, and every
 * migration throws away the thread's L1.
 *
 * That does not show in the mean (total CPU does not correlate with FPS: 258 % at 32 FPS and 280 % at 21) but in
 * the variance, which is what is noticeable. And the variance is exactly the problem: 54 % of frames fall in
 * 30-36 ms, straddling the 33.3 vblank, so a few tenths decide whether a frame shows at 33 ms or jumps to 50.
 *
 * It is not an exclusive affinity: it only changes the preferred core and leaves the mask as it was, so the
 * kernel can still move the thread if needed and this cannot cause starvation. That is why it can be on by
 * default.
 *
 * How it is checked: the "C6 anillo" line of the log counts migrations per second and says which core the
 * thread is on. Measured: the migrations are the same with -1 and with 1 (see the next comment).
 */
REXCVAR_DEFINE_INT32(nfsmw_nativo_anillo_nucleo, 1, "NFSMW",
                     "Switch: nucleo preferido del hilo del anillo (-1 = el de por defecto, que es el "
                     "mismo que el del hilo del juego; 0-2 = uno concreto). No es exclusivo: la "
                     "mascara no se toca, asi que el kernel puede moverlo igual");
/*
 * The real pin.
 *
 * Measured: the preferred core does not help. The kernel accepted it ("preferido 1 (aceptado)") and still the
 * thread showed up on cores 0, 1 and 2 with 0.24 migrations per loop, the same figure wherever it is set.
 * Horizon moves it anyway.
 *
 * It also became clear that migrations by themselves are not the problem: 44 per second, at about 12 us to
 * refill the L1 each, is 0.5 ms/s = 0.05 % of CPU. Negligible.
 *
 * What could matter is removing the competition, which is something else and which the migration counter does
 * not see: the ring (94.7 % of a core) and the game's main thread (88.2 %) want 183 % of a core that gives 100,
 * so they preempt each other. With the mask set to a single core the ring stops competing; the other two cores
 * (200 %) have to carry the remaining 183 %, which barely fits.
 *
 * Off by default: fixing a mask can leave a thread unable to run (unlike the preferred core), and measured on
 * the console, pinning the ring to one core made the peaks worse: the minimum dropped to 16.3 FPS against about
 * 21 without pinning (see docs/platform-notes.md, Threads). The A/B is done from the toml without recompiling,
 * and the log says whether migrations drop to zero.
 */
REXCVAR_DEFINE_BOOL(nfsmw_nativo_anillo_nucleo_exclusivo, false, "NFSMW",
                    "Switch: ademas de preferir el nucleo de nfsmw_nativo_anillo_nucleo, PROHIBE los "
                    "demas (mascara exclusiva). Es el pin de verdad. Ojo: si ese nucleo se satura, el "
                    "anillo se queda sin correr; mira las migraciones/s del log, que deben caer a 0");
REXCVAR_DEFINE_INT32(nfsmw_nativo_espera_regmem_us, 1000, "NFSMW",
                     "Renderizador nativo: pausa entre sondeos de WAIT_REG_MEM en microsegundos (en la "
                     "Switch es exacta; en Windows, por debajo de 1000 solo se cede el turno)");
REXCVAR_DEFINE_INT32(nfsmw_nativo_diag_fotograma_s, 0, "NFSMW",
                     "Renderizador nativo: pasados estos segundos, anota en el log cada dibujo y "
                     "cada copia de un fotograma entero (0 = no; solo pruebas)");
REXCVAR_DEFINE_STRING(nfsmw_nativo_diag_vertices_ps, "", "NFSMW",
                      "Renderizador nativo: en el fotograma trazado, para los dibujos con estos PS "
                      "(numeros separados por comas) anota los fetch del VS y los bytes de sus "
                      "primeros vertices y texels (solo pruebas)");
// The game measures with an occlusion query how much of the sun is visible (sub_82225438: GetData, Issue(BEGIN),
// a draw and Issue(END)) and uses it to turn off the flare when trees or terrain cover it. With the faked count of
// 1000 samples the flare was always drawn in full: sky burned to white and blue or purple halos in the trees.
REXCVAR_DEFINE_INT32(nfsmw_nativo_oclusion, 1, "NFSMW",
                     "Renderizador nativo: consultas de oclusion del juego (el destello del sol). 1 = medidas en la GPU, "
                     "como la Xbox 360; 0 = cuenta fingida de 1000 muestras (lo de antes de la build 146: el destello "
                     "no se tapa nunca); 2 = cuenta fingida de 0 muestras (solo pruebas: nunca hay destello)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(nfsmw_nativo_diag_constantes_ps, "", "NFSMW",
                      "Renderizador nativo (solo pruebas): numeros de PS (separados por comas) de los que se anotan sus "
                      "12 primeras constantes, como mucho cada nfsmw_nativo_diag_constantes_ms");
REXCVAR_DEFINE_INT32(nfsmw_nativo_diag_constantes_ms, 250, "NFSMW",
                     "Renderizador nativo (solo pruebas): intervalo minimo entre anotaciones de constantes por PS")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(nfsmw_nativo_oclusion_alternar_s, 0, "NFSMW",
                     "Renderizador nativo (solo pruebas): con nfsmw_nativo_oclusion = 1 y N > 0, alterna cada N segundos "
                     "entre consultas medidas (tramos pares) y la cuenta fingida de 1000 (tramos impares), y anota cada "
                     "cambio, para comparar el destello del sol en la misma carrera")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_nativo_oclusion_escala, 0, "NFSMW",
                     "Renderizador nativo: muestras por pixel con que se cuentan las consultas de oclusion (0 = las del "
                     "modo de antialiasing que tiene elegido el juego, como en la Xbox 360; 1-16 = fijas, solo pruebas)")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard lives in nfsmw_recorte_sombras.cpp, which owns the switch.
namespace nfsmw::guardia30 {
void Latir(double ms);
void Informe();
}  // namespace nfsmw::guardia30

namespace nfsmw::render_targets {
uint32_t MuestrasOriginalesModoActual(const uint8_t* base);  // nfsmw_render_targets.cpp
}

namespace nfsmw::nativo {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;  // X_STATUS_SUCCESS and X_STATUS_UNSUCCESSFUL need it too
using Reloj = std::chrono::steady_clock;
using ContextoSalida = rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext;

std::atomic<uint64_t> g_swaps_nativos{0};  // SwapsNativos(), for the watchdog

/*
 * Adding without a lock in the counters only the ring writes.
 *
 * Stack sampling pointed here: 36 % of the ring thread's time is in BucleAnillo -> Paquete -> PaqueteTipo3. And
 * every packet did a `fetch_add` on an atomic. On Cortex-A57 (ARMv8.0, without the ARMv8.1 atomic instructions)
 * a fetch_add is an ldxr/stxr loop with an exclusive reservation: ~15-20 cycles, and it also invalidates the line
 * on the other cores. At 83,000 packets per frame that is ~1.1 ms per frame spent counting packets.
 *
 * Why removing it is safe: these counters are written by a single thread, the ring thread (Paquete and
 * PaqueteTipo3 are only called from BucleAnillo and from BuferIndirecto, which hangs off it), and the only reader
 * is Informe, which runs on that same thread (BucleAnillo calls it on every loop). The shutdown Informe(true)
 * runs after hilo_anillo_->Wait(), that is, with the thread already dead. Without two writers there is no need
 * for an atomic read-modify-write.
 *
 * They stay std::atomic on purpose (the type does not change, nor do the readers); the only thing that changes
 * is how they are incremented: a relaxed load and store, which on 64-bit ARM64 are a plain ldr and str, without
 * exclusive reservation and without barriers. It is still correct for an outside reader (it never sees half a
 * value) and it stops costing.
 *
 * Do not use this for counters another thread touches: escrituras_wptr_ (written by the game thread through
 * MMIO), vblanks_ and contador_ (the vblank thread). Those keep fetch_add.
 */
template <typename T>
inline void SumarSoloAnillo(std::atomic<T>& contador, T cuanto = 1) {
  contador.store(contador.load(std::memory_order_relaxed) + cuanto, std::memory_order_relaxed);
}

constexpr uint32_t kBaseMmio = 0x7FC80000;
/*
 * From 16 to 128, because the packet count grew sixfold.
 *
 * 16 was chosen when there were ~14,000 packets per frame. Stack sampling gives the real count: 20.8 million
 * packets per interval, which at 18.1 packets per draw is ~83,000 per frame. At 1 in 16 that is 5,187 clock
 * reads per frame, and each Reloj::now() is a CNTPCT_EL0 read plus the conversion.
 *
 * With 128 there are 648 samples per frame, still plenty for a mean: the relative error of a sample of 648 is
 * 4 %, and these times are read to split percentages, not for exact figures.
 */
constexpr uint64_t kCronometroPaquetesCada = 128;  // packets per timed one (power of 2)
/*
 * From 8 to 64. At 1 in 8, this timer and above all the stage timer in nfsmw_nativo_dibujos.cpp (about 20 clock
 * reads per timed draw) added up to thousands of clock reads per frame in the alley, with 2,500-4,000 draws.
 * With 64 there are ~1,400 measured draws per second, plenty for a mean every 20 s. The "tiempos" line keeps
 * its scale: the measured time is multiplied by this same factor and divided by all the draws.
 */
constexpr uint64_t kCronometroDibujosCada = 64;   // draws per timed one (power of 2)
constexpr uint32_t kMascaraMmio = 0xFFFF0000;
constexpr uint32_t kTamanoMmio = 0x0000FFFF;
constexpr uint32_t kNumRegistros = 0x5003;  // RegisterFile::kRegisterCount

// Registers with a fixed value or with an effect, as in graphics_system.cpp.
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1GrphPrimarySurface = 0x1844;
// Gamma ramp, from DC_LUT_RW_MODE (0x1921) to DC_LUT_WRITE_EN_MASK (0x1927) (register_table.inc).
// The mask is 0x1927 (an earlier version stopped at 0x1926), and the output applies the ramp.
constexpr uint32_t kRegRampaPrimero = 0x1921;
constexpr uint32_t kRegRampaIndice = 0x1922;
constexpr uint32_t kRegRampaSecuencial = 0x1923;
constexpr uint32_t kRegRampa30 = 0x1925;
constexpr uint32_t kRegRampaMascara = 0x1927;
constexpr uint32_t kRegRampaUltimo = 0x1927;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

/*
 * Register generations, so that a draw does not redo work whose inputs did not change.
 *
 * Measured on the console (race): the ring thread is busy 42.5 ms of every 44.2 ms frame, so it is the
 * limit, not the GPU (37.1 ms of real work). Of those 42.5 ms, draw packets (op 22) take 29.5, and inside
 * the translator the most expensive stages are "texturas" (4.1 ms) and half of "pipeline", which is
 * really viewport and scissor.
 *
 * Both are pure functions of registers that almost never change between consecutive draws: the fetch
 * constants (0x4800-0x48BF) and the viewport state (viewport, scissor, clip and window offset). This
 * tracks when they really change (same technique as generacion_constantes_vs_/ps_: compare the value
 * before writing it), and the number travels in PeticionDibujo so the translator can skip the work with
 * a single uint64 comparison.
 *
 * There are 32 fetch constants of 6 words each: the first 16 are used by textures (kRegFetch in
 * nfsmw_nativo_dibujos.cpp) and vertex fetches read theirs from the same block.
 */
constexpr uint32_t kRegFetchPrimero = 0x4800;  // SHADER_CONSTANT_FETCH_00_0
constexpr uint32_t kRegFetchUltimo = 0x48BF;   // SHADER_CONSTANT_FETCH_31_5

// Viewport state registers: PA_SC_WINDOW_OFFSET/SCISSOR_TL/BR, PA_CL_VPORT_[XYZ]SCALE/OFFSET,
// PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL and PA_SU_VTX_CNTL. No VGT_* register is included:
// VGT_DRAW_INITIATOR (0x21FC) changes on every draw and would make the generation useless.
constexpr uint32_t kRegEncuadrePrimero = 0x2080;
constexpr uint32_t kRegEncuadreUltimo = 0x2302;
inline bool EsRegistroDeEncuadre(uint32_t indice) {
  // One subtraction and one comparison: rejects everything outside the range.
  if (indice - kRegEncuadrePrimero > kRegEncuadreUltimo - kRegEncuadrePrimero) {
    return false;
  }
  return indice <= 0x2082 ||                      // PA_SC_WINDOW_OFFSET .. SCISSOR_BR
         (indice >= 0x210F && indice <= 0x2114) ||  // PA_CL_VPORT_XSCALE .. ZOFFSET
         (indice >= 0x2204 && indice <= 0x2206) ||  // PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL
         indice == 0x2302;                          // PA_SU_VTX_CNTL
}

/*
 * Phase 2 of the Direct3D-level renderer: the ring-thread half.
 * Applying registers was measured at ~146 ms/s of the ring thread (~4 ms per frame, with the ring at
 * 95 %), almost all of it in EscribirRegistrosEnBloque: 94.5 million words in blocks against 2.2 million
 * one at a time in 10 s. The regular loop (TramoEscalar) byte-swaps each word, compares it and classifies
 * it with several branches. TramoVectorial does the same 4 words at a time with NEON: it splits the span
 * at the boundaries of each class (VS, PS and fetch constants and the small viewport state pieces), copies
 * each piece byte-swapped and only checks whether anything changed in the piece, which is all that was
 * used from the classification.
 */
inline void TramoEscalar(uint32_t indice, uint32_t* destino, const uint8_t* origen, uint32_t n, bool& vs,
                         bool& ps, bool& fetch, bool& encuadre) {
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t nuevo = rex::memory::load_and_swap<uint32_t>(origen + size_t(i) * 4);
    if (destino[i] != nuevo) {
      const uint32_t abs = indice + i;
      if (abs >= 0x4000) {
        if (abs < 0x4400) {
          vs = true;
        } else if (abs < 0x4800) {
          ps = true;
        } else if (abs <= kRegFetchUltimo) {
          fetch = true;
        }
      } else if (EsRegistroDeEncuadre(abs)) {
        encuadre = true;
      }
      destino[i] = nuevo;
    }
  }
}

// Copies n big-endian words byte-swapped; true if any of them differed from the previous value.
inline bool CopiarConCambio(uint32_t* destino, const uint8_t* origen, uint32_t n) {
  uint32_t i = 0;
  bool cambio = false;
#if defined(__aarch64__)
  uint32x4_t diferencia = vdupq_n_u32(0);
  for (; i + 4 <= n; i += 4) {
    const uint32x4_t nuevo = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(origen + size_t(i) * 4)));
    diferencia = vorrq_u32(diferencia, veorq_u32(nuevo, vld1q_u32(destino + i)));
    vst1q_u32(destino + i, nuevo);
  }
  cambio = vmaxvq_u32(diferencia) != 0;
#endif
  for (; i < n; ++i) {
    const uint32_t nuevo = rex::memory::load_and_swap<uint32_t>(origen + size_t(i) * 4);
    cambio |= destino[i] != nuevo;
    destino[i] = nuevo;
  }
  return cambio;
}

inline void TramoVectorial(uint32_t indice, uint32_t* destino, const uint8_t* origen, uint32_t n, bool& vs,
                           bool& ps, bool& fetch, bool& encuadre) {
  /*
   * Fast path. Almost every span is a block of VS or PS constants (or fetch constants) that lies entirely
   * inside its class: one CopiarConCambio plus marking the class if anything changed is enough, with no
   * search for boundaries (the loop below walks all 12 for every piece). Same if the span touches no class
   * and not the viewport state range: it is only copied. Anything that crosses a boundary still takes the
   * regular loop.
   * Same result as TramoEscalar: tested on the PC with 3.5 million blocks (every start from 0x1F00 to the
   * end with lengths 1 to 40, plus 2 million random ones), 0 differences. VerificarBloque still compares it
   * with the regular path for the first blocks of each run, like the rest of the vector path.
   */
  if (n != 0) {
    const uint32_t ultimo = indice + n - 1;  // EscribirRegistrosEnBloque already checked it stays inside the table
    if (indice >= 0x4000) {
      if (ultimo < 0x4400) {
        if (CopiarConCambio(destino, origen, n)) {
          vs = true;
        }
        return;
      }
      if (indice >= 0x4400 && ultimo < 0x4800) {
        if (CopiarConCambio(destino, origen, n)) {
          ps = true;
        }
        return;
      }
      if (indice >= 0x4800 && ultimo <= kRegFetchUltimo) {
        if (CopiarConCambio(destino, origen, n)) {
          fetch = true;
        }
        return;
      }
      if (indice > kRegFetchUltimo) {
        CopiarConCambio(destino, origen, n);  // booleans and loops: no class
        return;
      }
    } else if (ultimo < kRegEncuadrePrimero || (indice > kRegEncuadreUltimo && ultimo < 0x4000)) {
      CopiarConCambio(destino, origen, n);  // outside every class and the viewport state range
      return;
    }
  }
  // Where a class starts or ends (see EsRegistroDeEncuadre and TramoEscalar).
  static constexpr uint32_t kCortes[] = {kRegEncuadrePrimero, 0x2083, 0x210F, 0x2115, 0x2204, 0x2207,
                                         0x2302, 0x2303, 0x4000, 0x4400, 0x4800, kRegFetchUltimo + 1};
  uint32_t i = 0;
  while (i < n) {
    const uint32_t abs = indice + i;
    uint32_t fin = n;
    for (const uint32_t corte : kCortes) {
      if (corte > abs && corte - indice < fin) {
        fin = corte - indice;
      }
    }
    const bool cambio = CopiarConCambio(destino + i, origen + size_t(i) * 4, fin - i);
    if (cambio) {
      if (abs >= 0x4000) {
        if (abs < 0x4400) {
          vs = true;
        } else if (abs < 0x4800) {
          ps = true;
        } else if (abs <= kRegFetchUltimo) {
          fetch = true;
        }
      } else if (EsRegistroDeEncuadre(abs)) {
        encuadre = true;
      }
    }
    i = fin;
  }
}

constexpr uint32_t kMuestrasOclusion = 1000;  // query_occlusion_fake_sample_count
constexpr uint16_t kExtensionMaxima = 2048 >> 3;  // xenos::kTexture2DCubeMaxWidthHeight >> 3
constexpr int kMaxProfundidadIndirecta = 4;
constexpr auto kEsperaRegMemMax = std::chrono::milliseconds(200);
constexpr uint32_t kSalidaAncho = 1280;
constexpr uint32_t kSalidaAlto = 720;

uint64_t NanosegundosDesde(std::chrono::steady_clock::time_point inicio) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - inicio)
                      .count());
}

// Big-endian words of a command buffer. In the ring the position wraps around (the size is a power
// of two); in an indirect buffer it does not.
struct Lector {
  const uint8_t* base = nullptr;
  uint32_t mascara = 0;  // words - 1 in the ring; 0 in a linear buffer
  uint32_t pos = 0;
  uint32_t fin = 0;

  uint32_t Pendientes() const { return mascara ? ((fin - pos) & mascara) : (fin - pos); }
  uint32_t Mirar() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Leer() {
    const uint32_t valor = Mirar();
    Avanzar(1);
    return valor;
  }
  void Avanzar(uint32_t palabras) { pos = mascara ? ((pos + palabras) & mascara) : (pos + palabras); }
};

bool Compara(uint32_t info, uint32_t valor, uint32_t referencia) {
  switch (info & 0x7) {
    case 0x0:
      return false;
    case 0x1:
      return valor < referencia;
    case 0x2:
      return valor <= referencia;
    case 0x3:
      return valor == referencia;
    case 0x4:
      return valor != referencia;
    case 0x5:
      return valor >= referencia;
    case 0x6:
      return valor > referencia;
    default:
      return true;
  }
}

class SistemaGraficoNativo final : public rex::system::IGraphicsSystem {
 public:
  SistemaGraficoNativo() : registros_(kNumRegistros, 0), vistos_(kNumRegistros) {}
  ~SistemaGraficoNativo() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {
      // Steps C5c-C6: the XenosRecomp SPIR-V needs capabilities that the VkDevice only
      // enables if they are requested before it is created.
      rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
#if REX_PLATFORM_ANDROID
      // Native shaders read their own buffers. They do not use Xenos memory
      // export / EDRAM emulation, which requires vertex stores and atomics.
      provider_ = rex::ui::vulkan::VulkanProvider::Create(false, true);
#else
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
#endif
      if (!provider_) {
        REXLOG_ERROR("[nativo] No se pudo crear el dispositivo Vulkan");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto crear = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(crear);
    } else {
      crear();
    }
    if (!presenter_) {
      REXLOG_ERROR("[nativo] No se pudo crear el presentador");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[nativo] Sistema grafico nativo (C1): presentador del SDK, sin emulacion del Xenos");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registrado_) {
      mmio_registrado_ = memory_->AddVirtualMappedRange(kBaseMmio, kMascaraMmio, kTamanoMmio, this,
                                                        &LeerMmio, &EscribirMmio);
      if (!mmio_registrado_) {
        REXLOG_ERROR("[nativo] No se pudo registrar el rango MMIO de la GPU");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (activo_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }
    // Step C5a: the NFSSPV library sits next to the executable (it is built from the game
    // data, so it is not distributed). Without it the game still runs, without identification.
    // Next to the executable (the app folder on Android) or, failing that, with the game files, where it
    // is easier to copy on a phone.
#if defined(NFSC_RECOMP)
    constexpr const char* kNombreBiblioteca = "nfscarbon_shaders.nfsp";
#else
    constexpr const char* kNombreBiblioteca = "nfsmw_shaders.nfsp";
#endif
    std::filesystem::path biblioteca = rex::filesystem::GetExecutableFolder() / kNombreBiblioteca;
    if (std::error_code ec; !std::filesystem::is_regular_file(biblioteca, ec)) {
      const std::string juego = rex::cvar::GetFlagByName("game_data_root");
      if (!juego.empty() && std::filesystem::is_regular_file(std::filesystem::path(juego) / kNombreBiblioteca, ec)) {
        biblioteca = std::filesystem::path(juego) / kNombreBiblioteca;
      }
    }
    if (shaders_.Cargar(biblioteca)) {
      ActivarGanchos(&shaders_);  // step C5b: shader objects and Draw* records
    }
    ultimo_informe_ = Reloj::now();
    ultimo_tiempo_informe_ = ultimo_informe_;
    inicio_sistema_ = ultimo_informe_;
    hilo_vblank_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return BucleVblank(); }));
    hilo_vblank_->set_name("GPU VSync nativo");
    hilo_vblank_->Create();
    hilo_anillo_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return BucleAnillo(); }));
    hilo_anillo_->set_name("GPU anillo nativo");
    hilo_anillo_->Create();
    // From here on FlushState can write the composite marker (ProcesarMarcador).
    ActivarConsumidorMarcadores(true);
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_datos_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[nativo] Callback de interrupcion {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As in CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    anillo_palabras_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    anillo_base_.store(ptr, std::memory_order_release);
    generacion_anillo_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[nativo] Anillo en {:08X}, {} palabras", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    lectura_devuelta_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    ActivarGanchos(nullptr);  // the game hooks stop touching the library
    ActivarConsumidorMarcadores(false);  // FlushState goes back to the game's own path
    if (activo_.exchange(false)) {
      {
        std::lock_guard<std::mutex> cerrojo(anillo_mutex_);
      }
      anillo_cv_.notify_all();
      if (hilo_anillo_) {
        hilo_anillo_->Wait(0, 0, 0, nullptr);
        hilo_anillo_.reset();
      }
      if (hilo_vblank_) {
        hilo_vblank_->Wait(0, 0, 0, nullptr);
        hilo_vblank_.reset();
      }
      Informe(true);
    }
    // The framebuffers point to presenter images: destroy them before it.
    DestruirVulkan();
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- Registros MMIO -------------------------------------------------------

  static uint32_t LeerMmio(void* ppc_context, void* contexto, uint32_t direccion) {
    (void)ppc_context;
    return static_cast<SistemaGraficoNativo*>(contexto)->LeerRegistroMmio(direccion);
  }

  static void EscribirMmio(void* ppc_context, void* contexto, uint32_t direccion, uint32_t valor) {
    (void)ppc_context;
    static_cast<SistemaGraficoNativo*>(contexto)->EscribirRegistroMmio(direccion, valor);
  }

  uint32_t LeerRegistroMmio(uint32_t direccion) {
    const uint32_t r = (direccion & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE modo;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&modo);
        return std::min(uint32_t(modo.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE modo;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&modo);
        const uint32_t ancho = std::min(uint32_t(modo.display_width), uint32_t(0x0FFF));
        const uint32_t alto = std::min(uint32_t(modo.display_height), uint32_t(0x0FFF));
        return (ancho << 16) | alto;
      }
      default:
        break;
    }
    AnotarPrimeraVez("lectura", r, 0);
    return Registro(r);
  }

  void EscribirRegistroMmio(uint32_t direccion, uint32_t valor) {
    const uint32_t r = (direccion & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      puntero_escritura_.store(valor, std::memory_order_release);
      escrituras_wptr_.fetch_add(1, std::memory_order_relaxed);
      // Taking the lock for an instant avoids losing the wakeup if the ring
      // thread is right between checking and going to sleep.
      {
        std::lock_guard<std::mutex> cerrojo(anillo_mutex_);
      }
      anillo_cv_.notify_one();
    } else if (r != kRegD1GrphPrimarySurface) {
      AnotarPrimeraVez("escritura", r, valor);
    }
    GuardarRegistro(r, valor);  // no side effects through MMIO (graphics_system.cpp:265-281)
  }

  // The gamma ramp the game loads, with the same logic as the SDK's CommandProcessor::WriteRegister
  // (DC_LUT_SEQ_COLOR in red, green and blue; DC_LUT_30_COLOR with blue in bits 0-9). Called before the
  // value is stored. The write mask (blue at bit 0) and the index that only advances after DC_LUT_30_COLOR
  // match the SDK; every change bumps version_rampa_ and the output picks it up on the next Swap.
  void AnotarRampaGamma(uint32_t indice, uint32_t valor) {
    ++escrituras_rampa_[indice - kRegRampaPrimero];
    if (indice == kRegRampaIndice) {
      componente_rampa_ = 0;
    } else if (indice == kRegRampaMascara) {
      mascara_rampa_ = valor & 0b111;
    } else if (indice == kRegRampaSecuencial) {
      const uint32_t i = registros_[kRegRampaIndice] & 0xFF;
      if (mascara_rampa_ & (UINT32_C(1) << (2 - componente_rampa_))) {
        rampa_gamma_[i][componente_rampa_] = uint16_t((valor >> 6) & 0x3FF);
        ++version_rampa_;
      }
      if (++componente_rampa_ >= 3) {
        componente_rampa_ = 0;
        registros_[kRegRampaIndice] = (registros_[kRegRampaIndice] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
      }
    } else if (indice == kRegRampa30) {
      const uint32_t i = registros_[kRegRampaIndice] & 0xFF;
      if (mascara_rampa_ & 0b001) {
        rampa_gamma_[i][2] = uint16_t(valor & 0x3FF);
      }
      if (mascara_rampa_ & 0b010) {
        rampa_gamma_[i][1] = uint16_t((valor >> 10) & 0x3FF);
      }
      if (mascara_rampa_ & 0b100) {
        rampa_gamma_[i][0] = uint16_t((valor >> 20) & 0x3FF);
      }
      if (mascara_rampa_) {
        ++version_rampa_;
      }
      componente_rampa_ = 0;
      registros_[kRegRampaIndice] = (registros_[kRegRampaIndice] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
    }
  }

  // EVENT_WRITE_ZPD: the sample counters of an occlusion query.
  // How the game's D3D does it:
  //   - Issue(BEGIN) (sub_8258F810) marks the structure at base + 0 with 0xFFFFFEED and requests the
  //     counters at base + 32.
  //   - Issue(END) requests them at base + 0 (sub_8258EA28).
  //   - GetData (sub_8258F998) waits for base + 0 to lose the marks and returns
  //     ZPass(base + 0) - ZPass(base + 32).
  // Here (nfsmw_nativo_oclusion = 1): at the start, base + 32 is zeroed, and the draws up to the end are
  // counted on the GPU with host queries. At the end, base + 0 gets the last measured count of that
  // structure: one from an earlier frame, because host queries are read when their work finishes. Anything
  // that does not fit keeps the old behavior (a faked count).
  void ConsultaOclusion(uint32_t direccion) {
    using Cuentas = xenos::xe_gpu_depth_sample_counts;
    constexpr uint32_t kMarca = 0xEDFEFFFF;  // 0xFFFFFEED written big-endian by D3D, read as little-endian
    const auto marcada = [](const Cuentas* c) {
      return uint32_t(c->ZPass_A) == kMarca || uint32_t(c->ZPass_B) == kMarca || uint32_t(c->ZFail_A) == kMarca ||
             uint32_t(c->ZFail_B) == kMarca;
    };
    auto* cuentas = memory_->TranslatePhysical<Cuentas*>(direccion);
    const bool marcas_aqui = marcada(cuentas);
    const bool marcas_antes = direccion >= 32 && marcada(memory_->TranslatePhysical<Cuentas*>(direccion - 32));
    int32_t modo = REXCVAR_GET(nfsmw_nativo_oclusion);
    if (modo == 1 && destinos_ && !destinos_->OclusionGpuPermitida()) {
      modo = 2;  // safe fallback: no flare or driver occlusion calls
    }
    // Test cvar nfsmw_nativo_oclusion_alternar_s: odd intervals use the old faked count.
    if (const int32_t alternar_s = REXCVAR_GET(nfsmw_nativo_oclusion_alternar_s); modo == 1 && alternar_s > 0) {
      const auto ahora = Reloj::now();
      if (!oclusion_prueba_iniciada_) {
        oclusion_prueba_iniciada_ = true;
        oclusion_prueba_inicio_ = ahora;
      }
      const auto segundos =
          std::chrono::duration_cast<std::chrono::seconds>(ahora - oclusion_prueba_inicio_).count();
      const bool fingida = (segundos / alternar_s) % 2 == 1;
      if (fingida != oclusion_prueba_fingida_) {
        oclusion_prueba_fingida_ = fingida;
        if (fingida && oclusion_base_ != 0 && destinos_) {
          uint64_t descartadas = 0;
          destinos_->TerminarOclusion(oclusion_base_, descartadas);
          oclusion_base_ = 0;
        }
        REXLOG_INFO("[nativo] prueba de oclusion: {}", fingida ? "fingidas (1000)" : "medidas");
      }
      if (fingida) {
        modo = 0;
      }
    }
    const char* que = nullptr;
    uint32_t escrita = UINT32_MAX;
    uint64_t medidas = 0;
    bool medida = false;
    uint32_t escala = 0;
    if (modo == 1 && destinos_) {
      if (oclusion_base_ != 0 && direccion == oclusion_base_) {
        medida = destinos_->TerminarOclusion(direccion, medidas);
        oclusion_base_ = 0;
        escala = EscalaOclusion();
        escrita = medida ? uint32_t(std::min<uint64_t>(medidas * escala, 0xFFFFFF)) : kMuestrasOclusion;
        std::memset(cuentas, 0, sizeof(*cuentas));
        cuentas->ZPass_A = escrita;
        cuentas->Total_A = escrita;
        que = medida ? "final" : "final sin medida";
        if (medida) {
          ++oclusion_contadores_[1];
          oclusion_contadores_[5] += escrita;
          oclusion_contadores_[6] = std::max<uint64_t>(oclusion_contadores_[6], escrita);
        } else {
          ++oclusion_contadores_[2];
        }
        // By render target: the 320 one (the sun) or a scene one (640 or more, scaled by the AA mode).
        const size_t grupo = escala > 1 ? 1 : 0;
        ++oclusion_por_destino_[grupo];
        if (medida) {
          oclusion_max_por_destino_[grupo] = std::max(oclusion_max_por_destino_[grupo], escrita);
        }
        if (grupo == 1 && avisos_oclusion_escena_ < 12) {
          ++avisos_oclusion_escena_;
          const uint32_t info = Registro(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
          REXLOG_INFO("[nativo] C2 oclusion en destino de escena {}: estructura {:08X}, pitch {}, MSAA {}, escala {}, "
                      "muestras del host {} ({}), escritas {}",
                      avisos_oclusion_escena_, direccion, info & 0x3FFF, (info >> 16) & 0x3, escala, medidas,
                      medida ? "medidas" : "sin medida", escrita);
        }
      } else if (direccion >= 32 && (marcas_antes || bases_oclusion_.count(direccion - 32))) {
        const uint32_t base = direccion - 32;
        if (oclusion_base_ != 0) {
          uint64_t descartadas = 0;
          destinos_->TerminarOclusion(oclusion_base_, descartadas);  // la anterior no llego a su Issue(END)
          ++oclusion_contadores_[3];
        }
        if (bases_oclusion_.size() < 64) {
          bases_oclusion_.insert(base);
        }
        destinos_->EmpezarOclusion(base);
        oclusion_base_ = base;
        std::memset(cuentas, 0, sizeof(*cuentas));
        que = "principio";
        ++oclusion_contadores_[0];
      }
    }
    if (!que) {
      // The previous behavior (also the emulated path's): a faked count at the end.
      std::memset(cuentas, 0, sizeof(*cuentas));
      if (marcas_aqui) {
        escrita = modo == 2 ? 0 : kMuestrasOclusion;
        cuentas->ZPass_A = escrita;
        cuentas->Total_A = escrita;
        ++oclusion_contadores_[4];
      }
      que = modo == 1 ? "sin pareja" : "fingida";
    }
    if (avisos_oclusion_ < 8) {  // was 48; these lines are long and the ring thread writes them
      ++avisos_oclusion_;
      const uint8_t* base_virtual = memory_->virtual_membase();
      const auto leer_be = [base_virtual](uint32_t d) {
        uint32_t v = 0;
        std::memcpy(&v, base_virtual + d, sizeof(v));
        return __builtin_bswap32(v);
      };
#if defined(NFSC_RECOMP)
      // Most Wanted's globals (the renderer object and its AA mode): not mapped in NFS Carbon.
      const uint32_t renderizador = 0;
      const auto leer_juego = [](uint32_t) { return uint32_t(0); };
#else
      const uint32_t renderizador = leer_be(0x82A2D1AC);
      const auto& leer_juego = leer_be;
#endif
      const uint32_t info = Registro(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
      REXLOG_INFO("[nativo] C2 oclusion {}: ZPD en {:08X} ({}), marcas aqui {} y 32 antes {}; muestras del host {} ({}), "
                  "escala {}, escritas {}; bins {:016X}/{:016X}; RB_SURFACE_INFO {:08X} (pitch {}, MSAA {}), "
                  "RB_DEPTH_INFO {:08X}; modo de AA del juego {}, calidad {}",
                  avisos_oclusion_, direccion, que, marcas_aqui, marcas_antes, medidas, medida ? "medidas" : "sin medida",
                  escala, escrita == UINT32_MAX ? -1 : int64_t(escrita), bin_select_, bin_mask_, info, info & 0x3FFF,
                  (info >> 16) & 0x3, Registro(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                  renderizador ? int64_t(leer_juego(renderizador)) : -1, leer_juego(0x82A2CEE4));
    }
  }

  // Samples per pixel the Xbox 360 would count in the query's render target: the MSAA samples the target
  // requests and, for scene targets (pitch of 640 or more), those of the game's AA mode before it is
  // removed (nfsmw_render_sin_mosaico). The sun flare is queried on a 320 target without MSAA: 1.
  uint32_t EscalaOclusion() const {
    const int32_t fija = REXCVAR_GET(nfsmw_nativo_oclusion_escala);
    if (fija > 0) {
      return uint32_t(fija);
    }
    const uint32_t info = Registro(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t muestras_destino = UINT32_C(1) << std::min<uint32_t>((info >> 16) & 0x3, 2);
    const uint32_t muestras_escena =
        (info & 0x3FFF) >= 640 ? nfsmw::render_targets::MuestrasOriginalesModoActual(memory_->virtual_membase()) : 1;
    return std::max(muestras_destino, muestras_escena);
  }

  void AnotarPrimeraVez(const char* que, uint32_t indice, uint32_t valor) {
    if (indice >= kNumRegistros || vistos_[indice].exchange(1, std::memory_order_acq_rel)) {
      return;
    }
    REXLOG_INFO("[nativo] Registro MMIO {:04X}: primera {} ({:08X})", indice, que, valor);
  }

  // The registers are touched by the game thread (MMIO) and by the ring thread. As in the
  // emulated path, races on integer values are harmless.
  void GuardarRegistro(uint32_t indice, uint32_t valor) {
    if (indice < kNumRegistros) {
      /*
       * This entry point is used by MMIO from the game thread and by VIZ_QUERY, and it does not go
       * through EscribirRegistro. Neither touches the fetch constants or the viewport state today (the
       * 360's D3D writes GPU registers through the ring), but if one ever did and the generation were
       * not bumped, the translator would keep the stale viewport or textures. It is called very rarely
       * compared with the ring, so the safeguard costs nothing.
       */
      if (registros_[indice] != valor) {
        if (indice >= kRegFetchPrimero && indice <= kRegFetchUltimo) {
          ++generacion_fetch_;
        } else if (EsRegistroDeEncuadre(indice)) {
          ++generacion_encuadre_;
        }
      }
      registros_[indice] = valor;
    }
  }

  // A write from the ring, with the side effects it has when the GPU processes it
  // (CommandProcessor::WriteRegister). SCRATCH_REG values are copied to memory: the
  // D3D writes one and then waits with WAIT_REG_MEM until it shows up at SCRATCH_ADDR
  // (measured on the PC: without this the game stops within the first second).
  // COHER_STATUS_HOST stays marked as pending until the next poll.
  void EscribirRegistro(uint32_t indice, uint32_t valor) {
    if (indice >= kNumRegistros) {
      return;
    }
    /*
     * Only when the value really changes.
     *
     * The Xbox 360 D3D dumps the constant block before every draw, and many constants are identical
     * between draws of the same material. Bumping the generation on every write made 60 % of the draws
     * re-upload their 7.7 KB of constants to the buffer (12.54 MB per frame, measured) into memory with
     * no CPU cache (2654 MB/s), on the thread that already uses 96 % of a core.
     *
     * Comparing before writing costs one read of a line that is about to be written anyway, so it is
     * already in cache. And the result is bit-for-bit identical: if the value does not change, the
     * shader reads the same thing.
     */
    if (indice >= 0x4000) {
      if (indice < 0x4800) {  // VS and PS constants (native draws)
        if (registros_[indice] != valor) {
          ++(indice < 0x4400 ? generacion_constantes_vs_ : generacion_constantes_ps_);
        } else {
          ++constantes_sin_cambio_;
        }
        ++constantes_escritas_;
      } else if (indice <= kRegFetchUltimo) {  // fetch constants: textures and vertices
        if (registros_[indice] != valor) {
          ++generacion_fetch_;
        } else {
          ++fetch_sin_cambio_;
        }
        ++fetch_escritas_;
      }
    } else if (EsRegistroDeEncuadre(indice)) {
      if (registros_[indice] != valor) {
        ++generacion_encuadre_;
      } else {
        ++encuadre_sin_cambio_;
      }
      ++encuadre_escritas_;
    }
    if (indice == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      valor |= UINT32_C(0x80000000);
    }
    if (indice >= kRegRampaPrimero && indice <= kRegRampaUltimo) {
      AnotarRampaGamma(indice, valor);
    }
    registros_[indice] = valor;
    if (indice >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 &&
        indice <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = indice - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registros_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        const uint32_t direccion = registros_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(direccion), valor);
        SumarSoloAnillo(escrituras_memoria_);
      }
    }
  }

  // A block of consecutive registers without side effects, in one go. Returns false (and consumes nothing)
  // if it touches COHER_STATUS_HOST or the SCRATCH_REG registers, if it goes past the table or if it is
  // disabled.
  bool EscribirRegistrosEnBloque(uint32_t indice, uint32_t cuenta, Lector& datos) {
    if (registros_en_bloque_ < 0) {
      registros_en_bloque_ = REXCVAR_GET(nfsmw_nativo_registros_en_bloque) ? 1 : 0;
      REXLOG_INFO("[nativo] registros en bloque (nfsmw_nativo_registros_en_bloque) = {}",
                  registros_en_bloque_ ? "SI" : "no");
    }
    if (!registros_en_bloque_ || cuenta < 2 || indice >= kNumRegistros || cuenta > kNumRegistros - indice) {
      return false;
    }
    const uint32_t fin = indice + cuenta;
    const auto toca = [indice, fin](uint32_t desde, uint32_t hasta) { return indice < hasta && desde < fin; };
    if (toca(rex::graphics::XE_GPU_REG_COHER_STATUS_HOST, rex::graphics::XE_GPU_REG_COHER_STATUS_HOST + 1) ||
        toca(rex::graphics::XE_GPU_REG_SCRATCH_REG0, rex::graphics::XE_GPU_REG_SCRATCH_REG7 + 1) ||
        toca(kRegRampaPrimero, kRegRampaUltimo + 1)) {
      return false;
    }
    // Same as above, but a block does not know whether it changed until it has been copied, so
    // the generation is bumped afterwards, and only if some word in the range differs.
    const bool mira_vs = toca(0x4000, 0x4400);
    const bool mira_ps = toca(0x4400, 0x4800);
    // The same for the fetch constants and the viewport state. The viewport state is four small
    // separate pieces, so it is enough to check whether the block crosses the range that holds them:
    // inside the loop each word is already classified, and a generation bumped too often only makes
    // the translator do extra work, it never draws wrong.
    const bool mira_fetch = toca(kRegFetchPrimero, kRegFetchUltimo + 1);
    const bool mira_encuadre = toca(kRegEncuadrePrimero, kRegEncuadreUltimo + 1);
    const bool mira_valores = mira_vs || mira_ps || mira_fetch || mira_encuadre;
    bool cambio_vs = false;
    bool cambio_ps = false;
    bool cambio_fetch = false;
    bool cambio_encuadre = false;
    // The vector path, with a self-checking guard (see VerificarBloque).
    if (registros_vectoriales_ < 0) {
      registros_vectoriales_ = REXCVAR_GET(nfsmw_nativo_registros_vectoriales) ? 1 : 0;
      REXLOG_INFO("[nativo] registros vectoriales (nfsmw_nativo_registros_vectoriales) = {}",
                  registros_vectoriales_ ? "SI, comprobando los primeros bloques contra el camino de siempre"
                                         : "no");
    }
    const bool vectorial = registros_vectoriales_ > 0;
    const bool verificar = vectorial && bloques_verificados_ < kBloquesAVerificar;
    Lector datos_verificacion;
    if (verificar) {
      datos_verificacion = datos;
      copia_verificacion_.assign(registros_.begin() + indice, registros_.begin() + indice + cuenta);
    }
    uint32_t indice_actual = indice;
    uint32_t* destino = registros_.data() + indice;
    uint32_t quedan = cuenta;
    while (quedan) {
      // Contiguous span: up to the end of the ring (in a linear buffer, everything left).
      const uint32_t tramo = datos.mascara ? std::min(quedan, datos.mascara + 1 - datos.pos) : quedan;
      const uint8_t* origen = datos.base + size_t(datos.pos) * 4;
      if (mira_valores) {
        if (vectorial) {
          TramoVectorial(indice_actual, destino, origen, tramo, cambio_vs, cambio_ps, cambio_fetch,
                         cambio_encuadre);
        } else {
          TramoEscalar(indice_actual, destino, origen, tramo, cambio_vs, cambio_ps, cambio_fetch,
                       cambio_encuadre);
        }
        if (mira_vs || mira_ps) {
          constantes_escritas_ += tramo;
        }
        if (mira_fetch) {
          fetch_escritas_ += tramo;
        }
        if (mira_encuadre) {
          encuadre_escritas_ += tramo;
        }
      } else if (vectorial) {
        CopiarConCambio(destino, origen, tramo);
      } else {
        for (uint32_t i = 0; i < tramo; ++i) {
          destino[i] = rex::memory::load_and_swap<uint32_t>(origen + size_t(i) * 4);
        }
      }
      indice_actual += tramo;
      destino += tramo;
      datos.Avanzar(tramo);
      quedan -= tramo;
    }
    if (verificar) {
      VerificarBloque(indice, cuenta, datos_verificacion, mira_valores, cambio_vs, cambio_ps, cambio_fetch,
                      cambio_encuadre);
    }
    if (cambio_vs) {
      ++generacion_constantes_vs_;
    }
    if (cambio_ps) {
      ++generacion_constantes_ps_;
    }
    if (cambio_fetch) {
      ++generacion_fetch_;
    }
    if (cambio_encuadre) {
      ++generacion_encuadre_;
    }
    if ((mira_vs && !cambio_vs) || (mira_ps && !cambio_ps)) {
      ++bloques_constantes_sin_cambio_;
    }
    if (mira_vs || mira_ps) {
      ++bloques_constantes_;
    }
    return true;
  }

  /*
   * The vector path guard. For the first kBloquesAVerificar blocks, the block is replayed through the
   * regular path (TramoEscalar) on a copy of the previous registers, and the results are compared register
   * by register and class by class. If anything differs, the regular path's result is written, the log
   * reports it and the vector path is turned off for the rest of the run.
   */
  void VerificarBloque(uint32_t indice, uint32_t cuenta, Lector datos, bool mira_valores, bool& vs, bool& ps,
                       bool& fetch, bool& encuadre) {
    bool ref_vs = false;
    bool ref_ps = false;
    bool ref_fetch = false;
    bool ref_encuadre = false;
    uint32_t hecho = 0;
    while (hecho < cuenta) {
      const uint32_t quedan = cuenta - hecho;
      const uint32_t tramo = datos.mascara ? std::min(quedan, datos.mascara + 1 - datos.pos) : quedan;
      TramoEscalar(indice + hecho, copia_verificacion_.data() + hecho, datos.base + size_t(datos.pos) * 4, tramo,
                   ref_vs, ref_ps, ref_fetch, ref_encuadre);
      datos.Avanzar(tramo);
      hecho += tramo;
    }
    ++bloques_verificados_;
    const bool iguales_valores =
        std::equal(copia_verificacion_.begin(), copia_verificacion_.end(), registros_.begin() + indice);
    const bool iguales_clases =
        !mira_valores || (ref_vs == vs && ref_ps == ps && ref_fetch == fetch && ref_encuadre == encuadre);
    if (!iguales_valores || !iguales_clases) {
      REXLOG_ERROR("[nativo] registros vectoriales: el bloque {:04X}+{} NO coincide con el camino de siempre "
                   "(valores {}, clases VS {}/{} PS {}/{} fetch {}/{} encuadre {}/{}). Se APAGA el camino vectorial.",
                   indice, cuenta, iguales_valores ? "iguales" : "DISTINTOS", vs, ref_vs, ps, ref_ps, fetch,
                   ref_fetch, encuadre, ref_encuadre);
      std::copy(copia_verificacion_.begin(), copia_verificacion_.end(), registros_.begin() + indice);
      if (mira_valores) {
        vs = ref_vs;
        ps = ref_ps;
        fetch = ref_fetch;
        encuadre = ref_encuadre;
      }
      registros_vectoriales_ = 0;
      return;
    }
    if (bloques_verificados_ == kBloquesAVerificar) {
      REXLOG_INFO("[nativo] registros vectoriales: {} bloques comprobados contra el camino de siempre, todos "
                  "iguales. Sigue el camino vectorial.",
                  bloques_verificados_);
    }
  }

  /*
   * Phase 2 of the Direct3D-level renderer: the FlushState composite marker.
   * See kMarcadorMagia in nfsmw_nativo_ganchos.h and the end of nfsmw_d3d_registros_nativo.cpp. FlushState
   * writes one NOP with all its register spans instead of ~7 type-0 packets and ~6 padding words per draw.
   *   - Apply mode: the spans are written here with the same effect as a type-0 block (values, generations
   *     and counters of EscribirRegistrosEnBloque).
   *   - Check mode: the type-0 packets of the same dump come right before it and have already been applied.
   *     It only checks that the registers hold what the marker says, and the result goes back to the game
   *     thread.
   * A NOP without the magic belongs to the game itself and is ignored, as before.
   */
  void ProcesarMarcador(Lector& datos, uint32_t palabras) {
    if (palabras < 2 || (datos.Mirar() & 0xFFFFFF00u) != kMarcadorMagia) {
      return;
    }
    const uint32_t cabecera = datos.Leer();
    const uint32_t modo = cabecera & 0x0Fu;               // registros: 0 sin tramos, 1 aplicar, 2 comprobar
    const uint32_t modo_dibujo = (cabecera >> 4) & 0x0Fu;  // phase 2b: 0 none, kDibujoAplicar, kDibujoComprobar
    const uint32_t secuencia = datos.Leer();
    uint32_t n = palabras - 2;  // draw record, span headers and values
    // The spans, contiguous in memory: D3D never splits a packet; if it wraps around the ring, it is copied
    // aside.
    const uint8_t* p = datos.base + size_t(datos.pos) * 4;
    if (datos.mascara && datos.pos + n > datos.mascara + 1) {
      marcador_plano_.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        std::memcpy(&marcador_plano_[i], datos.base + size_t((datos.pos + i) & datos.mascara) * 4, 4);
      }
      p = reinterpret_cast<const uint8_t*>(marcador_plano_.data());
    }
    // Phase 2b: the Draw* record of this FlushState, ahead of the spans. It is stored at the end, once the
    // whole marker has been validated.
    RegistroDibujo dibujo;
    if (modo_dibujo != 0) {
      if ((modo_dibujo != kDibujoAplicar && modo_dibujo != kDibujoComprobar) || n < kPalabrasDibujo ||
          !LeerDibujoDelMarcador(p, dibujo)) {
        MarcadorMalo(modo, secuencia, cabecera);
        return;
      }
      p += size_t(kPalabrasDibujo) * 4;
      n -= kPalabrasDibujo;
    }
    // Validate the whole structure first: a broken marker writes nothing.
    uint32_t tramos = 0;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t cuenta = t >> 16;
      if (cuenta >= n - i || !TramoDeVolcado(t & 0xFFFFu, cuenta)) {
        MarcadorMalo(modo, secuencia, t);
        return;
      }
      i += 1 + cuenta;
      ++tramos;
    }
    if (modo > kMarcadorComprobar || (modo == 0 && (n != 0 || modo_dibujo == 0))) {
      MarcadorMalo(modo, secuencia, cabecera);
      return;
    }
    // For the DRAW_INDX packets that follow (EmparejarDibujo): until one accepts it or the next Draw* record
    // arrives.
    if (modo_dibujo != 0) {
      dibujo_marcador_ = dibujo;
      dibujo_marcador_modo_ = modo_dibujo;
    }
    if (modo == kMarcadorComprobar) {
      ComprobarMarcador(p, n, secuencia);
      return;
    }
    if (modo == 0) {
      return;  // only the draw record
    }
    // The fast path is decided once per marker: the block path and the vector path enabled and already
    // verified.
    const bool rapido =
        registros_en_bloque_ > 0 && registros_vectoriales_ > 0 && bloques_verificados_ >= kBloquesAVerificar;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t indice = t & 0xFFFFu;
      const uint32_t cuenta = t >> 16;
      if (rapido && cuenta >= 2 && marcador_rapido_ > 0) {
        AplicarTramoRapido(indice, cuenta, p + size_t(i + 1) * 4);
      } else {
        // The regular path, same as a type-0 packet with these registers.
        ++marcador_tramos_lentos_;
        Lector lineal;
        lineal.base = p;
        lineal.pos = i + 1;
        lineal.fin = n;
        if (cuenta < 2 || !EscribirRegistrosEnBloque(indice, cuenta, lineal)) {
          for (uint32_t k = 0; k < cuenta; ++k) {
            EscribirRegistro(indice + k, rex::memory::load_and_swap<uint32_t>(p + size_t(i + 1 + k) * 4));
          }
        }
      }
      i += 1 + cuenta;
    }
    ++marcadores_aplicados_;
    marcador_tramos_ += tramos;
    marcador_palabras_ += n - tramos;
  }

  void MarcadorMalo(uint32_t modo, uint32_t secuencia, uint32_t palabra) {
    ++marcadores_malos_;
    if (avisos_marcador_ < 8) {
      ++avisos_marcador_;
      REXLOG_ERROR("[nativo] marcador D3D {} MAL FORMADO (modo {}, palabra {:08X}): no se aplica, y el hilo del juego "
                   "apaga el camino del marcador",
                   secuencia, modo, palabra);
    }
    AnotarComprobacionMarcador(false, secuencia, 0, 0, palabra);
  }

  void ComprobarMarcador(const uint8_t* p, uint32_t n, uint32_t secuencia) {
    ++marcadores_comprobados_;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t indice = t & 0xFFFFu;
      const uint32_t cuenta = t >> 16;
      for (uint32_t k = 0; k < cuenta; ++k) {
        const uint32_t valor = rex::memory::load_and_swap<uint32_t>(p + size_t(i + 1 + k) * 4);
        if (registros_[indice + k] != valor) {
          ++marcadores_distintos_;
          if (avisos_marcador_ < 8) {
            ++avisos_marcador_;
            REXLOG_ERROR("[nativo] marcador D3D {}: el registro {:04X} vale {:08X} tras sus paquetes y {:08X} en el "
                         "marcador. El hilo del juego apaga el camino del marcador",
                         secuencia, indice + k, registros_[indice + k], valor);
          }
          AnotarComprobacionMarcador(false, secuencia, indice + k, registros_[indice + k], valor);
          return;
        }
      }
      i += 1 + cuenta;
    }
    AnotarComprobacionMarcador(true, secuencia, 0, 0, 0);
  }

  // A span of 2 or more registers from a single dump group (TramoDeVolcado). Same effect as
  // EscribirRegistrosEnBloque with those registers (values, generations and counters), without its checks
  // and without searching for boundaries: in each group, the classes lie in a single known piece.
  void AplicarTramoRapido(uint32_t indice, uint32_t cuenta, const uint8_t* origen) {
    uint32_t* destino = registros_.data() + indice;
    const bool verificar = tramos_rapidos_verificados_ < kTramosRapidosAVerificar;
    if (verificar) {
      copia_marcador_.assign(destino, destino + cuenta);
    }
    bool vs = false;
    bool ps = false;
    bool fetch = false;
    bool encuadre = false;
    const uint32_t fin = indice + cuenta;
    if (indice >= 0x4000) {
      // VS, PS, fetch or boolean constants: the whole span belongs to a single class.
      const bool cambio = CopiarConCambio(destino, origen, cuenta);
      if (indice < 0x4400) {
        vs = cambio;
      } else if (indice < 0x4800) {
        ps = cambio;
      } else if (indice <= kRegFetchUltimo) {
        fetch = cambio;
      }
    } else {
      // 0x2000-0x23FF: the viewport state of each group is a single piece (0x210F-0x2114,
      // 0x2204-0x2206 and 0x2302).
      uint32_t e0 = fin;
      uint32_t e1 = fin;
      if (indice >= 0x2100 && indice < 0x2180) {
        e0 = 0x210F;
        e1 = 0x2115;
      } else if (indice >= 0x2200 && indice < 0x2280) {
        e0 = 0x2204;
        e1 = 0x2207;
      } else if (indice >= 0x2300 && indice < 0x2380) {
        e0 = 0x2302;
        e1 = 0x2303;
      }
      e0 = std::min(std::max(e0, indice), fin);
      e1 = std::min(std::max(e1, e0), fin);
      CopiarConCambio(destino, origen, e0 - indice);
      encuadre = CopiarConCambio(destino + (e0 - indice), origen + size_t(e0 - indice) * 4, e1 - e0);
      CopiarConCambio(destino + (e1 - indice), origen + size_t(e1 - indice) * 4, fin - e1);
    }
    if (verificar) {
      VerificarTramoMarcador(indice, cuenta, origen, vs, ps, fetch, encuadre);
    }
    // The same counters and generations as EscribirRegistrosEnBloque for a block with these registers.
    if (indice >= 0x4000 && indice < 0x4800) {
      constantes_escritas_ += cuenta;
      ++bloques_constantes_;
      if (!(indice < 0x4400 ? vs : ps)) {
        ++bloques_constantes_sin_cambio_;
      }
    } else if (indice >= kRegFetchPrimero && indice <= kRegFetchUltimo) {
      fetch_escritas_ += cuenta;
    } else if (indice <= kRegEncuadreUltimo && fin > kRegEncuadrePrimero) {
      encuadre_escritas_ += cuenta;
    }
    if (vs) {
      ++generacion_constantes_vs_;
    }
    if (ps) {
      ++generacion_constantes_ps_;
    }
    if (fetch) {
      ++generacion_fetch_;
    }
    if (encuadre) {
      ++generacion_encuadre_;
    }
  }

  // The fast path guard: the first kTramosRapidosAVerificar spans are replayed through TramoEscalar (the
  // regular path, word by word) on a copy of the previous registers, and values and classes are compared.
  // If anything differs, the regular path's result is kept and the fast path is turned off for the rest
  // of the run.
  void VerificarTramoMarcador(uint32_t indice, uint32_t cuenta, const uint8_t* origen, bool& vs, bool& ps, bool& fetch,
                              bool& encuadre) {
    bool ref_vs = false;
    bool ref_ps = false;
    bool ref_fetch = false;
    bool ref_encuadre = false;
    TramoEscalar(indice, copia_marcador_.data(), origen, cuenta, ref_vs, ref_ps, ref_fetch, ref_encuadre);
    ++tramos_rapidos_verificados_;
    const bool iguales = std::equal(copia_marcador_.begin(), copia_marcador_.end(), registros_.begin() + indice) &&
                         ref_vs == vs && ref_ps == ps && ref_fetch == fetch && ref_encuadre == encuadre;
    if (!iguales) {
      REXLOG_ERROR("[nativo] marcador D3D: el camino rapido del tramo {:04X}+{} NO coincide con el de siempre "
                   "(clases VS {}/{} PS {}/{} fetch {}/{} encuadre {}/{}). Se APAGA el camino rapido.",
                   indice, cuenta, vs, ref_vs, ps, ref_ps, fetch, ref_fetch, encuadre, ref_encuadre);
      std::copy(copia_marcador_.begin(), copia_marcador_.end(), registros_.begin() + indice);
      vs = ref_vs;
      ps = ref_ps;
      fetch = ref_fetch;
      encuadre = ref_encuadre;
      marcador_rapido_ = 0;
      return;
    }
    if (tramos_rapidos_verificados_ == kTramosRapidosAVerificar) {
      REXLOG_INFO("[nativo] marcador D3D: {} tramos del camino rapido comprobados contra el de siempre, todos iguales. "
                  "Sigue el camino rapido.",
                  tramos_rapidos_verificados_);
    }
  }

  uint32_t Registro(uint32_t indice) const { return indice < kNumRegistros ? registros_[indice] : 0; }

  // --- GPU memory (the two low bits of the address are the byte order) ----

  uint32_t LeerMemoria(uint32_t direccion) const {
    uint32_t valor;
    std::memcpy(&valor, memory_->TranslatePhysical(direccion & ~uint32_t(0x3)), sizeof(valor));
    return xenos::GpuSwap(valor, static_cast<xenos::Endian>(direccion & 0x3));
  }

  void EscribirMemoria(uint32_t direccion, uint32_t valor) {
    const uint32_t ordenado = xenos::GpuSwap(valor, static_cast<xenos::Endian>(direccion & 0x3));
    std::memcpy(memory_->TranslatePhysical(direccion & ~uint32_t(0x3)), &ordenado, sizeof(ordenado));
    SumarSoloAnillo(escrituras_memoria_);
  }

  // --- Threads ------------------------------------------------------------------

  void Interrupcion(uint32_t fuente, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* hilo = rex::system::XThread::GetCurrentThread();
    if (!hilo) {
      return;
    }
    hilo->SetActiveCpu(uint8_t(cpu));
    uint64_t argumentos[] = {fuente, callback_datos_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(hilo->thread_state(), callback, argumentos, 2);
    interrupciones_.fetch_add(1, std::memory_order_relaxed);  // NOTE: also called from BucleVblank (another thread)
    AvisarProgresoAnillo();  // some D3D waits also check the interrupt counter
  }

  int BucleVblank() {
    rex::system::X_VIDEO_MODE modo;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&modo);
    const double hz = std::max(1.0, double(float(modo.refresh_rate)));
    const auto intervalo =
        std::chrono::duration_cast<Reloj::duration>(std::chrono::duration<double>(1.0 / hz));
    auto siguiente = Reloj::now() + intervalo;
    while (activo_.load(std::memory_order_acquire)) {
      const auto ahora = Reloj::now();
      // After a long pause (debugger, loading) hundreds of vblanks are not replayed.
      if (ahora - siguiente > std::chrono::milliseconds(250)) {
        siguiente = ahora;
      }
      while (ahora >= siguiente) {
        contador_.fetch_add(1, std::memory_order_relaxed);
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupcion(0, 2);
        siguiente += intervalo;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  int BucleAnillo() {
    /*
     * The ring thread drops to 0x2D, because of the presenter.
     *
     * Measured: our presentation thread took 54.26 ms per iteration, 53.38 of them in "record"
     * (twenty Vulkan calls that should cost less than a millisecond). It was not work: the thread
     * spent 998 ms of wall-clock time per second but only 300 ms of CPU. The other 700 ms it was
     * ready with no core, preempted by this thread, which runs at 81 % and shared its priority
     * (0x2C). On Horizon, priorities below 0x3B are not time-sliced, so at equal priority the
     * presenter only ran when the ring thread yielded... and the ring thread had just lost exactly
     * what used to make it yield.
     *
     * Result: only 13.37 of the 26.25 frames per second reached the window.
     *
     * This thread is lowered instead of raising the presenter, to stay out of the audio priority
     * (0x2B), which was hard to get right and is settled. The presenter needs ~2 ms about 27 times
     * per second (5 % of a core) and sleeps on a condition variable as soon as it is done, so what
     * it takes from this thread is negligible.
     */
#if REX_PLATFORM_SWITCH
    {
      const int32_t prioridad = REXCVAR_GET(nfsmw_nativo_anillo_prioridad);
      if (prioridad >= 0x1C && prioridad <= 0x3B) {
        RexSwitchSetCurrentThreadPriority(int(prioridad));
        REXLOG_INFO("[nativo] hilo del anillo a prioridad {:#x} (el presentador va en 0x2C y tiene "
                    "que poder desalojarlo)", uint32_t(prioridad));
      }
      /*
       * And which core it starts on. See nfsmw_nativo_anillo_nucleo.
       *
       * The core is logged before and after. Without the "before" the line cannot be interpreted: if
       * the process's default core were already 1, asking for core 1 would separate nothing at all and
       * the log would look like a success.
       */
      const int nucleo_antes = RexSwitchCurrentCore();
      const int32_t nucleo = REXCVAR_GET(nfsmw_nativo_anillo_nucleo);
      const bool exclusivo = nucleo >= 0 && REXCVAR_GET(nfsmw_nativo_anillo_nucleo_exclusivo);
      if (nucleo >= -1 && nucleo <= 2) {
        const bool puesto = exclusivo ? RexSwitchPinCurrentThreadToCore(int(nucleo))
                                      : RexSwitchSetCurrentThreadCore(int(nucleo));
        REXLOG_INFO("[nativo] hilo del anillo: nucleo {} {} ({}); estaba en el {} y ahora corre en "
                    "el {}",
                    nucleo, exclusivo ? "EXCLUSIVO (mascara reducida)" : "preferido",
                    puesto ? "aceptado" : "RECHAZADO por el kernel", nucleo_antes,
                    RexSwitchCurrentCore());
      }
      nucleo_anillo_ = RexSwitchCurrentCore();
    }
#endif
    uint32_t leido = 0;
    uint32_t generacion = 0;
    while (activo_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> cerrojo(anillo_mutex_);
        const auto antes_sin_trabajo = Reloj::now();  // for the "[tiron]" line
        const bool con_senal = anillo_cv_.wait_for(cerrojo, std::chrono::milliseconds(4), [&] {
          return !activo_.load(std::memory_order_acquire) ||
                 puntero_escritura_.load(std::memory_order_acquire) != leido ||
                 generacion_anillo_.load(std::memory_order_acquire) != generacion;
        });
        nfsmw::esperas::Sumar(nfsmw::esperas::kAnilloSinTrabajo, NanosegundosDesde(antes_sin_trabajo));
        // Wakeup counters (only this thread uses them; Informe runs on it).
        ++vueltas_anillo_;
        if (!con_senal) {
          ++esperas_anillo_agotadas_;
        }
#if REX_PLATFORM_SWITCH
        /*
         * Counts how many times the kernel has moved this thread to another core. It is one
         * register read per loop iteration (not a real syscall), and it is the only way to know
         * whether the preferred core does anything: if -1 and 1 give the same count, it has no
         * effect here.
         */
        {
          const int ahora = RexSwitchCurrentCore();
          if (ahora != nucleo_anillo_) {
            nucleo_anillo_ = ahora;
            ++migraciones_anillo_;
          }
        }
#endif
      }
      if (!activo_.load(std::memory_order_acquire)) {
        break;
      }
      Informe(false);
      const uint32_t gen = generacion_anillo_.load(std::memory_order_acquire);
      if (gen != generacion) {
        generacion = gen;
        leido = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
      const uint32_t base = anillo_base_.load(std::memory_order_acquire);
      const uint32_t palabras = anillo_palabras_.load(std::memory_order_acquire);
      if (!base || !palabras) {
        continue;
      }
      const uint32_t escrito = puntero_escritura_.load(std::memory_order_acquire) & (palabras - 1);
      if (escrito == leido) {
        continue;
      }
      ++vueltas_anillo_con_datos_;
      Lector lector;
      lector.base = memory_->TranslatePhysical(base);
      lector.mascara = palabras - 1;
      lector.pos = leido;
      lector.fin = escrito;
      const auto inicio_anillo = Reloj::now();
      while (lector.Pendientes()) {
        if (!Paquete(lector, 0)) {
          // Partial packet: the rest will arrive with the next CP_RB_WPTR.
          break;
        }
      }
      {
        const uint64_t ns_anillo = NanosegundosDesde(inicio_anillo);
        tiempo_anillo_ns_ += ns_anillo;
        SumarSoloAnillo(nfsmw::esperas::g_ns_anillo_trabajando, ns_anillo);  // without fetch_add
      }
      leido = lector.pos;
      // Deferred vertex copies must be done before telling the game that the GPU has read up to here:
      // after that it may reuse those buffers (nfsmw_nativo_subidas_hilo).
      if (destinos_) {
        destinos_->EsperarSubidas();
      }
      const uint32_t devolver = lectura_devuelta_.load(std::memory_order_acquire);
      if (devolver) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(devolver), leido);
      }
      // After returning it: the game's D3D waits sleep until this notification (nfsmw_espera_anillo.cpp).
      AvisarProgresoAnillo();
    }
    return 0;
  }

  // --- Paquetes PM4 -----------------------------------------------------------

  // Consumes a whole packet, or leaves the position untouched if it is not complete yet.
  bool Paquete(Lector& lector, int profundidad) {
    const uint32_t paquete = lector.Mirar();
    uint32_t carga = 0;
    switch (paquete >> 30) {
      case 0:
        carga = paquete ? ((paquete >> 16) & 0x3FFF) + 1 : 0;
        break;
      case 1:
        carga = 2;
        break;
      case 2:
        carga = 0;
        break;
      default:
        carga = ((paquete >> 16) & 0x3FFF) + 1;
        break;
    }
    if (lector.Pendientes() < carga + 1) {
      return false;
    }
    lector.Avanzar(1);
    SumarSoloAnillo(paquetes_);
    Lector datos = lector;
    lector.Avanzar(carga);
    if (!paquete) {
      return true;
    }
    // Timer on 1 of every kCronometroPaquetesCada packets, with the time scaled up: there are ~14,000 per
    // frame, and two clock reads per packet cost more than writing the register. WAIT_REG_MEM, long and
    // infrequent, is always timed.
    const bool cronometrar = (++cronometro_paquetes_ & (kCronometroPaquetesCada - 1)) == 0;
    const auto inicio = cronometrar ? Reloj::now() : Reloj::time_point{};
    switch (paquete >> 30) {
      case 0: {
        const uint32_t indice = paquete & 0x7FFF;
        const bool un_registro = (paquete >> 15) & 0x1;
        if (un_registro || !EscribirRegistrosEnBloque(indice, carga, datos)) {
          for (uint32_t i = 0; i < carga; ++i) {
            EscribirRegistro(un_registro ? indice : indice + i, datos.Leer());
          }
          palabras_sueltas_ += carga;  // Measurement only
        } else {
          palabras_bloque_ += carga;
        }
        if (cronometrar) {
          tiempo_registros_ns_ += NanosegundosDesde(inicio) * kCronometroPaquetesCada;
        }
        ++cuenta_registros_;
        break;
      }
      case 1: {
        const uint32_t valor1 = datos.Leer();
        const uint32_t valor2 = datos.Leer();
        EscribirRegistro(paquete & 0x7FF, valor1);
        EscribirRegistro((paquete >> 11) & 0x7FF, valor2);
        if (cronometrar) {
          tiempo_registros_ns_ += NanosegundosDesde(inicio) * kCronometroPaquetesCada;
        }
        ++cuenta_registros_;
        break;
      }
      case 2:
        break;
      default: {
        const uint32_t opcode = (paquete >> 8) & 0x7F;
        const bool espera = opcode == xenos::PM4_WAIT_REG_MEM;
        const auto inicio_op = espera && !cronometrar ? Reloj::now() : inicio;
        PaqueteTipo3(datos, paquete, carga, profundidad);
        // An indirect buffer is timed through its packets, so they are not counted twice.
        if (opcode != xenos::PM4_INDIRECT_BUFFER && opcode != xenos::PM4_INDIRECT_BUFFER_PFD) {
          if (espera) {
            tiempo_opcode_ns_[opcode] += NanosegundosDesde(inicio_op);
          } else if (cronometrar) {
            tiempo_opcode_ns_[opcode] += NanosegundosDesde(inicio) * kCronometroPaquetesCada;
          }
          ++cuenta_opcode_[opcode];
        }
        break;
      }
    }
    return true;
  }

  void PaqueteTipo3(Lector& datos, uint32_t paquete, uint32_t palabras, int profundidad) {
    const uint32_t opcode = (paquete >> 8) & 0x7F;
    SumarSoloAnillo(opcodes_[opcode]);
    if ((paquete & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      return;  // predicate fails; predicated Swaps are always dropped
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (palabras < 1) break;
        AnotarVallaCopias();  // Measurement only
        const uint32_t cpus = datos.Leer();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupcion(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP: {
        // VdSwap writes "SWAP", the physical address of the frontbuffer and its size.
        if (palabras >= 4) {
          datos.Leer();
          swap_frontbuffer_.store(datos.Leer(), std::memory_order_relaxed);
          swap_ancho_.store(datos.Leer(), std::memory_order_relaxed);
          swap_alto_.store(datos.Leer(), std::memory_order_relaxed);
        }
        TrazaSwap();
        Presentar();
        AnotarJuegoPorDelante();  // Measurement only
        break;
      }
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (palabras < 2) break;
        const uint32_t direccion = datos.Leer() & 0x1FFFFFFF;
        const uint32_t longitud = datos.Leer() & 0xFFFFF;
        if (profundidad < kMaxProfundidadIndirecta) {
          BuferIndirecto(direccion, longitud, profundidad + 1);
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (palabras < 5) break;
        const uint32_t info = datos.Leer();
        const uint32_t sondeo = datos.Leer();
        const uint32_t referencia = datos.Leer();
        const uint32_t mascara = datos.Leer();
        const auto inicio_regmem = Reloj::now();  // for the "[tiron]" line
        const auto limite = inicio_regmem + kEsperaRegMemMax;
        uint32_t vueltas = 0;
        for (;;) {
          if (!(info & 0x10) && sondeo == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (Registro(sondeo) & UINT32_C(0x80000000))) {
            GuardarRegistro(sondeo, 0);  // MakeCoherent: there is no shared memory to synchronize
          }
          const uint32_t valor = (info & 0x10) ? LeerMemoria(sondeo) : Registro(sondeo);
          if (Compara(info, valor & mascara, referencia)) {
            break;
          }
          // The real GPU waits; without a GPU some conditions will never be
          // met. The wait is cut short so the ring does not hang.
          if (Reloj::now() >= limite || !activo_.load(std::memory_order_acquire)) {
            if (esperas_agotadas_.fetch_add(1, std::memory_order_relaxed) == 0) {
              REXLOG_WARN("[nativo] WAIT_REG_MEM sin cumplirse ({} {:08X} ref {:08X} mascara {:08X})",
                          (info & 0x10) ? "memoria" : "registro", sondeo, referencia, mascara);
            }
            break;
          }
          const int32_t pausa_us = REXCVAR_GET(nfsmw_nativo_espera_regmem_us);
          rex::thread::Sleep(std::chrono::microseconds(pausa_us > 50 ? pausa_us : 50));
          ++vueltas;
        }
        /*
         * The guest has just waited for the GPU. It is the only point where it may legally rewrite
         * a vertex range it already referenced in this frame, so from here on upload deduplication
         * cannot reuse anything recorded so far. See nfsmw_nativo_vertices_dedupe.h. Relaxed is
         * enough: the same thread reads it.
         */
        g_sincronizaciones_anillo.fetch_add(1, std::memory_order_relaxed);
        nfsmw::esperas::Sumar(nfsmw::esperas::kAnilloRegMem, NanosegundosDesde(inicio_regmem));
        const uint64_t clave_espera = (uint64_t(info & 0x10) << 32) | sondeo;
        if (esperas_.size() < 64 || esperas_.count(clave_espera)) {
          Espera& espera = esperas_[clave_espera];
          ++espera.veces;
          espera.vueltas += vueltas;
          espera.referencia = referencia;
          espera.mascara = mascara;
          espera.info = info;
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (palabras < 3) break;
        const uint32_t info = datos.Leer();
        const uint32_t y = datos.Leer();
        const uint32_t o = datos.Leer();
        uint32_t valor = Registro(info & 0x1FFF);
        valor &= ((info >> 31) & 0x1) ? Registro(y & 0x1FFF) : y;
        valor |= ((info >> 30) & 0x1) ? Registro(o & 0x1FFF) : o;
        EscribirRegistro(info & 0x1FFF, valor);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (palabras < 2) break;
        const uint32_t registro = datos.Leer();
        const uint32_t direccion = datos.Leer();
        EscribirMemoria(direccion, Registro(registro));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (palabras < 1) break;
        AnotarVallaCopias();  // Measurement only
        uint32_t direccion = datos.Leer();
        for (uint32_t i = 1; i < palabras; ++i) {
          EscribirMemoria(direccion, datos.Leer());
          direccion += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (palabras < 6) break;
        const uint32_t info = datos.Leer();
        const uint32_t sondeo = datos.Leer();
        const uint32_t referencia = datos.Leer();
        const uint32_t mascara = datos.Leer();
        const uint32_t destino = datos.Leer();
        const uint32_t dato = datos.Leer();
        const uint32_t valor = (info & 0x10) ? LeerMemoria(sondeo) : Registro(sondeo);
        if (Compara(info, valor & mascara, referencia)) {
          if (info & 0x100) {
            EscribirMemoria(destino, dato);
          } else {
            EscribirRegistro(destino, dato);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (palabras < 1) break;
        EscribirRegistro(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, datos.Leer() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (palabras < 3) break;
        AnotarVallaCopias();  // Measurement only
        const uint32_t iniciador = datos.Leer();
        const uint32_t direccion = datos.Leer();
        const uint32_t valor = datos.Leer();
        EscribirRegistro(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, iniciador & 0x3F);
        EscribirMemoria(direccion, ((iniciador >> 31) & 0x1)
                                       ? contador_.load(std::memory_order_relaxed)
                                       : valor);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (palabras < 2) break;
        const uint32_t iniciador = datos.Leer();
        const uint32_t direccion = datos.Leer();
        EscribirRegistro(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, iniciador & 0x3F);
        // Screen extent of the previous draw: the maximum is faked.
        const uint16_t extension[] = {0, kExtensionMaxima, 0, kExtensionMaxima, 0, 1};
        uint8_t* destino = memory_->TranslatePhysical(direccion & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extension); ++i) {
          rex::memory::store_and_swap<uint16_t>(destino + i * 2, extension[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (palabras < 1) break;
        EscribirRegistro(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, datos.Leer() & 0x3F);
        const uint32_t direccion = Registro(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
        if (!direccion) break;
        ConsultaOclusion(direccion);
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (palabras < 1) break;
        const uint32_t tipo_indice = datos.Leer();
        uint32_t base = UINT32_MAX;
        switch ((tipo_indice >> 16) & 0xFF) {
          case 0: base = 0x4000; break;  // ALU
          case 1: base = 0x4800; break;  // fetch
          case 2: base = 0x4900; break;  // bool
          case 3: base = 0x4908; break;  // loop
          case 4: base = 0x2000; break;  // registros
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint32_t indice = base + (tipo_indice & 0x7FF);
          for (uint32_t i = 1; i < palabras; ++i) {
            EscribirRegistro(indice + i - 1, datos.Leer());
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (palabras < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | datos.Leer();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (palabras < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(datos.Leer()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (palabras < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | datos.Leer();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (palabras < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(datos.Leer()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (palabras < 2) break;
        const uint64_t alto = datos.Leer();
        const uint64_t bajo = datos.Leer();
        bin_mask_ = (alto << 32) | bajo;
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (palabras < 2) break;
        const uint64_t alto = datos.Leer();
        const uint64_t bajo = datos.Leer();
        bin_select_ = (alto << 32) | bajo;
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        // Same format: first value = register index, the rest is written consecutively.
        if (palabras < 1) break;
        const uint32_t indice = datos.Leer() & 0xFFFF;
        for (uint32_t i = 1; i < palabras; ++i) {
          EscribirRegistro(indice + i - 1, datos.Leer());
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        // Registers loaded from guest memory instead of from the ring.
        if (palabras < 3) break;
        const uint32_t direccion = datos.Leer() & 0x3FFFFFFF;
        const uint32_t tipo_indice = datos.Leer();
        const uint32_t cantidad = datos.Leer() & 0xFFF;
        uint32_t base = UINT32_MAX;
        switch ((tipo_indice >> 16) & 0xFF) {
          case 0: base = 0x4000; break;  // ALU
          case 1: base = 0x4800; break;  // fetch
          case 2: base = 0x4900; break;  // bool
          case 3: base = 0x4908; break;  // loop
          case 4: base = 0x2000; break;  // registros
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint8_t* origen = memory_->TranslatePhysical(direccion);
          const uint32_t indice = base + (tipo_indice & 0x7FF);
          for (uint32_t i = 0; i < cantidad; ++i) {
            EscribirRegistro(indice + i, rex::memory::load_and_swap<uint32_t>(origen + size_t(i) * 4));
          }
        }
        break;
      }
      case xenos::PM4_VIZ_QUERY: {
        // Like the emulated path: when the query ends it is reported as visible,
        // in case the game reads the result.
        if (palabras < 1) break;
        const uint32_t dato = datos.Leer();
        const uint32_t id = dato & 0x3F;
        if (!(dato & 0x100)) {
          EscribirRegistro(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
          EscribirRegistro(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t registro = id < 32 ? uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                            : uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          const uint32_t bit = UINT32_C(1) << (id < 32 ? id : id - 32);
          GuardarRegistro(registro, Registro(registro) | bit);
        }
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        // DRAW_INDX is preceded by the visibility query word.
        const uint32_t saltar = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (palabras < saltar + 1) break;
        if (saltar) {
          datos.Leer();
        }
        const uint32_t iniciador = datos.Leer();
        EscribirRegistro(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, iniciador);
        // Indices in a buffer (DMA): the base and size come in the packet and are
        // stored in their registers, like ExecutePacketType3Draw
        // (graphics/command_processor.cpp:1379-1390). Without this, indexed draws
        // read VGT_DMA_SIZE as zero.
        if (((iniciador >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (palabras < saltar + 3) break;
          EscribirRegistro(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, datos.Leer());
          EscribirRegistro(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, datos.Leer());
        }
        SumarSoloAnillo(dibujos_);
        // No fetch_add, which on the A57 is an ldxr/stxr loop on every draw. Only this thread writes
        // g_dibujos, and the "[tiron]" line reads it from this same thread (Presentar): SumarSoloAnillo is
        // enough.
        SumarSoloAnillo(nfsmw::esperas::g_dibujos);
        // A draw with RB_MODECONTROL in copy mode is a resolve (and/or a
        // clear): the emulated path treats it that way (vulkan/command_processor.cpp:3653).
        if ((Registro(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) ==
            uint32_t(xenos::EdramMode::kCopy)) {
          SumarSoloAnillo(copias_);
          ultima_copia_destino_.store(Registro(rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE),
                                      std::memory_order_relaxed);
          ultima_copia_control_.store(Registro(rex::graphics::XE_GPU_REG_RB_COPY_CONTROL),
                                      std::memory_order_relaxed);
          ultima_copia_info_.store(Registro(rex::graphics::XE_GPU_REG_RB_COPY_DEST_INFO),
                                   std::memory_order_relaxed);
          ultima_copia_pitch_.store(Registro(rex::graphics::XE_GPU_REG_RB_COPY_DEST_PITCH),
                                    std::memory_order_relaxed);
          HacerCopia();
        } else {
          ContarDibujoShaders();
          EmparejarDibujo();
          TrazarDibujo();
          // In mode 5 (depth only) the VS is enough: Xenos does not run the PS, and D3D leaves
          // the PS object at 0 in some of the race shadow draws.
          if (vs_dibujo_ &&
              (ps_dibujo_ || (Registro(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5)) {
            DibujarNativo();
          }
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {
        // Microcode in GPU memory; the type is in the two low bits.
        if (palabras < 2) break;
        const uint32_t direccion_tipo = datos.Leer();
        const uint32_t tamano = datos.Leer() & 0xFFFF;
        const uint8_t* origen = memory_->TranslatePhysical(direccion_tipo & ~uint32_t(0x3));
        if (tamano && (direccion_tipo & 0x3) <= 1) {
          CargarShaderCacheado(direccion_tipo, tamano, origen);
          break;
        }
        microcodigo_.resize(tamano);
        for (uint32_t i = 0; i < tamano; ++i) {
          microcodigo_[i] = rex::memory::load_and_swap<uint32_t>(origen + size_t(i) * 4);
        }
        IdentificarShader(direccion_tipo & 0x3);
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        // Microcode inside the packet itself.
        if (palabras < 2) break;
        const uint32_t tipo = datos.Leer();
        const uint32_t tamano = datos.Leer() & 0xFFFF;
        if (tamano > palabras - 2) break;
        CargarInmediato(datos, tipo, tamano);  // with an exact cache (nfsmw_nativo_im_inmediato_cache)
        break;
      }
      case xenos::PM4_NOP:
        // The FlushState composite marker (phase 2 of the Direct3D-level renderer).
        // A NOP without its magic belongs to the game itself and does nothing, as before.
        ProcesarMarcador(datos, palabras);
        break;
      default:
        // Invalidaciones y demas: todavia no dibuja.
        break;
    }
  }

  // Step C2: a draw in copy mode is a resolve and/or a clear.
  bool AsegurarDestinos() {
    if (!destinos_ && !destinos_fallidos_) {
      destinos_ = DestinosNativos::Crear(provider_ ? provider_->vulkan_device() : nullptr, memory_);
      if (!destinos_) {
        destinos_fallidos_ = true;
        REXLOG_ERROR("[nativo] No se pudieron crear los destinos nativos (C2)");
      }
    }
    return destinos_ != nullptr;
  }

  // Diagnostic nfsmw_nativo_diag_constantes_ps: the first constants of the requested PS, at most once
  // every nfsmw_nativo_diag_constantes_ms per PS (exposure and brightness of the visual treatment).
  void AnotarConstantesPs() {
    if (!ps_dibujo_) {
      return;
    }
    const std::string& lista = REXCVAR_GET(nfsmw_nativo_diag_constantes_ps);
    if (lista.empty()) {
      return;
    }
    if (lista != constantes_lista_texto_) {
      constantes_lista_texto_ = lista;
      constantes_lista_.clear();
      size_t inicio = 0;
      while (inicio <= lista.size()) {
        const size_t fin = std::min(lista.find(',', inicio), lista.size());
        if (fin > inicio) {
          constantes_lista_.insert(uint32_t(std::strtoul(lista.substr(inicio, fin - inicio).c_str(), nullptr, 10)));
        }
        inicio = fin + 1;
      }
    }
    if (!constantes_lista_.count(ps_dibujo_->numero)) {
      return;
    }
    const auto ahora = Reloj::now();
    auto& ultimo = constantes_ultimo_[ps_dibujo_->numero];
    if (ahora - ultimo < std::chrono::milliseconds(REXCVAR_GET(nfsmw_nativo_diag_constantes_ms))) {
      return;
    }
    ultimo = ahora;
    std::string valores;
    for (uint32_t k = 0; k < 12; ++k) {
      valores += fmt::format(" c{}=(", k);
      for (uint32_t c = 0; c < 4; ++c) {
        float f;
        const uint32_t bits = registros_[0x4400 + k * 4 + c];
        std::memcpy(&f, &bits, sizeof(f));
        valores += fmt::format("{}{:.4g}", c ? "," : "", f);
      }
      valores += ")";
    }
    REXLOG_INFO("[nativo] constantes PS n{} (Swap {}):{}", ps_dibujo_->numero, swaps_.load(), valores);
  }

  // Steps C3-C6: the draw, with its VS and PS identified, goes to Vulkan.
  /*
   * Measurement only. A fence or an interrupt the game can see: how many vertex copies are still
   * pending at that moment (the ring only waits for them before returning the read pointer). Ring
   * thread only.
   */
  /*
   * Measurement only. When the ring finishes a frame (its Swap, already counted in swaps_), how many
   * more Swaps the game has written: 0 or less = the ring is caught up; 1 = the next frame is already
   * entirely in the ring; 2 or more = two or more. The game counts after writing the Swap, so a
   * momentary -1 is normal. The first difference is recorded separately in case the two counters do
   * not start together. Ring thread only.
   */
  void AnotarJuegoPorDelante() {
    const int64_t diferencia = int64_t(::g_nfsmw_fotogramas_juego.load(std::memory_order_relaxed)) -
                               int64_t(swaps_.load(std::memory_order_relaxed));
    if (!juego_delante_primera_valida_) {
      juego_delante_primera_valida_ = true;
      juego_delante_primera_ = diferencia;
    }
    ++juego_delante_[diferencia <= 0 ? 0 : diferencia == 1 ? 1 : 2];
    juego_delante_max_ = std::max(juego_delante_max_, diferencia);
  }

  void AnotarVallaCopias() {
    ++vallas_total_;
    if (destinos_) {
      const size_t pendientes = destinos_->CopiasPendientes();
      if (pendientes) {
        ++vallas_con_copias_;
        vallas_copias_max_ = std::max(vallas_copias_max_, pendientes);
      }
    }
  }

  void DibujarNativo() {
    if (!AsegurarDestinos()) {
      return;
    }
    // AnotarConstantesPs used to read the text cvar on every draw, and REXCVAR_GET of a string
    // is a call with an initialization guard (a load with acquire) that cannot be folded. The
    // flag is refreshed once per Swap: the diagnostic takes at most one frame to turn on.
    if (diag_constantes_activo_) {
      AnotarConstantesPs();
    }
    PeticionDibujo peticion;
    peticion.registros = registros_.data();
    peticion.vs = vs_dibujo_;
    peticion.ps = ps_dibujo_;
    peticion.vs_microcodigo = vs_microcodigo_;
    peticion.generacion_vs = generacion_vs_;
    peticion.generacion_constantes_vs = generacion_constantes_vs_;
    peticion.generacion_constantes_ps = generacion_constantes_ps_;
    peticion.generacion_fetch = generacion_fetch_;
    peticion.generacion_encuadre = generacion_encuadre_;
    peticion.vegetacion_juego = vegetacion_juego_;  // nfsmw_d3d_vegetacion_juego
    // Timer on 1 of every kCronometroDibujosCada draws, with the time scaled up (as in Paquete).
    // The phase comes from a counter that is not reset on each report (dibujos_medidos_ is), offset to
    // the middle of the batch: counting from 1, draws 33, 97, 161... are timed here, and the stage timer
    // of DibujosVulkanImpl::Dibujar runs on 64, 128, 192... Those pay ~20 extra clock reads; previously,
    // after each report, 1 time in 8 they landed exactly on the draws timed here and inflated
    // "dibujos us/dibujo".
    if ((fase_cronometro_dibujos_++ & (kCronometroDibujosCada - 1)) == kCronometroDibujosCada / 2) {
      const auto inicio = Reloj::now();
      destinos_->Dibujar(peticion);
      tiempo_dibujos_ns_ += NanosegundosDesde(inicio) * kCronometroDibujosCada;
    } else {
      destinos_->Dibujar(peticion);
    }
    ++dibujos_medidos_;
  }

  void HacerCopia() {
    if (!AsegurarDestinos()) {
      return;
    }
    namespace g = rex::graphics;
    RegistrosCopia r;
    r.rb_surface_info = Registro(g::XE_GPU_REG_RB_SURFACE_INFO);
    r.rb_color_info[0] = Registro(g::XE_GPU_REG_RB_COLOR_INFO);
    r.rb_color_info[1] = Registro(g::XE_GPU_REG_RB_COLOR1_INFO);
    r.rb_color_info[2] = Registro(g::XE_GPU_REG_RB_COLOR2_INFO);
    r.rb_color_info[3] = Registro(g::XE_GPU_REG_RB_COLOR3_INFO);
    r.rb_depth_info = Registro(g::XE_GPU_REG_RB_DEPTH_INFO);
    r.rb_copy_control = Registro(g::XE_GPU_REG_RB_COPY_CONTROL);
    r.rb_copy_dest_base = Registro(g::XE_GPU_REG_RB_COPY_DEST_BASE);
    r.rb_copy_dest_pitch = Registro(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    r.rb_copy_dest_info = Registro(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    r.rb_color_clear = Registro(g::XE_GPU_REG_RB_COLOR_CLEAR);
    r.rb_color_clear_lo = Registro(g::XE_GPU_REG_RB_COLOR_CLEAR_LO);
    r.rb_depth_clear = Registro(g::XE_GPU_REG_RB_DEPTH_CLEAR);
    r.pa_sc_window_offset = Registro(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    r.pa_sc_window_scissor_tl = Registro(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    r.pa_sc_window_scissor_br = Registro(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    r.pa_su_sc_mode_cntl = Registro(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    r.pa_su_vtx_cntl = Registro(g::XE_GPU_REG_PA_SU_VTX_CNTL);
    r.fetch_vertices[0] = Registro(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    r.fetch_vertices[1] = Registro(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1);
    TrazarCopia(r);
    const auto inicio = Reloj::now();
    destinos_->Copiar(r);
    tiempo_copias_ns_ += NanosegundosDesde(inicio);
    ++copias_medidas_;
  }

  // Diagnostic (nfsmw_nativo_diag_fotograma_s): every draw and every copy of one whole
  // frame, from the first Swap after that many seconds until the next one.
  void TrazaSwap() {
    // Once per Swap, not once per draw (see DibujarNativo).
    diag_constantes_activo_ = !REXCVAR_GET(nfsmw_nativo_diag_constantes_ps).empty();
    if (trazando_) {
      trazando_ = false;
      traza_hecha_ = true;
      REXLOG_INFO("[traza] fin del fotograma: {} lineas", trazas_);
      return;
    }
    const int32_t segundos = REXCVAR_GET(nfsmw_nativo_diag_fotograma_s);
    if (!traza_hecha_ && segundos > 0 &&
        Reloj::now() - inicio_sistema_ >= std::chrono::seconds(segundos)) {
      trazando_ = true;
      trazas_ = 0;
      REXLOG_INFO("[traza] fotograma completo tras el Swap {}", swaps_.load());
    }
  }

  // One line per draw: shaders, render targets, state and the textures of the PS samplers
  // (address / format / dimension; ! if the fetch constant is not a texture).
  void TrazarDibujo() {
    if (!trazando_ || trazas_ >= 4000) {
      return;
    }
    ++trazas_;
    namespace g = rex::graphics;
    std::string texturas;
    if (ps_dibujo_) {
      for (const SamplerShader& s : ps_dibujo_->samplers) {
        if (s.registro >= 16) {
          continue;
        }
        const uint32_t base = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(s.registro) * 6;
        const uint32_t d0 = Registro(base);
        const uint32_t d1 = Registro(base + 1);
        const uint32_t d3 = Registro(base + 3);
        const uint32_t d5 = Registro(base + 5);
        texturas += fmt::format(" t{}={:08X}/f{}/d{}/s{:03X}/e{}{}", s.registro, d1 & 0xFFFFF000,
                                d1 & 0x3F, (d5 >> 9) & 0x3, (d3 >> 1) & 0xFFF, (d1 >> 6) & 0x3,
                                (d0 & 0x3) == 2 ? "" : "!");
      }
    }
    if (!vs_dibujo_ || !ps_dibujo_) {
      static constexpr const char* kMotivos[] = {"registro con shaders desconocidos",
                                                 "sin registro",
                                                 "registro con shaders distintos del IM_LOAD"};
      texturas += fmt::format(" | {}: objetos VS {:08X} PS {:08X}; IM_LOAD PS n{} VS n{}; pendientes:",
                              kMotivos[motivo_emparejado_ % 3], objeto_vs_, objeto_ps_,
                              ps_actual_ ? int(ps_actual_->numero) : -1,
                              vs_actual_ ? int(vs_actual_->numero) : -1);
      for (size_t i = 0; i < pendientes_.size() && i < 4; ++i) {
        const RegistroDibujo& pendiente = pendientes_[i];
        const EntradaShader* vs = ShaderDeObjetoCacheado(pendiente.vs);
        const EntradaShader* ps = ShaderDeObjetoCacheado(pendiente.ps);
        texturas += fmt::format(" [funcion {} tipo {} cuenta {} VS n{} PS n{}]",
                                int(pendiente.funcion), pendiente.args[0],
                                CuentaDelRegistro(pendiente), vs ? int(vs->numero) : -1,
                                ps ? int(ps->numero) : -1);
      }
    }
    const uint32_t iniciador = Registro(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    REXLOG_INFO("[traza] dibujo VS n{} PS n{} tipo {} cuenta {} sup {:08X} rt0 {:08X} rt1 {:08X} "
                "mascara {:08X} mezcla {:08X} colorctl {:08X} prof {:08X} stencil {:08X} "
                "modo {:08X}{}",
                vs_dibujo_ ? int(vs_dibujo_->numero) : -1, ps_dibujo_ ? int(ps_dibujo_->numero) : -1,
                iniciador & 0x3F, iniciador >> 16, Registro(g::XE_GPU_REG_RB_SURFACE_INFO),
                Registro(g::XE_GPU_REG_RB_COLOR_INFO), Registro(g::XE_GPU_REG_RB_COLOR1_INFO),
                Registro(g::XE_GPU_REG_RB_COLOR_MASK), Registro(g::XE_GPU_REG_RB_BLENDCONTROL0),
                Registro(g::XE_GPU_REG_RB_COLORCONTROL), Registro(g::XE_GPU_REG_RB_DEPTHCONTROL),
                Registro(g::XE_GPU_REG_RB_STENCILREFMASK), Registro(g::XE_GPU_REG_RB_MODECONTROL),
                texturas);
    TrazarVertices(iniciador);
  }

  // Diagnostic (nfsmw_nativo_diag_vertices_ps): the fetch of each VS element, original and
  // patched, the bytes of the draw's first vertices and the center texels of its 8888
  // textures, as they are in guest memory.
  void TrazarVertices(uint32_t iniciador) {
    namespace g = rex::graphics;
    if (!vs_dibujo_ || !ps_dibujo_ || trazas_ >= 4000 ||
        vs_microcodigo_.size() != vs_dibujo_->microcodigo.size()) {
      return;
    }
    const std::string lista = REXCVAR_GET(nfsmw_nativo_diag_vertices_ps);
    bool en_lista = false;
    uint32_t valor = 0;
    bool hay = false;
    for (char c : lista + ",") {
      if (c >= '0' && c <= '9') {
        valor = valor * 10 + uint32_t(c - '0');
        hay = true;
      } else if (hay) {
        en_lista = en_lista || valor == ps_dibujo_->numero;
        valor = 0;
        hay = false;
      }
    }
    if (!en_lista) {
      return;
    }
    ++trazas_;
    std::string detalle;
    uint32_t ranura = UINT32_MAX;
    uint32_t zancada = 0;
    for (const ElementoVertice& elemento : vs_dibujo_->elementos) {
      const size_t i = size_t(elemento.instruccion) * 3;
      const uint32_t o0 = vs_dibujo_->microcodigo[i], o1 = vs_dibujo_->microcodigo[i + 1];
      const uint32_t p0 = vs_microcodigo_[i], p1 = vs_microcodigo_[i + 1], p2 = vs_microcodigo_[i + 2];
      detalle += fmt::format(" {}{}@{}: original r{} s{:03X} fmt{}; parcheado r{} op{} s{:03X} fmt{} "
                             "f{} z{} o{} ({:08X} {:08X} {:08X});",
                             NombreUso(elemento.uso), elemento.indice_uso, elemento.instruccion,
                             (o0 >> 12) & 0x3F, o1 & 0xFFF, (o1 >> 16) & 0x3F, (p0 >> 12) & 0x3F,
                             p0 & 0x1F, p1 & 0xFFF, (p1 >> 16) & 0x3F,
                             ((p0 >> 20) & 0x1F) * 3 + ((p0 >> 25) & 0x3), p2 & 0xFF,
                             int32_t(p2 << 1) >> 9, p0, p1, p2);
      if (ranura == UINT32_MAX && (p0 & 0x1F) == 0 && !((p1 >> 30) & 0x1)) {
        ranura = ((p0 >> 20) & 0x1F) * 3 + ((p0 >> 25) & 0x3);
        zancada = (p2 & 0xFF) * 4;
      }
    }
    if (ranura < 96 && zancada && zancada <= 64) {
      const uint32_t f0 = Registro(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ranura * 2);
      const uint32_t f1 = Registro(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ranura * 2 + 1);
      const uint32_t fuente = (iniciador >> 6) & 0x3;
      uint32_t primero = Registro(g::XE_GPU_REG_VGT_INDX_OFFSET) & 0xFFFFFF;
      if (fuente == uint32_t(xenos::SourceSelect::kDMA)) {
        const bool de_32 = (iniciador >> 11) & 0x1;
        const uint32_t base_indices =
            Registro(g::XE_GPU_REG_VGT_DMA_BASE) & (de_32 ? 0x1FFFFFFC : 0x1FFFFFFE);
        const auto orden_indices =
            static_cast<xenos::Endian>(Registro(g::XE_GPU_REG_VGT_DMA_SIZE) >> 30);
        if (de_32) {
          uint32_t crudo;
          std::memcpy(&crudo, memory_->TranslatePhysical(base_indices), 4);
          primero = (primero + (xenos::GpuSwap(crudo, orden_indices) & 0xFFFFFF)) & 0xFFFFFF;
        } else {
          uint16_t crudo;
          std::memcpy(&crudo, memory_->TranslatePhysical(base_indices), 2);
          primero = (primero + xenos::GpuSwap(crudo, orden_indices)) & 0xFFFFFF;
        }
      }
      const uint64_t direccion = uint64_t(f0 & 0x1FFFFFFC) + uint64_t(primero) * zancada;
      std::string bytes;
      if (direccion + uint64_t(4) * zancada <= 0x20000000) {
        const uint8_t* datos = memory_->TranslatePhysical(uint32_t(direccion));
        for (uint32_t b = 0; b < 4 * zancada; ++b) {
          bytes += fmt::format("{}{:02X}", b % zancada == 0 ? " | " : (b % 4 == 0 ? " " : ""),
                               datos[b]);
        }
      }
      detalle += fmt::format(" fetch f{} {:08X} {:08X} (orden {}) fuente {} primero {} zancada {}:{}",
                             ranura, f0, f1, f1 & 0x3, fuente, primero, zancada, bytes);
    }
    for (const SamplerShader& s : ps_dibujo_->samplers) {
      if (s.registro >= 16) {
        continue;
      }
      const uint32_t base = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(s.registro) * 6;
      const uint32_t t0 = Registro(base), t1 = Registro(base + 1), t2 = Registro(base + 2);
      if ((t0 & 0x3) != 2 || (t1 & 0x3F) != 6) {
        continue;
      }
      const uint32_t ancho = (t2 & 0x1FFF) + 1, alto = ((t2 >> 13) & 0x1FFF) + 1;
      const uint32_t pitch = std::max<uint32_t>(((t0 >> 22) & 0x1FF) << 5, 1);
      const bool mosaico = (t0 >> 31) & 0x1;
      const uint32_t y = alto / 2;
      std::string texels;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t x = std::min(ancho - 1, (ancho > 8 ? ancho / 2 - 4 : 0) + k);
        const int64_t desplazamiento = mosaico ? DesplazamientoMosaicoDiag(int32_t(x), int32_t(y), pitch, 2)
                                               : int64_t(y) * pitch * 4 + int64_t(x) * 4;
        const uint64_t d = uint64_t(t1 & 0x1FFFF000) + uint64_t(desplazamiento);
        if (desplazamiento < 0 || d + 4 > 0x20000000) {
          break;
        }
        const uint8_t* texel = memory_->TranslatePhysical(uint32_t(d));
        texels += fmt::format(" {:02X}{:02X}{:02X}{:02X}", texel[0], texel[1], texel[2], texel[3]);
      }
      detalle += fmt::format("; t{} {}x{} {} pitch {} swizzle {:03X} orden {} signos {:02X} fila {}:{}",
                             s.registro, ancho, alto, mosaico ? "en mosaico" : "lineal", pitch,
                             (Registro(base + 3) >> 1) & 0xFFF, (t1 >> 6) & 0x3, (t0 >> 2) & 0xFF,
                             y, texels);
      const uint32_t direccion_textura = t1 & 0x1FFFF000;
      if (texturas_volcadas_.size() < 8 && texturas_volcadas_.insert(direccion_textura).second) {
        std::vector<uint8_t> volcado(size_t(ancho) * alto * 4);
        bool completo = true;
        for (uint32_t yy = 0; yy < alto && completo; ++yy) {
          for (uint32_t xx = 0; xx < ancho; ++xx) {
            const int64_t dt = mosaico ? DesplazamientoMosaicoDiag(int32_t(xx), int32_t(yy), pitch, 2)
                                       : int64_t(yy) * pitch * 4 + int64_t(xx) * 4;
            const uint64_t texel = uint64_t(direccion_textura) + uint64_t(dt);
            if (dt < 0 || texel + 4 > 0x20000000) {
              completo = false;
              break;
            }
            std::memcpy(volcado.data() + (size_t(yy) * ancho + xx) * 4,
                        memory_->TranslatePhysical(uint32_t(texel)), 4);
          }
        }
        const auto ruta = rex::filesystem::GetExecutableFolder() /
                          fmt::format("diag_textura_{:08X}_{}x{}.bin", direccion_textura, ancho, alto);
        std::ofstream fichero(ruta, std::ios::binary | std::ios::trunc);
        fichero.write(reinterpret_cast<const char*>(volcado.data()), std::streamsize(volcado.size()));
        detalle += fmt::format(" (volcada en {}{})", ruta.filename().string(),
                               completo ? "" : ", incompleta");
      }
    }
    detalle += "; constantes VS";
    for (uint32_t c = 0; c < 16; ++c) {
      const uint32_t b = 0x4000 + c * 4;
      detalle += fmt::format(" c{}=({:.5g} {:.5g} {:.5g} {:.5g})", c, std::bit_cast<float>(Registro(b)),
                             std::bit_cast<float>(Registro(b + 1)), std::bit_cast<float>(Registro(b + 2)),
                             std::bit_cast<float>(Registro(b + 3)));
    }
    detalle += "; constantes PS";
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t b = 0x4400 + c * 4;
      detalle += fmt::format(" c{}=({:.5g} {:.5g} {:.5g} {:.5g})", c, std::bit_cast<float>(Registro(b)),
                             std::bit_cast<float>(Registro(b + 1)), std::bit_cast<float>(Registro(b + 2)),
                             std::bit_cast<float>(Registro(b + 3)));
    }
    REXLOG_INFO("[traza] vertices PS n{} VS n{}:{}", ps_dibujo_->numero, vs_dibujo_->numero,
                detalle);
  }

  // GetTiledOffset2D (graphics/pipeline/texture/util.cpp), same as DesplazamientoMosaico2D in
  // nfsmw_nativo_dibujos.cpp; here only for the diagnostic. pitch in blocks.
  static int32_t DesplazamientoMosaicoDiag(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
    pitch = (pitch + 31) & ~uint32_t(31);
    const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
    const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
    const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
  }

  void TrazarCopia(const RegistrosCopia& r) {
    if (!trazando_ || trazas_ >= 4000) {
      return;
    }
    ++trazas_;
    REXLOG_INFO("[traza] copia control {:08X} destino {:08X} info {:08X} pitch {:08X} sup {:08X} "
                "rt0 {:08X} prof {:08X} borrado {:08X}/{:08X} prof_borrado {:08X}",
                r.rb_copy_control, r.rb_copy_dest_base, r.rb_copy_dest_info, r.rb_copy_dest_pitch,
                r.rb_surface_info, r.rb_color_info[0], r.rb_depth_info, r.rb_color_clear,
                r.rb_color_clear_lo, r.rb_depth_clear);
  }

  // Step C5a: the last VS (type 0) or PS (type 1) uploaded to the ring.
  // IM_LOAD with a cache. The game reloads the same shaders on almost every draw: in a race on the PC,
  // 70,000-97,000 loads per second of about 82 distinct microcodes. Each load byte-swapped the data,
  // computed the XXH3 to identify it and copied the VS, and each new VS then repeated the XXH3 in
  // HuellaVs and in AnotarEntradaVertices. If the address, size and bytes match an earlier load, its
  // swapped copy, identification, fingerprint and generation are reused. The bytes must be compared:
  // D3D patches VS microcode in place, so the address alone is not enough. Same microcode, same
  // generation: whatever is cached per generation (vertex input, fingerprint, coherence) stays valid.
  //
  // IM_LOAD without memcmp (nfsmw_nativo_im_load_sin_memcmp). The memcmp above cost ~0.6 us per IM_LOAD
  // (~41,000 per second in a race). Whoever writes microcode announces it (the D3D constructors and the
  // fetch patcher; the list and the cross-thread protocol are in nfsmw_microcodigo_versiones.h). Each way
  // records the versions of its slot it was checked against guest memory with; if they are unchanged,
  // nobody has written that microcode since, and it is used without reading it.
  // Self-checking guard:
  //   - watching: always the regular memcmp and, if the shortcut had a candidate, it is compared with the
  //     memcmp result. After kImAtajoAMirar agreements, no disagreement and at least one in-place patch
  //     seen (proof that the sub_825A2FB8 hook runs: without it, a patch would not bump any version), it
  //     moves to applying;
  //   - applying: the candidate is used without memcmp, except 1 in every kImAtajoComprobarCada, which is
  //     still compared;
  //   - a disagreement (the candidate no longer matches memory) or a patcher call that is neither of the
  //     two known ones: "DIFERENCIA" in the log and the shortcut turned off for the run. That load and
  //     the following ones use memcmp.
  void CargarShaderCacheado(uint32_t direccion_tipo, uint32_t tamano, const uint8_t* origen) {
    const uint32_t tipo = direccion_tipo & 0x3;
    const size_t bytes = size_t(tamano) * sizeof(uint32_t);
    auto& vias = cargas_cache_[tipo][size_t((uint64_t(direccion_tipo) * 0x9E3779B97F4A7C15ull) >> 57)];
    if (im_atajo_fase_ == kImAtajoSinEmpezar) {
      EmpezarImAtajo();
    }
    const uint32_t ranura = microcodigo::RanuraDe(direccion_tipo & ~uint32_t(0x3));
    // 1. The shortcut: a way with the same contents, checked with the current versions.
    CargaShader* candidato = nullptr;
    if (im_atajo_fase_ != kImAtajoApagado) {
      const uint32_t global = microcodigo::GlobalRelajado();
      for (CargaShader& via : vias) {
        if (!via.validada || via.direccion_tipo != direccion_tipo || via.tamano != tamano) {
          continue;
        }
        if (via.version_global != global) {
          // Someone has written microcode since it was checked: if not in its slot, it is still valid.
          if (via.version_ranura != microcodigo::InicioTrasGlobal(ranura)) {
            via.validada = false;
            ++im_i_version_cambiada_;
            continue;
          }
          if (microcodigo::g_parches_otros.load(std::memory_order_relaxed) != 0) {
            ApagarImAtajoPorOtros();
            break;
          }
          via.version_global = global;
        }
        candidato = &via;
        break;
      }
    }
    CargaShader* carga = nullptr;
    if (candidato && im_atajo_fase_ == kImAtajoAplicando &&
        (++im_atajo_turno_ & (kImAtajoComprobarCada - 1)) != 0) {
      // 2. Without reading the guest microcode.
      carga = candidato;
      ++cargas_cacheadas_;
      ++im_i_sin_memcmp_;
    } else {
      // 3. The regular path, with the versions read before and after reading guest memory.
      const bool validar = im_atajo_fase_ != kImAtajoApagado;
      microcodigo::Lectura lectura;
      if (validar) {
        lectura = microcodigo::AntesDeLeer(ranura);
      }
      for (CargaShader& via : vias) {
        if (via.direccion_tipo == direccion_tipo && via.tamano == tamano &&
            std::memcmp(via.crudo.data(), origen, bytes) == 0) {
          carga = &via;
          break;
        }
      }
      if (validar && candidato) {
        if (carga == candidato) {
          ++im_atajo_comprobadas_;
          ++im_i_comprobadas_;
          if (im_atajo_fase_ == kImAtajoMirando && im_atajo_comprobadas_ >= kImAtajoAMirar &&
              microcodigo::g_parches_en_su_sitio.load(std::memory_order_relaxed) != 0) {
            im_atajo_fase_ = kImAtajoAplicando;
            NFSMW_INFORME_ANILLO("[nativo] C5a IM_LOAD sin memcmp: {} cargas comprobadas contra el memcmp, 0 "
                                 "desacuerdos y {} parches en su sitio vistos: APLICANDO, el memcmp se salta si la "
                                 "version no ha cambiado (1 de cada {} se sigue comprobando)",
                                 im_atajo_comprobadas_,
                                 microcodigo::g_parches_en_su_sitio.load(std::memory_order_relaxed),
                                 kImAtajoComprobarCada);
          }
        } else {
          ApagarImAtajo(direccion_tipo, tamano, candidato->crudo, candidato->version_ranura,
                        candidato->version_global, carga != nullptr, origen, lectura);
        }
      }
      if (carga) {
        ++cargas_cacheadas_;
      } else {
        carga = vias[0].uso <= vias[1].uso ? &vias[0] : &vias[1];
        carga->direccion_tipo = direccion_tipo;
        carga->tamano = tamano;
        carga->crudo.resize(tamano);
        std::memcpy(carga->crudo.data(), origen, bytes);
        carga->host.resize(tamano);
        // The fresh copy is byte-swapped without reading game memory again: raw and host hold the same
        // contents, the ones the versions certify (with the guest idle, the same as before).
        const uint8_t* copia = reinterpret_cast<const uint8_t*>(carga->crudo.data());
        for (uint32_t i = 0; i < tamano; ++i) {
          carga->host[i] = rex::memory::load_and_swap<uint32_t>(copia + size_t(i) * 4);
        }
        carga->entrada = shaders_.cargada() ? shaders_.Identificar(tipo == 0, carga->host) : nullptr;
        carga->huella = XXH3_64bits(carga->host.data(), bytes);
        carga->generacion = ++generaciones_microcodigo_;
        carga->validada = false;
      }
      if (validar && im_atajo_fase_ != kImAtajoApagado) {
        ++im_i_con_memcmp_;
        if (microcodigo::DespuesDeLeer(ranura, lectura)) {
          carga->validada = true;
          carga->version_ranura = lectura.inicio;
          carga->version_global = lectura.global;
        } else {
          carga->validada = false;  // someone wrote to its slot during the read: it will be compared again
          ++im_i_anuladas_;
        }
      }
    }
    carga->uso = ++cargas_tic_;
    (tipo == 0 ? vs_actual_ : ps_actual_) = carga->entrada;
    if (trazando_ && trazas_ < 4000) {
      ++trazas_;
      REXLOG_INFO("[traza] IM_LOAD {} n{} ({} palabras, huella {:016X})", tipo == 0 ? "VS" : "PS",
                  carga->entrada ? int(carga->entrada->numero) : -1, carga->host.size(), carga->huella);
    }
    if (tipo == 0) {
      vs_microcodigo_ = carga->host;  // patched: the vertex input comes from here
      generacion_vs_ = carga->generacion;
      huella_vs_generacion_ = carga->generacion;
      huella_vs_ = carga->huella;
    }
  }

  // IM_LOAD without memcmp. The first load reads the cvar.
  void EmpezarImAtajo() {
    const bool pedido = REXCVAR_GET(nfsmw_nativo_im_load_sin_memcmp);
    im_atajo_fase_ = pedido && microcodigo::kCreacionesVigiladas ? kImAtajoMirando : kImAtajoApagado;
    NFSMW_INFORME_ANILLO("[nativo] C5a IM_LOAD sin memcmp (nfsmw_nativo_im_load_sin_memcmp) = {}",
                         !pedido ? "no (apagado con el cvar): memcmp en cada IM_LOAD"
                         : !microcodigo::kCreacionesVigiladas
                             ? "no: con NFSMW_NATIVE_SHADER_LIBRARY los constructores no avisan"
                             : "SI, mirando: cada carga se compara tambien con memcmp hasta tener pruebas");
  }

  const char* NombreFaseImAtajo() const {
    switch (im_atajo_fase_) {
      case kImAtajoMirando:
        return "mirando";
      case kImAtajoAplicando:
        return "aplicando";
      case kImAtajoApagado:
        return "APAGADA";
      default:
        return "sin empezar";
    }
  }

  // The shortcut's candidate does not match guest memory: someone wrote microcode without announcing it.
  // (Its fields are passed separately: CargaShader is declared further down and cannot appear in the
  // signature.)
  void ApagarImAtajo(uint32_t direccion_tipo, uint32_t tamano, const std::vector<uint32_t>& crudo,
                     uint32_t version_ranura, uint32_t version_global, bool otra_via, const uint8_t* origen,
                     const microcodigo::Lectura& lectura) {
    uint32_t primera = 0;
    while (primera < tamano && std::memcmp(crudo.data() + primera, origen + size_t(primera) * 4, 4) == 0) {
      ++primera;
    }
    REXLOG_ERROR("[nativo] C5a IM_LOAD sin memcmp: DIFERENCIA. El atajo daba la copia de {} {:08X} ({} palabras) "
                 "comprobada con las versiones ranura {} y global {}, y la memoria del invitado ya es otra (primera "
                 "palabra distinta: {}; el memcmp {}). Versiones de ahora: inicio {} fin {} global {}; fase {} con {} "
                 "cargas comprobadas. Escrituras avisadas: {} parches en su sitio, {} en la copia del anillo, {} "
                 "otros, {} creaciones. Alguien escribe microcodigo sin avisar: atajo APAGADO para el resto de la "
                 "sesion (todas las cargas con memcmp, como antes de la 184)",
                 (direccion_tipo & 0x3) == 0 ? "VS" : "PS", direccion_tipo & ~uint32_t(0x3), tamano,
                 version_ranura, version_global, primera,
                 otra_via ? "encuentra otra via" : "tiene que volver a copiarlo", lectura.inicio, lectura.fin,
                 lectura.global, NombreFaseImAtajo(), im_atajo_comprobadas_,
                 microcodigo::g_parches_en_su_sitio.load(std::memory_order_relaxed),
                 microcodigo::g_parches_en_copia.load(std::memory_order_relaxed),
                 microcodigo::g_parches_otros.load(std::memory_order_relaxed),
                 microcodigo::g_creaciones.load(std::memory_order_relaxed));
    im_atajo_fase_ = kImAtajoApagado;
  }

  // The fetch patcher was called from outside its two known call sites.
  void ApagarImAtajoPorOtros() {
    REXLOG_ERROR("[nativo] C5a IM_LOAD sin memcmp: DIFERENCIA. El parcheador de los fetch (sub_825A2FB8) se ha "
                 "llamado {} veces desde fuera de sus dos llamadas conocidas (retorno {:08X}, destino {:08X}): puede "
                 "escribir microcodigo de otra forma. Atajo APAGADO para el resto de la sesion (todas las cargas con "
                 "memcmp, como antes de la 184)",
                 microcodigo::g_parches_otros.load(std::memory_order_relaxed),
                 microcodigo::g_otro_retorno.load(std::memory_order_relaxed),
                 microcodigo::g_otro_destino.load(std::memory_order_relaxed));
    im_atajo_fase_ = kImAtajoApagado;
  }

  // IM_LOAD_IMMEDIATE (and unusual IM_LOAD loads): no cache.
  void IdentificarShader(uint32_t tipo) {
    if (tipo > 1) {
      return;
    }
    const EntradaShader* entrada =
        shaders_.cargada() ? shaders_.Identificar(tipo == 0, microcodigo_) : nullptr;
    (tipo == 0 ? vs_actual_ : ps_actual_) = entrada;
    if (trazando_ && trazas_ < 4000) {
      ++trazas_;
      REXLOG_INFO("[traza] IM_LOAD {} n{} ({} palabras, huella {:016X})", tipo == 0 ? "VS" : "PS",
                  entrada ? int(entrada->numero) : -1, microcodigo_.size(),
                  XXH3_64bits(microcodigo_.data(), microcodigo_.size() * sizeof(uint32_t)));
    }
    if (tipo == 0) {
      vs_inmediato_ = microcodigo_;  // patched: the vertex input comes from here
      vs_microcodigo_ = vs_inmediato_;
      generacion_vs_ = ++generaciones_microcodigo_;
    }
  }

  // IM_LOAD_IMMEDIATE with an exact cache (nfsmw_nativo_im_inmediato_cache).
  //
  // Who sends them: sub_825A37D8, the D3D copy path. If the GPU has not yet passed the fence of a VS's
  // last IM_LOAD, or outputs the PS does not read must be disabled (sub_825A36A8), it copies the microcode
  // at [VS+40] ([VS+600] bytes) into the ring itself and patches it there (sub_825A2FB8 on the copy).
  // Always type 0 (VS).
  // In a race there are stretches with 4,000-8,000 per second at 1.5-2.8 us each ("tiempos por
  // paquete", op 2B): the regular path byte-swaps word by word, computes the XXH3 in Identificar, copies
  // the VS to vs_inmediato_ and gives it a new generation, so the next draw redoes the fingerprint
  // (another XXH3 in HuellaVs), the fetch coherence and the vertex input key. And the contents repeat:
  // over a whole run there are 82 distinct microcodes between IM_LOAD and immediate loads.
  //   - Set chosen by a cheap signature (size and four words, not swapped). Within the set, the way with
  //     the same signature, the same size and the same bytes (memcmp against its raw copy): equality is
  //     exact; the signature only chooses where to look.
  //   - Hit: the way's swapped copy, identification, fingerprint and generation. Same microcode, same
  //     generation, as in the IM_LOAD cache (CargarShaderCacheado): whatever is cached per generation
  //     stays valid.
  //   - Miss, or a packet that wraps around the ring (not contiguous): the regular path; the miss is
  //     stored.
  // Self-checking guard:
  //   - watching: every load takes the regular path and, if the cache had a way, it is compared with what
  //     the cache would give (swapped copy and identification). After kInmAMirar agreements and no
  //     disagreement it moves to applying;
  //   - applying: hits neither swap nor identify; 1 in every kInmComprobarCada is still compared;
  //   - a disagreement, or a new way whose swapped raw copy is not what the regular path read (the ring
  //     position arithmetic): "DIFERENCIA" in the log and the cache turned off for the run. That load
  //     already uses the regular result.
  struct CargaInmediata {
    uint64_t firma = 0;  // 0 = via vacia
    uint32_t tamano = 0;
    std::vector<uint32_t> crudo;  // as it is in the ring (big-endian)
    std::vector<uint32_t> host;   // swapped: what gets identified and what the vertex input reads
    const EntradaShader* entrada = nullptr;
    uint64_t huella = 0;      // host XXH3, the same one HuellaVs would compute
    uint64_t generacion = 0;  // from generaciones_microcodigo_, when this content was stored
    uint64_t uso = 0;         // the least recently used way is the one replaced
  };

  void CargarInmediato(Lector datos, uint32_t tipo, uint32_t tamano) {
    if (inm_fase_ == kInmSinEmpezar) {
      EmpezarInmediato();
    }
    const bool vuelta = datos.mascara != 0 && size_t(datos.pos) + tamano > size_t(datos.mascara) + 1;
    if (inm_fase_ == kInmApagado || tipo > 1 || tamano == 0 || vuelta) {
      if (vuelta) {
        ++inm_i_vuelta_;
      } else {
        ++inm_i_sin_cache_;
      }
      CargarInmediatoSinCache(datos, tipo, tamano);
      return;
    }
    const uint8_t* crudo = datos.base + size_t(datos.pos) * 4;
    const size_t bytes = size_t(tamano) * sizeof(uint32_t);
    const uint64_t firma = FirmaInmediato(crudo, tamano);
    const size_t indice = size_t(firma >> (64 - kInmBitsConjunto));
    auto& conjunto = inm_cache_[tipo][indice];
    uint8_t& ultima = inm_ultima_[tipo][indice];
    CargaInmediata* via = nullptr;
    for (uint32_t k = 0; k < kInmVias; ++k) {  // first the way of the set's last hit
      const uint32_t v = (ultima + k) % kInmVias;
      CargaInmediata& candidata = conjunto[v];
      if (candidata.firma == firma && candidata.tamano == tamano &&
          std::memcmp(candidata.crudo.data(), crudo, bytes) == 0) {
        via = &candidata;
        ultima = uint8_t(v);
        break;
      }
    }
    if (via && inm_fase_ == kInmAplicando && (++inm_turno_ & (kInmComprobarCada - 1)) != 0) {
      // Hit: no byte swap, no identification, no copy, no new generation.
      via->uso = ++inm_tic_;
      UsarInmediato(tipo, *via);
      ++inm_i_aciertos_;
      ++cargas_cacheadas_;
      if (trazando_ && trazas_ < 4000) {
        ++trazas_;
        REXLOG_INFO("[traza] IM_LOAD {} n{} ({} palabras, huella {:016X}; IM_LOAD_IMMEDIATE de la cache)",
                    tipo == 0 ? "VS" : "PS", via->entrada ? int(via->entrada->numero) : -1, via->host.size(),
                    via->huella);
      }
      return;
    }
    // The regular path: swaps, identifies and leaves its result in use
    // (vs_actual_ or ps_actual_, vs_inmediato_...).
    CargarInmediatoSinCache(datos, tipo, tamano);
    const EntradaShader* entrada = tipo == 0 ? vs_actual_ : ps_actual_;
    if (via) {
      // Check: what the cache would have given against what the regular path just gave.
      uint32_t primera = 0;
      while (primera < tamano && primera < via->host.size() && via->host[primera] == microcodigo_[primera]) {
        ++primera;
      }
      if (primera != tamano || via->host.size() != tamano || via->entrada != entrada) {
        ApagarInmediato("un acierto de la cache no da lo mismo que el camino de siempre", tipo, tamano, firma, primera,
                        via->entrada, entrada);
        return;
      }
      ++inm_comprobadas_;
      ++inm_i_comprobadas_;
      via->uso = ++inm_tic_;
      if (inm_fase_ == kInmMirando && inm_comprobadas_ >= kInmAMirar) {
        inm_fase_ = kInmAplicando;
        NFSMW_INFORME_ANILLO("[nativo] C5a IM_LOAD_IMMEDIATE con cache: {} aciertos comprobados contra el camino de "
                             "siempre y 0 desacuerdos: APLICANDO, los aciertos ya no giran ni identifican (1 de "
                             "cada {} se sigue comprobando)",
                             inm_comprobadas_, kInmComprobarCada);
      }
      if (inm_fase_ == kInmAplicando) {
        UsarInmediato(tipo, *via);  // the same generation as the unchecked hits
      }
      return;
    }
    // Miss: into the least recently used way of the set.
    ++inm_i_fallos_;
    uint32_t sitio = 0;
    for (uint32_t k = 1; k < kInmVias; ++k) {
      if (conjunto[k].uso < conjunto[sitio].uso) {
        sitio = k;
      }
    }
    CargaInmediata& nueva = conjunto[sitio];
    nueva.firma = firma;
    nueva.tamano = tamano;
    nueva.crudo.resize(tamano);
    std::memcpy(nueva.crudo.data(), crudo, bytes);
    nueva.host.assign(microcodigo_.begin(), microcodigo_.end());
    nueva.entrada = entrada;
    nueva.huella = XXH3_64bits(nueva.host.data(), bytes);
    nueva.generacion = ++generaciones_microcodigo_;
    nueva.uso = ++inm_tic_;
    ultima = uint8_t(sitio);
    // The raw copy comes from the packet's position in the ring: once swapped it must equal what the
    // regular path read word by word. Only on misses (a few dozen per run).
    const uint8_t* copia = reinterpret_cast<const uint8_t*>(nueva.crudo.data());
    for (uint32_t i = 0; i < tamano; ++i) {
      if (rex::memory::load_and_swap<uint32_t>(copia + size_t(i) * 4) != nueva.host[i]) {
        nueva.firma = 0;  // empty way: cannot come out again
        nueva.uso = 0;
        ApagarInmediato("la copia cruda de una via nueva no es lo que leyo el camino de siempre", tipo, tamano, firma,
                        i, entrada, entrada);
        return;
      }
    }
    if (inm_fase_ == kInmAplicando) {
      UsarInmediato(tipo, nueva);
    }
  }

  // The regular IM_LOAD_IMMEDIATE path (formerly inline in PaqueteTipo3).
  void CargarInmediatoSinCache(Lector datos, uint32_t tipo, uint32_t tamano) {
    microcodigo_.resize(tamano);
    for (uint32_t i = 0; i < tamano; ++i) {
      microcodigo_[i] = datos.Leer();
    }
    IdentificarShader(tipo);
  }

  // The same state IdentificarShader leaves, taken from the way (without swapping, identifying or
  // hashing).
  void UsarInmediato(uint32_t tipo, const CargaInmediata& via) {
    (tipo == 0 ? vs_actual_ : ps_actual_) = via.entrada;
    if (tipo == 0) {
      vs_microcodigo_ = via.host;  // patched: the vertex input comes from here
      generacion_vs_ = via.generacion;
      huella_vs_generacion_ = via.generacion;
      huella_vs_ = via.huella;
    }
  }

  // Cheap signature of an unswapped packet: picks the set and rejects other shaders' ways before the
  // memcmp.
  static uint64_t FirmaInmediato(const uint8_t* crudo, uint32_t tamano) {
    const size_t posiciones[4] = {0, size_t(tamano) / 3, size_t(tamano) * 2 / 3, size_t(tamano) - 1};
    uint64_t h = uint64_t(tamano) * 0x9E3779B97F4A7C15ull;
    for (const size_t p : posiciones) {
      uint32_t palabra;
      std::memcpy(&palabra, crudo + p * 4, sizeof(palabra));
      h = (h ^ palabra) * 0xBF58476D1CE4E5B9ull;
      h ^= h >> 29;
    }
    return h | 1;  // never 0: 0 means an empty way
  }

  void EmpezarInmediato() {
    const bool pedido = REXCVAR_GET(nfsmw_nativo_im_inmediato_cache);
    inm_fase_ = pedido ? kInmMirando : kInmApagado;
    NFSMW_INFORME_ANILLO("[nativo] C5a IM_LOAD_IMMEDIATE con cache (nfsmw_nativo_im_inmediato_cache) = {}",
                         pedido ? "SI, mirando: cada acierto se compara con el camino de siempre hasta tener pruebas"
                                : "no (apagado con el cvar): cada IM_LOAD_IMMEDIATE se gira e identifica, como antes");
  }

  const char* NombreFaseInmediato() const {
    switch (inm_fase_) {
      case kInmMirando:
        return "mirando";
      case kInmAplicando:
        return "aplicando";
      case kInmApagado:
        return "APAGADA";
      default:
        return "sin empezar";
    }
  }

  // The cache does not match the regular path: it is turned off for the rest of the run.
  void ApagarInmediato(const char* motivo, uint32_t tipo, uint32_t tamano, uint64_t firma, uint32_t primera,
                       const EntradaShader* de_la_cache, const EntradaShader* de_siempre) {
    REXLOG_ERROR("[nativo] C5a IM_LOAD_IMMEDIATE con cache: DIFERENCIA, {}: {} de {} palabras (firma {:016X}), "
                 "primera palabra distinta {}, shader de la cache n{} y del camino de siempre n{}; fase {} con {} "
                 "aciertos comprobados. Cache APAGADA para el resto de la sesion (todo por el camino de siempre, "
                 "como antes de la 184); esta carga ya va con el camino de siempre",
                 motivo, tipo == 0 ? "VS" : "PS", tamano, firma, primera,
                 de_la_cache ? int(de_la_cache->numero) : -1, de_siempre ? int(de_siempre->numero) : -1,
                 NombreFaseInmediato(), inm_comprobadas_);
    inm_fase_ = kInmApagado;
  }

  void ContarDibujoShaders() {
    if (!shaders_.cargada()) {
      return;
    }
    if (!vs_actual_) {
      ++dibujos_sin_vs_;
#if defined(NFSC_RECOMP)
      // NFS Carbon diagnostics: which loaded vertex shaders the draws without a VS use.
      if (sin_vs_detalle_.size() < 256 || sin_vs_detalle_.count(HuellaVs())) {
        DetalleSinVs& d = sin_vs_detalle_[HuellaVs()];
        ++d.dibujos;
        d.palabras = uint32_t(vs_microcodigo_.size());
        d.ps = ps_actual_ ? int32_t(ps_actual_->numero) : -1;
        d.tipo = Registro(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR) & 0x3F;
        d.cuenta = Registro(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR) >> 16;
      }
#endif
    } else if (!ps_actual_) {
      ++dibujos_sin_ps_;
    } else {
      ++dibujos_identificados_;
    }
    const uint64_t par = (uint64_t(vs_actual_ ? vs_actual_->numero + 1 : 0) << 32) |
                         (ps_actual_ ? ps_actual_->numero + 1 : 0);
    // In a race there are 40 distinct pairs and ~2,200 draws per frame, mostly runs of the same
    // material. Remembering the previous pair avoids the set insert (a hash and a possible cache
    // miss) on every draw.
    if (par != ultimo_par_shaders_) {
      ultimo_par_shaders_ = par;
      if (pares_.size() < 4096) {
        pares_.insert(par);
      }
    }
  }

  // Step C5b: the game thread's Draw* record that corresponds to this ring draw.
  // They arrive in the same order; the primitive type and count are checked, and
  // at most 8 records without a draw are skipped.
  // Phase 2b: if the last marker carries its Draw* record and this draw accepts it (the usual acceptance:
  // type, count and shaders consistent with the IM_LOAD packets), apply mode uses it without the queue or
  // the search. If it is not accepted, it is kept for the next draw (like the head of the queue) and the
  // usual search runs. In check mode the usual search runs and the result of each path is compared
  // (CompararDibujoMarcador).
  void EmparejarDibujo() {
    vs_dibujo_ = nullptr;
    ps_dibujo_ = nullptr;
    motivo_emparejado_ = 0;
    objeto_vs_ = objeto_ps_ = 0;
    vegetacion_juego_ = 0;  // without a record there is no game verdict to compare
    if (!shaders_.cargada()) {
      return;
    }
    RefrescarObjetos();  // a single acquire load per draw (formerly four)
    const uint32_t iniciador = Registro(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR);
    const uint32_t tipo = iniciador & 0x3F;
    const uint32_t cuenta = iniciador >> 16;
    const uint32_t modo_marcador = dibujo_marcador_modo_;
    bool marcador_vale = false;
    if (modo_marcador != 0) {
      marcador_vale = dibujo_marcador_.args[0] == tipo && CuentaDelRegistro(dibujo_marcador_) == cuenta &&
                      ShadersCoherentes(dibujo_marcador_);
      if (!marcador_vale) {
        ++registros_de_marcador_rechazados_;
      } else {
        dibujo_marcador_modo_ = 0;  // accepted: valid for this draw and no other
        if (modo_marcador == kDibujoAplicar) {
          ++dibujos_con_registro_de_marcador_;
          UsarRegistroDeDibujo(dibujo_marcador_);
          return;
        }
      }
    }
    RegistroDibujo registro;
    while (pendientes_.size() < 64 && SacarDibujo(registro)) {
      pendientes_.push_back(registro);
    }
    size_t encontrado = SIZE_MAX;
    bool incoherente = false;
    // The size of a deque is not a field, it is computed by subtracting iterators. It was
    // queried on every loop iteration, and there are ~2,200 draws per frame.
    const size_t candidatos = std::min<size_t>(pendientes_.size(), 8);
    for (size_t i = 0; i < candidatos; ++i) {
      if (pendientes_[i].funcion != FuncionDibujo::kCualquiera &&
          (pendientes_[i].args[0] != tipo || CuentaDelRegistro(pendientes_[i]) != cuenta)) {
        continue;
      }
      // Type and count are not enough: in a series of identical draws, a record
      // without a draw in the ring shifts the matching. The record's VS and PS
      // must be those of the last IM_LOAD packets (observed: VS n15 matched with
      // another shader's microcode).
      if (!ShadersCoherentes(pendientes_[i])) {
        ++candidatos_incoherentes_;
        incoherente = true;
        continue;
      }
      encontrado = i;
      break;
    }
    if (modo_marcador == kDibujoComprobar && marcador_vale) {
      CompararDibujoMarcador(encontrado == SIZE_MAX ? nullptr : &pendientes_[encontrado], tipo, cuenta);
    }
    if (encontrado == SIZE_MAX) {
      ++dibujos_sin_registro_;
      dibujos_incoherentes_ += incoherente;
      motivo_emparejado_ = incoherente ? 2 : 1;
      if (avisos_c5b_ < 16) {
        ++avisos_c5b_;
        std::string primero;
        if (!pendientes_.empty()) {
          const RegistroDibujo& r = pendientes_.front();
          primero = fmt::format("; el primero pendiente es funcion {} tipo {} cuenta {}",
                                int(r.funcion), r.args[0], CuentaDelRegistro(r));
        }
        REXLOG_WARN("[nativo] C5b: dibujo del anillo sin registro de Draw*: tipo {} cuenta {} "
                    "({} pendientes{})",
                    tipo, cuenta, pendientes_.size(), primero);
      }
      UsarIdentidadDelAnillo();
      return;
    }
    registros_saltados_ += encontrado;
    const RegistroDibujo r = pendientes_[encontrado];
    pendientes_.erase(pendientes_.begin(), pendientes_.begin() + std::ptrdiff_t(encontrado + 1));
    UsarRegistroDeDibujo(r);
  }

  // The usual work with the matched record, whether it came from the queue or from the marker (phase 2b:
  // moved out of EmparejarDibujo unchanged).
  void UsarRegistroDeDibujo(const RegistroDibujo& r) {
    if (r.sombra) {
      CompararSombra(r.sombra);  // phase 1 of the Direct3D-level renderer
    }
    objeto_vs_ = r.vs;
    objeto_ps_ = r.ps;
    ++dibujos_emparejados_;
    vs_dibujo_ = ShaderDeObjetoCacheado(r.vs);
    ps_dibujo_ = ShaderDeObjetoCacheado(r.ps);
    // The game's vegetation verdict is only compared when drawing with this record's shaders; not with
    // the ring identity (UsarIdentidadDelAnillo, below) (nfsmw_d3d_vegetacion_juego).
    vegetacion_juego_ = IdentidadParaVegetacion(r.vegetacion, vs_dibujo_ != nullptr && ps_dibujo_ != nullptr);
    if (vs_dibujo_ && ps_dibujo_) {
      // Only when the VS or its microcode changes: it used to be one map write per draw.
      if ((vs_dibujo_ != mapeado_vs_ || generacion_vs_ != mapeado_generacion_) &&
          vs_por_microcodigo_.size() < 4096) {
        vs_por_microcodigo_[HuellaVs()] = vs_dibujo_;
        mapeado_vs_ = vs_dibujo_;
        mapeado_generacion_ = generacion_vs_;
      }
    } else {
      UsarIdentidadDelAnillo();
    }
    dibujos_con_vs_ += vs_dibujo_ != nullptr;
    dibujos_con_ps_ += ps_dibujo_ != nullptr;
    if (vs_dibujo_ && (vs_dibujo_ != anotado_vs_ || generacion_vs_ != anotado_generacion_)) {
      anotado_vs_ = vs_dibujo_;
      anotado_generacion_ = generacion_vs_;
      AnotarEntradaVertices(*vs_dibujo_);
    }
  }

  // Phase 2b: the VS and PS that would be used to draw with this record (null: no record), without
  // changing anything: what UsarRegistroDeDibujo and UsarIdentidadDelAnillo would do.
  std::pair<const EntradaShader*, const EntradaShader*> ShadersConRegistro(const RegistroDibujo* r) {
    const EntradaShader* vs = r ? ShaderDeObjetoCacheado(r->vs) : nullptr;
    const EntradaShader* ps = r ? ShaderDeObjetoCacheado(r->ps) : nullptr;
    if ((vs && ps) || !ps_actual_) {
      return {vs, ps};
    }
    const EntradaShader* identidad = vs_actual_;
    if (!identidad) {
      const auto it = vs_por_microcodigo_.find(HuellaVs());
      if (it == vs_por_microcodigo_.end()) {
        return {vs, ps};
      }
      identidad = it->second;
    }
    return {identidad, ps_actual_};
  }

  /*
   * Phase 2b: the comparison of the watching phase (and of 1 in every 1,024 afterwards). Only when the
   * draw accepts the marker's record: if it does not, the marker path ends in the same search and gives
   * the same result by construction. What decides the draw is compared: the VS and PS it would be drawn
   * with using the record from the search (or the ring identity if none is found) and using the marker's
   * record. A different record with the same shaders is only counted.
   */
  void CompararDibujoMarcador(const RegistroDibujo* busqueda, uint32_t tipo, uint32_t cuenta) {
    const auto con_busqueda = ShadersConRegistro(busqueda);
    const auto con_marcador = ShadersConRegistro(&dibujo_marcador_);
    const RegistroDibujo& m = dibujo_marcador_;
    uint32_t que = 0;  // 0 equal; 1 different shaders; 3 search finds no record and the identity is not valid
    if (con_busqueda != con_marcador) {
      que = busqueda ? 1u : 3u;
    } else if (!busqueda || busqueda->funcion != m.funcion || busqueda->vs != m.vs || busqueda->ps != m.ps ||
               busqueda->args[1] != m.args[1] || busqueda->args[2] != m.args[2] || busqueda->args[3] != m.args[3]) {
      ++comprobaciones_dibujo_otro_registro_;  // another record (or the identity), same shaders
    }
    ++comprobaciones_dibujo_;
    if (que != 0) {
      ++comprobaciones_dibujo_distintas_;
      if (avisos_dibujo_marcador_ < 8) {
        ++avisos_dibujo_marcador_;
        const auto numero = [](const EntradaShader* e) { return e ? int(e->numero) : -1; };
        REXLOG_ERROR("[nativo] registro de dibujo (fase 2b): la busqueda y el marcador no dan lo mismo (caso {}) en "
                     "un dibujo tipo {} cuenta {}: busqueda {} (VS {:08X} PS {:08X}) -> VS n{} PS n{}; marcador "
                     "(VS {:08X} PS {:08X}) -> VS n{} PS n{}. El hilo del juego apaga la fase 2b",
                     que, tipo, cuenta, busqueda ? "con registro" : "sin registro", busqueda ? busqueda->vs : 0u,
                     busqueda ? busqueda->ps : 0u, numero(con_busqueda.first), numero(con_busqueda.second), m.vs,
                     m.ps, numero(con_marcador.first), numero(con_marcador.second));
      }
    }
    AnotarComprobacionDibujo(que == 0, que, busqueda, &m);
  }

  // Phase 2b: the kPalabrasDibujo words of the Draw* record (EscribirDibujo in
  // nfsmw_d3d_registros_nativo.cpp). false if the function is not a Draw* one.
  static bool LeerDibujoDelMarcador(const uint8_t* p, RegistroDibujo& r) {
    // The low byte is the function and the 16 bits above it the game's vegetation verdict
    // (RegistroDibujo::vegetacion, nfsmw_d3d_vegetacion_juego); the high byte must be 0.
    const uint32_t palabra = rex::memory::load_and_swap<uint32_t>(p);
    const uint32_t funcion = palabra & 0xFFu;
    if (funcion > uint32_t(FuncionDibujo::kIndexadosUP) || (palabra >> 24) != 0) {
      return false;
    }
    r.funcion = FuncionDibujo(funcion);
    r.vegetacion = uint16_t(palabra >> 8);
    r.vs = rex::memory::load_and_swap<uint32_t>(p + 4);
    r.ps = rex::memory::load_and_swap<uint32_t>(p + 8);
    for (uint32_t i = 0; i < 4; ++i) {
      r.args[i] = rex::memory::load_and_swap<uint32_t>(p + 12 + 4 * i);
    }
    r.sombra = (uint64_t(rex::memory::load_and_swap<uint32_t>(p + 28)) << 32) |
               rex::memory::load_and_swap<uint32_t>(p + 32);
    return true;
  }

  /*
   * Phase 1 of the Direct3D-level renderer (docs/native-renderer.md).
   * The snapshot of the device mirror taken in this draw's Draw* call, against the state the ring has
   * read from the packets right when it reaches this draw. Everything that matches is state the renderer
   * can read from the mirror without packets; anything else has to come through another path.
   */
  void CompararSombra(uint64_t secuencia) {
    if (!LeerInstantanea(secuencia, sombra_foto_)) {
      ++sombra_perdidas_;
      return;
    }
    ++sombra_dibujos_;
    const InstantaneaEspejo& f = sombra_foto_;
    for (uint32_t g = 0; g < kGruposEspejo; ++g) {
      const uint64_t m = f.mascara[g];
      for (uint32_t i = 0; m && i < 64; ++i) {
        if (!((m >> (63 - i)) & 1)) {
          continue;
        }
        const uint32_t reg = f.base_registro[g] + i;
        ++sombra_estado_comparados_;
        if (Registro(reg) != f.estado[g][i]) {
          ++sombra_estado_distintos_;
          ++sombra_por_registro_[reg];
        }
      }
    }
    for (uint32_t i = 0; i < 192; ++i) {
      ++sombra_fetch_comparados_;
      if (Registro(0x4800 + i) != f.fetch[i]) {
        ++sombra_fetch_distintos_;
        ++sombra_por_registro_[0x4800 + i];
      }
    }
    for (uint32_t i = 0; i < 2048; ++i) {
      ++sombra_constantes_comparadas_;
      if (Registro(0x4000 + i) != f.constantes[i]) {
        ++sombra_constantes_distintas_;
        ++sombra_por_registro_[0x4000 + i];
      }
    }
  }

  // Whether the VS and PS of a Draw* record are the ones the ring has loaded.
  // The PS arrives unpatched: its microcode is compared. For the patched VS, the only
  // possible check is that its fetches write to the original's registers.
  bool ShadersCoherentes(const RegistroDibujo& registro) {
    const EntradaShader* ps = ShaderDeObjetoCacheado(registro.ps);
    if (ps && ps_actual_ && ps->huella != ps_actual_->huella) {
      return false;
    }
    const EntradaShader* vs = ShaderDeObjetoCacheado(registro.vs);
    if (!vs) {
      return true;
    }
    if (vs != coherencia_vs_ || generacion_vs_ != coherencia_generacion_) {
      coherencia_vs_ = vs;
      coherencia_generacion_ = generacion_vs_;
      coherencia_ = FetchCoherentes(*vs, vs_microcodigo_);
    }
    return coherencia_;
  }

  // No usable Draw* record (in the menu, the final composite has no record of its own
  // and the ring carries VS n111 and PS n19): the PS of the last IM_LOAD, which arrives
  // unpatched; the VS of the IM_LOAD if it was fully identified, and otherwise the one
  // already seen matched with this same patched microcode.
  void UsarIdentidadDelAnillo() {
    if (!ps_actual_) {
      return;
    }
    const EntradaShader* vs = vs_actual_;
    if (!vs) {
      const auto it = vs_por_microcodigo_.find(HuellaVs());
      if (it == vs_por_microcodigo_.end()) {
        return;
      }
      vs = it->second;
    }
    vs_dibujo_ = vs;
    ps_dibujo_ = ps_actual_;
    ++dibujos_por_im_load_;
  }

  uint64_t HuellaVs() {
    if (huella_vs_generacion_ != generacion_vs_) {
      huella_vs_generacion_ = generacion_vs_;
      huella_vs_ = XXH3_64bits(vs_microcodigo_.data(), vs_microcodigo_.size() * sizeof(uint32_t));
    }
    return huella_vs_;
  }

  static uint32_t CuentaDelRegistro(const RegistroDibujo& r) {
    switch (r.funcion) {
      case FuncionDibujo::kVertices:
        return r.args[2];  // r6: vertex count
      case FuncionDibujo::kIndexados:
        return r.args[3];  // r7: index count
      case FuncionDibujo::kVerticesUP:
        return r.args[1];  // r5: vertex count
      case FuncionDibujo::kIndexadosUP:
      default:
        return r.args[3];  // r7: index count (unconfirmed)
    }
  }

  /*
   * The object generation used to be read four times per draw (twice in ShadersCoherentes and
   * twice in EmparejarDibujo), and it is an atomic load with acquire: on the Switch's A57 that
   * is an ldar, which drains the load buffer. It is now read once, on entering the matching;
   * everything after that in the draw (including the trace) already has it.
   */
  void RefrescarObjetos() {
    const uint64_t generacion = GeneracionObjetos();
    if (generacion != generacion_objetos_) {
      objetos_.clear();
      memo_objetos_.fill(MemoObjeto{});
      generacion_objetos_ = generacion;
    }
  }

  const EntradaShader* ShaderDeObjetoCacheado(uint32_t objeto) {
    /*
     * A 16-slot direct-mapped cache in front of the map: the draw's VS and PS are looked up several
     * times (candidate coherence and matching), and between consecutive draws they are almost
     * always the same. It saves the hash and the cache miss of the unordered_map, which with
     * ~2,200 draws per frame were ~9,000 lookups. RefrescarObjetos has already validated the
     * generation: when it changes, both caches are cleared together.
     */
    MemoObjeto& memo = memo_objetos_[size_t((objeto * UINT32_C(2654435761)) >> 28)];
    if (memo.objeto == objeto) {
      return memo.entrada;
    }
    const EntradaShader* entrada;
    if (const auto it = objetos_.find(objeto); it != objetos_.end()) {
      entrada = it->second;
    } else {
      entrada = ShaderDeObjeto(objeto);
      objetos_.emplace(objeto, entrada);
    }
    memo.objeto = objeto;
    memo.entrada = entrada;
    return entrada;
  }

  // Vertex input of the patched VS. Each element of the container fetches into
  // a temporary register; the ring microcode is searched, among the fetch
  // instructions, for the one that writes to that register (D3D reorders them),
  // and it yields the fetch constant, format, stride and offset. Logged once per
  // variant, for validation.
  void AnotarEntradaVertices(const EntradaShader& vs) {
    if (vs_microcodigo_.size() != vs.microcodigo.size()) {
      ++variantes_longitud_distinta_;
      return;
    }
    if (variantes_vistas_.size() >= 256) {
      return;
    }
    const uint64_t clave = (uint64_t(vs.numero) << 48) ^ HuellaVs();  // the same XXH3, already computed
    if (!variantes_vistas_.insert(clave).second) {
      return;
    }
    std::string detalle;
    for (const ElementoVertice& elemento : vs.elementos) {
      const uint32_t registro = (vs.microcodigo[size_t(elemento.instruccion) * 3] >> 12) & 0x3F;
      size_t p = SIZE_MAX;
      for (const ElementoVertice& otro : vs.elementos) {
        const size_t q = size_t(otro.instruccion) * 3;
        if (((vs_microcodigo_[q] >> 12) & 0x3F) == registro && (vs_microcodigo_[q] & 0x1F) == 0) {
          p = q;
          break;
        }
      }
      if (p == SIZE_MAX) {
        detalle += fmt::format(" {}{}:?", NombreUso(elemento.uso), elemento.indice_uso);
        ++elementos_sin_fetch_;
        continue;
      }
      const uint32_t d0 = vs_microcodigo_[p], d1 = vs_microcodigo_[p + 1], d2 = vs_microcodigo_[p + 2];
      // exp_adjust (signed bits 24-29) and sign mode (bit 14): the emulated path multiplies the
      // value by 2^exp_adjust (spirv_translator_fetch.cpp:445-449); this path does not.
      const int32_t exponente = int32_t(d1 << 2) >> 26;
      detalle += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}/s{:03X}>{:03X}{}{}{}{}", NombreUso(elemento.uso),
                             elemento.indice_uso, ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3),
                             (d1 >> 16) & 0x3F, d2 & 0xFF, int32_t(d2 << 1) >> 9,
                             vs.microcodigo[size_t(elemento.instruccion) * 3 + 1] & 0xFFF,
                             d1 & 0xFFF, ((d1 >> 30) & 0x1) ? "/mini" : "",
                             ((d1 >> 12) & 0x1) ? "/signo" : "",
                             exponente ? fmt::format("/exp{}", exponente) : std::string(),
                             ((d1 >> 14) & 0x1) ? "/rf1" : "");
    }
    if (variantes_vistas_.size() <= 24) {  // was 200; the ring thread writes it
      REXLOG_INFO("[nativo] C5b: VS n{} (variante {:016X}): entrada de vertices{}", vs.numero,
                  clave, detalle);
    }
  }

  // Fetch constant 0, which VdSwap writes right before the Swap packet.
  TexturaSwap TexturaDelSwap() const {
    TexturaSwap textura;
    for (uint32_t i = 0; i < 6; ++i) {
      textura.dword[i] = Registro(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i);
    }
    return textura;
  }

  void BuferIndirecto(uint32_t direccion, uint32_t palabras, int profundidad) {
    SumarSoloAnillo(indirectos_);
    Lector lector;
    lector.base = memory_->TranslatePhysical(direccion);
    lector.pos = 0;
    lector.fin = palabras;
    while (lector.Pendientes()) {
      if (!Paquete(lector, profundidad)) {
        break;
      }
    }
  }

  // --- Presentacion de prueba -------------------------------------------------

  void Presentar() {
    const uint64_t swap = swaps_.fetch_add(1, std::memory_order_relaxed) + 1;
    g_swaps_nativos.fetch_add(1, std::memory_order_relaxed);
    contador_.fetch_add(1, std::memory_order_relaxed);
    if (!presenter_) {
      return;
    }
    // C2: the game image, if this Swap has a resolved texture.
    if (destinos_) {
      // The gamma ramp, if the game has changed it (same thread as AnotarRampaGamma).
      if (version_rampa_enviada_ != version_rampa_) {
        version_rampa_enviada_ = version_rampa_;
        destinos_->RampaGamma(rampa_gamma_);
      }
      const auto inicio = Reloj::now();
      const bool presentado = destinos_->Presentar(presenter_.get(), TexturaDelSwap(),
                                                   swap_ancho_.load(std::memory_order_relaxed),
                                                   swap_alto_.load(std::memory_order_relaxed));
      tiempo_presentar_ns_ += NanosegundosDesde(inicio);
      ++presentaciones_medidas_;
      if (presentado) {
        return;
      }
    }
    presenter_->RefreshGuestOutput(
        kSalidaAncho, kSalidaAlto, kSalidaAncho, kSalidaAlto,
        [this, swap](rex::ui::Presenter::GuestOutputRefreshContext& contexto) {
          return LimpiarSalida(static_cast<ContextoSalida&>(contexto), swap);
        });
  }

  bool LimpiarSalida(ContextoSalida& contexto, uint64_t swap) {
    const rex::ui::vulkan::VulkanDevice* dispositivo = provider_->vulkan_device();
    const auto& dfn = dispositivo->functions();
    const VkDevice device = dispositivo->device();

    if (render_pass_ == VK_NULL_HANDLE) {
      VkAttachmentDescription adjunto{};
      adjunto.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
      adjunto.samples = VK_SAMPLE_COUNT_1_BIT;
      adjunto.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      adjunto.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      adjunto.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      adjunto.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      adjunto.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      adjunto.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
      VkAttachmentReference referencia{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpase{};
      subpase.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpase.colorAttachmentCount = 1;
      subpase.pColorAttachments = &referencia;
      VkRenderPassCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = 1;
      info.pAttachments = &adjunto;
      info.subpassCount = 1;
      info.pSubpasses = &subpase;
      if (dfn.vkCreateRenderPass(device, &info, nullptr, &render_pass_) != VK_SUCCESS) {
        render_pass_ = VK_NULL_HANDLE;
        return false;
      }
    }

    if (pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      info.queueFamilyIndex = dispositivo->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return false;
      }
      VkCommandBufferAllocateInfo reserva{};
      reserva.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      reserva.commandPool = pool_;
      reserva.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      reserva.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &reserva, &comandos_) != VK_SUCCESS) {
        comandos_ = VK_NULL_HANDLE;
      }
      VkFenceCreateInfo info_fence{};
      info_fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (dfn.vkCreateFence(device, &info_fence, nullptr, &fence_) != VK_SUCCESS) {
        fence_ = VK_NULL_HANDLE;
      }
    }
    if (comandos_ == VK_NULL_HANDLE || fence_ == VK_NULL_HANDLE) {
      return false;
    }

    // A single command buffer: wait for the previous one before reusing it.
    if (fence_pendiente_) {
      dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
      dfn.vkResetFences(device, 1, &fence_);
      fence_pendiente_ = false;
    }
    dfn.vkResetCommandPool(device, pool_, 0);

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == contexto.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      Framebuffer& f = framebuffers_[siguiente_framebuffer_];
      siguiente_framebuffer_ = (siguiente_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView vista = contexto.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &vista;
      info.width = kSalidaAncho;
      info.height = kSalidaAlto;
      info.layers = 1;
      if (dfn.vkCreateFramebuffer(device, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = contexto.image_version();
      framebuffer = f.framebuffer;
    }

    VkCommandBufferBeginInfo inicio{};
    inicio.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    inicio.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(comandos_, &inicio) != VK_SUCCESS) {
      return false;
    }
    // Test color: green goes up and down with each Swap, so a capture
    // tells a running game from a stalled one.
    const float fase = float(swap % 240) / 239.0f;
    VkClearValue color{};
    color.color.float32[0] = 0.05f;
    color.color.float32[1] = 0.10f + 0.40f * fase;
    color.color.float32[2] = 0.35f;
    color.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pase{};
    pase.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pase.renderPass = render_pass_;
    pase.framebuffer = framebuffer;
    pase.renderArea.extent.width = kSalidaAncho;
    pase.renderArea.extent.height = kSalidaAlto;
    pase.clearValueCount = 1;
    pase.pClearValues = &color;
    dfn.vkCmdBeginRenderPass(comandos_, &pase, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdEndRenderPass(comandos_);
    if (dfn.vkEndCommandBuffer(comandos_) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo envio{};
    envio.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    envio.commandBufferCount = 1;
    envio.pCommandBuffers = &comandos_;
    {
      const auto cola = dispositivo->AcquireQueue(dispositivo->queue_family_graphics_compute(), 0);
      if (dfn.vkQueueSubmit(cola.queue(), 1, &envio, fence_) != VK_SUCCESS) {
        return false;
      }
    }
    fence_pendiente_ = true;
    contexto.SetIs8bpc(true);
    return true;
  }

  void DestruirVulkan() {
    destinos_.reset();  // uses the device: before everything else
    if (!provider_ || !provider_->vulkan_device()) {
      return;
    }
    const rex::ui::vulkan::VulkanDevice* dispositivo = provider_->vulkan_device();
    const auto& dfn = dispositivo->functions();
    const VkDevice device = dispositivo->device();
    if (fence_ != VK_NULL_HANDLE) {
      if (fence_pendiente_) {
        dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
        fence_pendiente_ = false;
      }
      dfn.vkDestroyFence(device, fence_, nullptr);
      fence_ = VK_NULL_HANDLE;
    }
    for (Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f = Framebuffer{};
      }
    }
    if (pool_ != VK_NULL_HANDLE) {
      dfn.vkDestroyCommandPool(device, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
      comandos_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
    }
  }

  // Report lines go to the report thread (NFSMW_INFORME_ANILLO, see InformeDiferido), except the
  // shutdown ones (forzar), which are written right here so they are not lost.
#define NFSMW_INFORME_SEGUN(diferir, ...) do { if (diferir) { NFSMW_INFORME_ANILLO(__VA_ARGS__); } else { REXLOG_INFO(__VA_ARGS__); } } while (0)
  void Informe(bool forzar) {
    const auto ahora = Reloj::now();
    if (!forzar &&
        ahora - ultimo_informe_ < std::chrono::seconds(REXCVAR_GET(nfsmw_nativo_informe_s))) {
      return;
    }
    const bool diferir_informe = !forzar;
    ultimo_informe_ = ahora;
    NFSMW_INFORME_SEGUN(diferir_informe,
        "[nativo] swaps={} vblanks={} paquetes={} interrupciones={} indirectos={} "
        "escrituras_memoria={} esperas_agotadas={} dibujos={} copias={}",
        swaps_.load(), vblanks_.load(), paquetes_.load(), interrupciones_.load(),
        indirectos_.load(), escrituras_memoria_.load(), esperas_agotadas_.load(), dibujos_.load(),
        copias_.load());
    // Ring thread cost since the previous report. "Anillo" includes the
    // WAIT_REG_MEM waits and the wait for the GPU when presenting.
    const double segundos = std::chrono::duration<double>(ahora - ultimo_tiempo_informe_).count();
    ultimo_tiempo_informe_ = ahora;
    if (segundos > 0.0) {
      NFSMW_INFORME_SEGUN(diferir_informe,
          "[nativo] tiempos: anillo {:.1f} ms/s; dibujos {:.1f} us/dibujo ({}); copias {:.1f} us/copia; "
          "presentar {:.2f} ms/Swap con la espera a la GPU",
          double(tiempo_anillo_ns_) / 1e6 / segundos,
          dibujos_medidos_ ? double(tiempo_dibujos_ns_) / 1e3 / double(dibujos_medidos_) : 0.0,
          dibujos_medidos_,
          copias_medidas_ ? double(tiempo_copias_ns_) / 1e3 / double(copias_medidas_) : 0.0,
          presentaciones_medidas_
              ? double(tiempo_presentar_ns_) / 1e6 / double(presentaciones_medidas_)
              : 0.0);
      // Ring thread wakeups: how many CP_RB_WPTR writes the game makes and how many iterations this
      // thread runs to serve them.
      {
        const uint64_t escrituras = escrituras_wptr_.load(std::memory_order_relaxed);
        const uint64_t d_escrituras = escrituras - escrituras_wptr_previas_;
        const uint64_t d_vueltas = vueltas_anillo_ - vueltas_anillo_previas_;
        const uint64_t d_con_datos = vueltas_anillo_con_datos_ - vueltas_anillo_con_datos_previas_;
        const uint64_t d_agotadas = esperas_anillo_agotadas_ - esperas_anillo_agotadas_previas_;
        /*
         * Core migrations go on this same line on purpose, next to the timed-out waits: both
         * measure the same thing (the ring not working when it should) and are best read
         * together. If nfsmw_nativo_anillo_nucleo at -1 and at 1 gives the same migrations/s,
         * the preferred core has no effect here.
         */
        const uint64_t d_migraciones = migraciones_anillo_ - migraciones_anillo_previas_;
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] despertares del anillo: {:.0f} escrituras de CP_RB_WPTR/s, {:.0f} vueltas/s, {:.0f} "
                    "con datos/s ({:.2f} escrituras por vuelta con datos), {:.0f} esperas de 4 ms agotadas/s; "
                    "nucleo {} con {:.0f} migraciones/s ({:.2f} por vuelta)",
                    double(d_escrituras) / segundos, double(d_vueltas) / segundos, double(d_con_datos) / segundos,
                    d_con_datos ? double(d_escrituras) / double(d_con_datos) : 0.0, double(d_agotadas) / segundos,
                    nucleo_anillo_, double(d_migraciones) / segundos,
                    d_vueltas ? double(d_migraciones) / double(d_vueltas) : 0.0);
        migraciones_anillo_previas_ = migraciones_anillo_;
        escrituras_wptr_previas_ = escrituras;
        vueltas_anillo_previas_ = vueltas_anillo_;
        vueltas_anillo_con_datos_previas_ = vueltas_anillo_con_datos_;
        esperas_anillo_agotadas_previas_ = esperas_anillo_agotadas_;
      }
      // Breakdown per packet, largest first (top 8).
      std::vector<std::tuple<uint64_t, int, uint64_t>> partes;
      if (tiempo_registros_ns_) {
        partes.emplace_back(tiempo_registros_ns_, -1, cuenta_registros_);
      }
      for (int op = 0; op < int(tiempo_opcode_ns_.size()); ++op) {
        if (tiempo_opcode_ns_[op]) {
          partes.emplace_back(tiempo_opcode_ns_[op], op, cuenta_opcode_[op]);
        }
      }
      std::sort(partes.begin(), partes.end(), std::greater<>());
      std::string reparto;
      for (size_t i = 0; i < partes.size() && i < 8; ++i) {
        const auto& [ns, op, n] = partes[i];
        reparto += op < 0 ? fmt::format(" registros {:.1f} ms/s ({})", double(ns) / 1e6 / segundos, n)
                          : fmt::format(" op {:02X} {:.1f} ms/s ({})", op,
                                        double(ns) / 1e6 / segundos, n);
      }
      NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] tiempos por paquete:{}", reparto);
      // How many register words take the block path and how many go one at a time.
      NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] registros por camino: {} palabras en bloque, {} una a una ({:.1f} palabras por paquete de "
                  "tipo 0/1)",
                  palabras_bloque_, palabras_sueltas_,
                  cuenta_registros_ ? double(palabras_bloque_ + palabras_sueltas_) / double(cuenta_registros_) : 0.0);
      palabras_bloque_ = palabras_sueltas_ = 0;
      // Phase 2 of the Direct3D-level renderer (ProcesarMarcador). Its time shows up as "op 10".
      if (marcadores_aplicados_ || marcadores_comprobados_ || marcadores_malos_) {
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] marcadores D3D (fase 2): {} aplicados ({:.1f} tramos y {:.1f} palabras cada uno; {} "
                    "tramos por el camino de siempre), {} comprobados contra sus paquetes ({} distintos), {} mal "
                    "formados; camino rapido {} ({} tramos comprobados)",
                    marcadores_aplicados_,
                    marcadores_aplicados_ ? double(marcador_tramos_) / double(marcadores_aplicados_) : 0.0,
                    marcadores_aplicados_ ? double(marcador_palabras_) / double(marcadores_aplicados_) : 0.0,
                    marcador_tramos_lentos_, marcadores_comprobados_, marcadores_distintos_, marcadores_malos_,
                    marcador_rapido_ > 0 ? "encendido" : "APAGADO", tramos_rapidos_verificados_);
        marcadores_aplicados_ = marcadores_comprobados_ = marcadores_distintos_ = marcadores_malos_ = 0;
        marcador_tramos_ = marcador_tramos_lentos_ = marcador_palabras_ = 0;
      }
      // Phase 2b: the draw record in the marker (EmparejarDibujo).
      if (dibujos_con_registro_de_marcador_ || registros_de_marcador_rechazados_ || comprobaciones_dibujo_) {
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] registro de dibujo en el marcador (fase 2b): {} dibujos sin busqueda, {} registros "
                    "del marcador no aceptados (tipo, cuenta o shaders: se busco como siempre), {} comprobados "
                    "contra la busqueda ({} distintos, {} con otro registro y los mismos shaders)",
                    dibujos_con_registro_de_marcador_, registros_de_marcador_rechazados_, comprobaciones_dibujo_,
                    comprobaciones_dibujo_distintas_, comprobaciones_dibujo_otro_registro_);
        dibujos_con_registro_de_marcador_ = registros_de_marcador_rechazados_ = 0;
        comprobaciones_dibujo_ = comprobaciones_dibujo_distintas_ = comprobaciones_dibujo_otro_registro_ = 0;
      }
      // Phase 1 of the Direct3D-level renderer.
      if (sombra_dibujos_ || sombra_perdidas_) {
        std::vector<std::pair<uint64_t, uint32_t>> peores;
        for (const auto& [reg, n] : sombra_por_registro_) {
          peores.emplace_back(n, reg);
        }
        std::sort(peores.begin(), peores.end(), std::greater<>());
        std::string lista;
        for (size_t i = 0; i < peores.size() && i < 12; ++i) {
          lista += fmt::format(" 0x{:04X}x{}", peores[i].second, peores[i].first);
        }
        size_t grupos_vistos = 0;
        for (uint32_t g = 0; g < kGruposEspejo; ++g) {
          grupos_vistos += sombra_foto_.mascara[g] != 0;
        }
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] sombra D3D: {} dibujos comparados ({} fotos perdidas; {} de {} grupos del espejo "
                    "aprendidos) | estado {} distintos de {} | fetch {} de {} | constantes {} de {} | registros "
                    "con diferencias: {}{}",
                    sombra_dibujos_, sombra_perdidas_, grupos_vistos, kGruposEspejo, sombra_estado_distintos_,
                    sombra_estado_comparados_, sombra_fetch_distintos_, sombra_fetch_comparados_,
                    sombra_constantes_distintas_, sombra_constantes_comparadas_, peores.size(),
                    lista.empty() ? "" : " (los que mas:" + lista + ")");
        sombra_dibujos_ = sombra_perdidas_ = 0;
        sombra_estado_comparados_ = sombra_estado_distintos_ = 0;
        sombra_fetch_comparados_ = sombra_fetch_distintos_ = 0;
        sombra_constantes_comparadas_ = sombra_constantes_distintas_ = 0;
        sombra_por_registro_.clear();
      }
      // What WAIT_REG_MEM waits for: the 4 with the most 1 ms iterations.
      std::vector<std::pair<uint64_t, Espera>> esperas(esperas_.begin(), esperas_.end());
      std::sort(esperas.begin(), esperas.end(), [](const auto& a, const auto& b) {
        return a.second.vueltas > b.second.vueltas;
      });
      std::string detalle_esperas;
      for (size_t i = 0; i < esperas.size() && i < 4; ++i) {
        const auto& [clave, e] = esperas[i];
        detalle_esperas += fmt::format(
            " {} {:08X} (funcion {} ref {:08X} mascara {:08X}): {} esperas, {} vueltas;",
            (clave >> 32) ? "memoria" : "registro", uint32_t(clave), e.info & 0x7, e.referencia,
            e.mascara, e.veces, e.vueltas);
      }
      if (!detalle_esperas.empty()) {
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] WAIT_REG_MEM:{}", detalle_esperas);
      }
    }
    tiempo_anillo_ns_ = tiempo_dibujos_ns_ = tiempo_copias_ns_ = tiempo_presentar_ns_ = 0;
    dibujos_medidos_ = copias_medidas_ = presentaciones_medidas_ = 0;
    tiempo_registros_ns_ = cuenta_registros_ = 0;
    tiempo_opcode_ns_.fill(0);
    cuenta_opcode_.fill(0);
    esperas_.clear();
    const uint32_t frontbuffer = swap_frontbuffer_.load();
    const uint32_t destino = ultima_copia_destino_.load();
    NFSMW_INFORME_SEGUN(diferir_informe,
        "[nativo] ultimo swap: frontbuffer={:08X} {}x{}; ultima copia: destino={:08X} "
        "control={:08X} info={:08X} pitch={:08X}; coinciden={}",
        frontbuffer, swap_ancho_.load(), swap_alto_.load(), destino, ultima_copia_control_.load(),
        ultima_copia_info_.load(), ultima_copia_pitch_.load(),
        (frontbuffer & 0x1FFFFFFF) == (destino & 0x1FFFFFFF));
    if (destinos_) {
      uint64_t copias = 0, borrados = 0, presentados = 0, rechazos = 0;
      destinos_->Estadisticas(copias, borrados, presentados, rechazos);
      uint64_t gpu_ns = 0, gpu_trabajos = 0;
      destinos_->TiempoGpu(gpu_ns, gpu_trabajos);
      const uint64_t presentados_intervalo = presentados - presentados_previos_gpu_;
      NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2: copias={} (de profundidad {}) borrados={} (de profundidad {}) "
                  "presentados={} rechazos={}; GPU {:.2f} ms por Swap ({} trabajos medidos)",
                  copias, destinos_->CopiasProfundidad(), borrados,
                  destinos_->BorradosProfundidad(), presentados, rechazos,
                  presentados_intervalo
                      ? double(gpu_ns - gpu_ns_previo_) / 1e6 / double(presentados_intervalo)
                      : 0.0,
                  gpu_trabajos - gpu_trabajos_previos_);
      // Occlusion queries since the previous report.
      {
        const auto d = [&](size_t i) { return oclusion_contadores_[i] - oclusion_contadores_previos_[i]; };
        uint64_t oc[5] = {};
        destinos_->EstadisticasOclusion(oc);
        if (d(0) || d(1) || d(2) || d(4)) {
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2 oclusion: modo {}; consultas del juego empezadas {}, terminadas con medida {} y "
                      "sin medida todavia {}, sin su final {}, fingidas fuera de pareja {}; escritas medidas: media "
                      "{:.1f}, maxima historica {}; terminadas por destino: pequeno (escala 1) {} (maxima {}), de "
                      "escena (escala del modo de AA) {} (maxima {}); del host: tramos {} (sin sitio {}), consultas "
                      "publicadas {}, muestras {} (maxima historica por consulta {})",
                      REXCVAR_GET(nfsmw_nativo_oclusion), d(0), d(1), d(2), d(3), d(4),
                      d(1) ? double(d(5)) / double(d(1)) : 0.0, oclusion_contadores_[6], oclusion_por_destino_[0],
                      oclusion_max_por_destino_[0], oclusion_por_destino_[1], oclusion_max_por_destino_[1],
                      oc[0] - oclusion_host_previo_[0], oc[1] - oclusion_host_previo_[1],
                      oc[2] - oclusion_host_previo_[2], oc[3] - oclusion_host_previo_[3], oc[4]);
        }
        oclusion_por_destino_ = {};
        oclusion_contadores_previos_ = oclusion_contadores_;
        std::copy(std::begin(oc), std::end(oc), oclusion_host_previo_);
      }
      {
        uint64_t esperas = 0, ns_esperas = 0;
        destinos_->EsperasGpu(esperas, ns_esperas);
        const double ms_esperas = double(ns_esperas - ns_esperas_gpu_previos_) / 1e6;
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2: esperas del anillo a la GPU: {} ({:.1f} ms; {:.2f} ms por Swap)",
                    esperas - esperas_gpu_previas_, ms_esperas,
                    presentados_intervalo ? ms_esperas / double(presentados_intervalo) : 0.0);
        esperas_gpu_previas_ = esperas;
        ns_esperas_gpu_previos_ = ns_esperas;
        // And the real duration of the jobs, to check it against the sum of the timestamps.
        uint64_t trabajos = 0, ns_trabajos = 0;
        destinos_->DuracionTrabajosGpu(trabajos, ns_trabajos);
        const uint64_t d_trabajos = trabajos - trabajos_gpu_previos_;
        const double ms_trabajos = double(ns_trabajos - ns_trabajo_gpu_previo_) / 1e6;
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2: trabajos de GPU por reloj de pared: {} ({:.1f} ms; {:.2f} ms cada uno; "
                    "{:.2f} ms por Swap)",
                    d_trabajos, ms_trabajos, d_trabajos ? ms_trabajos / double(d_trabajos) : 0.0,
                    presentados_intervalo ? ms_trabajos / double(presentados_intervalo) : 0.0);
        trabajos_gpu_previos_ = trabajos;
        ns_trabajo_gpu_previo_ = ns_trabajos;
      }
      {
        uint64_t coste[6] = {};
        destinos_->CosteGrabar(coste);
        uint64_t dc[6] = {};
        for (size_t i = 0; i < 6; ++i) {
          dc[i] = coste[i] - coste_grabar_previo_[i];
          coste_grabar_previo_[i] = coste[i];
        }
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2: bufers de comandos empezados {} ({:.1f} ms en Grabar, {:.1f} ms "
                    "reiniciando pools); lecturas de vuelta {} ({:.2f} M texels, {:.1f} ms "
                    "escribiendolas)",
                    dc[0], double(dc[1]) / 1e6, double(dc[2]) / 1e6, dc[3], double(dc[4]) / 1e6,
                    double(dc[5]) / 1e6);
      }
      std::array<uint64_t, kGpuCategorias> categorias{};
      destinos_->TiempoGpuPorCategoria(categorias);
      if (presentados_intervalo) {
        const auto ms = [&](uint32_t c) {
          return double(categorias[c] - gpu_categorias_previas_[c]) / 1e6 /
                 double(presentados_intervalo);
        };
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2: GPU por Swap: sombras {:.2f} ms, escena {:.2f}, reflejo {:.2f}, "
                    "320 (cubo y desenfoque) {:.2f}, menores {:.2f}, copias {:.2f}, borrados {:.2f}, "
                    "resto {:.2f}, escena sin profundidad {:.2f}, hueco entre trabajos {:.2f}",
                    ms(kGpuSombras), ms(kGpuEscena), ms(kGpuReflejo), ms(kGpu320),
                    ms(kGpuMenores), ms(kGpuCopias), ms(kGpuBorrados), ms(kGpuOtros),
                    ms(kGpuEscenaSinProfundidad), ms(kGpuHuecoEntreTrabajos));
      }
      gpu_categorias_previas_ = categorias;
      // The size of the remaining copies.
      if (presentados_intervalo) {
        std::array<uint64_t, 4> copias_t{}, pixeles_t{};
        destinos_->CopiasPorTamano(copias_t, pixeles_t);
        static constexpr const char* kCubetas[] = {"<=64x64", "<=320x320", "<=1024x1024", "mayores"};
        std::string linea;
        for (uint32_t c = 0; c < 4; ++c) {
          const uint64_t n = copias_t[c] - copias_cubeta_previas_[c];
          const uint64_t px = pixeles_t[c] - pixeles_cubeta_previos_[c];
          if (n) {
            linea += fmt::format(" {} {:.1f} copias y {:.2f} Mpixeles", kCubetas[c],
                                 double(n) / double(presentados_intervalo),
                                 double(px) / 1e6 / double(presentados_intervalo));
          }
        }
        if (!linea.empty()) {
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2 copias por fotograma y tamano:{}", linea);
        }
        copias_cubeta_previas_ = copias_t;
        pixeles_cubeta_previos_ = pixeles_t;
      }
      // Actual fragments and vertices of each pass, to separate per-pixel cost from geometry cost.
      // The counts do not depend on the machine: what is measured on the PC holds for the console.
      {
        std::array<uint64_t, kGpuCategorias> frag{}, vert{}, prim{};
        destinos_->EstadisticasPipeline(frag, vert, prim);
        static constexpr const char* kTiposEstad[] = {"resto",  "sombras", "escena",  "reflejo",
                                                      "cubo",   "menores", "copias",  "borrados",
                                                      "escena sin profundidad", "hueco"};
        std::string linea;
        for (uint32_t c = 0; presentados_intervalo && c < kGpuCategorias; ++c) {
          const uint64_t f = frag[c] - fragmentos_categoria_previos_[c];
          const uint64_t v = vert[c] - vertices_categoria_previos_[c];
          const uint64_t p = prim[c] - primitivas_categoria_previas_[c];
          if (f || v) {
            linea += fmt::format(" {} {:.2f} M fragmentos, {:.0f} k vertices, {:.0f} k primitivas;",
                                 kTiposEstad[c], double(f) / 1e6 / double(presentados_intervalo),
                                 double(v) / 1e3 / double(presentados_intervalo),
                                 double(p) / 1e3 / double(presentados_intervalo));
          }
        }
        if (!linea.empty()) {
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2 por fotograma y tipo de pasada:{}", linea);
        }
        fragmentos_categoria_previos_ = frag;
        vertices_categoria_previos_ = vert;
        primitivas_categoria_previas_ = prim;
      }
      // Which pixel shaders fill the screen, from the diagnostic frames
      // (nfsmw_nativo_estadisticas_por_dibujo_s). Accumulated from the start, not per interval.
      {
        std::vector<uint64_t> frag_ps, dib_ps;
        uint64_t fotogramas = 0;
        destinos_->EstadisticasPorShader(frag_ps, dib_ps, fotogramas);
        if (fotogramas > fotogramas_diagnostico_previos_) {
          fotogramas_diagnostico_previos_ = fotogramas;
          static constexpr const char* kTiposPs[] = {"resto",  "sombras", "escena",  "reflejo",
                                                     "cubo",   "menores", "copias",  "borrados",
                                                     "escena sin profundidad", "hueco"};
          const size_t por_categoria = frag_ps.size() / kGpuCategorias;
          for (uint32_t c = 0; c < kGpuCategorias; ++c) {
            uint64_t total = 0;
            std::vector<uint32_t> orden;
            for (size_t i = 0; i < por_categoria; ++i) {
              const size_t e = size_t(c) * por_categoria + i;
              total += frag_ps[e];
              if (frag_ps[e]) {
                orden.push_back(uint32_t(e));
              }
            }
            uint64_t dibujos = 0;
            for (size_t i = 0; i < por_categoria; ++i) {
              dibujos += dib_ps[size_t(c) * por_categoria + i];
            }
            if (!total && !dibujos) {
              continue;  // that category was not measured
            }
            std::sort(orden.begin(), orden.end(),
                      [&](uint32_t a, uint32_t b) { return frag_ps[a] > frag_ps[b]; });
            std::string linea;
            for (size_t i = 0; i < orden.size() && i < 8; ++i) {
              const uint32_t e = orden[i];
              const uint32_t ps = uint32_t(e % por_categoria);
              linea += fmt::format(" PS {} {:.2f} M ({:.0f} %, {:.1f} dibujos)",
                                   ps ? fmt::format("n{}", ps - 1) : std::string("sin"),
                                   double(frag_ps[e]) / 1e6 / double(fotogramas),
                                   100.0 * double(frag_ps[e]) / double(total),
                                   double(dib_ps[e]) / double(fotogramas));
            }
            NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2 fragmentos de {} por pixel shader ({} fotogramas, {:.2f} M y {:.0f} "
                        "dibujos por fotograma):{}",
                        kTiposPs[c], fotogramas, double(total) / 1e6 / double(fotogramas),
                        double(dibujos) / double(fotogramas), linea);
          }
        }
      }
      // Real scale of the GPU timestamps. Between two reports, the end timestamp of the last timed job
      // must advance as much as the clock (with a lag of one or two frames at each end). On the console
      // NVK declares 1 ns per unit and the ratio comes out at 1.628 in every interval measured, menus
      // included: the GPU ms in the reports must be multiplied by the accumulated scale.
      {
        const uint64_t marca = destinos_->MarcaGpuFinalNs();
        const Reloj::time_point ahora = Reloj::now();
        if (marca != 0 && marca != marca_gpu_previa_ns_) {
          if (marca_gpu_previa_ns_ != 0 && marca > marca_gpu_previa_ns_) {
            const double real_ns = double(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ahora - reloj_marca_previo_).count());
            const double avance_ns = double(marca - marca_gpu_previa_ns_);
            escala_real_ns_ += real_ns;
            escala_marcas_ns_ += avance_ns;
            const double escala = escala_real_ns_ / escala_marcas_ns_;
            NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2: escala de las marcas de GPU {:.3f} en este intervalo ({:.3f} acumulada; tiempo "
                        "real entre informes / avance de las marcas); GPU real por Swap {:.2f} ms",
                        real_ns / avance_ns, escala,
                        presentados_intervalo
                            ? double(gpu_ns - gpu_ns_previo_) / 1e6 / double(presentados_intervalo) * escala
                            : 0.0);
          }
          marca_gpu_previa_ns_ = marca;
          reloj_marca_previo_ = ahora;
        }
      }
      // Mode of the intermediate timestamps of the jobs timed in the interval.
      {
        const uint64_t precisos = destinos_->TrabajosMarcasPrecisas();
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2: marcas precisas en {} de {} trabajos medidos", precisos - trabajos_precisos_previos_,
                    gpu_trabajos - gpu_trabajos_previos_);
        trabajos_precisos_previos_ = precisos;
      }
      // Intervals between Swaps and GPU overlaps.
      {
        std::array<uint64_t, kCubetasSwap> cubetas{};
        uint64_t solapes = 0;
        double peor_ms = 0.0;
        destinos_->IntervalosEntreSwaps(cubetas, solapes, peor_ms);
        uint64_t total = 0;
        for (uint32_t i = 0; i < kCubetasSwap; ++i) {
          total += cubetas[i] - cubetas_swap_previas_[i];
        }
        if (total) {
          const auto c = [&](uint32_t i) { return cubetas[i] - cubetas_swap_previas_[i]; };
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2: intervalos entre Swaps: <15 ms {} | 15-18 {} | 18-25 {} | 25-30 {} | 30-36 {} | "
                      "36-50 {} | 50-60 {} | 60-75 {} | 75-100 {} | 100-150 {} | >=150 {}; "
                      "peor fotograma {:.1f} ms; trabajos solapados en la GPU {}",
                      c(0), c(1), c(2), c(3), c(4), c(5), c(6), c(7), c(8), c(9), c(10), peor_ms,
                      solapes - solapes_gpu_previos_);
        }
        cubetas_swap_previas_ = cubetas;
        nfsmw::guardia30::Informe();  // which step the 30 FPS guard was on
        {
          // How much of the game's constant dump is redundant.
          static uint64_t esc_prev = 0, sin_prev = 0, blo_prev = 0, blosin_prev = 0;
          const uint64_t esc = constantes_escritas_ - esc_prev;
          const uint64_t sin_c = constantes_sin_cambio_ - sin_prev;
          const uint64_t blo = bloques_constantes_ - blo_prev;
          const uint64_t blosin = bloques_constantes_sin_cambio_ - blosin_prev;
          esc_prev = constantes_escritas_; sin_prev = constantes_sin_cambio_;
          blo_prev = bloques_constantes_; blosin_prev = bloques_constantes_sin_cambio_;
          if (esc > 0 || blo > 0) {
            NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 constantes del juego: {} escrituras sueltas ({} sin cambio, {:.1f} %) | "
                        "{} bloques ({} sin cambio, {:.1f} %)",
                        esc, sin_c, esc ? 100.0 * double(sin_c) / double(esc) : 0.0,
                        blo, blosin, blo ? 100.0 * double(blosin) / double(blo) : 0.0);
          }
        }
        {
          /*
           * How often the fetch constants and the viewport state really change. This
           * measure tells how much the translator can skip: if changes per frame are far
           * fewer than draws per frame, the "texturas" stage and the viewport/scissor
           * are almost entirely repeated work.
           */
          static uint64_t fe_prev = 0, fs_prev = 0, fg_prev = 0;
          static uint64_t ee_prev = 0, es_prev = 0, eg_prev = 0, dib_prev = 0;
          const uint64_t fe = fetch_escritas_ - fe_prev, fs = fetch_sin_cambio_ - fs_prev;
          const uint64_t fg = generacion_fetch_ - fg_prev;
          const uint64_t ee = encuadre_escritas_ - ee_prev, es = encuadre_sin_cambio_ - es_prev;
          const uint64_t eg = generacion_encuadre_ - eg_prev;
          // dibujos_medidos_ was already reset above in this same report: the right count is
          // the ring's accumulated counter.
          const uint64_t dib_total = dibujos_.load(std::memory_order_relaxed);
          const uint64_t dib = dib_total - dib_prev;
          fe_prev = fetch_escritas_; fs_prev = fetch_sin_cambio_; fg_prev = generacion_fetch_;
          ee_prev = encuadre_escritas_; es_prev = encuadre_sin_cambio_; eg_prev = generacion_encuadre_;
          dib_prev = dib_total;
          if (fe > 0 || ee > 0) {
            NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 generaciones: fetch {} escrituras ({} sin cambio), {} cambios "
                        "({:.2f} por dibujo) | encuadre {} escrituras ({} sin cambio), {} cambios "
                        "({:.2f} por dibujo); dibujos {}",
                        fe, fs, fg, dib ? double(fg) / double(dib) : 0.0,
                        ee, es, eg, dib ? double(eg) / double(dib) : 0.0, dib);
          }
        }
        solapes_gpu_previos_ = solapes;
      }
      // Breakdown of the time spent in Presentar.
      {
        uint64_t c[12] = {};
        destinos_->CostePresentar(c);
        const auto d = [&](int i) { return c[i] - coste_presentar_previo_[i]; };
        const auto media = [&](int ns, int veces) { return d(veces) ? double(d(ns)) / 1e6 / double(d(veces)) : 0.0; };
        if (d(0) || d(2) || d(5) || d(8)) {
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2: presentar (ms por vez): espera a la salida anterior {:.2f} ({}); envio del trabajo: "
                      "candado de la cola {:.2f}, vkQueueSubmit {:.2f} ({}); envio de la salida: candado {:.2f}, "
                      "vkQueueSubmit {:.2f} ({}); RefreshGuestOutput: antes de la llamada {:.2f}, dentro {:.2f}, "
                      "despues {:.2f} ({})",
                      media(1, 0), d(0), media(3, 2), media(4, 2), d(2), media(6, 5), media(7, 5), d(5),
                      media(9, 8), media(10, 8), media(11, 8), d(8));
        }
        std::copy(c, c + 12, coste_presentar_previo_);
      }
      gpu_ns_previo_ = gpu_ns;
      gpu_trabajos_previos_ = gpu_trabajos;
      presentados_previos_gpu_ = presentados;
      const EstadisticasDibujos d = destinos_->EstadisticasDeDibujos();
      std::string causas;
      for (const auto& [causa, n] : d.causas) {
        causas += fmt::format(" {}={}", causa, n);
      }
      // Per-stage cost of the draws recorded since the previous report, measured on the timed ones.
      const uint64_t grabados = d.dibujados - dibujados_previos_;
      const uint64_t cronometrados = d.dibujados_cronometrados - cronometrados_previos_;
      if (grabados && cronometrados) {
        static constexpr const char* kEtapas[] = {"estado",  "indices",  "texturas",
                                                  "pase",    "subidas",  "pipeline",
                                                  "grabar",  "(de la etapa pase, el cambio de pase)",
                                                  "[del cambio de pase, cerrar el anterior]",
                                                  "[destinos]", "[render pass y framebuffer]",
                                                  "[abrir el pase]"};
        std::string etapas;
        for (size_t i = 0; i < d.etapas_ns.size(); ++i) {
          etapas += fmt::format(" {} {:.1f}", kEtapas[i],
                                double(d.etapas_ns[i] - etapas_previas_[i]) / 1e3 / double(cronometrados));
        }
        /*
         * The divisor is the number of recorded draws, not the number of incoming draws.
         * `dibujados_cronometrados_` is incremented in nfsmw_nativo_dibujos.cpp:2440, right after
         * vkCmdDraw/vkCmdDrawIndexed: it only counts recorded draws, never rejected ones.
         * Arithmetic check on a log (1 in 8 timed): sample 107,457 and recorded 859,614, and
         * 859,614/8 = 107,452 (exact). With the incoming draws it would have been 147,852.
         *
         * In practice: to turn these us/draw into ms per frame, multiply by the recorded draws
         * (~1,395/frame), not by the incoming ones (~1,957/frame). Getting it wrong inflates every
         * stage by 40 %. Also, these samples pay for 12 clock reads, so they come out ~3 us above
         * "dibujos us/dibujo".
         */
        // The ratio comes from the counts themselves (it used to be a hard-coded 8; the stage timer
        // now runs on 1 of every 64 draws).
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 etapas (us por dibujo GRABADO, muestra de {} = 1 de cada {:.0f} grabados; "
                    "grabados {}):{}",
                    cronometrados, double(grabados) / double(cronometrados), grabados, etapas);
      }
      // The scenery LOD hook. If "forzadas" does not resemble "que el juego dibuja", the hook is
      // not reaching the objects and the popping will remain (this has happened before).
      NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 {}", nfsmw::escenario_lod::Resumen());
      // The early vegetation rejection has its own counter. Without this line there is no way to
      // check whether the shortcut works (the number would have to be deduced by subtracting
      // recorded draws from incoming ones).
      {
        const uint64_t pronto = d.vegetacion_pronto - vegetacion_pronto_previa_;
        vegetacion_pronto_previa_ = d.vegetacion_pronto;
        if (presentados_intervalo) {
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 vegetacion de sombras tirada pronto: {:.0f} por fotograma ({} en el "
                      "intervalo); se ahorran indices, texturas, subida y pase de cada uno",
                      double(pronto) / double(presentados_intervalo), pronto);
        }
      }
      {
        // How much of the scene forces shading before the depth test.
        const uint64_t con = d.escena_con_descarte, sin = d.escena_sin_descarte;
        const uint64_t dcon = con - descarte_con_previo_, dsin = sin - descarte_sin_previo_;
        descarte_con_previo_ = con;
        descarte_sin_previo_ = sin;
        if (dcon + dsin) {
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 descarte temprano en la escena: {} dibujos lo permiten, {} lo impiden "
                      "(prueba de alfa, kill o profundidad) = {:.0f} %",
                      dsin, dcon, 100.0 * double(dcon) / double(dcon + dsin));
        }
      }
      dibujados_previos_ = d.dibujados;
      cronometrados_previos_ = d.dibujados_cronometrados;
      etapas_previas_ = d.etapas_ns;
      {
        const std::array<uint64_t, 20> contadores = {d.pases, d.envios_llenos,
                                                      d.ns_envios_llenos, d.bytes_vertices,
                                                      d.bytes_indices, d.samplers,
                                                      d.samplers_cache, d.ns_pases,
                                                      d.ns_vertices, d.entradas_calculadas,
                                                      d.ns_entradas, d.entradas_reutilizadas,
                                                      d.pases_por_generacion, d.pases_por_destino,
                                                      d.pases_reanudados, d.ns_render_pass,
                                                      d.bytes_repetidos_fotograma,
                                                      d.bytes_iguales_anterior, d.ns_hash_vertices,
                                                      d.texels_pases};
        // Deduplication of vertex uploads within the frame.
        const std::array<uint64_t, 3> dedupe = {d.dedupe_aciertos, d.dedupe_bytes,
                                                d.dedupe_colisiones};
        const auto ddelta = [&](size_t i) { return dedupe[i] - dedupe_previos_[i]; };
        const auto delta = [&](size_t i) { return contadores[i] - contadores_previos_[i]; };
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 contadores: pases {} envios por subida llena {} ({:.1f} ms) "
                    "vertices {:.1f} MB indices {:.1f} MB samplers {} (cache {}); "
                    "{:.1f} ms en cambios de pase ({:.1f} us cada uno), {:.1f} ms copiando vertices",
                    delta(0), delta(1), double(delta(2)) / 1e6, double(delta(3)) / 1048576.0,
                    double(delta(4)) / 1048576.0, delta(5), delta(6), double(delta(7)) / 1e6,
                    delta(0) ? double(delta(7)) / 1e3 / double(delta(0)) : 0.0,
                    cronometrados ? double(delta(8)) / 1e6 * double(grabados) / double(cronometrados) : 0.0);
        if (presentados_intervalo) {
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 vertices sin repetir: {} enlaces reaprovechados, {:.2f} MB que "
                      "NO se han copiado ({:.2f} MB por fotograma, {:.0f} % de los subidos){}",
                      ddelta(0), double(ddelta(1)) / 1048576.0,
                      double(ddelta(1)) / 1048576.0 / double(presentados_intervalo),
                      delta(3) + ddelta(1)
                          ? 100.0 * double(ddelta(1)) / double(delta(3) + ddelta(1))
                          : 0.0,
                      ddelta(2) ? fmt::format("; *** {} COLISIONES: la tabla se queda corta ***",
                                              ddelta(2))
                                : "");
        }
        dedupe_previos_ = dedupe;
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 contadores: entradas de vertices calculadas {} ({:.1f} ms, "
                    "{:.1f} us cada una), reutilizadas {}",
                    delta(9), double(delta(10)) / 1e6,
                    delta(9) ? double(delta(10)) / 1e3 / double(delta(9)) : 0.0, delta(11));
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 pases: por bufer de comandos nuevo {}, por destinos {}, "
                    "reanudados tras copia o borrado {}; {:.1f} ms en vkCmdBegin/EndRenderPass; "
                    "area abierta {:.1f} Mtexels ({:.2f} Mtexels por fotograma)",
                    delta(12), delta(13), delta(14), double(delta(15)) / 1e6,
                    double(delta(19)) / 1e6,
                    presentados_intervalo ? double(delta(19)) / 1e6 / double(presentados_intervalo)
                                          : 0.0);
        if (presentados_intervalo) {
          static constexpr const char* kTipos[] = {"resto",  "sombras", "escena",  "reflejo",
                                                   "cubo",   "menores", "copias",  "borrados",
                                                   "escena sin profundidad", "hueco"};
          std::string area;
          for (uint32_t c = 0; c < kGpuCategorias; ++c) {
            const uint64_t t = d.texels_por_categoria[c] - texels_categoria_previos_[c];
            const uint64_t n = d.pases_por_categoria[c] - pases_categoria_previos_[c];
            if (!t && !n) {
              continue;
            }
            const uint64_t dib = d.dibujos_por_categoria[c] - dibujos_categoria_previos_[c];
            const uint64_t tri = d.triangulos_por_categoria[c] - triangulos_categoria_previos_[c];
            area += fmt::format(" {} {:.2f} Mtexels en {:.1f} pases ({:.0f} dibujos, {:.0f} k triangulos)",
                                kTipos[c], double(t) / 1e6 / double(presentados_intervalo),
                                double(n) / double(presentados_intervalo),
                                double(dib) / double(presentados_intervalo),
                                double(tri) / 1e3 / double(presentados_intervalo));
          }
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 area abierta por fotograma y tipo de destino:{}", area);
        }
        if (presentados_intervalo) {
          // Draws without color, with and without a required pixel shader.
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 dibujos sin color por fotograma: {:.0f} con el pixel shader de sobra, "
                      "{:.0f} que lo necesitan (prueba de alfa, kill o profundidad); en el mapa de sombras "
                      "{:.0f} con la prueba de alfa puesta y {:.0f} sin ella",
                      double(d.dibujos_ps_inutil - dibujos_ps_inutil_previos_) /
                          double(presentados_intervalo),
                      double(d.dibujos_ps_necesario - dibujos_ps_necesario_previos_) /
                          double(presentados_intervalo),
                      double(d.sombras_alfa_activa - sombras_alfa_activa_previa_) /
                          double(presentados_intervalo),
                      double(d.sombras_alfa_apagada - sombras_alfa_apagada_previa_) /
                          double(presentados_intervalo));
        }
        sombras_alfa_activa_previa_ = d.sombras_alfa_activa;
        sombras_alfa_apagada_previa_ = d.sombras_alfa_apagada;
        dibujos_ps_inutil_previos_ = d.dibujos_ps_inutil;
        dibujos_ps_necesario_previos_ = d.dibujos_ps_necesario;
        texels_categoria_previos_ = d.texels_por_categoria;
        pases_categoria_previos_ = d.pases_por_categoria;
        dibujos_categoria_previos_ = d.dibujos_por_categoria;
        triangulos_categoria_previos_ = d.triangulos_por_categoria;
        // Which constants mode each interval used (test cvar nfsmw_nativo_constantes_ubo_alternar_s).
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 constantes por UBO: {} de {} envios", d.envios_ubo - envios_ubo_previos_,
                    d.envios - envios_previos_);
        {
          const uint64_t miradas = d.compartidas_miradas - compartidas_miradas_previas_;
          const uint64_t cambiadas = d.compartidas_cambiadas - compartidas_cambiadas_previas_;
          compartidas_miradas_previas_ = d.compartidas_miradas;
          compartidas_cambiadas_previas_ = d.compartidas_cambiadas;
          if (miradas > 0) {
            NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 constantes compartidas: {} de {} dibujos cambian los 488 bytes "
                        "({:.1f} %); los otros solo pagan el memcmp",
                        cambiadas, miradas, 100.0 * double(cambiadas) / double(miradas));
          }
        }
        envios_ubo_previos_ = d.envios_ubo;
        envios_previos_ = d.envios;
        // Diagnostic: whether the game has touched the gamma ramp since the previous report.
        {
          uint64_t total = 0;
          for (uint64_t n : escrituras_rampa_) {
            total += n;
          }
          if (total != escrituras_rampa_previas_) {
            escrituras_rampa_previas_ = total;
            uint32_t distintas = 0;
            for (uint32_t i = 0; i < 256; ++i) {
              const uint16_t identidad = uint16_t(i * 0x3FF / 0xFF);
              if (rampa_gamma_[i][0] != identidad || rampa_gamma_[i][1] != identidad ||
                  rampa_gamma_[i][2] != identidad) {
                ++distintas;
              }
            }
            const auto muestra = [&](uint32_t i) {
              return fmt::format("[{}]={}/{}/{}", i, rampa_gamma_[i][0], rampa_gamma_[i][1], rampa_gamma_[i][2]);
            };
            NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2 rampa de gamma: escrituras modo {} indice {} secuencial {} PWL {} 30 bits {} "
                        "1926 {} mascara {} (vale {}); entradas de la tabla de 256 distintas de la identidad {}; "
                        "{} {} {} {} {} (identidad: i*1023/255); version {}",
                        escrituras_rampa_[0], escrituras_rampa_[1], escrituras_rampa_[2], escrituras_rampa_[3],
                        escrituras_rampa_[4], escrituras_rampa_[5], escrituras_rampa_[6], mascara_rampa_, distintas,
                        muestra(0), muestra(64), muestra(128), muestra(192), muestra(255), version_rampa_);
            // The whole table, to compare the image with the Xbox 360 one outside the game.
            bool canales_iguales = true;
            for (const auto& entrada : rampa_gamma_) {
              canales_iguales = canales_iguales && entrada[0] == entrada[1] && entrada[0] == entrada[2];
            }
            static constexpr const char* kNombresCanal[3] = {"roja", "verde", "azul"};
            for (uint32_t c = 0; c < (canales_iguales ? 1u : 3u); ++c) {
              std::string lista;
              lista.reserve(256 * 5);
              for (uint32_t i = 0; i < 256; ++i) {
                lista += fmt::format("{}{}", i ? " " : "", rampa_gamma_[i][c]);
              }
              NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C2 rampa de gamma, tabla {}: {}",
                          canales_iguales ? "de los tres canales" : kNombresCanal[c], lista);
            }
          }
        }
        if (delta(16) || delta(17)) {
          NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6 vertices repetidos: {:.1f} MB copiados; {:.1f} MB repetidos en el "
                      "mismo fotograma y {:.1f} MB iguales a un fotograma anterior; {:.1f} ms en hashes",
                      double(delta(3)) / 1048576.0, double(delta(16)) / 1048576.0,
                      double(delta(17)) / 1048576.0, double(delta(18)) / 1e6);
        }
        contadores_previos_ = contadores;
      }
      NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C6: dibujados={} rechazados={} pipelines={} ({} ms creando) "
                  "texturas={} ({} MB) subidas de textura={} MB subidos={}; rechazos por "
                  "causa:{}",
                  d.dibujados, d.rechazados, d.pipelines, d.ms_pipelines, d.texturas,
                  d.megas_texturas, d.subidas_textura, d.megas_subidos, causas);
    }
    if (shaders_.cargada()) {
      const EstadisticasShaders e = shaders_.Estadisticas();
      NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C5a: cargas={} distintos={} identificados={} sin_identificar={} "
                  "ambiguos={}; dibujos con VS y PS={} sin VS={} sin PS={}; pares VS/PS={}",
                  e.cargas + cargas_cacheadas_, e.distintos, e.identificados, e.sin_identificar, e.ambiguos,
                  dibujos_identificados_, dibujos_sin_vs_, dibujos_sin_ps_, pares_.size());
#if defined(NFSC_RECOMP)
      {
        std::vector<std::pair<uint64_t, DetalleSinVs>> lista(sin_vs_detalle_.begin(), sin_vs_detalle_.end());
        std::sort(lista.begin(), lista.end(),
                  [](const auto& a, const auto& b) { return a.second.dibujos > b.second.dibujos; });
        std::string texto;
        for (size_t i = 0; i < lista.size() && i < 8; ++i) {
          const DetalleSinVs& d = lista[i].second;
          texto += fmt::format(" VS {:016X} ({} palabras): {} dibujos, PS n{}, tipo {}, cuenta {};", lista[i].first,
                               d.palabras, d.dibujos, d.ps, d.tipo, d.cuenta);
        }
        NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C5a dibujos sin VS por microcodigo:{}", texto);
      }
#endif
      // IM_LOAD without memcmp (CargarShaderCacheado).
      {
        const uint64_t sin_memcmp = im_i_sin_memcmp_;
        const uint64_t con_memcmp = im_i_con_memcmp_;
        NFSMW_INFORME_SEGUN(diferir_informe,
                            "[nativo] C5a IM_LOAD sin memcmp (build 184): fase {}; en el intervalo {} cargas sin leer "
                            "el microcodigo ({:.1f} %) y {} con memcmp ({} comparadas con el atajo e iguales, {} por "
                            "una escritura en su ranura, {} sin apuntar por una escritura a medias); comprobadas "
                            "para aplicar {} de {}{} | escrituras avisadas desde el arranque: {} parches en su sitio, "
                            "{} en la copia del anillo, {} otros, {} creaciones",
                            NombreFaseImAtajo(), sin_memcmp,
                            sin_memcmp + con_memcmp
                                ? 100.0 * double(sin_memcmp) / double(sin_memcmp + con_memcmp)
                                : 0.0,
                            con_memcmp, im_i_comprobadas_, im_i_version_cambiada_, im_i_anuladas_,
                            std::min(im_atajo_comprobadas_, kImAtajoAMirar), kImAtajoAMirar,
                            im_atajo_fase_ == kImAtajoMirando && im_atajo_comprobadas_ >= kImAtajoAMirar
                                ? " (sin ningun parche en su sitio visto: el gancho de sub_825A2FB8 no corre)"
                                : "",
                            microcodigo::g_parches_en_su_sitio.load(std::memory_order_relaxed),
                            microcodigo::g_parches_en_copia.load(std::memory_order_relaxed),
                            microcodigo::g_parches_otros.load(std::memory_order_relaxed),
                            microcodigo::g_creaciones.load(std::memory_order_relaxed));
        im_i_sin_memcmp_ = im_i_con_memcmp_ = im_i_comprobadas_ = im_i_version_cambiada_ = im_i_anuladas_ = 0;
      }
      // IM_LOAD_IMMEDIATE with an exact cache (CargarInmediato).
      if (inm_fase_ != kInmSinEmpezar) {
        const uint64_t total =
            inm_i_aciertos_ + inm_i_comprobadas_ + inm_i_fallos_ + inm_i_vuelta_ + inm_i_sin_cache_;
        uint32_t ocupadas = 0;
        for (const auto& por_tipo : inm_cache_) {
          for (const auto& conjunto : por_tipo) {
            for (const CargaInmediata& via : conjunto) {
              ocupadas += via.firma != 0 ? 1 : 0;
            }
          }
        }
        NFSMW_INFORME_SEGUN(diferir_informe,
                            "[nativo] C5a IM_LOAD_IMMEDIATE con cache (build 184): fase {}; en el intervalo {} "
                            "cargas: {} aciertos sin girar ni identificar ({:.1f} %), {} aciertos comprobados contra "
                            "el camino de siempre, {} fallos (vias nuevas), {} por dar la vuelta al anillo y {} sin "
                            "cache; vias ocupadas {} de {}; aciertos comprobados desde el arranque {}",
                            NombreFaseInmediato(), total, inm_i_aciertos_,
                            total ? 100.0 * double(inm_i_aciertos_) / double(total) : 0.0, inm_i_comprobadas_,
                            inm_i_fallos_, inm_i_vuelta_, inm_i_sin_cache_, ocupadas,
                            2 * kInmVias * (uint32_t(1) << kInmBitsConjunto), inm_comprobadas_);
        inm_i_aciertos_ = inm_i_comprobadas_ = inm_i_fallos_ = inm_i_vuelta_ = inm_i_sin_cache_ = 0;
      }
      // Measurement only: fences with pending vertex copies (AnotarVallaCopias).
      NFSMW_INFORME_SEGUN(diferir_informe,
                          "[nativo] C6 vallas con copias de vertices pendientes (build 185, solo medida): {} de {} "
                          "vallas e interrupciones en el intervalo (como mucho {} copias pendientes)",
                          vallas_con_copias_, vallas_total_, vallas_copias_max_);
      vallas_con_copias_ = vallas_total_ = 0;
      vallas_copias_max_ = 0;
      // Measurement only (AnotarJuegoPorDelante).
      NFSMW_INFORME_SEGUN(diferir_informe,
                          "[nativo] C6 juego por delante del anillo (build 186, solo medida): al terminar el "
                          "anillo un fotograma, el juego llevaba 0 Swaps mas en {}, 1 en {} y 2 o mas en {} "
                          "(como mucho {}; diferencia del primer fotograma {})",
                          juego_delante_[0], juego_delante_[1], juego_delante_[2], juego_delante_max_,
                          juego_delante_primera_);
      juego_delante_[0] = juego_delante_[1] = juego_delante_[2] = 0;
      juego_delante_max_ = 0;
      const EstadisticasGanchos g = EstadisticasDeGanchos();
      NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] C5b: shaders creados VS={} (conocidos {}) PS={} (conocidos {}); "
                  "dibujos emparejados={} sin registro={} registros saltados={} perdidos={}; "
                  "con VS={} con PS={}; registros con shaders distintos de los IM_LOAD={} "
                  "(dibujos que se quedan sin registro por eso={}); dibujos con la identidad "
                  "del anillo={}; variantes VS={} "
                  "(longitud distinta {}, elementos sin fetch {})",
                  g.creados_vs, g.conocidos_vs, g.creados_ps, g.conocidos_ps,
                  dibujos_emparejados_, dibujos_sin_registro_, registros_saltados_, g.perdidos,
                  dibujos_con_vs_, dibujos_con_ps_, candidatos_incoherentes_,
                  dibujos_incoherentes_, dibujos_por_im_load_, variantes_vistas_.size(),
                  variantes_longitud_distinta_, elementos_sin_fetch_);
    }
    if (!histograma_anotado_ && swaps_.load() >= 60) {
      histograma_anotado_ = true;
      std::string lineas;
      for (uint32_t op = 0; op < opcodes_.size(); ++op) {
        const uint64_t n = opcodes_[op].load();
        if (n) {
          lineas += fmt::format(" {:02X}={}", op, n);
        }
      }
      NFSMW_INFORME_SEGUN(diferir_informe, "[nativo] paquetes tipo 3 por opcode (hex=cuenta):{}", lineas);
    }
  }

#undef NFSMW_INFORME_SEGUN

  struct Framebuffer {
    uint64_t version = UINT64_MAX;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;

  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  bool mmio_registrado_ = false;

  std::vector<uint32_t> registros_;
  std::vector<std::atomic<uint8_t>> vistos_;

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_datos_{0};
  std::atomic<uint32_t> anillo_base_{0};
  std::atomic<uint32_t> anillo_palabras_{0};
  std::atomic<uint32_t> generacion_anillo_{0};
  std::atomic<uint32_t> lectura_devuelta_{0};
  std::atomic<uint32_t> puntero_escritura_{0};
  // Ring wakeups: the game thread counts the writes; the ring thread counts the rest.
  std::atomic<uint64_t> escrituras_wptr_{0};
  uint64_t escrituras_wptr_previas_ = 0;
  // Which core the ring thread runs on and how many times it has been moved to another one.
  int nucleo_anillo_ = -1;
  uint64_t migraciones_anillo_ = 0;
  uint64_t migraciones_anillo_previas_ = 0;
  uint64_t vueltas_anillo_ = 0;
  uint64_t vueltas_anillo_previas_ = 0;
  uint64_t vueltas_anillo_con_datos_ = 0;
  uint64_t vueltas_anillo_con_datos_previas_ = 0;
  uint64_t esperas_anillo_agotadas_ = 0;
  uint64_t esperas_anillo_agotadas_previas_ = 0;
  std::atomic<uint32_t> contador_{0};
  uint64_t bin_mask_ = 0xFFFFFFFFull;
  uint64_t bin_select_ = 0xFFFFFFFFull;

  std::atomic<bool> activo_{false};
  std::mutex anillo_mutex_;
  std::condition_variable anillo_cv_;
  rex::system::object_ref<rex::system::XHostThread> hilo_vblank_;
  rex::system::object_ref<rex::system::XHostThread> hilo_anillo_;

  std::atomic<uint64_t> swaps_{0};
  std::atomic<uint64_t> vblanks_{0};
  std::atomic<uint64_t> paquetes_{0};
  std::atomic<uint64_t> interrupciones_{0};
  uint64_t vallas_total_ = 0;       // AnotarVallaCopias, ring thread only
  uint64_t vallas_con_copias_ = 0;
  size_t vallas_copias_max_ = 0;
  uint64_t juego_delante_[3] = {};  // AnotarJuegoPorDelante, ring thread only
  int64_t juego_delante_max_ = 0;
  int64_t juego_delante_primera_ = 0;
  bool juego_delante_primera_valida_ = false;
  std::atomic<uint64_t> indirectos_{0};
  std::atomic<uint64_t> escrituras_memoria_{0};
  std::atomic<uint64_t> esperas_agotadas_{0};
  std::atomic<uint64_t> dibujos_{0};
  std::atomic<uint64_t> copias_{0};
  std::atomic<uint32_t> ultima_copia_destino_{0};
  std::atomic<uint32_t> ultima_copia_control_{0};
  std::atomic<uint32_t> ultima_copia_info_{0};
  std::atomic<uint32_t> ultima_copia_pitch_{0};
  std::atomic<uint32_t> swap_frontbuffer_{0};
  std::atomic<uint32_t> swap_ancho_{0};
  std::atomic<uint32_t> swap_alto_{0};
  std::array<std::atomic<uint64_t>, 128> opcodes_{};
  bool histograma_anotado_ = false;  // only the ring thread touches it
  // Step C2: render targets, copies and presentation (ring thread only).
  std::unique_ptr<DestinosNativos> destinos_;
  bool destinos_fallidos_ = false;
  // Step C5a: identified shaders (ring thread only).
  ShadersNativos shaders_;
  const EntradaShader* vs_actual_ = nullptr;
  const EntradaShader* ps_actual_ = nullptr;
  std::vector<uint32_t> microcodigo_;
  uint64_t dibujos_identificados_ = 0;
  uint64_t dibujos_sin_vs_ = 0;
#if defined(NFSC_RECOMP)
  struct DetalleSinVs {
    uint64_t dibujos = 0;
    uint32_t palabras = 0;
    int32_t ps = -1;
    uint32_t tipo = 0;
    uint32_t cuenta = 0;
  };
  std::unordered_map<uint64_t, DetalleSinVs> sin_vs_detalle_;
#endif
  uint64_t dibujos_sin_ps_ = 0;
  std::unordered_set<uint64_t> pares_;
  // Step C5b: matching with the game's Draw* calls (ring thread only).
  std::deque<RegistroDibujo> pendientes_;
  std::unordered_map<uint32_t, const EntradaShader*> objetos_;
  uint64_t generacion_objetos_ = UINT64_MAX;
  // direct cache in front of objetos_ (ShaderDeObjetoCacheado).
  struct MemoObjeto {
    uint32_t objeto = UINT32_MAX;  // UINT32_MAX = empty slot: not a guest object
    const EntradaShader* entrada = nullptr;
  };
  std::array<MemoObjeto, 16> memo_objetos_{};
  uint64_t ultimo_par_shaders_ = UINT64_MAX;  // ContarDibujoShaders: the previous draw's pair
  std::span<const uint32_t> vs_microcodigo_;  // from the IM_LOAD cache or from vs_inmediato_
  const EntradaShader* vs_dibujo_ = nullptr;
  const EntradaShader* ps_dibujo_ = nullptr;
  uint64_t dibujos_emparejados_ = 0;
  uint64_t dibujos_sin_registro_ = 0;
  uint64_t registros_saltados_ = 0;
  uint64_t dibujos_con_vs_ = 0;
  uint64_t dibujos_con_ps_ = 0;
  uint64_t candidatos_incoherentes_ = 0;
  uint64_t dibujos_incoherentes_ = 0;
  const EntradaShader* coherencia_vs_ = nullptr;
  uint64_t coherencia_generacion_ = UINT64_MAX;
  bool coherencia_ = false;
  const EntradaShader* anotado_vs_ = nullptr;
  uint64_t anotado_generacion_ = UINT64_MAX;
  uint32_t avisos_c5b_ = 0;
  uint64_t variantes_longitud_distinta_ = 0;
  uint64_t elementos_sin_fetch_ = 0;
  std::unordered_set<uint64_t> variantes_vistas_;
  // Steps C3-C6: generations, so unchanged data is not uploaded again.
  uint64_t generacion_vs_ = 0;  // one per distinct VS microcode (CargarShaderCacheado)
  // Cache de IM_LOAD (CargarShaderCacheado): 2 tipos x 128 conjuntos x 2 vias.
  struct CargaShader {
    uint32_t direccion_tipo = 0;
    uint32_t tamano = 0;
    std::vector<uint32_t> crudo;  // as it is in game memory
    std::vector<uint32_t> host;   // swapped: what gets identified and what the vertex input reads
    const EntradaShader* entrada = nullptr;
    uint64_t huella = 0;      // XXH3 de host
    uint64_t generacion = 0;  // from generaciones_microcodigo_, when this content was loaded
    uint64_t uso = 0;         // the least recently used way is the one replaced
    // The versions of its slot this content was checked against guest memory with
    // (microcodigo::AntesDeLeer and DespuesDeLeer). validada = false: it must be compared with memcmp again.
    bool validada = false;
    uint32_t version_ranura = 0;
    uint32_t version_global = 0;
  };
  std::array<std::array<std::array<CargaShader, 2>, 128>, 2> cargas_cache_{};
  uint64_t cargas_tic_ = 0;
  uint64_t cargas_cacheadas_ = 0;
  uint64_t generaciones_microcodigo_ = 0;
  // IM_LOAD without memcmp (CargarShaderCacheado). Ring thread only.
  static constexpr int kImAtajoSinEmpezar = -1;
  static constexpr int kImAtajoMirando = 0;
  static constexpr int kImAtajoAplicando = 1;
  static constexpr int kImAtajoApagado = 2;
  static constexpr uint64_t kImAtajoAMirar = 200000;
  static constexpr uint64_t kImAtajoComprobarCada = 4096;  // potencia de 2
  int im_atajo_fase_ = kImAtajoSinEmpezar;
  uint64_t im_atajo_turno_ = 0;
  uint64_t im_atajo_comprobadas_ = 0;  // agreements with the memcmp since startup
  // Since the last report.
  uint64_t im_i_sin_memcmp_ = 0;
  uint64_t im_i_con_memcmp_ = 0;
  uint64_t im_i_comprobadas_ = 0;
  uint64_t im_i_version_cambiada_ = 0;
  uint64_t im_i_anuladas_ = 0;
  std::vector<uint32_t> vs_inmediato_;  // VS from IM_LOAD_IMMEDIATE, uncached
  // Exact IM_LOAD_IMMEDIATE cache (CargarInmediato): 2 types x 32 sets x 8 ways. Ring thread only.
  static constexpr uint32_t kInmBitsConjunto = 5;
  static constexpr uint32_t kInmVias = 8;
  static constexpr int kInmSinEmpezar = -1;
  static constexpr int kInmMirando = 0;
  static constexpr int kInmAplicando = 1;
  static constexpr int kInmApagado = 2;
  static constexpr uint64_t kInmAMirar = 20000;
  static constexpr uint64_t kInmComprobarCada = 1024;  // potencia de 2
  std::array<std::array<std::array<CargaInmediata, kInmVias>, (size_t(1) << kInmBitsConjunto)>, 2> inm_cache_{};
  std::array<std::array<uint8_t, (size_t(1) << kInmBitsConjunto)>, 2> inm_ultima_{};  // way of the last hit
  int inm_fase_ = kInmSinEmpezar;
  uint64_t inm_turno_ = 0;
  uint64_t inm_tic_ = 0;
  uint64_t inm_comprobadas_ = 0;  // hits checked and equal since startup
  // Since the last report.
  uint64_t inm_i_aciertos_ = 0;
  uint64_t inm_i_comprobadas_ = 0;
  uint64_t inm_i_fallos_ = 0;
  uint64_t inm_i_vuelta_ = 0;
  uint64_t inm_i_sin_cache_ = 0;
  const EntradaShader* mapeado_vs_ = nullptr;
  uint64_t mapeado_generacion_ = UINT64_MAX;
  // How many constant writes do not change the value. Tells whether comparing before bumping
  // the generation pays off, and by how much.
  uint64_t constantes_escritas_ = 0;
  uint64_t constantes_sin_cambio_ = 0;
  uint64_t bloques_constantes_ = 0;
  uint64_t bloques_constantes_sin_cambio_ = 0;
  uint64_t generacion_constantes_vs_ = 0;
  int registros_en_bloque_ = -1;  // -1 = cvar not read yet
  uint64_t generacion_constantes_ps_ = 0;
  // Generations of the fetch constants and the viewport state (viewport, scissor, clip). They
  // travel in PeticionDibujo so the translator does not redo per draw what has not changed.
  uint64_t generacion_fetch_ = 0;
  uint64_t generacion_encuadre_ = 0;
  uint64_t fetch_escritas_ = 0;
  uint64_t fetch_sin_cambio_ = 0;
  uint64_t encuadre_escritas_ = 0;
  uint64_t encuadre_sin_cambio_ = 0;
  // Ring thread times since the last report (that thread only).
  uint64_t tiempo_anillo_ns_ = 0;
  uint64_t tiempo_dibujos_ns_ = 0;
  uint64_t tiempo_copias_ns_ = 0;
  uint64_t tiempo_presentar_ns_ = 0;
  uint64_t dibujos_medidos_ = 0;
  uint64_t fase_cronometro_dibujos_ = 0;  // phase of the DibujarNativo timer, never reset
  uint64_t copias_medidas_ = 0;
  uint64_t presentaciones_medidas_ = 0;
  // Ring breakdown per packet: registers (types 0 and 1) and each type-3 opcode.
  uint64_t tiempo_registros_ns_ = 0;
  uint64_t cuenta_registros_ = 0;
  uint64_t palabras_bloque_ = 0;   // Measurement only
  // Vector path of EscribirRegistrosEnBloque and its guard.
  static constexpr uint64_t kBloquesAVerificar = 200000;
  int registros_vectoriales_ = -1;
  uint64_t bloques_verificados_ = 0;
  std::vector<uint32_t> copia_verificacion_;
  // Phase 1 of the Direct3D-level renderer (CompararSombra).
  InstantaneaEspejo sombra_foto_;
  uint64_t sombra_dibujos_ = 0;
  uint64_t sombra_perdidas_ = 0;
  uint64_t sombra_estado_comparados_ = 0;
  uint64_t sombra_estado_distintos_ = 0;
  uint64_t sombra_fetch_comparados_ = 0;
  uint64_t sombra_fetch_distintos_ = 0;
  uint64_t sombra_constantes_comparadas_ = 0;
  uint64_t sombra_constantes_distintas_ = 0;
  std::unordered_map<uint32_t, uint64_t> sombra_por_registro_;
  // Phase 2 of the Direct3D-level renderer (ProcesarMarcador). Ring thread only.
  static constexpr uint64_t kTramosRapidosAVerificar = 200000;
  int marcador_rapido_ = 1;  // 0: the fast path was turned off by a mismatch (VerificarTramoMarcador)
  uint64_t tramos_rapidos_verificados_ = 0;
  std::vector<uint32_t> copia_marcador_;
  std::vector<uint32_t> marcador_plano_;  // a marker that wrapped around the ring, made contiguous (should not happen)
  uint64_t marcadores_aplicados_ = 0;
  uint64_t marcadores_comprobados_ = 0;
  uint64_t marcadores_distintos_ = 0;
  uint64_t marcadores_malos_ = 0;
  uint64_t marcador_tramos_ = 0;
  uint64_t marcador_tramos_lentos_ = 0;  // through the regular path (EscribirRegistrosEnBloque or one at a time)
  uint64_t marcador_palabras_ = 0;
  uint32_t avisos_marcador_ = 0;
  // Phase 2b: the Draw* record carried by the last marker, for the DRAW_INDX that follows. It stays
  // until a draw accepts it or the next Draw* record arrives, like the head of the queue.
  RegistroDibujo dibujo_marcador_;
  uint32_t dibujo_marcador_modo_ = 0;  // 0 none, kDibujoAplicar or kDibujoComprobar
  uint16_t vegetacion_juego_ = 0;  // kVeg* flags of the record used to draw (0: none)
  uint64_t dibujos_con_registro_de_marcador_ = 0;
  uint64_t registros_de_marcador_rechazados_ = 0;
  uint64_t comprobaciones_dibujo_ = 0;
  uint64_t comprobaciones_dibujo_distintas_ = 0;
  uint64_t comprobaciones_dibujo_otro_registro_ = 0;
  uint32_t avisos_dibujo_marcador_ = 0;
  uint64_t palabras_sueltas_ = 0;
  uint64_t cronometro_paquetes_ = 0;
  std::array<uint64_t, 128> tiempo_opcode_ns_{};
  std::array<uint64_t, 128> cuenta_opcode_{};
  // WAIT_REG_MEM per polled register or address since the last report.
  struct Espera {
    uint64_t veces = 0;
    uint64_t vueltas = 0;  // failed polls, each with a 1 ms wait
    uint32_t referencia = 0;
    uint32_t mascara = 0;
    uint32_t info = 0;
  };
  std::unordered_map<uint64_t, Espera> esperas_;
  // Draw stages at the previous report (for the deltas).
  uint64_t dibujados_previos_ = 0;
  uint64_t cronometrados_previos_ = 0;
  uint64_t vegetacion_pronto_previa_ = 0;
  std::array<uint64_t, kEtapasDibujo> etapas_previas_{};
  uint64_t descarte_con_previo_ = 0;
  uint64_t descarte_sin_previo_ = 0;
  std::array<uint64_t, 20> contadores_previos_{};
  std::array<uint64_t, 3> dedupe_previos_{};  // "C6 contadores"
  std::array<uint64_t, kGpuCategorias> texels_categoria_previos_{};  // "C6 area abierta"
  std::array<uint64_t, kGpuCategorias> pases_categoria_previos_{};
  uint64_t dibujos_ps_inutil_previos_ = 0;
  uint64_t sombras_alfa_activa_previa_ = 0;
  uint64_t sombras_alfa_apagada_previa_ = 0;
  uint64_t dibujos_ps_necesario_previos_ = 0;
  std::array<uint64_t, kGpuCategorias> dibujos_categoria_previos_{};
  std::array<uint64_t, kGpuCategorias> triangulos_categoria_previos_{};
  uint64_t envios_ubo_previos_ = 0;  // "C6 constantes por UBO"
  uint64_t compartidas_miradas_previas_ = 0;  // "C6 constantes compartidas"
  uint64_t compartidas_cambiadas_previas_ = 0;
  std::array<uint64_t, kCubetasSwap> cubetas_swap_previas_{};  // "C2: intervalos entre Swaps"
  uint64_t solapes_gpu_previos_ = 0;
  uint64_t coste_presentar_previo_[12] = {};  // "C2: presentar"
  uint64_t envios_previos_ = 0;
  // Gamma ramp loaded by the game (AnotarRampaGamma), identity at startup. The output applies it.
  std::array<std::array<uint16_t, 3>, 256> rampa_gamma_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();
  uint32_t componente_rampa_ = 0;
  uint32_t mascara_rampa_ = 0b111;  // default DC_LUT_WRITE_EN_MASK (register_table.inc)
  uint64_t version_rampa_ = 0;
  uint64_t version_rampa_enviada_ = 0;  // the identity is already in the output
  std::array<uint64_t, kRegRampaUltimo - kRegRampaPrimero + 1> escrituras_rampa_{};
  uint64_t escrituras_rampa_previas_ = 0;
  // Occlusion queries (ConsultaOclusion), ring thread only.
  uint32_t oclusion_base_ = 0;                    // structure of the open query (0 = none)
  std::unordered_set<uint32_t> bases_oclusion_;   // structures seen in an Issue(BEGIN)
  uint32_t avisos_oclusion_ = 0;                  // details of the first ones in the log
  // Finished queries per render target kind (0: under 640 or without MSAA on the Xbox 360; 1: scene,
  // scaled) in the interval, and the largest count written for each since the start.
  std::array<uint64_t, 2> oclusion_por_destino_{};
  std::array<uint32_t, 2> oclusion_max_por_destino_{};
  uint32_t avisos_oclusion_escena_ = 0;
  // Diagnostic nfsmw_nativo_diag_constantes_ps.
  std::string constantes_lista_texto_;
  std::unordered_set<uint32_t> constantes_lista_;
  std::unordered_map<uint32_t, Reloj::time_point> constantes_ultimo_;
  // Prueba nfsmw_nativo_oclusion_alternar_s.
  bool oclusion_prueba_iniciada_ = false;
  bool oclusion_prueba_fingida_ = false;
  Reloj::time_point oclusion_prueba_inicio_{};
  // Started, finished with a measurement, finished without a measurement yet, started without their
  // end, written with the faked count outside a pair, sum of the written measurements and the
  // largest one written.
  std::array<uint64_t, 7> oclusion_contadores_{};
  std::array<uint64_t, 7> oclusion_contadores_previos_{};
  uint64_t oclusion_host_previo_[5] = {};  // DestinosNativos::EstadisticasOclusion at the previous report
  // GPU time at the previous report (C2).
  uint64_t gpu_ns_previo_ = 0;
  uint64_t esperas_gpu_previas_ = 0;     // C2 report: ring waits for the GPU
  uint64_t ns_esperas_gpu_previos_ = 0;
  uint64_t trabajos_gpu_previos_ = 0;
  uint64_t ns_trabajo_gpu_previo_ = 0;
  uint64_t coste_grabar_previo_[6] = {};  // C2 report: Grabar and readbacks
  uint64_t gpu_trabajos_previos_ = 0;
  uint64_t presentados_previos_gpu_ = 0;
  std::array<uint64_t, kGpuCategorias> gpu_categorias_previas_{};
  std::array<uint64_t, kGpuCategorias> fragmentos_categoria_previos_{};
  std::array<uint64_t, kGpuCategorias> vertices_categoria_previos_{};
  std::array<uint64_t, kGpuCategorias> primitivas_categoria_previas_{};
  uint64_t fotogramas_diagnostico_previos_ = 0;
  std::array<uint64_t, 4> copias_cubeta_previas_{};
  std::array<uint64_t, 4> pixeles_cubeta_previos_{};
  // Real scale of the GPU timestamps (C2 report).
  uint64_t marca_gpu_previa_ns_ = 0;
  uint64_t trabajos_precisos_previos_ = 0;
  Reloj::time_point reloj_marca_previo_{};
  double escala_real_ns_ = 0.0;
  double escala_marcas_ns_ = 0.0;
  Reloj::time_point ultimo_tiempo_informe_{};
  Reloj::time_point ultimo_informe_{};
  // Trace of one frame (nfsmw_nativo_diag_fotograma_s).
  Reloj::time_point inicio_sistema_{};
  bool trazando_ = false;
  bool traza_hecha_ = false;
  // nfsmw_nativo_diag_constantes_ps is not empty. Refreshed on every Swap.
  bool diag_constantes_activo_ = false;
  std::unordered_set<uint32_t> texturas_volcadas_;  // nfsmw_nativo_diag_vertices_ps
  uint32_t trazas_ = 0;
  uint32_t motivo_emparejado_ = 0;  // 0 emparejado, 1 sin registro, 2 registro incoherente
  uint32_t objeto_vs_ = 0;           // objects of the paired record
  uint32_t objeto_ps_ = 0;
  // Ring identity for draws without a usable record (UsarIdentidadDelAnillo).
  std::unordered_map<uint64_t, const EntradaShader*> vs_por_microcodigo_;
  uint64_t huella_vs_ = 0;
  uint64_t huella_vs_generacion_ = UINT64_MAX;
  uint64_t dibujos_por_im_load_ = 0;

  // Vulkan objects for the test image: only the ring thread touches them.
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer comandos_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool fence_pendiente_ = false;
  std::array<Framebuffer, rex::ui::vulkan::VulkanPresenter::kMaxActiveGuestOutputImageVersions>
      framebuffers_{};
  size_t siguiente_framebuffer_ = 0;
};

}  // namespace

uint64_t SwapsNativos() {
  return g_swaps_nativos.load(std::memory_order_relaxed);
}

bool Activo() {
  return REXCVAR_GET(nfsmw_renderizador) == "nativo";
}

std::unique_ptr<rex::system::IGraphicsSystem> CrearSistemaGrafico() {
  return std::make_unique<SistemaGraficoNativo>();
}

}  // namespace nfsmw::nativo
