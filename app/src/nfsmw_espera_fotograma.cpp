// nfsmw - frame handoff between the game's two threads without a Sleep(0) loop.
//
// WHY (PC profile of a race)
//   With the D3D wait already sleeping (nfsmw_espera_anillo.cpp), the thread that prepares the frames
//   (XThread F800002C) is still at 101 %: 41 % in yields and 12 % in the game's Sleep (sub_8262F258).
//   The game splits each frame between two threads with a flag at 0x82A2CF40:
//     - That thread, in sub_824411B8, waits in sub_82441F18 for the flag to go back to 0 (the previous
//       frame has been executed) by calling Sleep(0) (sub_8262D988) in a loop. Then sub_82442058 sets it
//       to 1 and fills the command list 0x82909650 inside sub_82445660.
//     - The "Main XThread", in sub_82441CC8, waits with another Sleep(0) loop for the flag to be 1, runs
//       the commands (sub_823C83F8), sets it to 0 and immediately calls sub_8262DE18 (a game trace).
//   On the console both threads run at 44-83 % in a race.
//
// WHAT IT DOES
//   The Sleep(0) calls of those two loops (return addresses 0x82441FB4 and 0x82441D24) sleep until the
//   flag changes, or at most nfsmw_espera_fotograma_max_us, instead of yielding. The change is signaled by
//   the entry to sub_82445660 (right after setting it to 1) and by the call to sub_8262DE18 (right after
//   setting it to 0). The game's loop checks the flag again, so an extra notification changes nothing and
//   a lost one only costs the maximum time. The game's other Sleep calls are not touched.
//
// MEASURED ON THE CONSOLE (race, no overclock), AND WHAT IS NOT KNOWN
//   Per 10 s window: the preparer between 736 and 4888 waits (1.0 to 6.1 s asleep), the executor between
//   260 and 717 (0.1 to 1.1 s). In the worst window that is 434 waits/s for the preparer, 17.7 per frame,
//   and 21.7 ms of each frame with that thread asleep. That is not a problem by itself: the preparer
//   waits for the executor to finish the command list, and while it waits it has nothing to do.
//   What the earlier counters could not tell: the average per wait was 1.23 ms with the timeout at
//   1.00 ms. That fits both "almost all use up the timeout" (the notification does not work) and "the
//   ones in between use it up and the last one is woken by the notification" (the notification works),
//   because on Horizon, with 3 cores and the ring thread at 95 %, getting back on the CPU after the
//   timeout already costs those tenths. That is why the waits ended by a notification and the worst one
//   are counted separately. Mostly by notification means the timeout can go up to 4-8 ms, removing 15
//   wakeups per frame at no cost; almost none means the notification is broken and the handoff pays up to
//   1 ms of delay on every delivery.

#include "nfsmw_esperas_tiron.h"
#include "nfsmw_ordenes_publicadas.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports
#include <rex/thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>

#if REX_PLATFORM_SWITCH
// Only for RexSwitchSetCurrentThreadPriorityOk. switch.h is deliberately not included.
#include "../../sdk/src/core/threading_switch.h"
#endif

/*
 * The two handoff threads, one step above the rest of the game.
 *
 * What the logs showed. This file's log reports the worst wait of each 10 s window, and with a
 * 1 ms timeout it comes out again and again between 7 and 17 ms (executor, worst 16.94; preparer, worst
 * 17.20). A wait with a 1 ms timeout can only last 17 ms if the thread, already awake, does not get a
 * core. And that is exactly what to expect: the game's threads all run at 0x3B, the only priority that
 * Horizon shares in 10 ms time slices, with the CPU at 280 % out of 300. The thread that sets the frame
 * pace (Main XThread, the executor) wakes up and sits in the queue behind another game thread until
 * that one's slice runs out. A 33 ms frame plus a lost slice is the jump to 50 ms that is seen as a
 * stutter.
 *
 * And it rules out the other explanation: the ring dropped from 80.8 to 75.7 % CPU and the FPS did not
 * move, while the Main XThread stayed at 77-80 % over several builds. The game thread is now the
 * limiting factor, not the ring.
 *
 * 0x3A (lower number = higher priority) preempts the other game threads on wakeup, and stays below
 * the ring (0x2D), the presenter (0x2C) and the audio (0x2B), which are not touched.
 *
 * The risk, and why it is acceptable: below 0x3B Horizon does not time-slice, so a thread at 0x3A
 * that never yields would starve the 0x3B threads on that core. These two yield: they block in the
 * handoff hundreds of times per second (this same log counts it), the executor in the D3D wait
 * (nfsmw_espera_anillo.cpp), and neither goes above 80 % of a core; and with mask 0x7 the others have
 * two more cores. If something still got stuck, 0 restores the previous behavior.
 *
 * It is applied on each thread's first wait, which is when we know which is which.
 */
