// nfsmw - native renderer, step C5b (see nfsmw_nativo_ganchos.h).

#include "nfsmw_nativo_ganchos.h"

#include "nfsmw_nativo_shaders.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include "nfsmw_informe_diferido.h"  // deferred reports

#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <unordered_map>
#include <array>  // nfsmw_d3d_vegetacion_juego
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_nativo_sombra_d3d, false, "NFSMW",
                    "Renderizador nativo (24/09, fase 1 del renderizador a nivel de Direct3D): en 1 de cada 64 "
                    "dibujos fotografia el espejo de registros del dispositivo y el anillo lo compara con lo que "
                    "lee de los paquetes (linea «sombra D3D»). No cambia lo que se dibuja")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Phase 2b of the Direct3D-level renderer: see g_dibujo_en_curso and AnotarDibujo below.
REXCVAR_DEFINE_BOOL(nfsmw_d3d_marcador_registro, true, "NFSMW",
                    "Renderizador nativo (25/09, fase 2b del renderizador a nivel de Direct3D): el marcador del "
                    "FlushState de cada Draw* lleva su registro (VS, PS, argumentos) y el anillo lo usa sin la cola "
                    "ni la busqueda de EmparejarDibujo. Empieza comprobando contra la busqueda y se apaga solo al "
                    "primer desacuerdo. false = como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Shadow map vegetation filtered on the game thread (see DecidirVegetacion).
REXCVAR_DEFINE_BOOL(nfsmw_d3d_vegetacion_juego, true, "NFSMW",
                    "Renderizador nativo (25/09, build 184): los DrawVertices y DrawIndexedVertices que el anillo "
                    "tiraria como vegetacion del mapa de sombras (sin color, con prueba de alfa o descarte) se saltan "
                    "enteros en el hilo del juego: sin FlushState, DRAW_INDX ni registro. Empieza mirando (el anillo "
                    "comprueba el veredicto de cada dibujo) y se apaga solo al primer desacuerdo. false = como antes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The settings of the ring's vegetation discard (nfsmw_nativo_dibujos.cpp): the game side checks the
// same ones.
REXCVAR_DECLARE(bool, nfsmw_nativo_ps_solo_alfa);
REXCVAR_DECLARE(int32_t, nfsmw_nativo_ps_solo_alfa_alternar_s);
REXCVAR_DECLARE(bool, nfsmw_sombras_sin_vegetacion);

namespace nfsmw::nativo {
namespace {

// Where SetVertexShader (sub_8259C2A8) and SetPixelShader (sub_8259BDC0) store the bound shader inside
// the device object (from the recompiled code).
#if defined(NFSC_RECOMP)
// NFS Carbon: SetVertexShader (sub_826E4790) and SetPixelShader (sub_826E4350); the inlined FlushState reads the
// same words (sub_826FBB68).
constexpr uint32_t kDispositivoVs = 0x3080;
constexpr uint32_t kDispositivoPs = 0x307C;
#else
constexpr uint32_t kDispositivoVs = 0x4FE8;
constexpr uint32_t kDispositivoPs = 0x3290;
#endif

// One producer (the thread using D3D) and one consumer (the ring thread).
constexpr uint32_t kTamanoCola = uint32_t(1) << 16;

std::atomic<ShadersNativos*> g_shaders{nullptr};

std::mutex g_mutex;
std::unordered_map<uint32_t, const EntradaShader*> g_objetos;
std::atomic<uint64_t> g_generacion{0};
std::atomic<uint64_t> g_creados_vs{0};
std::atomic<uint64_t> g_conocidos_vs{0};
std::atomic<uint64_t> g_creados_ps{0};
std::atomic<uint64_t> g_conocidos_ps{0};
std::atomic<uint32_t> g_avisos{0};

RegistroDibujo g_cola[kTamanoCola];
std::atomic<uint32_t> g_escritura{0};
std::atomic<uint32_t> g_lectura{0};
std::atomic<uint64_t> g_perdidos{0};

uint32_t LeerBE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Phase 1 of the Direct3D-level renderer (see InstantaneaEspejo in the header).
constexpr uint32_t kFotoCada = 64;            // 1 in 64 draws
constexpr uint32_t kFotos = 256;              // snapshots in flight between the game thread and the ring thread
constexpr uint32_t kDispositivoFetch = 0x480;
constexpr uint32_t kDispositivoConstantesVs = 0x780;
constexpr uint32_t kDispositivoConstantesPs = 0x1780;

std::atomic<uint64_t> g_mascara_grupo[kGruposEspejo];
std::atomic<uint32_t> g_desplazamiento_grupo[kGruposEspejo];  // 0 = aun no se ha visto

struct Foto {
  std::atomic<uint64_t> secuencia{0};  // 0 while being written
  InstantaneaEspejo datos;
};
Foto g_fotos[kFotos];
uint64_t g_siguiente_foto = 1;  // game thread only
uint32_t g_contador_fotos = 0;

int GrupoDe(uint32_t registro_base) {
  if (registro_base >= 0x2000 && registro_base < 0x2400 && (registro_base & 0x7F) == 0) {
    return int((registro_base - 0x2000) >> 7);
  }
  return registro_base == 0x4900 ? 8 : -1;
}

/*
 * Phase 2b: the draw record inside the marker.
 * Each Draw* stores its record before calling the original, and its FlushState runs inside the
 * original. With 2b the record does not go straight to the queue: it stays in g_dibujo_en_curso, that
 * FlushState takes it (TomarDibujoEnCurso) and puts it in its marker, and the DRAW_INDX that follows in
 * the ring uses it without a lookup if it accepts it. If the marker cannot carry it (no room, phase 2 off)
 * or nobody takes it, it goes to the queue as usual (EntregarDibujo, TerminarDibujo).
 * Self-checking guard: in the observing phase the record goes both ways (queue and marker in check
 * mode); the draw uses the usual lookup and the ring compares the shaders from the lookup with those from
 * the marker's record (CompararDibujoMarcador). After kComprobacionesDibujo matches, marker only; even so
 * 1 in kComprobarDibujoCada is still checked. At the first difference it is off for the whole session
 * ("[d3d_marcador] registro de dibujo: DIFERENCIA" in the log) and records go back to the queue.
 */
constexpr uint64_t kComprobacionesDibujo = 20000;
constexpr uint64_t kComprobarDibujoCada = 1024;  // potencia de 2
RegistroDibujo g_dibujo_en_curso;  // only the thread using D3D (the Draw* calls and their FlushState)
bool g_dibujo_pendiente = false;
uint64_t g_turno_dibujo = 0;
std::atomic<bool> g_dibujo_apagado{false};
std::atomic<bool> g_dibujo_aplicando{false};
std::atomic<uint64_t> g_dibujo_iguales{0};  // written only by the ring thread
std::atomic<bool> g_dibujo_distinto{false};
// The first disagreement, written by the ring thread before g_dibujo_distinto (release).
uint32_t g_dibujo_que = 0;
RegistroDibujo g_dibujo_busqueda;
RegistroDibujo g_dibujo_marcador;
bool g_dibujo_hay_busqueda = false;
bool g_dibujo_hay_marcador = false;
// Report every 10 s (only the thread using D3D).
uint64_t g_i_dibujos = 0;
uint64_t g_i_en_marcador = 0;
uint64_t g_i_comprobando = 0;
uint64_t g_i_por_cola = 0;
uint64_t g_i_sin_flushstate = 0;
int64_t g_i_siguiente_ms = 0;

bool DibujoEnMarcador() {
  static const bool activo = REXCVAR_GET(nfsmw_d3d_marcador_registro);
  return activo && !g_dibujo_apagado.load(std::memory_order_relaxed);
}

// The usual path: to the ring's queue (if it is full, the record is dropped and counted, as before).
void EncolarDibujo(const RegistroDibujo& registro) {
  const uint32_t escritura = g_escritura.load(std::memory_order_relaxed);
  const uint32_t siguiente = (escritura + 1) & (kTamanoCola - 1);
  if (siguiente == g_lectura.load(std::memory_order_acquire)) {
    g_perdidos.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_cola[escritura] = registro;
  g_escritura.store(siguiente, std::memory_order_release);
}

void ApagarDibujo() {
  if (g_dibujo_apagado.load(std::memory_order_relaxed)) {
    return;
  }
  g_dibujo_apagado.store(true, std::memory_order_relaxed);
  static constexpr const char* kQue[] = {
      "?", "los registros dan shaders distintos", "?",
      "la busqueda no encuentra registro y la identidad del anillo da otros shaders"};
  const RegistroDibujo& b = g_dibujo_busqueda;
  const RegistroDibujo& m = g_dibujo_marcador;
  REXLOG_ERROR("[d3d_marcador] registro de dibujo: DIFERENCIA con la busqueda del anillo ({}): busqueda {} (funcion "
               "{} tipo {} VS {:08X} PS {:08X}), marcador {} (funcion {} tipo {} VS {:08X} PS {:08X}). Fase 2b APAGADA "
               "para el resto de la sesion: los registros vuelven a la cola",
               kQue[g_dibujo_que < 4 ? g_dibujo_que : 0], g_dibujo_hay_busqueda ? "con registro" : "sin registro",
               int(b.funcion), b.args[0], b.vs, b.ps, g_dibujo_hay_marcador ? "con registro" : "sin registro",
               int(m.funcion), m.args[0], m.vs, m.ps);
}

void InformeDibujo() {
  const int64_t ahora = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  if (ahora < g_i_siguiente_ms) {
    return;
  }
  const bool primero = g_i_siguiente_ms == 0;
  g_i_siguiente_ms = ahora + 10000;
  if (!primero) {
    NFSMW_INFORME_DIFERIDO("[d3d_marcador] registro de dibujo (fase 2b), ultimos 10 s: {} en el marcador ({} comprobando), {} "
                "por la cola, {} sin FlushState | fase {} | comprobados en el anillo {} de {}",
                g_i_en_marcador, g_i_comprobando, g_i_por_cola, g_i_sin_flushstate,
                g_dibujo_apagado.load(std::memory_order_relaxed)     ? "APAGADA"
                : g_dibujo_aplicando.load(std::memory_order_relaxed) ? "aplicando"
                                                                     : "mirando",
                g_dibujo_iguales.load(std::memory_order_relaxed), kComprobacionesDibujo);
  }
  g_i_en_marcador = g_i_comprobando = g_i_por_cola = g_i_sin_flushstate = 0;
}

}  // namespace


namespace ganchos_detalle {

const EntradaShader* IdentificarCreacion(const uint8_t* base, uint32_t direccion, bool vertices) {
  ShadersNativos* shaders = g_shaders.load(std::memory_order_acquire);
  if (!shaders || !direccion || direccion > UINT32_MAX - 24) {
    return nullptr;
  }
  const uint8_t* p = base + direccion;
  // 2005 container (Most Wanted) or 2008 (NFS Carbon), contiguous in both games.
  const uint32_t firma = LeerBE(p);
  if ((firma != 0x102A0E00u && firma != 0x102A1100u && firma != 0x102A0E01u && firma != 0x102A1101u) ||
      (firma & 1) != uint32_t(vertices)) {
    return nullptr;
  }
  const uint64_t total = uint64_t(LeerBE(p + 4)) + LeerBE(p + 8);
  if (total < 24 || total > 65536 || uint64_t(direccion) + total > (uint64_t(1) << 32)) {
    return nullptr;
  }
  return shaders->IdentificarContenedor(std::span<const uint8_t>(p, size_t(total)));
}

void RecordarCreacion(uint32_t objeto, const EntradaShader* entrada, bool vertices) {
  if (!g_shaders.load(std::memory_order_acquire)) {
    return;
  }
  (vertices ? g_creados_vs : g_creados_ps).fetch_add(1, std::memory_order_relaxed);
  if (entrada) {
    (vertices ? g_conocidos_vs : g_conocidos_ps).fetch_add(1, std::memory_order_relaxed);
  } else if (g_avisos.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_WARN("[nativo] C5b: {} shader creado en {:08X} que no esta en la biblioteca",
                vertices ? "vertex" : "pixel", objeto);
  }
  if (!objeto) {
    return;
  }
  std::lock_guard<std::mutex> cerrojo(g_mutex);
  // The address of a freed object gets reused: an unknown shader clears any earlier association with it.
  if (entrada) {
    g_objetos.insert_or_assign(objeto, entrada);
  } else {
    g_objetos.erase(objeto);
  }
  g_generacion.fetch_add(1, std::memory_order_acq_rel);
}

}  // namespace ganchos_detalle

// see nfsmw_nativo_ganchos.h.
const ShadersNativos* BibliotecaActiva() {
  return g_shaders.load(std::memory_order_acquire);
}

void ActivarGanchos(ShadersNativos* shaders) {
  g_shaders.store(shaders, std::memory_order_release);
  if (!shaders) {
    std::lock_guard<std::mutex> cerrojo(g_mutex);
    g_objetos.clear();
    g_generacion.fetch_add(1, std::memory_order_acq_rel);
  }
}

void AnotarDibujo(FuncionDibujo funcion, const uint8_t* base, uint32_t dispositivo, uint32_t r4,
                  uint32_t r5, uint32_t r6, uint32_t r7, uint16_t vegetacion) {
  if (!dispositivo || !g_shaders.load(std::memory_order_relaxed)) {
    return;
  }
  RegistroDibujo registro;
  registro.vs = LeerBE(base + dispositivo + kDispositivoVs);
  registro.ps = LeerBE(base + dispositivo + kDispositivoPs);
  registro.args[0] = r4;
  registro.args[1] = r5;
  registro.args[2] = r6;
  registro.args[3] = r7;
  registro.funcion = funcion;
  registro.sombra = 0;
  registro.vegetacion = vegetacion;  // nfsmw_d3d_vegetacion_juego
  static const bool sombra = REXCVAR_GET(nfsmw_nativo_sombra_d3d);
  if (sombra && ++g_contador_fotos % kFotoCada == 0) {
    const uint64_t secuencia = g_siguiente_foto++;
    Foto& foto = g_fotos[secuencia % kFotos];
    foto.secuencia.store(0, std::memory_order_release);  // being written
    InstantaneaEspejo& d = foto.datos;
    const uint8_t* disp = base + dispositivo;
    for (uint32_t g = 0; g < kGruposEspejo; ++g) {
      const uint32_t desp = g_desplazamiento_grupo[g].load(std::memory_order_relaxed);
      d.mascara[g] = desp ? g_mascara_grupo[g].load(std::memory_order_relaxed) : 0;
      d.base_registro[g] = g < 8 ? 0x2000 + g * 0x80 : 0x4900;
      if (!desp) {
        continue;
      }
      for (uint32_t i = 0; i < 64; ++i) {
        if ((d.mascara[g] >> (63 - i)) & 1) {
          d.estado[g][i] = LeerBE(disp + desp + i * 4);
        }
      }
    }
    for (uint32_t i = 0; i < 192; ++i) {
      d.fetch[i] = LeerBE(disp + kDispositivoFetch + i * 4);
    }
    for (uint32_t i = 0; i < 1024; ++i) {
      d.constantes[i] = LeerBE(disp + kDispositivoConstantesVs + i * 4);
      d.constantes[1024 + i] = LeerBE(disp + kDispositivoConstantesPs + i * 4);
    }
    foto.secuencia.store(secuencia, std::memory_order_release);
    registro.sombra = secuencia;
  }
  // Phase 2b: with the record in the marker, this Draw*'s FlushState decides (TomarDibujoEnCurso);
  // otherwise, to the queue as usual.
  if (DibujoEnMarcador()) {
    if (g_dibujo_pendiente) {  // should not happen: the previous Draw* reached neither its FlushState nor TerminarDibujo
      EncolarDibujo(g_dibujo_en_curso);
      ++g_i_sin_flushstate;
    }
    g_dibujo_en_curso = registro;
    g_dibujo_pendiente = true;
    if ((++g_i_dibujos & 4095) == 0) {
      InformeDibujo();
    }
    return;
  }
  EncolarDibujo(registro);
}

bool TomarDibujoEnCurso(RegistroDibujo& registro) {
  if (!g_dibujo_pendiente) {
    return false;
  }
  g_dibujo_pendiente = false;
  registro = g_dibujo_en_curso;
  return true;
}

uint32_t DecidirModoDibujo() {
  if (!DibujoEnMarcador()) {
    return 0;
  }
  // Relaxed: an acquire load per draw would be an ldar on the A57. The acquire only if the ring has seen
  // something.
  if (g_dibujo_distinto.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    ApagarDibujo();
    return 0;
  }
  if (!g_dibujo_aplicando.load(std::memory_order_relaxed)) {
    const uint64_t iguales = g_dibujo_iguales.load(std::memory_order_relaxed);
    if (iguales < kComprobacionesDibujo) {
      return kDibujoComprobar;
    }
    g_dibujo_aplicando.store(true, std::memory_order_relaxed);
    REXLOG_INFO("[d3d_marcador] registro de dibujo: {} dibujos comprobados contra la busqueda del anillo, 0 "
                "desacuerdos: el marcador lleva ya el registro y el anillo no busca (1 de cada {} se sigue "
                "comprobando)",
                iguales, kComprobarDibujoCada);
  }
  return (++g_turno_dibujo & (kComprobarDibujoCada - 1)) == 0 ? kDibujoComprobar : kDibujoAplicar;
}

void EntregarDibujo(const RegistroDibujo& registro, uint32_t modo, bool en_marcador) {
  if (!en_marcador) {
    ++g_i_por_cola;
    EncolarDibujo(registro);
    return;
  }
  ++g_i_en_marcador;
  if (modo == kDibujoComprobar) {  // the ring's lookup has to be able to find it to compare
    ++g_i_comprobando;
    EncolarDibujo(registro);
  }
}

void TerminarDibujo() {
  if (!g_dibujo_pendiente) {
    return;
  }
  g_dibujo_pendiente = false;
  ++g_i_sin_flushstate;
  EncolarDibujo(g_dibujo_en_curso);
}

void AnotarComprobacionDibujo(bool igual, uint32_t que, const RegistroDibujo* busqueda,
                              const RegistroDibujo* marcador) {
  if (igual) {  // written only by the ring thread
    g_dibujo_iguales.store(g_dibujo_iguales.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    return;
  }
  if (g_dibujo_distinto.load(std::memory_order_relaxed)) {
    return;
  }
  g_dibujo_que = que;
  g_dibujo_hay_busqueda = busqueda != nullptr;
  if (busqueda) {
    g_dibujo_busqueda = *busqueda;
  }
  g_dibujo_hay_marcador = marcador != nullptr;
  if (marcador) {
    g_dibujo_marcador = *marcador;
  }
  g_dibujo_distinto.store(true, std::memory_order_release);
}

void AprenderGrupoEspejo(uint32_t registro_base, uint64_t mascara, uint32_t desplazamiento) {
  const int g = GrupoDe(registro_base);
  if (g < 0 || !desplazamiento) {
    return;
  }
  // The dump copies from origen + 4 * bit position: the mirror of register base_registro + i is at
  // desplazamiento + 4 * i. Always the same per group; the first one seen is stored.
  uint32_t esperado = 0;
  g_desplazamiento_grupo[g].compare_exchange_strong(esperado, desplazamiento, std::memory_order_relaxed);
  const uint64_t antes = g_mascara_grupo[g].load(std::memory_order_relaxed);
  if ((antes | mascara) != antes) {
    g_mascara_grupo[g].store(antes | mascara, std::memory_order_relaxed);
  }
}

bool LeerInstantanea(uint64_t secuencia, InstantaneaEspejo& salida) {
  const Foto& foto = g_fotos[secuencia % kFotos];
  if (foto.secuencia.load(std::memory_order_acquire) != secuencia) {
    return false;
  }
  salida = foto.datos;
  std::atomic_thread_fence(std::memory_order_acquire);
  return foto.secuencia.load(std::memory_order_relaxed) == secuencia;  // not rewritten while being copied
}

bool SacarDibujo(RegistroDibujo& registro) {
  const uint32_t lectura = g_lectura.load(std::memory_order_relaxed);
  if (lectura == g_escritura.load(std::memory_order_acquire)) {
    return false;
  }
  registro = g_cola[lectura];
  g_lectura.store((lectura + 1) & (kTamanoCola - 1), std::memory_order_release);
  return true;
}

const EntradaShader* ShaderDeObjeto(uint32_t objeto) {
  std::lock_guard<std::mutex> cerrojo(g_mutex);
  const auto it = g_objetos.find(objeto);
  return it != g_objetos.end() ? it->second : nullptr;
}

uint64_t GeneracionObjetos() {
  return g_generacion.load(std::memory_order_acquire);
}

/*
 * Shadow map vegetation, filtered on the game thread (nfsmw_d3d_vegetacion_juego).
 *
 * The ring drops, right on entry to Dibujar, the colorless draws with an alpha test, a kill or PS-written
 * depth: the shadow map vegetation, ~480 per frame in a race (267-760). But each one still costs ~2.2-2.6 us
 * of ring time outside Dibujar (its FlushState marker, the DRAW_INDX, the pairing and its share of IM_LOAD)
 * plus its whole Draw* on the game thread (FlushState with streams, shaders and IM_LOAD, and the EDRAM mode
 * changes 5 and 4 with their packets). Here the same decision is taken in the D3D Draw*, before the
 * original, using the device's register mirror, and if it is vegetation the original is not called: the
 * Draw* never exists for the ring.
 *
 * Why the whole Draw* can be removed (from reading the PowerPC code)
 *   - DrawVertices (82593A10) and DrawIndexedVertices (82593C50), before FlushState, only touch
 *     VGT_INDX_OFFSET in the mirror (dev+0x2D14, with its dirty bit 1<<61 in dev+0x28): the same is done
 *     here. After FlushState they only write their DRAW_INDX (normal path: bit 0x04 of dev+0x28C0 clear and a
 *     single-segment count).
 *   - What the skipped FlushState would have dumped stays marked dirty and the next one dumps it (or the
 *     partial dump 825A42E0 of Resolve and ClearF) with the mirror's values, which are the same: the ring's
 *     state at the next draw is the usual one. The same goes for the shaders: their dirty bits stay set and
 *     the next FlushState patches and loads them for whatever shader pair is bound then.
 *   - What gets written to the ring without a dump first: the occlusion query (8258F810 and 8258EA28)
 *     writes RB_MODECONTROL, RB_COLOR_INFO and RB_SAMPLE_COUNT_ADDR directly and leaves them dirty to be
 *     restored. In the ring the only lazy register read without a dump before it is RB_SURFACE_INFO
 *     (EscalaOclusion). That is why nothing is skipped with group 0x2000 dirty (render targets pending a
 *     dump) or with an occlusion query open.
 *   - Predicated tiling and ZPass: inside a D3D block (dev+0x28C0 & 0x3F: 0x10 tiling, 0x04 extension path
 *     with SET_BIN_MASK and EVENT_WRITE_EXT, 0x01, 0x02 and 0x20 ZPass, 0x08 the one from 825AB9A0) nothing
 *     is skipped: there the packets are recorded to be replayed per tile.
 *
 * The criterion: Dibujar's up to its early discard, with the state the ring will see for that draw
 *   - PS and VS bound (dev+0x3290 and dev+0x4FE8) and present in the library (ShaderDeObjeto: the identity
 *     the ring takes from the record).
 *   - Effective EDRAM mode 4. It is the mirror's (RB_MODECONTROL, dev+0x2D94) except for one change
 *     FlushState makes before dumping it: with a PS bound and its dirty bit 0x100 in dev+0x30, the shader
 *     loader (825A3AF0) calls 825A3A58, which turns mode 5 into 4. The switch to 5 (825A3968) only happens
 *     without a PS, and the streams (825A2D80) only set bit 0x80. No other function in the FlushState tree
 *     writes RB_MODECONTROL, RB_COLOR_MASK, RB_COLORCONTROL or the PS.
 *   - No color: no target i with RB_COLOR_MASK (dev+0x2D1C) nonzero and the PS writing it.
 *   - Alpha test (RB_COLORCONTROL, dev+0x2D7C: enabled and a function other than ALWAYS), the PS's own
 *     kill, or depth written by the PS.
 *   - And to skip it: nfsmw_nativo_ps_solo_alfa without alternation, nfsmw_sombras_sin_vegetacion, no
 *     occlusion query open, outside blocks, no render targets pending a dump, and a single segment
 *     (count 1..65535).
 *
 * Self-checking guard, in two phases
 *   1. Observing: nothing is skipped. Each Draw* carries its verdict in the record and the ring computes
 *      its own in Dibujar, with its own registers and PS, and counts game yes/no against ring yes/no.
 *      After kComprobaciones checked draws the game would skip, with no disagreement, it moves to applying.
 *   2. Applying: they are skipped. 1 in kMuestraCada is still sent, flagged kVegMuestra, for the ring to
 *      check, and the ones not skipped still carry their negative verdict, which the ring checks on all of
 *      them.
 *   The filter is switched off for the rest of the session, with DIFERENCIA in the log, on: a differing
 *   verdict in either direction, an occlusion query open in the ring that the game does not see, a draw the
 *   game sees as vegetation that the ring does not draw with its record's shaders, or a ring verdict that
 *   differs from Dibujar's early discard.
 *
 * Memory ordering
 *   The ring writes the disagreement details and then g_distinto with release. The game reads g_distinto
 *   relaxed on every Draw* and, if it is true, issues an acquire fence before reading the details: the fence
 *   synchronizes with the release and the details are seen in full (the same pairing as phase 2b in
 *   DecidirModoDibujo). Nobody waits or sleeps on this flag: the game only polls it, so no wake-up can be
 *   lost and nothing can hang (the earlier hangs were a thread asleep waiting for a wake-up). The worst case
 *   is seeing it a few microseconds late. The ring's counters have a single writer (the ring) and the game
 *   reads them relaxed: they only count. The verdict travels from the game to the ring inside the draw
 *   record, over the same already synchronized path as phase 2b (marker) or the queue (release and acquire
 *   on its indices).
 */
namespace {
namespace vegetacion {

constexpr uint64_t kComprobaciones = 200000;  // observing phase: draws the game would skip, checked in the ring
constexpr uint64_t kMuestraCada = 4096;       // applying phase: 1 in kMuestraCada is sent anyway (power of 2)
// The device's register mirror (FlushState groups: 0x2100 from +0x2D0C, 0x2200 from +0x2D74).
constexpr uint32_t kModoEdram = 0x2D94;     // RB_MODECONTROL (0x2208)
constexpr uint32_t kMascaraColor = 0x2D1C;  // RB_COLOR_MASK (0x2104)
constexpr uint32_t kControlColor = 0x2D7C;  // RB_COLORCONTROL (0x2202)
constexpr uint32_t kIndiceBase = 0x2D14;    // VGT_INDX_OFFSET (0x2102)
// Its dirty masks (64-bit big-endian): +0x20 fetch, booleans and group 0x2000 (bits 0x3FFFC000 of the low
// word); +0x28 groups 0x2100 (bit 63 = 0x2100), 0x2180, 0x2200 and 0x2280; +0x30 streams (0x400), shaders
// (0x1E0; 0x100 is the PS) and groups 0x2300 and 0x2380.
constexpr uint32_t kSucios20 = 0x20;
constexpr uint32_t kSucios28 = 0x28;
constexpr uint32_t kSucios30 = 0x30;
constexpr uint32_t kBloques = 0x28C0;      // D3D block byte (BeginTiling 825992F0, ZPass 825999D8...)
constexpr uint8_t kBloquesMascara = 0x3F;  // 0x40 and 0x80 are set at device creation: not blocks
constexpr uint32_t kTipoOclusion = 9;      // D3DQUERYTYPE_OCCLUSION, en consulta+4 (8258F810)
constexpr uint32_t kMaxConsultas = 16;

enum Que : uint32_t { kQueNada, kQueJuegoSi, kQueAnilloSi, kQueOclusion, kQueSinIdentidad, kQueModelo, kQues };

// ---- Game thread (the one using D3D: the Draw* calls and the queries' Issue) ----
int8_t g_activo = -1;  // nfsmw_d3d_vegetacion_juego, read the first time (without a local static guard)
bool g_aplicando = false;
bool g_apagado = false;
bool g_ajustes = false;  // refreshed every 1024 Draw* calls checked
uint64_t g_turno = 0;
uint64_t g_mirados_total = 0;
struct Memo {
  uint32_t objeto = 0;
  const EntradaShader* entrada = nullptr;
};
std::array<Memo, 32> g_memo{};
uint64_t g_memo_generacion = UINT64_MAX;
// Report every 10 s.
int64_t g_i_siguiente_ms = 0;
uint64_t g_i_mirados = 0;
uint64_t g_i_si = 0;
uint64_t g_i_saltados = 0;
uint64_t g_i_muestras = 0;
uint64_t g_i_bloque = 0;
uint64_t g_i_destinos = 0;
uint64_t g_i_oclusion = 0;
uint64_t g_i_ajustes = 0;
uint64_t g_i_cuenta = 0;
std::array<uint32_t, 64> g_i_bloques{};  // the block ones, by value of dev+0x28C0 & 0x3F

// ---- Open D3D occlusion queries (Issue BEGIN without its END), per query object ----
// The D3D thread touches them; the lock only prevents a race should they ever be called from another
// thread.
std::mutex g_consultas_cerrojo;
std::array<uint32_t, kMaxConsultas> g_consultas{};
uint32_t g_n_consultas = 0;
std::atomic<uint32_t> g_consultas_abiertas{0};  // UINT32_MAX: count lost, nothing is skipped any more

// ---- Shared with the ring thread ----
// Counters: only the ring writes them, without atomic read-modify-write (the A57 has no LSE). The game
// reads them relaxed for the report and to decide when to move to applying.
enum Contador : uint32_t {
  kSiSi,
  kNoNo,
  kSiNo,
  kNoSi,
  kNoSiBloque,
  kNoSiDestinos,
  kNoSiOclusion,
  kNoSiAjustes,
  kNoSiCuenta,
  kAjustesDistintos,
  kSinComparar,
  kAcuerdos,
  kMuestrasBien,
  kContadores
};
// Own cache line: the ring writes them on every draw and they must not share a line with anything the
// game thread reads on every Draw* (g_distinto), or every write would steal it from the other core.
alignas(64) std::atomic<uint64_t> g_contadores[kContadores];
alignas(64) std::array<uint64_t, kContadores> g_previos{};  // own cache line; the previous report's values (game thread only)
// The first disagreement: the ring writes the details and then g_distinto with release (see Memory
// ordering). Own cache line (the game reads it on every Draw*; see g_contadores).
alignas(64) std::atomic<bool> g_distinto{false};
uint32_t g_que = kQueNada;
uint16_t g_banderas = 0;
DetalleVegetacion g_detalle;

uint64_t LeerBE64(const uint8_t* p) {
  return (uint64_t(LeerBE(p)) << 32) | LeerBE(p + 4);
}

void EscribirBE(uint8_t* p, uint32_t valor) {
  p[0] = uint8_t(valor >> 24);
  p[1] = uint8_t(valor >> 16);
  p[2] = uint8_t(valor >> 8);
  p[3] = uint8_t(valor);
}

const char* SiNo(bool valor) {
  return valor ? "si" : "no";
}

inline void Contar(Contador c) {  // ring thread only
  g_contadores[c].store(g_contadores[c].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// Ring thread only: the details before the flag (release); the game reads them after its acquire fence.
void Desacuerdo(Que que, uint16_t banderas, const DetalleVegetacion& anillo) {
  if (g_distinto.load(std::memory_order_relaxed)) {
    return;
  }
  g_que = que;
  g_banderas = banderas;
  g_detalle = anillo;
  g_distinto.store(true, std::memory_order_release);
}

// The object of a bound shader, with the same identity the ring takes from the record (ShaderDeObjeto).
// The generation is read relaxed: a shader created on another thread only reaches a Draw* on this thread
// through some synchronization in the game, and then this read already sees the new generation
// (write-read coherence).
const EntradaShader* Entrada(uint32_t objeto) {
  const uint64_t generacion = g_generacion.load(std::memory_order_relaxed);
  if (generacion != g_memo_generacion) {
    g_memo.fill(Memo{});
    g_memo_generacion = generacion;
  }
  Memo& memo = g_memo[size_t((objeto * UINT32_C(2654435761)) >> 27)];
  if (memo.objeto != objeto) {
    memo.objeto = objeto;
    memo.entrada = ShaderDeObjeto(objeto);
  }
  return memo.entrada;
}

// Dibujar's criterion up to its early discard (nfsmw_nativo_dibujos.cpp), with the state the ring will
// see.
bool Estructura(const uint8_t* d, uint32_t& motivo) {
  const uint32_t objeto_ps = LeerBE(d + kDispositivoPs);
  const uint32_t objeto_vs = LeerBE(d + kDispositivoVs);
  if (!objeto_ps || !objeto_vs) {
    motivo = kVegMotivoSinShaders;
    return false;
  }
  // The EDRAM mode the ring will see: the mirror's, with the switch from 5 to 4 FlushState makes before
  // dumping it if a PS is bound and its dirty bit is set (825A3AF0 and 825A3A58). Without a PS it already
  // returned above.
  uint32_t modo = LeerBE(d + kModoEdram) & 0x7;
  if (modo == 5 && (LeerBE64(d + kSucios30) & 0x100)) {
    modo = 4;
  }
  if (modo != 4) {
    motivo = kVegMotivoModo;
    return false;
  }
  const EntradaShader* ps = Entrada(objeto_ps);
  const EntradaShader* vs = Entrada(objeto_vs);
  if (!ps || !vs) {
    motivo = kVegMotivoDesconocidos;
    return false;
  }
  const uint32_t mascara = LeerBE(d + kMascaraColor);
  for (uint32_t i = 0; i < 4; ++i) {
    if (((mascara >> (i * 4)) & 0xF) && ((ps->salidas >> i) & 0x1)) {
      motivo = kVegMotivoColor;
      return false;
    }
  }
  const uint32_t control = LeerBE(d + kControlColor);
  const bool prueba_alfa = ((control >> 3) & 0x1) && (control & 0x7) != 7;
  if (!prueba_alfa && !ps->descarta && !(ps->salidas & 0x10)) {
    motivo = kVegMotivoSinDescarte;
    return false;
  }
  motivo = 0;
  return true;
}

void RefrescarAjustesVegetacion() {
  g_ajustes = REXCVAR_GET(nfsmw_nativo_ps_solo_alfa) && REXCVAR_GET(nfsmw_nativo_ps_solo_alfa_alternar_s) <= 0 &&
              REXCVAR_GET(nfsmw_sombras_sin_vegetacion);
}

// Game thread only, after the acquire fence.
void ApagarVegetacion() {
  if (g_apagado) {
    return;
  }
  g_apagado = true;
  static constexpr const char* kQueTexto[kQues] = {
      "?",
      "el juego ve vegetacion y el anillo no",
      "el anillo ve vegetacion y el juego no",
      "consulta de oclusion abierta en el anillo y no en el juego",
      "el juego ve vegetacion y el anillo no dibuja con los shaders de su registro",
      "el veredicto del anillo no es el descarte temprano de Dibujar"};
  const DetalleVegetacion& a = g_detalle;
  const uint16_t b = g_banderas;
  REXLOG_ERROR("[vegetacion] DIFERENCIA con el anillo ({}): juego {} (motivo {}, ajustes {}, oclusion {}, bloque {}, "
               "destinos {}, saltaria {}, muestra {}; fase {}); anillo: estructura {}, ajustes {}, oclusion {}, "
               "descarte temprano en Dibujar {}, RB_MODECONTROL {:08X}, RB_COLOR_MASK {:08X}, RB_COLORCONTROL {:08X}, "
               "PS n{} (salidas {:X}, descarta {}), VS n{}. Filtro de la vegetacion en el juego APAGADO para el resto "
               "de la sesion: todos los Draw* vuelven al anillo",
               kQueTexto[g_que < kQues ? g_que : 0], SiNo((b & kVegSi) != 0), (b >> kVegMotivo) & 0xF,
               SiNo((b & kVegAjustes) != 0), SiNo((b & kVegOclusion) != 0), SiNo((b & kVegBloque) != 0),
               SiNo((b & kVegDestinos) != 0), SiNo((b & kVegSaltaria) != 0), SiNo((b & kVegMuestra) != 0),
               g_aplicando ? "aplicando" : "mirando", SiNo(a.estructura), SiNo(a.ajustes), SiNo(a.oclusion),
               SiNo(a.temprana), a.modo, a.mascara, a.control, a.ps, a.salidas, SiNo(a.descarta), a.vs);
}

// Every 10 s, from the game thread (NFSMW_INFORME_DIFERIDO). The ring's counters, as differences.
void InformeVegetacion() {
  const int64_t ahora = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  if (ahora < g_i_siguiente_ms) {
    return;
  }
  const bool primero = g_i_siguiente_ms == 0;
  g_i_siguiente_ms = ahora + 10000;
  std::array<uint64_t, kContadores> anillo{};
  for (uint32_t i = 0; i < kContadores; ++i) {
    anillo[i] = g_contadores[i].load(std::memory_order_relaxed);
  }
  if (primero) {
    REXLOG_INFO("[vegetacion] vegetacion del mapa de sombras filtrada en el juego (build 184): empieza mirando; salta "
                "cuando el anillo haya comprobado {} dibujos que saltaria sin ningun desacuerdo",
                kComprobaciones);
  } else {
    std::array<uint64_t, kContadores> d{};
    for (uint32_t i = 0; i < kContadores; ++i) {
      d[i] = anillo[i] - g_previos[i];
    }
    std::string bloques;
    for (uint32_t v = 1; v < 64; ++v) {
      if (g_i_bloques[v]) {
        bloques += fmt::format(" {:02X}:{}", v, g_i_bloques[v]);
      }
    }
    if (!bloques.empty()) {
      bloques = " (valores" + bloques + ")";
    }
    const bool perdida = g_consultas_abiertas.load(std::memory_order_relaxed) == UINT32_MAX;
    NFSMW_INFORME_DIFERIDO(
        "[vegetacion] Draw* (build 184), ultimos 10 s: {} mirados, {} de vegetacion para el juego: {} saltados y {} "
        "enviados para comprobar; no se saltan {} en bloques del D3D{}, {} con destinos por volcar, {} con una "
        "consulta de oclusion abierta, {} con los ajustes apagados o alternando, {} por la cuenta | anillo: si y si "
        "{}, no y no {}, juego si y anillo no {}, juego no y anillo si {} ({} por bloque, {} por destinos, {} por "
        "oclusion solo en el juego, {} por ajustes, {} por la cuenta), ajustes distintos {}, sin comparar {}, "
        "muestras bien {} | fase {} | comprobados {} de {}{}",
        g_i_mirados, g_i_si, g_i_saltados, g_i_muestras, g_i_bloque, bloques, g_i_destinos, g_i_oclusion, g_i_ajustes,
        g_i_cuenta, d[kSiSi], d[kNoNo], d[kSiNo], d[kNoSi], d[kNoSiBloque], d[kNoSiDestinos], d[kNoSiOclusion],
        d[kNoSiAjustes], d[kNoSiCuenta], d[kAjustesDistintos], d[kSinComparar], d[kMuestrasBien],
        g_aplicando ? "aplicando" : "mirando", anillo[kAcuerdos], kComprobaciones,
        perdida ? " | consultas de oclusion: SE PERDIO LA CUENTA, no se salta nada" : "");
  }
  g_previos = anillo;
  g_i_mirados = g_i_si = g_i_saltados = g_i_muestras = g_i_bloque = g_i_destinos = g_i_oclusion = g_i_ajustes =
      g_i_cuenta = 0;
  g_i_bloques.fill(0);
}

}  // namespace vegetacion
}  // namespace

uint16_t DecidirVegetacion(FuncionDibujo funcion, uint8_t* base, uint32_t dispositivo, uint32_t r5, uint32_t r6,
                           uint32_t r7, bool& saltar) {
  using namespace vegetacion;
  saltar = false;
  if (g_activo < 0) {
    g_activo = REXCVAR_GET(nfsmw_d3d_vegetacion_juego) ? 1 : 0;
  }
  if (!g_activo || g_apagado || !dispositivo || !g_shaders.load(std::memory_order_relaxed) ||
      (funcion != FuncionDibujo::kVertices && funcion != FuncionDibujo::kIndexados)) {
    return 0;
  }
  // Relaxed: an acquire load per draw would be an ldar on the A57. The acquire only if the ring has seen
  // something.
  if (g_distinto.load(std::memory_order_relaxed)) {
    std::atomic_thread_fence(std::memory_order_acquire);
    ApagarVegetacion();
    return 0;
  }
  if ((g_mirados_total++ & 1023) == 0) {
    RefrescarAjustesVegetacion();
    InformeVegetacion();
  }
  ++g_i_mirados;
  const uint8_t* d = base + dispositivo;
  uint32_t motivo = 0;
  const bool si = Estructura(d, motivo);
  const uint32_t abiertas = g_consultas_abiertas.load(std::memory_order_relaxed);
  const uint8_t bloques = uint8_t(d[kBloques] & kBloquesMascara);
  const bool destinos = (uint32_t(LeerBE64(d + kSucios20)) & 0x3FFFC000u) != 0;
  const uint32_t cuenta = funcion == FuncionDibujo::kVertices ? r6 : r7;
  uint16_t banderas = uint16_t(kVegHay | (motivo << kVegMotivo));
  if (si) {
    banderas = uint16_t(banderas | kVegSi);
  }
  if (abiertas) {
    banderas = uint16_t(banderas | kVegOclusion);
  }
  if (g_ajustes) {
    banderas = uint16_t(banderas | kVegAjustes);
  }
  if (bloques) {
    banderas = uint16_t(banderas | kVegBloque);
  }
  if (destinos) {
    banderas = uint16_t(banderas | kVegDestinos);
  }
  if (!si) {
    return banderas;
  }
  ++g_i_si;
  // Why it would not be skipped, in this order (each draw counts under a single reason).
  if (bloques) {
    ++g_i_bloque;
    ++g_i_bloques[bloques];
    return banderas;
  }
  if (destinos) {
    ++g_i_destinos;
    return banderas;
  }
  if (abiertas) {
    ++g_i_oclusion;
    return banderas;
  }
  if (!g_ajustes) {
    ++g_i_ajustes;
    return banderas;
  }
  if (cuenta == 0 || cuenta > 0xFFFF) {
    ++g_i_cuenta;
    return banderas;
  }
  banderas = uint16_t(banderas | kVegSaltaria);
  if (!g_aplicando) {
    const uint64_t comprobados = g_contadores[kAcuerdos].load(std::memory_order_relaxed);
    if (comprobados < kComprobaciones) {
      return banderas;  // observing phase: sent, and the ring compares it
    }
    g_aplicando = true;
    REXLOG_INFO("[vegetacion] {} dibujos de vegetacion que el juego saltaria comprobados en el anillo, 0 desacuerdos: "
                "se saltan ya en el Draw* del D3D (1 de cada {} se sigue enviando para comprobarlo)",
                comprobados, kMuestraCada);
  }
  if ((++g_turno & (kMuestraCada - 1)) == 0) {
    ++g_i_muestras;
    return uint16_t(banderas | kVegMuestra);
  }
  // The whole Draw* is skipped. The only thing it does to the mirror before FlushState: VGT_INDX_OFFSET and
  // its dirty bit (DrawVertices sets it to its start vertex, r5; DrawIndexedVertices to 0). Everything else
  // stays dirty for the next dump, with the same mirror values.
  uint8_t* espejo = base + dispositivo;
  const uint32_t indice = funcion == FuncionDibujo::kVertices ? r5 : 0u;
  if (LeerBE(espejo + kIndiceBase) != indice) {
    EscribirBE(espejo + kIndiceBase, indice);
    const uint64_t sucios = LeerBE64(espejo + kSucios28) | (uint64_t(1) << 61);
    EscribirBE(espejo + kSucios28, uint32_t(sucios >> 32));
    EscribirBE(espejo + kSucios28 + 4, uint32_t(sucios));
  }
  ++g_i_saltados;
  saltar = true;
  return banderas;
}

void AnotarConsultaD3D(const uint8_t* base, uint32_t consulta, uint32_t banderas) {
  using namespace vegetacion;
  if (!consulta || LeerBE(base + consulta + 4) != kTipoOclusion) {
    return;
  }
  const bool empieza = (banderas & 2) != 0;
  if (!empieza && !(banderas & 1)) {
    return;
  }
  std::lock_guard<std::mutex> cerrojo(g_consultas_cerrojo);
  uint32_t i = 0;
  while (i < g_n_consultas && g_consultas[i] != consulta) {
    ++i;
  }
  if (empieza) {  // BEGIN takes precedence over END, as in the original
    if (i == g_n_consultas) {
      if (g_n_consultas == kMaxConsultas) {
        g_consultas_abiertas.store(UINT32_MAX, std::memory_order_relaxed);
        return;
      }
      g_consultas[g_n_consultas++] = consulta;
    }
  } else if (i < g_n_consultas) {
    g_consultas[i] = g_consultas[--g_n_consultas];
  }
  if (g_consultas_abiertas.load(std::memory_order_relaxed) != UINT32_MAX) {
    g_consultas_abiertas.store(g_n_consultas, std::memory_order_relaxed);
  }
}

uint16_t IdentidadParaVegetacion(uint16_t banderas, bool con_sus_shaders) {
  using namespace vegetacion;
  if (!(banderas & kVegHay)) {
    return 0;
  }
  if (con_sus_shaders) {
    return banderas;
  }
  Contar(kSinComparar);
  if (banderas & kVegSi) {
    Desacuerdo(kQueSinIdentidad, banderas, DetalleVegetacion{});
  }
  return 0;
}

void AnotarVegetacionAnillo(uint16_t banderas, const DetalleVegetacion& anillo) {
  using namespace vegetacion;
  const bool si = (banderas & kVegSi) != 0;
  const bool saltaria = (banderas & kVegSaltaria) != 0;
  const bool completo = anillo.estructura && anillo.ajustes && !anillo.oclusion;
  Contar(saltaria ? (completo ? kSiSi : kSiNo) : (completo ? kNoSi : kNoNo));
  if (si != anillo.estructura) {
    Desacuerdo(si ? kQueJuegoSi : kQueAnilloSi, banderas, anillo);
    return;
  }
  if (saltaria) {
    if (!anillo.ajustes) {
      Contar(kAjustesDistintos);  // a setting just changed and each thread reads it at its own time: not an error
    } else if (anillo.oclusion) {
      Desacuerdo(kQueOclusion, banderas, anillo);
    } else {
      Contar(kAcuerdos);
      if (banderas & kVegMuestra) {
        Contar(kMuestrasBien);
      }
    }
    return;
  }
  if (completo) {  // same structure, but the game would not skip it: why
    Contar((banderas & kVegBloque)       ? kNoSiBloque
           : (banderas & kVegDestinos)   ? kNoSiDestinos
           : (banderas & kVegOclusion)   ? kNoSiOclusion
           : !(banderas & kVegAjustes)   ? kNoSiAjustes
                                         : kNoSiCuenta);
  }
}

void AnotarVegetacionModelo(uint16_t banderas, const DetalleVegetacion& anillo) {
  vegetacion::Desacuerdo(vegetacion::kQueModelo, banderas, anillo);
}

EstadisticasGanchos EstadisticasDeGanchos() {
  EstadisticasGanchos e;
  e.creados_vs = g_creados_vs.load(std::memory_order_relaxed);
  e.conocidos_vs = g_conocidos_vs.load(std::memory_order_relaxed);
  e.creados_ps = g_creados_ps.load(std::memory_order_relaxed);
  e.conocidos_ps = g_conocidos_ps.load(std::memory_order_relaxed);
  e.perdidos = g_perdidos.load(std::memory_order_relaxed);
  return e;
}

}  // namespace nfsmw::nativo

// The constructors and the fetch patcher report what they write into the microcode (IM_LOAD without
// memcmp, see nfsmw_microcodigo_versiones.h).
#include "nfsmw_microcodigo_versiones.h"

// The experimental library (nfsmw_shader_hooks.cpp) hooks the same constructors; with it enabled these
// are not compiled.
// NFS Carbon has its own hooks (nfscarbon-recomp/src/carbon_nativo_ganchos.cpp): these addresses are Most Wanted's.
#if !defined(NFSMW_NATIVE_SHADER_LIBRARY) && !defined(NFSC_RECOMP)
// r3 = original container; they return the object in r3. The original is always called and no PPC
// register is touched.
REX_EXTERN(__imp__sub_8259BC90);
REX_HOOK_RAW(sub_8259BC90) {  // pixel shader
  const auto* entrada =
      nfsmw::nativo::ganchos_detalle::IdentificarCreacion(base, ctx.r3.u32, false);
  __imp__sub_8259BC90(ctx, base);
  nfsmw::nativo::ganchos_detalle::RecordarCreacion(ctx.r3.u32, entrada, false);
  nfsmw::nativo::microcodigo::AvisarCreacion(base, ctx.r3.u32, false);
}

REX_EXTERN(__imp__sub_8259C038);
REX_HOOK_RAW(sub_8259C038) {  // vertex shader
  const auto* entrada = nfsmw::nativo::ganchos_detalle::IdentificarCreacion(base, ctx.r3.u32, true);
  __imp__sub_8259C038(ctx, base);
  nfsmw::nativo::ganchos_detalle::RecordarCreacion(ctx.r3.u32, entrada, true);
  nfsmw::nativo::microcodigo::AvisarCreacion(base, ctx.r3.u32, true);
}
#endif

/*
 * The VS fetch patcher reports what it writes (IM_LOAD without memcmp, see nfsmw_microcodigo_versiones.h).
 * sub_825A2FB8(r3 device, r4 VS, r5 destination, r6 declaration) writes the patched fetches to r5.
 *   - In place (r5 = [VS+40], from sub_825A3AF0): bumps the versions of that microcode's slot before and
 *     after writing.
 *   - On the ring's copy (from sub_825A37D8, return 0x825A38D0): not memory of any IM_LOAD; only counted.
 *   - Any other call: also bumps the destination's versions, but is counted separately and the ring turns
 *     off the shortcut.
 * Careful: the two calls in the generated code must go to sub_825A2FB8 and not to __imp__sub_825A2FB8
 * (nfsmw_recomp.58.cpp and nfsmw_recomp.124.cpp, changed by a patch; tools/llamadas_directas.py already
 * handles them because this address appears here). If the one from sub_825A3AF0 went to __imp__, this
 * hook would not see the in-place patches and the ring's guard would stay in the observing phase (or
 * switch off with DIFERENCIA).
 * It changes no PPC register.
 */
#if !defined(NFSC_RECOMP)
REX_EXTERN(__imp__sub_825A2FB8);
REX_HOOK_RAW(sub_825A2FB8) {
  namespace mc = nfsmw::nativo::microcodigo;
  const uint32_t destino = ctx.r5.u32;
  const uint32_t vs = ctx.r4.u32;
  const uint32_t retorno = uint32_t(ctx.lr);
  const bool en_su_sitio = vs != 0 && mc::LeerInvitado32(base, vs + 40) == destino;
  if (!en_su_sitio && retorno == 0x825A38D0u) {
    mc::Contar(mc::g_parches_en_copia);
    __imp__sub_825A2FB8(ctx, base);
    return;
  }
  const uint32_t ranura = mc::RanuraDe(mc::Fisica(destino) & ~uint32_t(3));
  mc::EmpezarEscritura(ranura);
  __imp__sub_825A2FB8(ctx, base);
  // The counts, before TerminarEscritura's global++: a ring that sees the new global value already sees
  // them.
  if (en_su_sitio) {
    mc::Contar(mc::g_parches_en_su_sitio);
  } else {
    mc::g_otro_retorno.store(retorno, std::memory_order_relaxed);
    mc::g_otro_destino.store(destino, std::memory_order_relaxed);
    mc::Contar(mc::g_parches_otros);
  }
  mc::TerminarEscritura(ranura);
}
#endif  // !NFSC_RECOMP
