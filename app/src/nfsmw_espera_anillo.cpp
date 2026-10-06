// nfsmw - the game's D3D waits without spinning (native renderer).
//
// WHY (PC profile of a race)
//   The "Main XThread" runs at 100 % of a core in a race and 84 % of that is a busy-wait. sub_82597690
//   (wait for the GPU to read the ring up to the write pointer) calls sub_825A5D18 in a loop, without
//   sleeping. That function only checks whether the read pointer the GPU reports back ([[device+10384]])
//   has changed since the last iteration and, if 2 s pass without changes, declares the GPU hung. Seven
//   other D3D waits also call it. On the console that thread runs at 80 % in a race and takes CPU away
//   from the PM4 ring thread, which is the one that sets the FPS.
//
// WHAT IT DOES
//   If the read pointer is the same as in the previous iteration, it waits at most
//   nfsmw_espera_anillo_max_us for the ring thread to report it again or deliver an interrupt
//   (ProgresoAnillo), and then calls the original function, which decides as before. It does not change
//   what the game sees, only when it looks. With bit 0x04 of [device+10433] (GPU already declared hung)
//   or with Xenos emulation, it does not wait.
//
// MEASURED ON THE CONSOLE (race, no overclock)
//   697 D3D iterations in 10 s, 697 waits, 1414.4 ms asleep and only 123 ended by the ring advancing.
//   That is: 70 waits/s (2.8 per frame), 5.8 ms per frame with the "Main XThread" stopped, and 82 % use
//   up the 2 ms timeout. It is not a notification failure: the ring thread runs at 95 % and its batches
//   are long (3.5 wakeups with data per frame for 23 writes of CP_RB_WPTR), plus 6.8 ms per frame spent
//   in presentation without advancing the read pointer. It is real backpressure, not a busy-wait.
//   That is why the timeout is left alone: lowering it only multiplies the wakeups on a 3-core machine
//   where the ring thread is the bottleneck; raising it saves nothing because the notification already
//   wakes up the waiter.
//
// MIND THE ADDRESSES: the read pointer word is in the physical area (0xE0000000 and above). On PC
//   the recompiled code adds 0x1000 to it (REX_PHYS_HOST_OFFSET in nfsmw_pch.h); on the Switch, nothing.
//   Without that offset an earlier version read another word, never saw the pointer stopped and never
//   slept once.

#include "nfsmw_esperas_tiron.h"
#include "nfsmw_nativo_sistema.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_espera_anillo_bloqueante, true, "NFSMW",
                    "Renderizador nativo: el D3D del juego duerme mientras espera a que el hilo del anillo "
                    "avance, en vez de dar vueltas en sub_825A5D18")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_espera_anillo_max_us, 2000, "NFSMW",
                     "Renderizador nativo: espera maxima por vuelta de las esperas del D3D del juego, en "
                     "microsegundos")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::nativo {
namespace {

std::mutex g_progreso_mutex;
std::condition_variable g_progreso_cv;
std::atomic<uint32_t> g_progreso{0};
std::atomic<int> g_esperando{0};

}  // namespace

uint32_t ProgresoAnillo() {
  return g_progreso.load(std::memory_order_acquire);
}

// Order: the ring thread writes the read pointer, bumps the counter and only then checks whether anyone
// is waiting. The waiter reads the counter before looking at the pointer, registers itself and checks the
// counter under the lock: no notification is lost between checking and falling asleep.
//
// Whether someone is waiting is checked with the lock held. Checking it without the lock is a Dekker
// handshake, and on ARM (and on x86 with stores without a lock prefix) each side may not yet see what the
// other wrote: that is how the vertex copy thread hung in earlier versions. Here the wait is bounded and
// would only cost that bound, but the lock is a few tens of nanoseconds per ring pass.
void AvisarProgresoAnillo() {
  g_progreso.fetch_add(1, std::memory_order_acq_rel);
  bool avisar;
  {
    std::lock_guard<std::mutex> cerrojo(g_progreso_mutex);
    avisar = g_esperando.load(std::memory_order_acquire) > 0;
  }
  if (avisar) {
    g_progreso_cv.notify_all();
  }
}

bool EsperarProgresoAnillo(uint32_t visto, std::chrono::microseconds limite) {
  bool avanzo = false;
  {
    std::unique_lock<std::mutex> cerrojo(g_progreso_mutex);
    g_esperando.fetch_add(1, std::memory_order_acq_rel);
    avanzo = g_progreso_cv.wait_for(cerrojo, limite, [visto] {
      return g_progreso.load(std::memory_order_acquire) != visto;
    });
    g_esperando.fetch_sub(1, std::memory_order_acq_rel);
  }
  return avanzo;
}

}  // namespace nfsmw::nativo

// Most Wanted's D3D waits. NFS Carbon hooks its own (nfscarbon-recomp, carbon_graphics.cpp).
#if !defined(NFSC_RECOMP)
namespace {

constexpr uint32_t kOffLecturaDevuelta = 10384;  // pointer to the word where the GPU reports the read pointer
constexpr uint32_t kOffEstado = 10433;           // bit 0x04: GPU considered hung

// The same as REX_LOAD_U32 / REX_LOAD_U8 in the recompiled code (nfsmw_pch.h).
inline uint32_t DesplazamientoFisico(uint32_t direccion) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return direccion >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)direccion;
  return 0u;