REXCVAR_DEFINE_BOOL(nfsmw_ejecutor_sin_vueltas, true, "NFSMW",
                    "24/09 (build 169): el Main XThread espera las ordenes del preparador DURMIENDO en pausas cortas "
                    "en vez de dar vueltas sin parar en sub_82441CC8 (12,6 % de un nucleo en la 162). false = como "
                    "antes");
REXCVAR_DEFINE_INT32(nfsmw_ejecutor_pausa_us, 100, "NFSMW",
                     "Pausa del ejecutor mientras espera ordenes (us). Menos = responde antes y gasta mas CPU");
REXCVAR_DEFINE_INT32(nfsmw_ejecutor_espera_max_us, 2000, "NFSMW",
                     "Como mucho esto (us) por espera del ejecutor; luego el bucle del juego vuelve a mirar");

REXCVAR_DEFINE_INT32(nfsmw_relevo_prioridad, 0x3A, "NFSMW",
                     "Switch: prioridad de Horizon de los dos hilos del relevo de fotogramas (el que "
                     "prepara y el que ejecuta, Main XThread). 0 = no tocar (0x3B, como el resto del "
                     "juego). 0x3A = un escalon por encima de los demas hilos del juego, para que al "
                     "despertar no esperen un turno de 10 ms")
    .range(0, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(nfsmw_espera_fotograma_bloqueante, true, "NFSMW",
                    "Relevo de fotogramas entre los dos hilos del juego (bandera 0x82A2CF40): duermen hasta el "
                    "cambio en vez de llamar a Sleep(0) en bucle")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfsmw_espera_fotograma_max_us, 1000, "NFSMW",
                     "Espera maxima por vuelta del relevo de fotogramas, en microsegundos")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kBandera = 0x82A2CF40;
constexpr uint32_t kRetornoPreparador = 0x82441FB4;  // Sleep(0) of sub_82441F18: waits for the flag to stop being 1
constexpr uint32_t kRetornoEjecutor = 0x82441D24;    // Sleep(0) of sub_82441CC8: waits for the flag to stop being 0

std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<uint32_t> g_cambios{0};
std::atomic<int> g_esperando{0};

std::atomic<uint64_t> g_esperas_preparador{0};
std::atomic<uint64_t> g_esperas_ejecutor{0};
std::atomic<uint64_t> g_ns_preparador{0};
std::atomic<uint64_t> g_ns_ejecutor{0};
// How many waits end because the notification arrives and how many use up the timeout, as in
// nfsmw_espera_anillo.cpp ("N terminadas por avance del anillo"). Without this, "the handoff wakes up
// immediately" cannot be told apart from "the notification never arrives and the whole timeout is always
// paid", and in a console measurement the average per wait (1.23 ms with a cap of 1.00) fit both. The
// answer tells whether nfsmw_espera_fotograma_max_us can be raised (fewer wakeups, same delay) or whether
// raising it would add up to that timeout of delay to each handoff.
std::atomic<uint64_t> g_avisos_preparador{0};
std::atomic<uint64_t> g_avisos_ejecutor{0};
std::atomic<uint64_t> g_ns_max_preparador{0};
std::atomic<uint64_t> g_ns_max_ejecutor{0};
std::atomic<int64_t> g_siguiente_informe_ms{0};
// The executor with no commands (see the sub_823C83F8 hook).
std::atomic<uint64_t> g_sin_ordenes_esperas{0};
std::atomic<uint64_t> g_sin_ordenes_pausas{0};
std::atomic<uint64_t> g_sin_ordenes_ns{0};

