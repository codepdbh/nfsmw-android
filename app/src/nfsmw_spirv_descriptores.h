// Four-set packing based on victorgbd/NFSMW-Recompiled-Mobile (3d9358e, GPL-3.0).
// Validate every instruction before editing; never send a partially remapped module to a driver.
#pragma once
#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace nfsmw::spirv {
inline bool JuntarConjuntos(std::vector<uint32_t>& p) {
  if (p.size() < 5 || p[0] != 0x07230203) return false;
  std::unordered_map<uint32_t, uint32_t> conjuntos;
  for (size_t i = 5; i < p.size();) {
    const uint32_t n = p[i] >> 16, op = p[i] & 0xFFFF;
    if (!n || n > p.size() - i) return false;
    if (op == 71) {
      if (n < 3) return false;
      if (p[i + 2] == 33 || p[i + 2] == 34) {
        if (n != 4 || p[i + 1] >= p[3]) return false;
        if (p[i + 2] == 34) {
          if (p[i + 3] > 4 || !conjuntos.emplace(p[i + 1], p[i + 3]).second) return false;
        }
      }
    }
    i += n;
  }
  // DescriptorSet and Binding annotations may appear in either order.
  for (size_t i = 5; i < p.size(); i += p[i] >> 16) {
    if ((p[i] & 0xFFFF) != 71 || (p[i] >> 16) != 4) continue;
    const auto it = conjuntos.find(p[i + 1]);
    if (it == conjuntos.end()) continue;
    if (p[i + 2] == 34 && it->second >= 2) p[i + 3] = it->second - 1;
    else if (p[i + 2] == 33 && it->second == 2) ++p[i + 3];
  }
  return true;
}
}  // namespace nfsmw::spirv