#endif
}

uint32_t Leer32(const uint8_t* base, uint32_t direccion) {
  uint32_t v = 0;
  std::memcpy(&v, base + direccion + DesplazamientoFisico(direccion), sizeof(v));
  return __builtin_bswap32(v);
}

uint8_t Leer8(const uint8_t* base, uint32_t direccion) {
  return base[uint64_t(direccion) + DesplazamientoFisico(direccion)];
}

std::atomic<uint64_t> g_vueltas{0};
std::atomic<uint64_t> g_esperas{0};
std::atomic<uint64_t> g_avances{0};
std::atomic<uint64_t> g_ns_esperando{0};
// The worst wait of the interval. The average says nothing here: in a console measurement there were 697
// waits and 1414 ms, i.e. 2.03 ms on average with a cap of 2.00, and only 123 ended because the ring
// advanced. So 82 % use up the whole timeout. What needs to be known is whether any goes far beyond the
// cap (the ring thread can spend 7 ms inside presentation without advancing the read pointer, and then
// the game's D3D pays two or three timeouts in a row for nothing).
std::atomic<uint64_t> g_ns_maxima{0};
std::atomic<int64_t> g_siguiente_informe_ms{0};

void InformeEsperasCompletas();  // Defined below

// Only called when sleeping (and every 2^16 iterations without sleeping): reading the clock on every
// iteration cost more than the check itself (an earlier version: ~20 million iterations per second).
void Informe() {
  using namespace std::chrono;
  const int64_t ahora = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  int64_t siguiente = g_siguiente_informe_ms.load(std::memory_order_relaxed);
  if (ahora < siguiente) {
    return;
  }
  if (siguiente == 0) {
    g_siguiente_informe_ms.store(ahora + 10000, std::memory_order_relaxed);
    return;
  }
  if (!g_siguiente_informe_ms.compare_exchange_strong(siguiente, ahora + 10000)) {
    return;
  }
  const uint64_t vueltas = g_vueltas.exchange(0);
  const uint64_t esperas = g_esperas.exchange(0);
  const uint64_t avances = g_avances.exchange(0);
  const uint64_t ns = g_ns_esperando.exchange(0);
  const uint64_t ns_max = g_ns_maxima.exchange(0);
  REXLOG_INFO("[espera_anillo] ultimos 10 s: {} vueltas del D3D, {} esperas ({} terminadas por avance del "
              "anillo, {} agotaron el plazo de {} us), {:.1f} ms durmiendo, peor {:.2f} ms",
              vueltas, esperas, avances, esperas > avances ? esperas - avances : uint64_t(0),
              REXCVAR_GET(nfsmw_espera_anillo_max_us), double(ns) / 1e6, double(ns_max) / 1e6);
  InformeEsperasCompletas();
}

/*
 * The complete wait, and who asks for it.
 *
 * The code above measures each polling iteration (capped at 2 ms), so a 60 ms block of the game shows up
 * as thirty expired iterations and never as "worst 60 ms". That is why several builds did not show where
 * the game blocks in the corners and the alley. This measures the whole of sub_82597690 ("wait for the
 * GPU to read the ring up to here", called by the 13 D3D waits, eWaitUntilRenderingDone included) from
 * entry to exit, and breaks it down by return address (who called it).
 *
 * In our port the GPU memory writes (MEM_WRITE, EVENT_WRITE_SHD) are done when the packet is processed
 * on the PM4 ring thread, not when the real GPU finishes. So "waiting for the GPU" here means waiting
 * for the ring to consume the queue: if this comes out large, the ring is lagging behind.
 *
 * (Still inside the anonymous namespace above; the hook is at the end of the file, outside it.)
 */
constexpr size_t kLlamantes = 12;
struct Llamante {
  std::atomic<uint32_t> lr{0};
  std::atomic<uint64_t> n{0};
  std::atomic<uint64_t> ns{0};
  std::atomic<uint64_t> ns_max{0};
  std::atomic<uint64_t> largas{0};  // waits over 8 ms: a quarter of a frame, already a stutter
};
Llamante g_llamantes[kLlamantes];
std::atomic<uint64_t> g_completas_n{0};
std::atomic<uint64_t> g_completas_ns{0};
std::atomic<uint64_t> g_completas_largas{0};
std::atomic<uint64_t> g_completas_ns_max{0};

