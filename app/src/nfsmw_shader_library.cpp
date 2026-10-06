#include "nfsmw_shader_library.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads
 * through 64- and 32-bit pointers without may_alias, and GCC may hoist that read above the write of the
 * data being hashed (strict aliasing). That made the texture key read claves[4] before writing it, and
 * the same texture was created several times (tools/comprobar_clave_textura.py checks a built ELF for
 * this). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h ya se ha incluido con su implementacion antes de este punto: XXH_FORCE_MEMORY_ACCESS 0 llegaria tarde"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace nfsmw::native {
namespace {
constexpr std::array<uint8_t, 8> kFirma{'N','F','S','S','P','V',0,0};
constexpr size_t kMaxArchivo = 64 * 1024 * 1024;
constexpr size_t kMaxShaders = 4096;
constexpr size_t kMaxOriginal = 64 * 1024;
constexpr size_t kMaxSpirv = 4 * 1024 * 1024;

void Exigir(bool condicion, const char* error) {
  if (!condicion) throw std::runtime_error(error);
}

uint32_t BE(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
         uint32_t(p[2]) << 8 | p[3];
}

struct Lector {
  std::span<const uint8_t> datos;
  size_t posicion = 0;
  std::span<const uint8_t> Tomar(size_t n) {
    Exigir(n <= datos.size() - posicion, "Paquete de shaders truncado");
    auto r = datos.subspan(posicion, n);
    posicion += n;
    return r;
  }
  uint32_t U32() {
    auto p = Tomar(4);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  }
  uint64_t U64() {
    const uint64_t bajo = U32();
    return bajo | uint64_t(U32()) << 32;
  }
};

void U32(std::vector<uint8_t>& d, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) d.push_back(uint8_t(v >> (i * 8)));
}
void U64(std::vector<uint8_t>& d, uint64_t v) {
  U32(d, uint32_t(v)); U32(d, uint32_t(v >> 32));
}

void Validar(Shader& s) {
  Exigir(s.original.size() >= 24 && s.original.size() <= kMaxOriginal,
         "Longitud del contenedor fuera de limites");
  const uint32_t firma = BE(s.original.data());
  // 2005 container (Most Wanted) or 2008 (NFS Carbon). The 2008 physical part can also hold the
  // definitions after the microcode, so it is only 4-byte aligned.
  const bool formato2008 = (firma & ~1u) == 0x102A1100;
  Exigir((firma & ~1u) == 0x102A0E00 || formato2008, "Firma de contenedor desconocida");
  const uint32_t virtuales = BE(s.original.data() + 4);
  const uint32_t fisicos = BE(s.original.data() + 8);
  Exigir(virtuales >= (formato2008 ? 36u : 24u) && fisicos && !(fisicos % (formato2008 ? 4 : 12)) &&
             uint64_t(virtuales) + fisicos == s.original.size(),
         "Longitudes del contenedor incoherentes");
  s.vertices = (firma & 1) != 0;
  s.huella = XXH3_64bits(s.original.data(), s.original.size());
  Exigir(s.spirv.size() >= 5 && s.spirv.size() <= kMaxSpirv / 4,
         "Longitud SPIR-V fuera de limites");
  Exigir(s.spirv[0] == 0x07230203 && s.spirv[4] == 0,
         "Cabecera SPIR-V incorrecta");
  // This checks the wrapper; it does not replace spirv-val in the packager.
  bool entrada = false;
  for (size_t i = 5; i < s.spirv.size();) {
    const uint32_t palabras = s.spirv[i] >> 16, op = s.spirv[i] & 65535;
    Exigir(palabras && palabras <= s.spirv.size() - i, "Instruccion SPIR-V truncada");
    if (op == 15) {  // OpEntryPoint: modelo de ejecucion, id, nombre e interfaz.
      Exigir(!entrada && palabras >= 5 && s.spirv[i+1] == (s.vertices ? 0u : 4u) &&
                 s.spirv[i+3] == 0x6e69616d && s.spirv[i+4] == 0,
             "La etapa o la entrada main de SPIR-V no coincide con el contenedor");
      entrada = true;
    }
    i += palabras;
  }
  Exigir(entrada, "SPIR-V sin entrada main");
}

bool Menor(const Shader& a, const Shader& b) {
  if (a.huella != b.huella) return a.huella < b.huella;
  return a.original < b.original;
}
}  // namespace

