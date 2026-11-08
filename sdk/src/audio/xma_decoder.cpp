/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>
#include <rex/string/buffer.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>

extern "C" {
#include "libavutil/cpu.h"
#include "libavutil/log.h"
}  // extern "C"

REXCVAR_DEFINE_BOOL(ffmpeg_verbose, false, "Audio", "Verbose FFmpeg output (debug and above)");
REXCVAR_DEFINE_BOOL(audio_ffmpeg_simd, true, "Audio",
                    "Diagnostico: false = el decodificador XMA de FFmpeg usa solo codigo C, sin "
                    "NEON ni SSE (para comparar el audio)");
REXCVAR_DECLARE(bool, audio_diag_prioridad_critica);  // definida en audio_system.cpp
/*
 * On by default only on the Switch, like the server priority and the frame wait: there the game's
 * audio server thread runs at 0x2D, below the GPU ring (0x2C), and the ring preempts it in the middle
 * of decoding: 17.3 % of a core and peaks of 14.39 ms with a 10.7 ms buffer cushion (console). The
 * worker runs at 0x2B, above the ring.
 * Validated on the PC: the game thread goes from 167-302 ms to 10.7-16.1 ms per 10 s, without decoding
 * a single context, and the data is ready in 14-16 us on average (0.51 ms worst case), far below the
 * 5.3 ms of an audio block. 0 differences from the recompiled code.
 */
#if REX_PLATFORM_SWITCH
#define REX_XMA_EN_TRABAJADOR_POR_DEFECTO 1
#elif REX_PLATFORM_ANDROID || REX_PLATFORM_GNU_LINUX
// PARCHE LOCAL (Linux desktop): with 0, music, police radio and menu sounds come out robotic there.
#define REX_XMA_EN_TRABAJADOR_POR_DEFECTO 2
#else
#define REX_XMA_EN_TRABAJADOR_POR_DEFECTO 0
#endif
REXCVAR_DEFINE_INT32(audio_xma_en_trabajador, REX_XMA_EN_TRABAJADOR_POR_DEFECTO, "Audio",
                     "Quien descodifica el XMA al llegar un kick (XMAEnableContext): 0 = el hilo que lo pide, "
                     "como hasta ahora; 1 = el hilo XMA Decoder, que en la Switch va a 0x2B y no lo preempta "
                     "el anillo de la GPU; 2 = el trabajador, y el hilo que pide el kick solo termina los "
                     "contextos que el trabajador no haya cogido (para el PC, donde no hay esas prioridades)");

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal XmaDecoder handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc
//
// XMA details:
// https://devel.nuclex.org/external/svn/directx/trunk/include/xma2defs.h
// https://github.com/gdawg/fsbext/blob/master/src/xma_header.h
//
// XAudio2 uses XMA under the covers, and seems to map with the same
// restrictions of frame/subframe/etc:
// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.xaudio2.xaudio2_buffer(v=vs.85).aspx
//
// XMA contexts are 64b in size and tight bitfields. They are in physical
// memory not usually available to games. Games will use MmMapIoSpace to get
// the 64b pointer in user memory so they can party on it. If the game doesn't
// do this, it's likely they are either passing the context to XAudio or
// using the XMA* functions.