/*
 * Wakeup delay, measured rather than inferred from the "worst".
 *
 * It is the time from when the other thread notifies (or the timeout expires) until the sleeping one
 * really runs again. With spare CPU it is tenths of a millisecond; if the scheduler holds it back, it
 * is milliseconds. What counts are the ones above 3 ms: each one is a frame that can jump from the 33 ms
 * vblank to the 50 ms one. This is the measurement that says whether nfsmw_relevo_prioridad helps:
 * going from 0 to 0x3A must lower the number of "late" ones.
 */
std::atomic<int64_t> g_ultimo_aviso_ns{0};
std::atomic<uint64_t> g_despertar_ns{0};
std::atomic<uint64_t> g_despertar_n{0};
std::atomic<uint64_t> g_despertar_tardios{0};  // more than 3 ms
std::atomic<uint64_t> g_despertar_max_ns{0};
constexpr int64_t kDespertarTardioNs = 3000000;

int64_t AhoraNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void AnotarDespertar(int64_t retraso_ns) {
  if (retraso_ns < 0) {
    retraso_ns = 0;
  }
  g_despertar_ns.fetch_add(uint64_t(retraso_ns), std::memory_order_relaxed);
  g_despertar_n.fetch_add(1, std::memory_order_relaxed);
  if (retraso_ns > kDespertarTardioNs) {
    g_despertar_tardios.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t previo = g_despertar_max_ns.load(std::memory_order_relaxed);
  while (uint64_t(retraso_ns) > previo &&
         !g_despertar_max_ns.compare_exchange_weak(previo, uint64_t(retraso_ns),
                                                   std::memory_order_relaxed)) {
  }
}

// Once per thread: raises its priority the first time it enters the handoff. The role is given by the
// return address, so there is no need to know in advance which thread is which.
void SubirPrioridadUnaVez(bool& hecho, const char* papel) {
  if (hecho) {
    return;
  }
  hecho = true;
#if REX_PLATFORM_SWITCH
  const int32_t prioridad = REXCVAR_GET(nfsmw_relevo_prioridad);
  if (prioridad >= 0x1C && prioridad <= 0x3B) {
    const bool ok = RexSwitchSetCurrentThreadPriorityOk(int(prioridad));
    REXLOG_INFO("[espera_fotograma] hilo {} del relevo a prioridad {:#x} ({})", papel,
                uint32_t(prioridad), ok ? "aceptada" : "RECHAZADA por el kernel");
  } else {
    REXLOG_INFO("[espera_fotograma] hilo {} del relevo: prioridad sin tocar (0x3B)", papel);
  }
#else
  (void)papel;
#endif
}

void Maximo(std::atomic<uint64_t>& destino, uint64_t valor) {
  uint64_t previo = destino.load(std::memory_order_relaxed);
  while (valor > previo && !destino.compare_exchange_weak(previo, valor, std::memory_order_relaxed)) {
  }
}

uint32_t Leer32(const uint8_t* base, uint32_t direccion) {  // direccion < 0xE0000000: sin desplazamiento fisico
  uint32_t v = 0;
  std::memcpy(&v, base + direccion, sizeof(v));
  return __builtin_bswap32(v);
}

// Whether someone is waiting is checked with the lock held (as in nfsmw_espera_anillo.cpp): without the
// lock, each side may not yet see what the other wrote and the notification is lost; here it would cost
// the maximum time, a stutter of up to 1 ms.
void Avisar() {
  g_ultimo_aviso_ns.store(AhoraNs(), std::memory_order_relaxed);
  g_cambios.fetch_add(1, std::memory_order_acq_rel);
  bool avisar;
  {
    std::lock_guard<std::mutex> cerrojo(g_mutex);
    avisar = g_esperando.load(std::memory_order_acquire) > 0;
  }
  if (avisar) {
    g_cv.notify_all();
  }
}

// Sleeps until the flag stops being 'valor', until a notification or at most the maximum time. Returns
// the nanoseconds slept; 'por_aviso' says whether it ended because the condition was met (notification
// or flag already changed) rather than by using up the timeout.
uint64_t Esperar(const uint8_t* base, uint32_t valor, bool& por_aviso) {
  por_aviso = true;
  const uint32_t visto = g_cambios.load(std::memory_order_acquire);
  if (Leer32(base, kBandera) != valor) {
    return 0;
  }
  const auto antes = std::chrono::steady_clock::now();
  const int64_t plazo_us = REXCVAR_GET(nfsmw_espera_fotograma_max_us);
  {
    std::unique_lock<std::mutex> cerrojo(g_mutex);
    g_esperando.fetch_add(1, std::memory_order_acq_rel);
    // wait_for with a predicate returns false only if the timeout expired with the predicate still unmet.
    por_aviso = g_cv.wait_for(cerrojo, std::chrono::microseconds(plazo_us),
                              [base, valor, visto] {
                                return g_cambios.load(std::memory_order_acquire) != visto ||
                                       Leer32(base, kBandera) != valor;
                              });
    g_esperando.fetch_sub(1, std::memory_order_acq_rel);
  }
  const auto despues = std::chrono::steady_clock::now();
  const uint64_t dormido =
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(despues - antes).count());
  // Wakeup delay: from the notification if there was one; if the timeout expired, how far past it.
  const int64_t ahora_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               despues.time_since_epoch())
                               .count();
  if (por_aviso && g_cambios.load(std::memory_order_acquire) != visto) {
    AnotarDespertar(ahora_ns - g_ultimo_aviso_ns.load(std::memory_order_relaxed));
  } else if (!por_aviso) {
    AnotarDespertar(int64_t(dormido) - plazo_us * 1000);
  }
  return dormido;
}

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
  const uint64_t esperas_p = g_esperas_preparador.exchange(0);
  const uint64_t esperas_e = g_esperas_ejecutor.exchange(0);
  NFSMW_INFORME_DIFERIDO("[espera_fotograma] ultimos 10 s: preparador {} esperas ({} por aviso, {:.1f} ms durmiendo, "
              "peor {:.2f} ms), ejecutor {} esperas ({} por aviso, {:.1f} ms durmiendo, peor {:.2f} ms); "
              "plazo {} us",
              esperas_p, g_avisos_preparador.exchange(0), double(g_ns_preparador.exchange(0)) / 1e6,
              double(g_ns_max_preparador.exchange(0)) / 1e6, esperas_e, g_avisos_ejecutor.exchange(0),
              double(g_ns_ejecutor.exchange(0)) / 1e6, double(g_ns_max_ejecutor.exchange(0)) / 1e6,
              REXCVAR_GET(nfsmw_espera_fotograma_max_us));
  // The executor sleeps while waiting for commands instead of spinning.
  NFSMW_INFORME_DIFERIDO("[espera_fotograma] ejecutor sin ordenes: {} esperas, {} pausas de {} us, {:.1f} ms durmiendo "
              "(antes, dando vueltas)",
              g_sin_ordenes_esperas.exchange(0), g_sin_ordenes_pausas.exchange(0),
              REXCVAR_GET(nfsmw_ejecutor_pausa_us), double(g_sin_ordenes_ns.exchange(0)) / 1e6);
  // What decides whether nfsmw_relevo_prioridad helps. See g_despertar_*.
  const uint64_t n = g_despertar_n.exchange(0);
  const uint64_t ns = g_despertar_ns.exchange(0);
  NFSMW_INFORME_DIFERIDO("[espera_fotograma] despertar: medio {:.3f} ms, peor {:.2f} ms, {} TARDIOS (mas de 3 ms) de {}; "
              "prioridad del relevo {:#x}",
              n ? double(ns) / 1e6 / double(n) : 0.0, double(g_despertar_max_ns.exchange(0)) / 1e6,
              g_despertar_tardios.exchange(0), n, uint32_t(REXCVAR_GET(nfsmw_relevo_prioridad)));
}

}  // namespace

