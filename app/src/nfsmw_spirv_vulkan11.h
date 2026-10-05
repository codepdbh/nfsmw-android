// nfsmw - the shader library's SPIR-V for Vulkan 1.1 drivers
//
// DXC builds the library for vulkan1.2, which writes SPIR-V 1.5. A Vulkan 1.1 driver only has to accept up to
// SPIR-V 1.3 (1.4 needs VK_KHR_spirv_1_4), and many Mali, Adreno 6xx and PowerVR phones ship such drivers.
// Since the library no longer reads constants through 64-bit pointers (shader_common.h: no Int64, no
// PhysicalStorageBufferAddresses), what is left of 1.4/1.5 in its modules is:
//   - the version word;
//   - the OpEntryPoint interface: from 1.4 on it lists every global variable the entry point uses (uniform
//     buffers, push constants, the texture heaps); up to 1.3 it may only list Input and Output variables.
// RuntimeDescriptorArray already comes with OpExtension "SPV_EXT_descriptor_indexing", which Vulkan 1.1 gets
// from VK_EXT_descriptor_indexing. Checked with spirv-val --target-env vulkan1.1 on all 152 modules of the PAL
// Spanish library after this conversion (tools/tests/spirv_vulkan11_test.cpp runs it over a folder of modules).
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace nfsmw::spirv11 {

constexpr uint32_t kMagia = 0x07230203;
constexpr uint32_t kVersion13 = 0x00010300;
constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpVariable = 59;
constexpr uint32_t kInput = 1;
constexpr uint32_t kOutput = 3;

// Whether a module is newer than SPIR-V 1.3 and has to be converted for a Vulkan 1.1 device.
inline bool Necesita(const uint32_t* palabras, size_t n) {
  return n >= 5 && palabras[0] == kMagia && palabras[1] > kVersion13;
}

// The module as SPIR-V 1.3. Empty if the input is not valid SPIR-V (the caller then keeps the original and the
// driver decides).
inline std::vector<uint32_t> Convertir(const uint32_t* palabras, size_t n) {
  std::vector<uint32_t> salida;
  if (n < 5 || palabras[0] != kMagia) {
    return salida;
  }
  // Storage class of every OpVariable.
  std::unordered_map<uint32_t, uint32_t> clase;
  for (size_t i = 5; i < n;) {
    const uint32_t cuenta = palabras[i] >> 16;
    const uint32_t op = palabras[i] & 0xFFFF;
    if (cuenta == 0 || i + cuenta > n) {
      return {};
    }
    if (op == kOpVariable && cuenta >= 4) {
      clase[palabras[i + 2]] = palabras[i + 3];
    }
    i += cuenta;
  }
  salida.reserve(n);
  salida.insert(salida.end(), palabras, palabras + 5);
  salida[1] = kVersion13;
  for (size_t i = 5; i < n;) {
    const uint32_t cuenta = palabras[i] >> 16;
    const uint32_t op = palabras[i] & 0xFFFF;
    if (op != kOpEntryPoint || cuenta < 4) {
      salida.insert(salida.end(), palabras + i, palabras + i + cuenta);
      i += cuenta;
      continue;
    }
    // OpEntryPoint: execution model, function id, a nul-terminated name padded to whole words, interface ids.
    size_t j = i + 3;
    for (; j < i + cuenta; ++j) {
      const uint32_t w = palabras[j];
      if ((w & 0xFF) == 0 || ((w >> 8) & 0xFF) == 0 || ((w >> 16) & 0xFF) == 0 || (w >> 24) == 0) {
        ++j;
        break;
      }
    }
    const size_t inicio = salida.size();
    salida.insert(salida.end(), palabras + i, palabras + j);
    for (; j < i + cuenta; ++j) {
      const auto it = clase.find(palabras[j]);
      if (it != clase.end() && (it->second == kInput || it->second == kOutput)) {
        salida.push_back(palabras[j]);
      }
    }
    salida[inicio] = (uint32_t(salida.size() - inicio) << 16) | kOpEntryPoint;
    i += cuenta;
  }
  return salida;
}

}  // namespace nfsmw::spirv11
