// Copyright 2026 Doalo
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Rhénium : intrinsèques MSVC que clang ne fournit pas sur ARM32 et que la 156
// appelle donc comme des fonctions.

// cppgc::StackStartMarker (content/renderer/renderer_main.cc) : adresse de
// l'emplacement de l'adresse de retour de l'appelant, juste au-dessus de son
// pointeur de trame (r11), comme le donnerait MSVC.
extern "C" __declspec(noinline) void* _AddressOfReturnAddress(void) {
  return static_cast<char*>(__builtin_frame_address(1)) + sizeof(void*);
}