namespace rex::audio {

XmaDecoder::XmaDecoder(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()), function_dispatcher_(function_dispatcher) {}

XmaDecoder::~XmaDecoder() = default;

void av_log_callback(void* avcl, int level, const char* fmt, va_list va) {
  if (!REXCVAR_GET(ffmpeg_verbose) && level > AV_LOG_WARNING) {
    return;
  }

  string::StringBuffer buff;
  buff.AppendVarargs(fmt, va);
  auto msg = buff.to_string_view();

  switch (level) {
    case AV_LOG_ERROR:
      REXAPU_ERROR("ffmpeg: {}", msg);
      break;
    case AV_LOG_WARNING:
      REXAPU_WARN("ffmpeg: {}", msg);
      break;
    case AV_LOG_INFO:
      REXAPU_INFO("ffmpeg: {}", msg);
      break;
    case AV_LOG_VERBOSE:
    case AV_LOG_DEBUG:
    default:
      REXAPU_DEBUG("ffmpeg: {}", msg);
      break;
  }
}

X_STATUS XmaDecoder::Setup(system::KernelState* kernel_state) {
  // Setup ffmpeg logging callback
  av_log_set_callback(av_log_callback);
  // Diagnostic (audio_ffmpeg_simd): before opening any codec, make FFmpeg's function tables (FFT,
  // MDCT, float_dsp) be chosen without SIMD.
  if (!REXCVAR_GET(audio_ffmpeg_simd)) {
    av_force_cpu_flags(0);
    REXAPU_INFO("XMA: FFmpeg sin SIMD (audio_ffmpeg_simd = false)");
  }

  // Register APU/XMA MMIO handlers
  // XMA registers are at 0x7FEA0000-0x7FEAFFFF
  memory()->AddVirtualMappedRange(
      0x7FEA0000,  // base address
      0xFFFF0000,  // mask
      0x0000FFFF,  // size (64KB)
      this,        // context (XmaDecoder*)
      reinterpret_cast<runtime::MMIOReadCallback>(MMIOReadRegisterThunk),
      reinterpret_cast<runtime::MMIOWriteCallback>(MMIOWriteRegisterThunk));
  REXAPU_DEBUG("XMA: Registered MMIO handlers at 0x7FEA0000-0x7FEAFFFF");

  // Setup XMA context data.
  // The Xbox 360 kernel allocates the contexts with X_PAGE_NOCACHE |
  // X_PAGE_READWRITE and writes MmGetPhysicalAddress for the address to the
  // register.
  context_data_first_ptr_ = memory()->SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA) * kContextCount, 256,
                                                      memory::kSystemHeapPhysical);
  context_data_last_ptr_ = context_data_first_ptr_ + (sizeof(XMA_CONTEXT_DATA) * kContextCount - 1);
  register_file_[XmaRegister::ContextArrayAddress] =
      memory()->GetPhysicalAddress(context_data_first_ptr_);

  // Setup XMA contexts.
  for (size_t i = 0; i < kContextCount; ++i) {
    uint32_t guest_ptr = context_data_first_ptr_ + i * sizeof(XMA_CONTEXT_DATA);
    XmaContext& context = contexts_[i];
    if (context.Setup(i, memory(), guest_ptr)) {
      assert_always();
    }
  }
  register_file_[XmaRegister::NextContextIndex] = 1;
  context_bitmap_.Resize(kContextCount);

  worker_running_ = true;
  work_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(work_event_);
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("XMA Decoder");

  worker_thread_->Create();
#if REX_PLATFORM_WIN32
  // Diagnostic audio_diag_prioridad_critica: as on the Switch, above the game threads
  // (THREAD_PRIORITY_TIME_CRITICAL = 15).
  if (REXCVAR_GET(audio_diag_prioridad_critica) && worker_thread_->thread()) {
    worker_thread_->thread()->set_priority(15);
  }
#endif

  return X_STATUS_SUCCESS;
}

/*
 * Measuring the XMA cost on the calling thread.
 *
 * The kick (XMAEnableContext) decodes right here, on the thread that requests it (in NFSMW, the game's
 * audio server thread), and the lock (XMADisableContext) waits for the context lock if the worker
 * holds it. On the PC, sub_825E1CD0, which does both once per voice, takes 15 % of that thread's CPU,
 * with peaks of 22 ms in 500 ms windows.
 *
 * This changes nothing: every 10 s it logs how many kicks and locks there were, how many contexts and
 * how much time went into them, so decisions can be based on console numbers.
 */