void BibliotecaShaders::Cargar(std::span<const uint8_t> archivo) {
  Exigir(archivo.size() >= 24 && archivo.size() <= kMaxArchivo,
         "Tamano del paquete de shaders fuera de limites");
  Lector l{archivo};
  auto firma = l.Tomar(8);
  Exigir(std::equal(firma.begin(), firma.end(), kFirma.begin()), "Firma de paquete desconocida");
  Exigir(l.U32() == 1, "Version de paquete no admitida");
  const uint32_t cantidad = l.U32();
  Exigir(cantidad && cantidad <= kMaxShaders, "Cantidad de shaders fuera de limites");
  const uint64_t huella = l.U64();
  Exigir(XXH3_64bits(archivo.data() + 24, archivo.size() - 24) == huella,
         "Paquete de shaders alterado");
  std::vector<Shader> nuevos;
  nuevos.reserve(cantidad);
  for (uint32_t i = 0; i < cantidad; ++i) {
    const uint32_t original = l.U32(), palabras = l.U32();
    const uint64_t esperada = l.U64();
    Exigir(original >= 24 && original <= kMaxOriginal &&
               palabras >= 5 && palabras <= kMaxSpirv / 4, "Entrada demasiado grande");
    Shader s;
    auto datos = l.Tomar(original);
    s.original.assign(datos.begin(), datos.end());
    // Reading the whole range first avoids allocations with a truncated file.
    Lector codigo{l.Tomar(size_t(palabras) * 4)};
    s.spirv.reserve(palabras);
    for (uint32_t j = 0; j < palabras; ++j) s.spirv.push_back(codigo.U32());
    Validar(s);
    Exigir(s.huella == esperada, "Huella de contenedor incorrecta");
    if (!nuevos.empty()) Exigir(Menor(nuevos.back(), s), "Entradas repetidas o desordenadas");
    nuevos.push_back(std::move(s));
  }
  Exigir(l.posicion == archivo.size(), "Datos sobrantes en el paquete de shaders");
  shaders_ = std::move(nuevos);
}

void BibliotecaShaders::Cargar(const std::filesystem::path& archivo) {
  std::ifstream f(archivo, std::ios::binary | std::ios::ate);
  Exigir(bool(f), "No se pudo abrir el paquete de shaders");
  const auto n = f.tellg();
  Exigir(n >= 24 && n <= std::streamoff(kMaxArchivo), "Tamano del paquete fuera de limites");
  std::vector<uint8_t> datos(static_cast<size_t>(n));
  f.seekg(0);
  Exigir(bool(f.read(reinterpret_cast<char*>(datos.data()), datos.size())), "Lectura incompleta del paquete");
  Cargar(datos);
}

const Shader* BibliotecaShaders::Buscar(std::span<const uint8_t> original) const {
  if (original.size() < 24 || original.size() > kMaxOriginal) return nullptr;
  const uint64_t huella = XXH3_64bits(original.data(), original.size());
  auto it = std::lower_bound(shaders_.begin(), shaders_.end(), huella,
      [](const Shader& s, uint64_t h) { return s.huella < h; });
  // A hash collision can never pick another shader: the bytes are compared.
  for (; it != shaders_.end() && it->huella == huella; ++it)
    if (it->original.size() == original.size() &&
        std::equal(original.begin(), original.end(), it->original.begin())) return &*it;
  return nullptr;
}

std::vector<uint8_t> EmpaquetarShaders(std::vector<Shader> shaders) {
  Exigir(!shaders.empty() && shaders.size() <= kMaxShaders, "Cantidad de shaders fuera de limites");
  for (auto& s : shaders) Validar(s);
  std::sort(shaders.begin(), shaders.end(), Menor);
  std::vector<uint8_t> cuerpo;
  uint32_t cantidad = 0;
  const Shader* anterior = nullptr;
  for (const auto& s : shaders) {
    if (anterior && anterior->original == s.original) {
      Exigir(anterior->spirv == s.spirv, "Un contenedor tiene dos traducciones distintas");
      continue;
    }
    const size_t n = 16 + s.original.size() + s.spirv.size() * 4;
    Exigir(n <= kMaxArchivo - 24 - cuerpo.size(), "Paquete demasiado grande");
    U32(cuerpo, uint32_t(s.original.size())); U32(cuerpo, uint32_t(s.spirv.size()));
    U64(cuerpo, s.huella);
    cuerpo.insert(cuerpo.end(), s.original.begin(), s.original.end());
    for (uint32_t palabra : s.spirv) U32(cuerpo, palabra);
    ++cantidad;
    anterior = &s;
  }
  std::vector<uint8_t> archivo(kFirma.begin(), kFirma.end());
  U32(archivo, 1); U32(archivo, cantidad);
  U64(archivo, XXH3_64bits(cuerpo.data(), cuerpo.size()));
  archivo.insert(archivo.end(), cuerpo.begin(), cuerpo.end());
  return archivo;
}
}  // namespace nfsmw::native
