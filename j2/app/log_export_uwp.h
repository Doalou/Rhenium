// Copyright 2026 The Rhénium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RHENIUM_LOG_EXPORT_UWP_H_
#define RHENIUM_LOG_EXPORT_UWP_H_

#include <string>

namespace rhenium {

// Recopie le journal de l'app (|journal_path|, vidé ici) suivi de celui du
// moteur (|engine_log_path|), tous deux dans LocalState et invisibles depuis
// le PC, vers un fichier horodaté de Phone\Downloads\<app> lisible par USB :
// toutes les secondes sur un thread à part, puis une dernière fois en cas
// d'exception non gérée, après avoir noté le code d'exception, le thread et
// la pile (adresses relatives à l'exe, à situer avec rhenium_shell.exe.pdb).
void StartLogExport(const std::wstring& journal_path,
                    const std::wstring& engine_log_path);

// Ajoute une ligne horodatée au journal de l'app.
void Note(const char* format, ...);

}  // namespace rhenium

#endif  // RHENIUM_LOG_EXPORT_UWP_H_