namespace {

struct MedidaXma {
  std::atomic<uint64_t> kicks{0};
  std::atomic<uint64_t> kick_contextos{0};
  std::atomic<uint64_t> kick_ns{0};
  std::atomic<uint64_t> kick_max_ns{0};
  std::atomic<uint64_t> locks{0};
  std::atomic<uint64_t> lock_contextos{0};
  std::atomic<uint64_t> lock_ns{0};
  std::atomic<uint64_t> lock_max_ns{0};
  std::atomic<int64_t> ultimo_ms{0};
  // Work split (audio_xma_en_trabajador).
  std::atomic<uint64_t> contextos_trabajador{0};  // decoded by the XMA Decoder thread
  std::atomic<uint64_t> contextos_respaldo{0};    // the ones the kick thread had to finish
  std::atomic<uint64_t> trabajo_ns{0};            // time the worker spends decoding
  std::atomic<uint64_t> trabajo_max_ns{0};
  std::atomic<uint64_t> latencia_ns{0};   // from the kick until the context is decoded
  std::atomic<uint64_t> latencia_max_ns{0};
  std::atomic<uint64_t> latencia_n{0};
};

MedidaXma g_medida_xma;

/*
 * Contexts a kick has just enabled that the worker has not served yet: 320 bits in 10 words.
 * Marking is one atomic operation and walking them takes no lock, so the game thread pays almost
 * nothing to notify. The kick time is stored to measure the latency until the data is ready.
 */
constexpr uint32_t kPalabrasPendientes = 10;  // 320 contextos / 32
std::atomic<uint32_t> g_pendientes[kPalabrasPendientes];
std::atomic<int64_t> g_pendiente_desde_ns{0};

int64_t AhoraNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

void MarcarPendiente(uint32_t context_id) {
  g_pendientes[context_id / 32].fetch_or(1u << (context_id % 32), std::memory_order_release);
}

// Returns true if this thread takes the context; only one can.
bool TomarPendiente(uint32_t context_id) {
  const uint32_t bit = 1u << (context_id % 32);
  return (g_pendientes[context_id / 32].fetch_and(~bit, std::memory_order_acquire) & bit) != 0;
}

int64_t AhoraMsXma() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t DesdeNs(const std::chrono::steady_clock::time_point& antes) {
  using namespace std::chrono;
  return uint64_t(duration_cast<nanoseconds>(steady_clock::now() - antes).count());
}

void MaximoXma(std::atomic<uint64_t>& maximo, uint64_t valor) {
  uint64_t previo = maximo.load(std::memory_order_relaxed);
  while (valor > previo && !maximo.compare_exchange_weak(previo, valor, std::memory_order_relaxed)) {
  }
}

void InformarXma() {
  const int64_t ahora = AhoraMsXma();
  int64_t ultimo = g_medida_xma.ultimo_ms.load(std::memory_order_relaxed);
  if (ultimo == 0) {
    g_medida_xma.ultimo_ms.compare_exchange_strong(ultimo, ahora, std::memory_order_relaxed);
    return;
  }
  if (ahora - ultimo < 10000 ||
      !g_medida_xma.ultimo_ms.compare_exchange_strong(ultimo, ahora, std::memory_order_relaxed)) {
    return;
  }
  const uint64_t kicks = g_medida_xma.kicks.exchange(0, std::memory_order_relaxed);
  const uint64_t kick_contextos = g_medida_xma.kick_contextos.exchange(0, std::memory_order_relaxed);
  const uint64_t kick_ns = g_medida_xma.kick_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t kick_max = g_medida_xma.kick_max_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t locks = g_medida_xma.locks.exchange(0, std::memory_order_relaxed);
  const uint64_t lock_contextos = g_medida_xma.lock_contextos.exchange(0, std::memory_order_relaxed);
  const uint64_t lock_ns = g_medida_xma.lock_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t lock_max = g_medida_xma.lock_max_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t del_trabajador = g_medida_xma.contextos_trabajador.exchange(0, std::memory_order_relaxed);
  const uint64_t del_respaldo = g_medida_xma.contextos_respaldo.exchange(0, std::memory_order_relaxed);
  const uint64_t trabajo_ns = g_medida_xma.trabajo_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t trabajo_max = g_medida_xma.trabajo_max_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t latencia_ns = g_medida_xma.latencia_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t latencia_max = g_medida_xma.latencia_max_ns.exchange(0, std::memory_order_relaxed);
  const uint64_t latencia_n = g_medida_xma.latencia_n.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[xma] en el hilo que llama, {:.1f} s: kicks {} con {} contextos descodificados, {:.1f} ms (maximo "
              "{:.2f} ms); locks {} con {} contextos, {:.1f} ms esperando (maximo {:.2f} ms)",
              double(ahora - ultimo) / 1000.0, kicks, kick_contextos, double(kick_ns) / 1e6, double(kick_max) / 1e6,
              locks, lock_contextos, double(lock_ns) / 1e6, double(lock_max) / 1e6);
  REXLOG_INFO("[xma] reparto (modo {}): trabajador {} contextos en {:.1f} ms (maximo {:.2f} ms), respaldo del hilo "
              "del kick {}; del kick al dato listo media {:.0f} us y maxima {:.2f} ms ({} medidas)",
              REXCVAR_GET(audio_xma_en_trabajador), del_trabajador, double(trabajo_ns) / 1e6,
              double(trabajo_max) / 1e6, del_respaldo,
              latencia_n ? double(latencia_ns) / double(latencia_n) / 1e3 : 0.0, double(latencia_max) / 1e6,
              latencia_n);
}

}  // namespace

