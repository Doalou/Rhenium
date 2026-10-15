// Cale entre la sonde (MSVC, C++/WinRT, avec exceptions) et V8 (clang-cl, sans
// exceptions) : une API C minimale, compilée par clang-cl avec les mêmes macros V8_*
// que v8_monolith.lib (voir ..\..\sonde\build.ps1 -WithV8).

#include <cstdio>
#include <memory>

#include "libplatform/libplatform.h"
#include "src/base/logging.h"  // SetPrintStackTrace : appelé par V8_Fatal avant d'abandonner
#include "v8.h"

namespace {

using Progress = void (*)(const char* step);
Progress g_progress = nullptr;  // pour les rappels d'erreur, qui n'ont pas de contexte

void Report(Progress progress, const char* step) {
  if (progress) progress(step);
}

void OnFatalError(const char* location, const char* message) {
  char text[1024];
  std::snprintf(text, sizeof(text), "V8 : ERREUR FATALE dans %s : %s", location ? location : "?",
                message ? message : "?");
  Report(g_progress, text);
}

void OnOutOfMemory(const char* location, const v8::OOMDetails& details) {
  char text[1024];
  std::snprintf(text, sizeof(text), "V8 : MÉMOIRE INSUFFISANTE dans %s (%s) : %s",
                location ? location : "?", details.is_heap_oom ? "tas JS" : "processus",
                details.detail ? details.detail : "sans détail");
  Report(g_progress, text);
}

// V8_Fatal (CHECK, UNREACHABLE…) écrit son message par OutputDebugStringA, puis appelle ceci.
void OnV8Fatal() {
  Report(g_progress, "V8 : V8_Fatal (message ci-dessus), arrêt du processus");
}

void CopyOut(const char* text, char* out, int out_size) {
  std::snprintf(out, static_cast<size_t>(out_size), "%s", text ? text : "");
}

}  // namespace

extern "C" const char* rhenium_v8_version() {
  return v8::V8::GetVersion();
}

// Initialise V8, exécute `source` et écrit le résultat converti en chaîne dans `out`.
// Une seule fois par processus : les drapeaux de V8 sont figés après l'initialisation.
// Renvoie 0 si le script a réussi, 1 s'il a levé une exception JS, 2 s'il ne compile
// pas ; le message correspondant est dans `out`.
extern "C" int rhenium_v8_run(const char* flags, const char* source, char* out,
                              int out_size, Progress progress) {
  g_progress = progress;
  v8::base::SetPrintStackTrace(&OnV8Fatal);

  Report(progress, "V8 : drapeaux");
  v8::V8::SetFlagsFromString(flags);

  Report(progress, "V8 : plateforme");
  static std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());

  Report(progress, "V8 : initialisation");
  v8::V8::Initialize();

  std::unique_ptr<v8::ArrayBuffer::Allocator> allocator(
      v8::ArrayBuffer::Allocator::NewDefaultAllocator());
  v8::Isolate::CreateParams params;
  params.array_buffer_allocator = allocator.get();

  Report(progress, "V8 : création de l'isolate");
  // Allocate + Initialize plutôt que New : les gestionnaires d'erreur sont posés avant
  // l'initialisation du tas, l'étape la plus susceptible d'échouer.
  v8::Isolate* isolate = v8::Isolate::Allocate();
  isolate->SetFatalErrorHandler(&OnFatalError);
  isolate->SetOOMErrorHandler(&OnOutOfMemory);
  v8::Isolate::Initialize(isolate, params);

  int status = 0;
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);

    Report(progress, "V8 : création du contexte");
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);
    v8::TryCatch try_catch(isolate);

    v8::Local<v8::String> code =
        v8::String::NewFromUtf8(isolate, source).ToLocalChecked();
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;

    Report(progress, "V8 : compilation du script");
    if (!v8::Script::Compile(context, code).ToLocal(&script)) {
      status = 2;
    } else {
      Report(progress, "V8 : exécution du script");
      if (!script->Run(context).ToLocal(&result)) status = 1;
    }

    v8::String::Utf8Value text(isolate,
                               status == 0 ? result : try_catch.Exception());
    CopyOut(*text ? *text : "(aucun message)", out, out_size);
  }

  Report(progress, "V8 : fermeture de l'isolate");
  isolate->Dispose();
  return status;
}
