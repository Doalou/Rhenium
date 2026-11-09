// Copyright 2026 Doalo
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Rhénium : fonctions de bureau absentes de Windows 10 Mobile. Le crochet
// d'échec du chargement différé de KERNELBASE bloque le thread appelant, et le
// système tue l'app quelques secondes plus tard. Défini ici, il remplace celui
// de la bibliothèque d'import : la fonction absente rend 0, comme sur un
// système qui ne la gère pas, et le journal la nomme.

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "rhenium/log_export_uwp.h"

// ARM32 : arguments en registres, nettoyés par l'appelant. Un retour de 0
// convient à toute signature à résultat entier ou pointeur.
extern "C" int RheniumMissingFunction() {
  return 0;
}

extern "C" FARPROC WINAPI DelayLoadFailureHook(LPCSTR dll, LPCSTR proc) {
  char name[64];
  if (reinterpret_cast<ULONG_PTR>(proc) >> 16) {
    snprintf(name, sizeof(name), "%s", proc);
  } else {
    snprintf(name, sizeof(name), "#%u",
             static_cast<unsigned>(reinterpret_cast<ULONG_PTR>(proc)));
  }
  // Une note par fonction : GetSysColor revient des dizaines de fois.
  static SRWLOCK lock = SRWLOCK_INIT;
  static char seen[64][48];
  static int seen_count = 0;
  bool first = true;
  AcquireSRWLockExclusive(&lock);
  for (int i = 0; i < seen_count && first; ++i)
    first = strncmp(seen[i], name, sizeof(seen[i]) - 1) != 0;
  if (first && seen_count < 64)
    snprintf(seen[seen_count++], sizeof(seen[0]), "%s", name);
  ReleaseSRWLockExclusive(&lock);
  if (first) {
    rhenium::Note("fonction absente : %s!%s, remplacée par un retour de 0",
                  dll ? dll : "?", name);
  }
  return reinterpret_cast<FARPROC>(&RheniumMissingFunction);
}