// The game's Sleep(ms): li r4,0 and a jump to sub_8262F258. The two handoff loops only check the flag on
// return.
REX_EXTERN(__imp__sub_8262D988);
REX_HOOK_RAW(sub_8262D988) {
  static const bool activo = REXCVAR_GET(nfsmw_espera_fotograma_bloqueante);
  if (activo && ctx.r3.u32 == 0) {
    const uint32_t retorno = uint32_t(ctx.lr);
    if (retorno == kRetornoPreparador) {
      thread_local bool prioridad_puesta = false;
      SubirPrioridadUnaVez(prioridad_puesta, "preparador");
      bool por_aviso = false;
      const uint64_t ns = Esperar(base, 1, por_aviso);
      g_esperas_preparador.fetch_add(1, std::memory_order_relaxed);
      g_ns_preparador.fetch_add(ns, std::memory_order_relaxed);
      if (por_aviso) {
        g_avisos_preparador.fetch_add(1, std::memory_order_relaxed);
      }
      Maximo(g_ns_max_preparador, ns);
      nfsmw::esperas::Sumar(nfsmw::esperas::kRelevoPreparador, ns);
      Informe();
      ctx.r3.u64 = 0;
      return;
    }
    if (retorno == kRetornoEjecutor) {
      thread_local bool prioridad_puesta = false;
      SubirPrioridadUnaVez(prioridad_puesta, "ejecutor (Main XThread)");
      bool por_aviso = false;
      const uint64_t ns = Esperar(base, 0, por_aviso);
      g_esperas_ejecutor.fetch_add(1, std::memory_order_relaxed);
      g_ns_ejecutor.fetch_add(ns, std::memory_order_relaxed);
      if (por_aviso) {
        g_avisos_ejecutor.fetch_add(1, std::memory_order_relaxed);
      }
      Maximo(g_ns_max_ejecutor, ns);
      nfsmw::esperas::Sumar(nfsmw::esperas::kRelevoEjecutor, ns);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_8262D988(ctx, base);
}

/*
 * During stutters the preparer slept almost half of the time in here (sub_826E8EE8 <- sub_8245DE88 <-
 * sub_82285F78 <- sub_823AFF80), in a loop of KeWaitForSingleObject and Sleep on an object with virtual
 * calls (states 1 and 7). Measurement only: how long it lasts, the object's vtable and the caller, for
 * the "[tiron]" line. It changes nothing of what it does.
 */
REX_EXTERN(__imp__sub_826E8EE8);
REX_HOOK_RAW(sub_826E8EE8) {
  const uint32_t objeto = ctx.r3.u32;
  const uint32_t llamante = uint32_t(ctx.lr);
  const auto antes = std::chrono::steady_clock::now();
  __imp__sub_826E8EE8(ctx, base);
  const uint64_t ns = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - antes).count());
  nfsmw::esperas::Sumar(nfsmw::esperas::kJuegoMedio, ns);
  if (objeto) {
    nfsmw::esperas::g_vtabla_medio.store(Leer32(base, objeto), std::memory_order_relaxed);
  }
  nfsmw::esperas::g_llamante_medio.store(llamante, std::memory_order_relaxed);
}

