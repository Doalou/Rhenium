// Windows 10 Mobile n'a pas kernel32legacy.dll, vers laquelle le schéma des api sets envoie
// des fonctions qu'utilisent V8 et //base (kernel32-legacy, synch-ansi, mm-time) : l'app ne
// démarrerait pas. On définit ici leurs pointeurs d'import (__imp_*), que l'éditeur de
// liens prend à la place des imports, avec des équivalents disponibles pour les apps.

#include <windows.h>

namespace {

// V8 ne s'en sert que pour IsWindows8OrGreater / IsWindows10OrGreater : vrai sur W10M.
BOOL WINAPI RheniumVerifyVersionInfoW(LPOSVERSIONINFOEXW, DWORD, DWORDLONG) {
  return TRUE;
}

// V8 crée ses sémaphores sans nom (src/base/platform/semaphore.cc).
HANDLE WINAPI RheniumCreateSemaphoreA(LPSECURITY_ATTRIBUTES attributes, LONG initial_count,
                                      LONG maximum_count, LPCSTR name) {
  if (name != nullptr) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return nullptr;
  }
  return CreateSemaphoreExW(attributes, initial_count, maximum_count, nullptr, 0,
                            SEMAPHORE_ALL_ACCESS);
}

// Horloge à la milliseconde de V8 (TimeTicks, Date.now via platform-win32). Compteur de
// performance plutôt que GetTickCount64, qui n'avance que par pas d'environ 16 ms. Le
// débordement à 32 bits est géré par V8 comme pour timeGetTime.
DWORD WINAPI RheniumTimeGetTime() {
  static LONGLONG const frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<DWORD>(now.QuadPart / frequency * 1000 + now.QuadPart % frequency * 1000 / frequency);
}

// //base (time_win.cc) règle la résolution du minuteur système : sans objet pour une app,
// le minuteur de W10M n'est pas réglable. MMRESULT est un UINT, absent des en-têtes « app ».
UINT WINAPI RheniumTimeBeginPeriod(UINT) {
  return 0;  // TIMERR_NOERROR
}

UINT WINAPI RheniumTimeEndPeriod(UINT) {
  return 0;  // TIMERR_NOERROR
}

}  // namespace

extern "C" {
// Sans console, V8 écrit ses messages (dont ceux d'erreur fatale) par OutputDebugStringA,
// invisibles sur le téléphone : la sonde les redirige vers son journal.
void (*rhenium_debug_sink)(char const* text) = nullptr;
}

namespace {
void WINAPI RheniumOutputDebugStringA(LPCSTR text) {
  if (rhenium_debug_sink && text) rhenium_debug_sink(text);
}
}  // namespace

extern "C" {
decltype(&RheniumOutputDebugStringA) __imp_OutputDebugStringA = &RheniumOutputDebugStringA;
decltype(&RheniumVerifyVersionInfoW) __imp_VerifyVersionInfoW = &RheniumVerifyVersionInfoW;
decltype(&RheniumCreateSemaphoreA) __imp_CreateSemaphoreA = &RheniumCreateSemaphoreA;
decltype(&RheniumTimeGetTime) __imp_timeGetTime = &RheniumTimeGetTime;
decltype(&RheniumTimeBeginPeriod) __imp_timeBeginPeriod = &RheniumTimeBeginPeriod;
decltype(&RheniumTimeEndPeriod) __imp_timeEndPeriod = &RheniumTimeEndPeriod;
}
