// nfsmw - language of the game's text
//
// The PAL discs carry the text of ten languages in NFS/ZZDATA (LANGUAGES\ENGLISH.BIN ... POLISH.BIN, checked
// in the ZDIR.BIN of the PAL Spanish disc: all ten names are in its hash index). What makes an edition Spanish,
// French, German or Italian is the executable: at startup sub_823B48A0 calls SetCurrentLanguage with a
// constant (li r3,4 in the Spanish one); the French, German and Italian builds differ in those constants
// (docs/editions.md). Movies and speech stay in the disc's language; the text, menus and their textures
// (LANGUAGES\LANGUAGETEXTURES.BIN) follow the current language.
//
// The table the game uses, 0x828FF000, 20 bytes per language, ten entries:
//   +0 id, +4 name ("SPANISH"), +8 file ("LANGUAGES\SPANISH.BIN"), +12 and +16 per-language data.
//   ids: 0 English, 1 French, 2 German, 3 Italian, 4 Spanish, 5 Dutch, 6 Swedish, 7 Danish, 12 Polish,
//   13 Finnish.
// The current language is the word at 0x828FF0C8 (-1 until set). Its only writer is SetCurrentLanguage,
// sub_822BCBD0(r3 = id), called once at startup and by the game's own next/previous language function
// (sub_822BC640), which skips any language whose file is missing through sub_823BCBC8(r3 = file name).
//
// With nfsmw_idioma >= 0 the first SetCurrentLanguage (the startup one) gets that id instead, if its file is
// on the disc; otherwise the edition's language stays and a warning is logged.
#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(nfsmw_idioma, -1, "NFSMW",
                     "Idioma de los textos del juego: -1 = el de la edicion; 0 ingles, 1 frances, 2 aleman, "
                     "3 italiano, 4 espanol, 5 neerlandes, 6 sueco, 7 danes, 12 polaco, 13 fines. Las voces y los "
                     "videos siguen en el idioma del disco")
    .range(-1, 13)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::idioma {
namespace {

constexpr uint32_t kTabla = 0x828FF000;
constexpr uint32_t kEntrada = 20;
constexpr uint32_t kEntradas = 10;
constexpr uint32_t kActual = 0x828FF0C8;

uint32_t Leer32(const uint8_t* base, uint32_t dir) {
  uint32_t v = 0;
  std::memcpy(&v, base + dir, sizeof(v));
  return __builtin_bswap32(v);
}

std::atomic<bool> g_aplicado{false};

}  // namespace
}  // namespace nfsmw::idioma

// The file check the game's language selector uses: r3 = file name, returns nonzero if it is on the disc.
REX_EXTERN(__imp__sub_823BCBC8);

// SetCurrentLanguage(r3 = language id).
REX_EXTERN(__imp__sub_822BCBD0);
REX_HOOK_RAW(sub_822BCBD0) {
  using namespace nfsmw::idioma;
  const int32_t pedido = REXCVAR_GET(nfsmw_idioma);
  if (pedido < 0 || g_aplicado.exchange(true)) {
    __imp__sub_822BCBD0(ctx, base);
    return;
  }
  const uint32_t de_la_edicion = ctx.r3.u32;
  // The table must be the one described above: ten entries whose +8 points at "LANGUAGES\...".
  uint32_t fichero = 0;
  bool tabla_ok = true;
  for (uint32_t i = 0; i < kEntradas; ++i) {
    const uint32_t entrada = kTabla + i * kEntrada;
    const uint32_t nombre = Leer32(base, entrada + 8);
    if (nombre < 0x82000000u || nombre >= 0x83000000u ||
        std::memcmp(base + nombre, "LANGUAGES\\", 10) != 0) {
      tabla_ok = false;
      break;
    }
    if (Leer32(base, entrada) == uint32_t(pedido)) {
      fichero = nombre;
    }
  }
  if (!tabla_ok || Leer32(base, kActual) != 0xFFFFFFFFu) {
    REXLOG_WARN("[idioma] la tabla de idiomas no es la esperada: se mantiene el idioma de la edicion ({})",
                de_la_edicion);
    __imp__sub_822BCBD0(ctx, base);
    return;
  }
  bool existe = false;
  if (fichero) {
    // A separate context: the real one goes to SetCurrentLanguage untouched apart from r3.
    PPCContext prueba = ctx;
    prueba.r3.u64 = fichero;
    __imp__sub_823BCBC8(prueba, base);
    existe = prueba.r3.u32 != 0;
    ctx.fpscr.setcsr(ctx.fpscr.csr);
  }
  if (!existe) {
    REXLOG_WARN("[idioma] el idioma {} no esta en este disco{}: se mantiene el de la edicion ({})", pedido,
                fichero ? "" : " (id desconocido)", de_la_edicion);
    __imp__sub_822BCBD0(ctx, base);
    return;
  }
  REXLOG_INFO("[idioma] textos en '{}' (id {}) en lugar del idioma de la edicion ({})",
              reinterpret_cast<const char*>(base + fichero), pedido, de_la_edicion);
  ctx.r3.u64 = uint32_t(pedido);
  __imp__sub_822BCBD0(ctx, base);
}
