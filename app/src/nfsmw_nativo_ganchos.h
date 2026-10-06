// nfsmw - native renderer, part C5b: what the ring does not say about each draw, taken on the game
// thread.
//
// The vertex shader microcode reaches the ring already patched by D3D: reordered fetches, swizzles from
// the vertex declaration, and outputs the pixel shader does not read nulled out (measured on PC). Looking
// it up in the library by content is not reliable. The exact identity is on the game thread:
//   - The shader constructors (sub_8259BC90 PS, sub_8259C038 VS) receive the original container and
//     return the object: object -> container.
//   - Each D3D Draw* reads the bound VS and PS from the device and leaves a record in a queue. The ring
//     sink pairs each draw with its record, in the same order, checking primitive type and count.

#pragma once

#include <cstdint>

namespace nfsmw::nativo {

class ShadersNativos;
struct EntradaShader;

enum class FuncionDibujo : uint8_t {
  kVertices,      // DrawVertices(dispositivo, tipo, inicio, cuenta)
  kIndexados,     // DrawIndexedVertices(dispositivo, tipo, base, inicio, cuenta)
  kVerticesUP,    // DrawVerticesUP(dispositivo, tipo, cuenta, datos, zancada)
  kIndexadosUP,   // DrawIndexedVerticesUP: argument order unconfirmed
  // A draw function whose arguments are not mapped (NFS Carbon's other six, reached through function
  // pointers): the record matches any type and count, and only its shaders are checked against the ring.
  kCualquiera,
};

struct RegistroDibujo {
  uint32_t vs = 0;        // bound vertex shader object
  uint32_t ps = 0;        // bound pixel shader object
  uint32_t args[4] = {};  // r4..r7 of the call
  FuncionDibujo funcion = FuncionDibujo::kVertices;
  uint16_t vegetacion = 0;  // the game's vegetation verdict (kVeg* flags; 0 = none)
  uint64_t sombra = 0;    // sequence of its mirror snapshot (0 = none), see InstantaneaEspejo
};

/*
 * Direct3D-level renderer, phase 1 (shadow mode).
 *
 * The game's D3D keeps a copy (mirror) of the Xenos registers in the device, and FlushState (825A40C0)
 * dumps it to the ring by dirty groups. If that mirror holds everything a draw needs, the renderer can
 * read it when drawing instead of reading the ~35 packets of each draw. This checks that without
 * touching anything: on 1 in 64 Draw* calls the mirror is snapshotted, and the ring, when pairing that
 * draw, compares the snapshot register by register with what it read from the packets ("sombra D3D"
 * line).
 */
constexpr uint32_t kGruposEspejo = 9;  // 0x2000..0x2380 in steps of 0x80, and 0x4900 (booleans)
struct InstantaneaEspejo {
  uint64_t mascara[kGruposEspejo] = {};         // registers of each group that D3D handles (learned)
  uint32_t base_registro[kGruposEspejo] = {};
  uint32_t estado[kGruposEspejo][64] = {};      // estado[g][i] = registro base_registro[g] + i
  uint32_t fetch[192] = {};                     // 0x4800.. (+0x480 of the device)
  uint32_t constantes[2048] = {};               // 0x4000.. VS (+0x780) y 0x4400.. PS (+0x1780)
};
// From the register dump hook (825A2AA0): which registers of which group, and where their mirror is.
void AprenderGrupoEspejo(uint32_t registro_base, uint64_t mascara, uint32_t desplazamiento);
// Ring thread only. false if the snapshot has already been reused for another draw.
bool LeerInstantanea(uint64_t secuencia, InstantaneaEspejo& salida);

/*
 * Phase 2 of the Direct3D-level renderer: FlushState's compound marker. See nfsmw_d3d_registros_nativo.cpp.
 *
 * FlushState (825A40C0) dumps the dirty registers of the device mirror with ~7 type 0 packets and ~6
 * padding words per draw. With the marker it writes a single type 3 NOP packet with all the segments
 * inside, at the same place in the ring where the packets would have gone:
 *
 *   [0] 0xC0001000 | (P - 1) << 16    PM4 type 3, NOP (0x10), P payload words, no predicate
 *   [1] kMarcadorMagia | mode         1 = apply; 2 = check (the type 0 packets of the dump go first)
 *   [2] sequence                      marker number, for the log only
 *   [3] count << 16 | register        first segment, followed by its 'count' values as in the mirror (big-endian)
 *   ...                               the remaining segments, in FlushState order
 *
 * A NOP without the magic belongs to the game itself and is ignored, as before.
 */
constexpr uint32_t kMarcadorMagia = 0x4E465300;  // "NFS" and the mode in the low byte (see phase 2b)
constexpr uint32_t kMarcadorAplicar = 1;
constexpr uint32_t kMarcadorComprobar = 2;

// A marker segment must fit entirely within one of the groups FlushState dumps: 0x2000+16, 0x2100+21,
// 0x2180+5, 0x2200+12, 0x2280+21, 0x2300+38, 0x2380+8, the VS (0x4000) and PS (0x4400) constants, the
// fetches (0x4800+192) and the booleans and loops (0x4900+40). That way it never touches a register with
// side effects in the ring (COHER_STATUS_HOST, SCRATCH, the gamma ramp: all below 0x2000) or mixes
// classes.
inline bool TramoDeVolcado(uint32_t registro, uint32_t cuenta) {
  static constexpr uint8_t kTamano2000[8] = {16, 0, 21, 5, 12, 21, 38, 8};  // per 0x80 block (0x2080 is not included)
  if (cuenta == 0) {
    return false;
  }
  if (registro >= 0x2000 && registro < 0x2400) {
    return (registro & 0x7Fu) + cuenta <= kTamano2000[(registro - 0x2000) >> 7];
  }
  if (registro >= 0x4000 && registro < 0x4800) {
    return registro + cuenta <= (registro < 0x4400 ? 0x4400u : 0x4800u);
  }
  if (registro >= 0x4800 && registro < 0x48C0) {
    return registro + cuenta <= 0x48C0;
  }
  if (registro >= 0x4900 && registro < 0x4928) {
    return registro + cuenta <= 0x4928;
  }
  return false;
}

// The native system turns it on when the ring thread starts and off at shutdown. When off, FlushState
// always takes the game's path: with the Xenos emulation nobody would understand the marker.
void ActivarConsumidorMarcadores(bool activo);
// Ring thread only, when reading a check marker: whether the register state matches the marker. A
// difference (or a malformed marker) turns the marker path off for the rest of the session.
void AnotarComprobacionMarcador(bool igual, uint32_t secuencia, uint32_t registro, uint32_t en_registros,
                                uint32_t en_marcador);

/*
 * Phase 2b of the Direct3D-level renderer: the draw record inside the marker.
 * Each Draw* stores its RegistroDibujo before the original (AnotarDibujo), and its FlushState runs inside
 * it. With 2b that FlushState's marker carries the record, and the DRAW_INDX that follows in the ring, if
 * it accepts it (type, count and shaders, as usual), uses it without the queue or EmparejarDibujo's
 * lookup; otherwise it looks it up as usual. The draw mode goes in the high nibble of the marker's mode
 * byte (the low one is the registers': 0 = marker without segments) and, if it is not 0,
 * kPalabrasDibujo words follow right after the sequence: function, VS, PS, the four arguments and the
 * D3D shadow snapshot (high and low).
 */
constexpr uint32_t kDibujoAplicar = 2;    // the ring uses this record and does no lookup
constexpr uint32_t kDibujoComprobar = 3;  // the record also goes through the queue: the ring looks up and compares
constexpr uint32_t kPalabrasDibujo = 9;
// From FlushState (nfsmw_d3d_registros_nativo.cpp): takes the record of the current Draw*, if there is one.
bool TomarDibujoEnCurso(RegistroDibujo& registro);
// For the record just taken: 0 = through the queue; kDibujoAplicar or kDibujoComprobar = in the marker.
uint32_t DecidirModoDibujo();
// After writing (or not) the marker: to the queue if it does not go in the marker, or if it goes in check mode.
void EntregarDibujo(const RegistroDibujo& registro, uint32_t modo, bool en_marcador);
// From the Draw* hooks, after the original: if its FlushState did not take the record, to the queue.
void TerminarDibujo();
// Ring thread only, with a marker record the draw accepts: whether it gives the same shaders as the usual lookup.
// que: 1 the lookup's record gives different ones, 3 the lookup finds nothing and the identity gives different
// ones.
void AnotarComprobacionDibujo(bool igual, uint32_t que, const RegistroDibujo* busqueda, const RegistroDibujo* marcador);

/*
 * Shadow map vegetation filtered on the game side (nfsmw_d3d_vegetacion_juego).
 * See DecidirVegetacion in nfsmw_nativo_ganchos.cpp. Every DrawVertices and DrawIndexedVertices carries the game's
 * verdict in its record (RegistroDibujo::vegetacion; in the phase 2b marker, in bits 8-23 of the function word),
 * and the ring compares it in Dibujar with its own (VeredictoVegetacion in nfsmw_nativo_dibujos.cpp).
 */
constexpr uint16_t kVegHay = 1u << 0;       // there is a verdict: the game has looked at this Draw*
constexpr uint16_t kVegSi = 1u << 1;        // Dibujar's criterion, with the D3D mirror, says vegetation
constexpr uint16_t kVegOclusion = 1u << 2;  // a D3D occlusion query is open
constexpr uint16_t kVegAjustes = 1u << 3;   // nfsmw_nativo_ps_solo_alfa (not alternating) and nfsmw_sombras_sin_vegetacion
constexpr uint16_t kVegBloque = 1u << 4;    // inside a D3D block (tiling, ZPass...: dev+0x28C0 & 0x3F)
constexpr uint16_t kVegDestinos = 1u << 5;  // render targets still to be flushed (group 0x2000 dirty in the mirror)
constexpr uint16_t kVegSaltaria = 1u << 6;  // the game would skip it: yes, settings, none of the above, count 1..65535
constexpr uint16_t kVegMuestra = 1u << 7;   // applying phase: sent anyway so the ring checks it
constexpr uint32_t kVegMotivo = 8;          // bits 8-11: why the game says no (kVegMotivo*)
constexpr uint16_t kVegMotivoSinShaders = 1;    // no PS or no VS bound
constexpr uint16_t kVegMotivoDesconocidos = 2;  // PS or VS not in the library
constexpr uint16_t kVegMotivoModo = 3;          // the effective EDRAM mode is not 4 (color and depth)
constexpr uint16_t kVegMotivoColor = 4;         // escribe color
constexpr uint16_t kVegMotivoSinDescarte = 5;   // no alpha test, no kill and no depth in the PS

// What the ring sees for that draw, for the guard and the DIFERENCIA line.
struct DetalleVegetacion {
  uint32_t modo = 0;        // RB_MODECONTROL
  uint32_t mascara = 0;     // RB_COLOR_MASK
  uint32_t control = 0;     // RB_COLORCONTROL
  int32_t vs = -1;          // number in the library (-1: none)
  int32_t ps = -1;
  uint32_t salidas = 0;     // of the PS
  bool descarta = false;    // of the PS
  bool estructura = false;  // the criterion without the settings or the occlusion
  bool ajustes = false;     // the two discard settings, as Dibujar reads them
  bool oclusion = false;    // occlusion query open on the ring
  bool temprana = false;    // only on a model error: what Dibujar's early discard decided
};

// Game thread, in the Draw* hooks (nfsmw_d3d_trace.cpp), before the original: the kVeg* flags for the record (0
// if it does not look at this Draw*). With saltar = true the original is not called and nothing is recorded: the
// Draw* does not exist for the ring.
uint16_t DecidirVegetacion(FuncionDibujo funcion, uint8_t* base, uint32_t dispositivo, uint32_t r5, uint32_t r6,
                           uint32_t r7, bool& saltar);
// Game thread: IDirect3DQuery9::Issue (8258F810), before the original (query and flags: 2 BEGIN, 1 END).
void AnotarConsultaD3D(const uint8_t* base, uint32_t consulta, uint32_t banderas);
// Ring thread (UsarRegistroDeDibujo): the record's flags if the draw uses its shaders; otherwise 0.
uint16_t IdentidadParaVegetacion(uint16_t banderas, bool con_sus_shaders);
// Ring thread (Dibujar): the ring's verdict against the game's.
void AnotarVegetacionAnillo(uint16_t banderas, const DetalleVegetacion& anillo);
// Ring thread (Dibujar): the computed verdict does not match the early discard Dibujar made.
void AnotarVegetacionModelo(uint16_t banderas, const DetalleVegetacion& anillo);

struct EstadisticasGanchos {
  uint64_t creados_vs = 0;
  uint64_t conocidos_vs = 0;
  uint64_t creados_ps = 0;
  uint64_t conocidos_ps = 0;
  uint64_t perdidos = 0;  // records that did not fit in the queue
};

// The native system enables them after loading the library; nullptr disables them.
// While disabled the hooks only cost an atomic load.
void ActivarGanchos(ShadersNativos* shaders);
// The library that ActivarGanchos enabled (nullptr if none), from any thread. For pipeline prewarming
// (nfsmw_nativo_dibujos.cpp).
const ShadersNativos* BibliotecaActiva();

// From the Draw* hooks (nfsmw_d3d_trace.cpp), before the original.
// vegetacion = the kVeg* flags from DecidirVegetacion for the record (0 = no verdict).
void AnotarDibujo(FuncionDibujo funcion, const uint8_t* base, uint32_t dispositivo, uint32_t r4,
                  uint32_t r5, uint32_t r6, uint32_t r7, uint16_t vegetacion = 0);

// Ring thread only.
bool SacarDibujo(RegistroDibujo& registro);
const EntradaShader* ShaderDeObjeto(uint32_t objeto);  // nullptr si no se conoce
uint64_t GeneracionObjetos();  // changes with every shader created
EstadisticasGanchos EstadisticasDeGanchos();

// From the shader constructor hooks: before the original, the library entry of the container at `direccion`
// (2005 or 2008, nullptr if unknown); after it, the object the original returned.
namespace ganchos_detalle {
const EntradaShader* IdentificarCreacion(const uint8_t* base, uint32_t direccion, bool vertices);
void RecordarCreacion(uint32_t objeto, const EntradaShader* entrada, bool vertices);
}  // namespace ganchos_detalle

}  // namespace nfsmw::nativo
