// Remplace third_party/googletest/.../gtest/gtest_prod.h, non téléchargé (fetch.ps1 sans tests).
// V8 n'en utilise que FRIEND_TEST, défini ici comme dans googletest.
#ifndef GOOGLETEST_INCLUDE_GTEST_GTEST_PROD_H_
#define GOOGLETEST_INCLUDE_GTEST_GTEST_PROD_H_

#define FRIEND_TEST(test_case_name, test_name) \
  friend class test_case_name##_##test_name##_Test

#endif  // GOOGLETEST_INCLUDE_GTEST_GTEST_PROD_H_