void XmaDecoder::WorkerThreadMain() {
  while (worker_running_) {
    // Okay, let's loop through XMA contexts to find ones we need to decode!
    bool did_work = false;
    // First the ones a kick has just enabled: they are reached without walking all 320 or touching their
    // locks.
    bool habia_pendientes = false;
    for (uint32_t palabra = 0; palabra < kPalabrasPendientes && worker_running_; ++palabra) {
      uint32_t bits = g_pendientes[palabra].load(std::memory_order_acquire);
      uint32_t indice = 0;
      while (rex::bit_scan_forward(bits, &indice)) {
        const uint32_t n = palabra * 32 + indice;
        bits &= ~(1u << indice);
        if (!TomarPendiente(n)) {
          continue;  // the kick thread took it (mode 2)
        }
        habia_pendientes = true;
        const auto antes = std::chrono::steady_clock::now();
        XmaContext& context = contexts_[n];
        const bool worked = context.Work();
        const uint64_t ns = DesdeNs(antes);
        if (worked) {
          context.SignalWorkDone();
          PROFILE_XMA_FRAME_DECODED();
          g_medida_xma.contextos_trabajador.fetch_add(1, std::memory_order_relaxed);
          g_medida_xma.trabajo_ns.fetch_add(ns, std::memory_order_relaxed);
          MaximoXma(g_medida_xma.trabajo_max_ns, ns);
        }
        did_work = did_work || worked;
      }
    }
    if (habia_pendientes) {
      // From the kick to the data being written: the latency the game could notice.
      const int64_t desde = g_pendiente_desde_ns.load(std::memory_order_acquire);
      if (desde) {
        const uint64_t latencia = uint64_t(AhoraNs() - desde);
        g_medida_xma.latencia_ns.fetch_add(latencia, std::memory_order_relaxed);
        g_medida_xma.latencia_n.fetch_add(1, std::memory_order_relaxed);
        MaximoXma(g_medida_xma.latencia_max_ns, latencia);
      }
    }
    // Full sweep as a safety net: it is how it used to work, and it serves any context left enabled
    // without a mark. Only when there were no pending ones, so the 320 locks are not walked after every
    // kick and the fast loop keeps the work being counted.
    if (!habia_pendientes) {
      for (uint32_t n = 0; n < kContextCount && worker_running_; n++) {
        XmaContext& context = contexts_[n];
        bool worked = context.Work();
        if (worked) {
          context.SignalWorkDone();
          PROFILE_XMA_FRAME_DECODED();
        }
        did_work = did_work || worked;
      }
    }

    if (paused_) {
      pause_fence_.Signal();
      resume_fence_.Wait();
    }

    if (did_work) {
      continue;
    }
    // No work done this iteration, block until signaled.
    rex::thread::Wait(work_event_.get(), false);
  }
}

void XmaDecoder::Shutdown() {
  if (!worker_thread_) {
    return;
  }

  worker_running_ = false;

  if (work_event_) {
    work_event_->Set();
  }

  if (paused_) {
    Resume();
  }

  // Wait up to 2 seconds for worker thread to exit gracefully.
  auto result = rex::thread::Wait(worker_thread_->thread(), false, std::chrono::milliseconds(2000));
  if (result == rex::thread::WaitResult::kTimeout) {
    REXAPU_WARN("XMA: Worker thread did not exit within 2s, abandoning");
  }
  worker_thread_.reset();

  if (context_data_first_ptr_) {
    memory()->SystemHeapFree(context_data_first_ptr_);
  }

  context_data_first_ptr_ = 0;
  context_data_last_ptr_ = 0;
}

int XmaDecoder::GetContextId(uint32_t guest_ptr) {
  static_assert_size(XMA_CONTEXT_DATA, 64);
  if (guest_ptr < context_data_first_ptr_ || guest_ptr > context_data_last_ptr_) {
    return -1;
  }
  assert_zero(guest_ptr & 0x3F);
  return (guest_ptr - context_data_first_ptr_) >> 6;
}