void AnotarEsperaCompleta(uint32_t lr, uint64_t ns) {
  g_completas_n.fetch_add(1, std::memory_order_relaxed);
  g_completas_ns.fetch_add(ns, std::memory_order_relaxed);
  const bool larga = ns > 8000000;
  if (larga) g_completas_largas.fetch_add(1, std::memory_order_relaxed);
  {
    uint64_t previo = g_completas_ns_max.load(std::memory_order_relaxed);
    while (ns > previo && !g_completas_ns_max.compare_exchange_weak(previo, ns, std::memory_order_relaxed)) {
    }
  }
  // Slot per caller: the first one with that lr, or the first empty one. With 13 possible callers and 12
  // slots, in the worst case one is left without a slot and shows up as "otros" in the total.
  for (size_t i = 0; i < kLlamantes; ++i) {
    uint32_t esperado = 0;
    if (g_llamantes[i].lr.load(std::memory_order_relaxed) == lr ||
        g_llamantes[i].lr.compare_exchange_strong(esperado, lr, std::memory_order_relaxed) ||
        esperado == lr) {
      g_llamantes[i].n.fetch_add(1, std::memory_order_relaxed);
      g_llamantes[i].ns.fetch_add(ns, std::memory_order_relaxed);
      if (larga) g_llamantes[i].largas.fetch_add(1, std::memory_order_relaxed);
      uint64_t previo = g_llamantes[i].ns_max.load(std::memory_order_relaxed);
      while (ns > previo &&
             !g_llamantes[i].ns_max.compare_exchange_weak(previo, ns, std::memory_order_relaxed)) {
      }
      return;
    }
  }
}

void InformeEsperasCompletas() {
  const uint64_t n = g_completas_n.exchange(0);
  if (!n) {
    return;
  }
  const uint64_t ns = g_completas_ns.exchange(0);
  const uint64_t largas = g_completas_largas.exchange(0);
  const uint64_t ns_max = g_completas_ns_max.exchange(0);
  std::string por_llamante;
  for (size_t i = 0; i < kLlamantes; ++i) {
    const uint32_t lr = g_llamantes[i].lr.load(std::memory_order_relaxed);
    if (!lr) continue;
    const uint64_t ln = g_llamantes[i].n.exchange(0);
    if (!ln) continue;
    const uint64_t lns = g_llamantes[i].ns.exchange(0);
    const uint64_t lmax = g_llamantes[i].ns_max.exchange(0);
    const uint64_t llargas = g_llamantes[i].largas.exchange(0);
    char buf[96];
    std::snprintf(buf, sizeof(buf), " %08X:%llu/%.1fms/peor%.1f/largas%llu", lr, (unsigned long long)ln,
                  double(lns) / 1e6, double(lmax) / 1e6, (unsigned long long)llargas);
    por_llamante += buf;
  }
  REXLOG_INFO("[espera_anillo] esperas COMPLETAS del D3D (sub_82597690) en 10 s: {} en {:.1f} ms, {} de mas "
              "de 8 ms, peor {:.1f} ms; por llamante (lr:n/ms/peor/largas):{}",
              n, double(ns) / 1e6, largas, double(ns_max) / 1e6, por_llamante);
}

}  // namespace

REX_EXTERN(__imp__sub_825A5D18);
REX_HOOK_RAW(sub_825A5D18) {
  static const bool activo = nfsmw::nativo::Activo() && REXCVAR_GET(nfsmw_espera_anillo_bloqueante);
  if (activo) {
    const uint32_t estructura = ctx.r3.u32;
    const uint32_t dispositivo = estructura ? Leer32(base, estructura) : 0;
    if (dispositivo && !(Leer8(base, dispositivo + kOffEstado) & 0x04)) {
      const uint32_t palabra = Leer32(base, dispositivo + kOffLecturaDevuelta);
      if (palabra) {
        const uint64_t vueltas = g_vueltas.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint32_t visto = nfsmw::nativo::ProgresoAnillo();
        if (Leer32(base, palabra) == Leer32(base, estructura + 8)) {
          const auto antes = std::chrono::steady_clock::now();
          g_esperas.fetch_add(1, std::memory_order_relaxed);
          if (nfsmw::nativo::EsperarProgresoAnillo(
                  visto, std::chrono::microseconds(REXCVAR_GET(nfsmw_espera_anillo_max_us)))) {
            g_avances.fetch_add(1, std::memory_order_relaxed);
          }
          const uint64_t ns_espera = uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - antes)
                  .count());
          g_ns_esperando.fetch_add(ns_espera, std::memory_order_relaxed);
          nfsmw::esperas::Sumar(nfsmw::esperas::kSitioAnillo, ns_espera);
          {
            uint64_t previo = g_ns_maxima.load(std::memory_order_relaxed);
            while (ns_espera > previo &&
                   !g_ns_maxima.compare_exchange_weak(previo, ns_espera, std::memory_order_relaxed)) {
            }
          }
          Informe();
        } else if ((vueltas & 0xFFFF) == 0) {
          Informe();
        }
      }
    }
  }
  __imp__sub_825A5D18(ctx, base);
}

// The complete D3D wait for the ring to reach a point, with who asks for it.
// See the comment of AnotarEsperaCompleta. Always measured: two clock reads per wait.
REX_EXTERN(__imp__sub_82597690);
REX_HOOK_RAW(sub_82597690) {
  const uint32_t lr = uint32_t(ctx.lr);
  const auto antes = std::chrono::steady_clock::now();
  __imp__sub_82597690(ctx, base);
  const uint64_t ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - antes).count());
  AnotarEsperaCompleta(lr, ns);
}
#endif  // !NFSC_RECOMP
