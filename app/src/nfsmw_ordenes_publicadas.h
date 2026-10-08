// Command publication on weakly ordered CPUs; guest words are big endian.
// Adapted from victorgbd/NFSMW-Recompiled-Mobile, commit 3d9358e (GPL-3.0).
#pragma once
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

namespace nfsmw::ordenes {
inline uint32_t Intercambiar(uint32_t n) {
  if constexpr (std::endian::native == std::endian::little)
    return ((n & 255) << 24) | ((n & 0xFF00) << 8) | ((n >> 8) & 0xFF00) | (n >> 24);
  return n;
}
inline uint32_t Publicadas(uint8_t* lista) {
  return Intercambiar(__atomic_load_n(reinterpret_cast<uint32_t*>(lista), __ATOMIC_ACQUIRE));
}
inline void Publicar(uint8_t* lista, uint32_t cuenta) {
  __atomic_store_n(reinterpret_cast<uint32_t*>(lista), Intercambiar(cuenta), __ATOMIC_RELEASE);
}
inline void Escribir(uint8_t* destino, uint32_t valor) {
  valor = Intercambiar(valor);
  std::memcpy(destino, &valor, sizeof(valor));
}
}  // namespace nfsmw::ordenes
