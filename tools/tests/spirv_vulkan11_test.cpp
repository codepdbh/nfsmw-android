// Converts every .spv of a folder with app/src/nfsmw_spirv_vulkan11.h and writes the result to another folder,
// so it can be checked with spirv-val --target-env vulkan1.1 (NDK shader-tools). It checks itself that every
// module needed the conversion and came out as SPIR-V 1.3, no longer than the input.
//
//   clang++ -std=c++20 -I app/src tools/tests/spirv_vulkan11_test.cpp -o spirv_vulkan11_test
//   spirv_vulkan11_test <folder with .spv> <output folder>
#include "nfsmw_spirv_vulkan11.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "uso: spirv_vulkan11_test <entrada> <salida>\n");
    return 2;
  }
  std::filesystem::create_directories(argv[2]);
  int modulos = 0, fallos = 0;
  for (const auto& e : std::filesystem::directory_iterator(argv[1])) {
    if (e.path().extension() != ".spv") continue;
    std::ifstream in(e.path(), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    std::vector<uint32_t> w(bytes.size() / 4);
    std::memcpy(w.data(), bytes.data(), w.size() * 4);
    ++modulos;
    if (!nfsmw::spirv11::Necesita(w.data(), w.size())) {
      std::fprintf(stderr, "%s: no necesita conversion\n", e.path().filename().string().c_str());
      ++fallos;
      continue;
    }
    const std::vector<uint32_t> c = nfsmw::spirv11::Convertir(w.data(), w.size());
    if (c.empty() || c[1] != nfsmw::spirv11::kVersion13 || nfsmw::spirv11::Necesita(c.data(), c.size()) ||
        c.size() > w.size()) {
      std::fprintf(stderr, "%s: conversion incorrecta\n", e.path().filename().string().c_str());
      ++fallos;
      continue;
    }
    std::ofstream out(std::filesystem::path(argv[2]) / e.path().filename(), std::ios::binary);
    out.write(reinterpret_cast<const char*>(c.data()), std::streamsize(c.size() * 4));
  }
  std::printf("%d modulos, %d fallos\n", modulos, fallos);
  return fallos ? 1 : 0;
}