uint32_t XmaDecoder::AllocateContext() {
  size_t index = context_bitmap_.Acquire();
  if (index == -1) {
    // Out of contexts.
    return 0;
  }

  XmaContext& context = contexts_[index];
  assert_false(context.is_allocated());
  context.set_is_allocated(true);
  return context.guest_ptr();
}

void XmaDecoder::ReleaseContext(uint32_t guest_ptr) {
  auto context_id = GetContextId(guest_ptr);
  assert_true(context_id >= 0);

  XmaContext& context = contexts_[context_id];
  assert_true(context.is_allocated());
  context.Release();
  context_bitmap_.Release(context_id);
}

bool XmaDecoder::BlockOnContext(uint32_t guest_ptr, bool poll) {
  auto context_id = GetContextId(guest_ptr);
  assert_true(context_id >= 0);

  XmaContext& context = contexts_[context_id];
  return context.Block(poll);
}

uint32_t XmaDecoder::ReadRegister(uint32_t addr) {
  auto r = (addr & 0xFFFF) / 4;

  assert_true(r < XmaRegisterFile::kRegisterCount);

  switch (r) {
    case XmaRegister::ContextArrayAddress:
      break;
    case XmaRegister::CurrentContextIndex: {
      // 0606h (1818h) is rotating context processing # set to hardware ID of
      // context being processed.
      // If bit 200h is set, the locking code will possibly collide on hardware
      // IDs and error out, so we should never set it (I think?).
      uint32_t& current_context_index = register_file_[XmaRegister::CurrentContextIndex];
      uint32_t& next_context_index = register_file_[XmaRegister::NextContextIndex];
      // To prevent games from seeing a stuck XMA context, return a rotating
      // number.
      current_context_index = next_context_index;
      next_context_index = (next_context_index + 1) % kContextCount;
      break;
    }
    default:
      const auto register_info = register_file_.GetRegisterInfo(r);
      if (register_info) {
        REXAPU_DEBUG("XMA: Read from unhandled register ({:04X}, {})", r, register_info->name);
      } else {
        REXAPU_DEBUG("XMA: Read from unknown register ({:04X})", r);
      }
      break;
  }

  return rex::byte_swap(register_file_[r]);
}

