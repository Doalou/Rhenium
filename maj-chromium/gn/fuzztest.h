// Copyright 2026 Doalo
// Remplace third_party/fuzztest/src/fuzztest/fuzztest.h pour le sous-ensemble de
// base_unittests de Rhénium : fuzztest (abseil, re2, centipede) n'est pas porté
// sur Windows ARM32. Les tests ordinaires restent, les tests FUZZ_TEST sont sautés.

#ifndef RHENIUM_FUZZTEST_STUB_H_
#define RHENIUM_FUZZTEST_STUB_H_

#define FUZZ_TEST(suite, function) \
  [[maybe_unused]] static auto* const rhenium_fuzz_##suite##_##function = &function

#endif  // RHENIUM_FUZZTEST_STUB_H_
