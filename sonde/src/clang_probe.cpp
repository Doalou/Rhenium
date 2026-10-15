// Module compilé par clang-cl sans exceptions C++, comme Chromium (build.ps1 -Compiler clang).
// Vérifie que le code généré par LLVM pour Windows ARM32 tourne sur le téléphone et se lie
// avec la coquille MSVC. LLVM ne sait pas compiler les exceptions C++ pour cette cible.

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>

// Somme des carrés de 1 à 100 après un tri par la STL : 338350.
extern "C" uint32_t rhenium_clang_sum_squares() {
  std::array<uint32_t, 100> values;
  std::iota(values.rbegin(), values.rend(), 1u);
  std::sort(values.begin(), values.end());
  uint64_t total = 0;
  for (uint32_t v : values) total += static_cast<uint64_t>(v) * v;
  return static_cast<uint32_t>(total);
}

// Division 64 bits : sur ARM32, passe par les fonctions d'aide de la bibliothèque d'exécution.
extern "C" uint64_t rhenium_clang_div(uint64_t a, uint32_t b) {
  return a / b;
}

extern "C" char const* rhenium_clang_version() {
  return "clang " __clang_version__;
}
