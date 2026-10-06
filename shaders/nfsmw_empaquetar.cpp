#include "../app/src/nfsmw_shader_library.h"
#include "nfsmw_contenedor.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
std::vector<uint8_t> Leer(const fs::path& ruta) {
  std::ifstream f(ruta, std::ios::binary | std::ios::ate);
  if (!f || f.tellg() < 0 || f.tellg() > 64 * 1024 * 1024)
    throw std::runtime_error("Archivo de entrada ilegible o demasiado grande: " + ruta.string());
  std::vector<uint8_t> d(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(d.data()), d.size())) throw std::runtime_error("Lectura incompleta");
  return d;
}

int main(int argc, char** argv) try {
  if (argc != 4) throw std::runtime_error("Uso: nfsmw_empaquetar <contenedores> <spirv validados> <salida nueva>");
  const fs::path originales = argv[1], compilados = argv[2], salida = argv[3];
  if (fs::exists(salida)) throw std::runtime_error("La salida ya existe");
  std::vector<fs::path> rutas;
  for (const auto& e : fs::directory_iterator(originales))
    if (e.is_regular_file() && e.path().extension() == ".bin") rutas.push_back(e.path());
  std::sort(rutas.begin(), rutas.end());
  std::vector<nfsmw::native::Shader> shaders;
  for (const auto& ruta : rutas) {
    nfsmw::native::Shader s;
    s.original = Leer(ruta);
    // NFS Carbon's 2008 containers are what XenosRecomp reads directly; BibliotecaShaders::Cargar validates them.
    const bool formato2008 = s.original.size() >= 4 && s.original[0] == 0x10 && s.original[1] == 0x2A &&
                             s.original[2] == 0x11;
    nfsmw::Flujo flujo;
    if (!formato2008) (void)nfsmw::Convertir2005(s.original, flujo);
    auto spv = Leer(compilados / (ruta.stem().string() + ".spv"));
    if (spv.size() % 4) throw std::runtime_error("SPIR-V desalineado");
    for (size_t i = 0; i < spv.size(); i += 4)
      s.spirv.push_back(uint32_t(spv[i]) | uint32_t(spv[i+1]) << 8 |
                       uint32_t(spv[i+2]) << 16 | uint32_t(spv[i+3]) << 24);
    shaders.push_back(std::move(s));
  }
  auto paquete = nfsmw::native::EmpaquetarShaders(std::move(shaders));
  nfsmw::native::BibliotecaShaders biblioteca;
  biblioteca.Cargar(paquete);
  for (const auto& ruta : rutas)
    if (!biblioteca.Buscar(Leer(ruta))) throw std::runtime_error("Un shader no se encuentra tras empaquetar");
  std::ofstream f(salida, std::ios::binary);
  if (!f.write(reinterpret_cast<const char*>(paquete.data()), paquete.size()) || !f.flush())
    throw std::runtime_error("No se pudo escribir el paquete completo");
  std::printf("%zu contenedores -> %zu shaders unicos, %zu bytes; todos recuperables\n",
              rutas.size(), biblioteca.shaders().size(), paquete.size());
  return 0;
} catch (const std::exception& e) {
  std::fprintf(stderr, "%s\n", e.what());
  return 1;
}