/*
 * The executor spun without sleeping while waiting for commands.
 *
 * Stack sampling in a race: the Main XThread spent 12.6 % of a core in the code of sub_82441CC8 itself (with LTO
 * it has sub_823C83F8 inlined). It is this loop (loc_82441D6C):
 *
 *     done = list[+12] == 0 || (list[+8] != 0 && list[+0] == list[+4]);
 *     while (!done) { sub_823C83F8(list, list[+0] - list[+4]); ...recompute done... }
 *
 * list = 0x82909650: +0 commands written by the preparer, +4 commands executed, +8 "the preparer has closed
 * the list", +12 list open. While the preparer is still writing (+8 == 0) and there are no new commands
 * (+0 == +4), sub_823C83F8 is called with 0 commands, returns at once, and is called again, without sleeping.
 * On the Xbox 360 that was a hardware thread; here it is one core out of 3, and the executor runs at 0x3A,
 * where Horizon does not time-slice: it does not give the core up to the game threads (0x3B) until it moves
 * elsewhere.
 *
 * Here, only in that call (return address 0x82441DC4) and only with 0 commands: it sleeps in short pauses
 * while the list is still open, not closed and without new commands, for at most
 * nfsmw_ejecutor_espera_max_us. Then the original is called as before (with 0 commands it does nothing), and
 * the game's loop checks the conditions again.
 */