void XmaDecoder::WriteRegister(uint32_t addr, uint32_t value) {
  SCOPE_profile_cpu_f("apu");

  uint32_t r = (addr & 0xFFFF) / 4;
  value = rex::byte_swap(value);

  assert_true(r < XmaRegisterFile::kRegisterCount);
  register_file_[r] = value;

  if (r >= XmaRegister::Context0Kick && r <= XmaRegister::Context9Kick) {
    // Context kick command.
    // This will kick off the given hardware contexts.
    // Basically, this kicks the SPU and says "hey, decode that audio!"
    // XMAEnableContext

    // The context ID is a bit in the range of the entire context array.
    uint32_t base_context_id = (r - XmaRegister::Context0Kick) * 32;
    uint32_t kicked_value = value;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        context.Enable();
      }
    }
    /*
     * Who decodes (measured on the console).
     *
     * The thread requesting the kick used to decode right here, because waiting for the worker's sweep
     * stalled the audio for 50-100 ms. But on the console that thread is the game's audio server, it
     * runs at 0x2D (below the GPU ring, 0x2C) and the ring preempts it in the middle of decoding:
     * 1,555 kicks/s, 17.3 % of a core and peaks of 14.39 ms, when the buffer cushion is 10.7 ms.
     *
     * The worker runs at 0x2B, above the ring. When notified before anything is decoded, on the Switch
     * it starts immediately (it preempts the kick requester) and the game, which does not wait for
     * notifications but polls the context sleeping 50 us per iteration, finds the data already written.
     */
    const int32_t modo_trabajador = REXCVAR_GET(audio_xma_en_trabajador);
    const auto kick_antes = std::chrono::steady_clock::now();
    uint64_t kick_contextos = 0;
    if (modo_trabajador != 0) {
      uint32_t marcados = kicked_value;
      for (int i = 0; marcados && i < 32; ++i, marcados >>= 1) {
        if (marcados & 1) {
          MarcarPendiente(base_context_id + i);
        }
      }
      g_pendiente_desde_ns.store(AhoraNs(), std::memory_order_release);
      work_event_->Set();
    }
    for (int i = 0; kicked_value && i < 32; ++i, kicked_value >>= 1) {
      if (kicked_value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        // Mode 1: the worker does everything. Mode 2: only what the worker has not already taken is
        // finished here, without waiting for it (useful on the PC, where it has priority over nobody).
        if (modo_trabajador == 1) {
          continue;
        }
        if (modo_trabajador == 2 && !TomarPendiente(context_id)) {
          continue;  // the worker has taken it
        }
        ++kick_contextos;
        if (modo_trabajador != 0) {
          g_medida_xma.contextos_respaldo.fetch_add(1, std::memory_order_relaxed);
        }
        if (context.Work()) {
          context.SignalWorkDone();
        }
      }
    }
    // Measurement only: how much decoding right here costs the thread requesting the kick.
    const uint64_t kick_ns = DesdeNs(kick_antes);
    g_medida_xma.kicks.fetch_add(1, std::memory_order_relaxed);
    g_medida_xma.kick_contextos.fetch_add(kick_contextos, std::memory_order_relaxed);
    g_medida_xma.kick_ns.fetch_add(kick_ns, std::memory_order_relaxed);
    MaximoXma(g_medida_xma.kick_max_ns, kick_ns);
    if (modo_trabajador == 0) {
      // With the worker already notified above, repeating the notification leaves the (auto-reset) event
      // signaled after its iteration and costs a full sweep of the 320 contexts per kick, 1,555 times per
      // second.
      work_event_->Set();
    }
    InformarXma();
  } else if (r >= XmaRegister::Context0Lock && r <= XmaRegister::Context9Lock) {
    // Context lock command.
    // This requests a lock by flagging the context.
    // XMADisableContext
    uint32_t base_context_id = (r - XmaRegister::Context0Lock) * 32;
    const auto lock_antes = std::chrono::steady_clock::now();
    uint64_t lock_contextos = 0;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        auto& context = contexts_[context_id];
        ++lock_contextos;
        context.Disable();
        // [XMA fix] Added Block(false) after Disable(). Without this, the game
        // could call XMADisableContext and start modifying the context struct
        // while a decode was still in progress on the worker thread. Block()
        // waits for the context mutex to be free (poll=false means wait, not spin).
        context.Block(false);
      }
    }
    // Measurement only: how long the thread requesting the lock waits for the worker to release the
    // context.
    const uint64_t lock_ns = DesdeNs(lock_antes);
    g_medida_xma.locks.fetch_add(1, std::memory_order_relaxed);
    g_medida_xma.lock_contextos.fetch_add(lock_contextos, std::memory_order_relaxed);
    g_medida_xma.lock_ns.fetch_add(lock_ns, std::memory_order_relaxed);
    MaximoXma(g_medida_xma.lock_max_ns, lock_ns);
    InformarXma();
    // Signal the decoder thread to start processing.
    // work_event_->Set();
  } else if (r >= XmaRegister::Context0Clear && r <= XmaRegister::Context9Clear) {
    // Context clear command.
    // This will reset the given hardware contexts.
    uint32_t base_context_id = (r - XmaRegister::Context0Clear) * 32;
    for (int i = 0; value && i < 32; ++i, value >>= 1) {
      if (value & 1) {
        uint32_t context_id = base_context_id + i;
        XmaContext& context = contexts_[context_id];
        context.Clear();
      }
    }
  } else {
    // 0601h (1804h) is written to with 0x02000000 and 0x03000000 around a lock
    // operation
    switch (r) {
      default: {
        const auto register_info = register_file_.GetRegisterInfo(r);
        if (register_info) {
          REXAPU_DEBUG("XMA: Write to unhandled register ({:04X}, {}): {:08X}", r,
                       register_info->name, value);
        } else {
          REXAPU_DEBUG("XMA: Write to unknown register ({:04X}): {:08X}", r, value);
        }
        break;
      }
#pragma warning(suppress : 4065)
    }
  }
}

void XmaDecoder::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  if (work_event_) {
    work_event_->Set();
  }
  pause_fence_.Wait();
}

void XmaDecoder::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_fence_.Signal();
}

}  // namespace rex::audio
