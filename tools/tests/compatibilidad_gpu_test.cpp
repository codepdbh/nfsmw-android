#include "nfsmw_spirv_descriptores.h"
#include "nfsmw_spirv_vulkan11.h"
#include "nfsmw_ordenes_publicadas.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

int main(int argc, char** argv) {
  // Binding before DescriptorSet, cube sharing, samplers and constants must all agree with the host.
  std::vector<uint32_t> p = {0x07230203, 0x00010500, 0, 16, 0,
      (4u<<16)|71, 7, 33, 0, (4u<<16)|71, 7, 34, 2,
      (4u<<16)|71, 8, 34, 3, (4u<<16)|71, 9, 34, 4,
      (4u<<16)|71, 10, 34, 0};
  assert(nfsmw::spirv::JuntarConjuntos(p));
  assert(p[8] == 1 && p[12] == 1 && p[16] == 2 && p[20] == 3 && p[24] == 0);
  // A malformed trailing instruction must leave earlier annotations untouched.
  auto bad = p; bad.push_back((4u << 16) | 71); auto original = bad;
  assert(!nfsmw::spirv::JuntarConjuntos(bad) && bad == original);
  bad = p; bad[5] = 71; original = bad;
  assert(!nfsmw::spirv::JuntarConjuntos(bad) && bad == original);
  alignas(4) uint8_t count[4]{};
  std::vector<uint32_t> commands(200000);
  std::thread producer([&] {
    for (uint32_t i = 0; i < commands.size(); ++i) {
      commands[i] = 0x82000000 + i;
      nfsmw::ordenes::Publicar(count, i + 1);
    }
  });
  uint32_t consumed = 0;
  while (consumed < commands.size()) {
    uint32_t published = nfsmw::ordenes::Publicadas(count);
    while (consumed < published) { assert(commands[consumed] == 0x82000000 + consumed); ++consumed; }
  }
  producer.join();
  assert(count[0] == 0 && count[1] == 3 && count[2] == 13 && count[3] == 64);
  int modules = 0;
  if (argc == 3) {
    std::filesystem::create_directories(argv[2]);
    for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
      if (entry.path().extension() != ".spv") continue;
      std::ifstream input(entry.path(), std::ios::binary);
      std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
      assert(bytes.size() % 4 == 0);
      std::vector<uint32_t> words(bytes.size() / 4);
      std::memcpy(words.data(), bytes.data(), bytes.size());
      if (nfsmw::spirv11::Necesita(words.data(), words.size())) words = nfsmw::spirv11::Convertir(words.data(), words.size());
      assert(nfsmw::spirv::JuntarConjuntos(words));
      std::ofstream output(std::filesystem::path(argv[2]) / entry.path().filename(), std::ios::binary);
      output.write(reinterpret_cast<const char*>(words.data()), words.size() * 4);
      ++modules;
    }
  }
  std::printf("Compatibility checks passed: malformed SPIR-V, descriptor packing, 200000 ARM queue publications; %d real modules\n", modules);
}