constexpr uint32_t kRetornoOrdenes = 0x82441DC4;
REX_EXTERN(__imp__sub_823C83F8);
REX_HOOK_RAW(sub_823C83F8) {
  static const bool activo = REXCVAR_GET(nfsmw_ejecutor_sin_vueltas);
  if (activo && uint32_t(ctx.lr) == kRetornoOrdenes && ctx.r4.u32 == 0) {
    const uint32_t lista = ctx.r3.u32;
    const auto sin_ordenes = [base, lista] {
      return Leer32(base, lista + 12) != 0 && base[lista + 8] == 0 && Leer32(base, lista) == Leer32(base, lista + 4);
    };
    if (sin_ordenes()) {
      const auto antes = std::chrono::steady_clock::now();
      const auto pausa = std::chrono::microseconds(std::max(REXCVAR_GET(nfsmw_ejecutor_pausa_us), 10));
      const auto limite = antes + std::chrono::microseconds(std::max(REXCVAR_GET(nfsmw_ejecutor_espera_max_us), 50));
      uint64_t pausas = 0;
      do {
        rex::thread::Sleep(pausa);
        ++pausas;
      } while (sin_ordenes() && std::chrono::steady_clock::now() < limite);
      g_sin_ordenes_esperas.fetch_add(1, std::memory_order_relaxed);
      g_sin_ordenes_pausas.fetch_add(pausas, std::memory_order_relaxed);
      const uint64_t ns_sin_ordenes = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now() - antes)
                                                   .count());
      g_sin_ordenes_ns.fetch_add(ns_sin_ordenes, std::memory_order_relaxed);
      // Also in the stutter frame ("[tiron] juego" line, nfsmw_esperas_tiron.h).
      nfsmw::esperas::Sumar(nfsmw::esperas::kEjecutorSinOrdenes, ns_sin_ordenes);
    }
  }
#if REX_PLATFORM_ANDROID && !defined(NFSC_RECOMP)
  // Acquire the producer's count before the guest executor reads command bytes.
  constexpr uint32_t lista = 0x82909650;
  const uint32_t publicadas = nfsmw::ordenes::Publicadas(base + lista);
  const uint32_t hechas = Leer32(base, lista + 4);
  ctx.r4.u64 = std::min(ctx.r4.u32, publicadas - hechas);
#endif
  __imp__sub_823C83F8(ctx, base);
}

#if REX_PLATFORM_ANDROID && !defined(NFSC_RECOMP)
REX_EXTERN(__imp__sub_823C8378);
REX_HOOK_RAW(sub_823C8378) {
  constexpr uint32_t lista = 0x82909650;
  if (Leer32(base, lista + 12) == 0) {
    __imp__sub_823C8378(ctx, base);  // Closed list: original immediate dispatch.
    return;
  }
  const uint32_t bytes = (ctx.r6.u32 + 15) & ~15u;
  const uint32_t entrada = Leer32(base, lista + 20);
  std::memmove(base + entrada, base + ctx.r4.u32, bytes);
  nfsmw::ordenes::Escribir(base + entrada, ctx.r5.u32);
  nfsmw::ordenes::Escribir(base + entrada + 4, bytes);
  nfsmw::ordenes::Escribir(base + lista + 20, entrada + bytes);
  // Release publishes the entry and tail together, instead of relying on ARM store ordering.
  nfsmw::ordenes::Publicar(base + lista, nfsmw::ordenes::Publicadas(base + lista) + 1);
  ctx.r3.u64 = entrada;
}
#endif

// Right after the flag is set to 1 (sub_82442058).
// Also how long the preparer takes to fill the list (the whole call) and the time spent outside it between two
// fills (its simulation plus its handoff wait), for the "[tiron] juego" line. Once per frame.
REX_EXTERN(__imp__sub_82445660);
REX_HOOK_RAW(sub_82445660) {
  Avisar();
  static int64_t fin_anterior_ns = 0;  // only called by the thread that prepares the frames
  const int64_t inicio_ns = AhoraNs();
  if (fin_anterior_ns != 0) {
    nfsmw::esperas::Sumar(nfsmw::esperas::kPreparadorFuera, uint64_t(inicio_ns - fin_anterior_ns));
  }
  __imp__sub_82445660(ctx, base);
  fin_anterior_ns = AhoraNs();
  nfsmw::esperas::Sumar(nfsmw::esperas::kPreparadorLista, uint64_t(fin_anterior_ns - inicio_ns));
}

// Right after it is set to 0 (sub_82441CC8); sub_82441F18 also calls it, and an extra notification does not matter.
REX_EXTERN(__imp__sub_8262DE18);
REX_HOOK_RAW(sub_8262DE18) {
  Avisar();
  __imp__sub_8262DE18(ctx, base);
}
