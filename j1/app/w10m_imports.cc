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

// //base (file_util_win.cc) et le cache disque de //net déplacent par MoveFileW.
BOOL WINAPI RheniumMoveFileW(LPCWSTR from, LPCWSTR to) {
  return MoveFileExW(from, to, MOVEFILE_COPY_ALLOWED);
}

// Moniteurs d'alimentation et de batterie : état inconnu, comme un PC sans batterie.
BOOL WINAPI RheniumGetSystemPowerStatus(LPSYSTEM_POWER_STATUS status) {
  *status = {};
  status->ACLineStatus = 255;
  status->BatteryFlag = 128;  // pas de batterie système
  status->BatteryLifePercent = 255;
  status->BatteryLifeTime = static_cast<DWORD>(-1);
  status->BatteryFullLifeTime = static_cast<DWORD>(-1);
  return TRUE;
}

// base::win::ObjectWatcher (et le sandbox) attendent un objet par
// RegisterWaitForSingleObject / UnregisterWaitEx : refaits sur les attentes du pool de
// threads, ouvertes aux apps. Le handle rendu est l'adresse d'un RheniumWait.
struct RheniumWait {
  PTP_WAIT wait;
  WAITORTIMERCALLBACK callback;
  void* context;
  HANDLE object;
  DWORD timeout;
  bool once;
};

void ArmWait(RheniumWait* w) {
  FILETIME due;
  FILETIME* due_ptr = nullptr;
  if (w->timeout != INFINITE) {
    ULARGE_INTEGER relative;
    relative.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(w->timeout) * 10000);
    due.dwLowDateTime = relative.LowPart;
    due.dwHighDateTime = relative.HighPart;
    due_ptr = &due;
  }
  SetThreadpoolWait(w->wait, w->object, due_ptr);
}

void CALLBACK OnWaitDone(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT result) {
  auto* w = static_cast<RheniumWait*>(context);
  w->callback(w->context, result == WAIT_TIMEOUT);
  if (!w->once)
    ArmWait(w);
}

BOOL WINAPI RheniumRegisterWaitForSingleObject(PHANDLE new_wait, HANDLE object,
                                               WAITORTIMERCALLBACK callback, PVOID context,
                                               ULONG milliseconds, ULONG flags) {
  auto* w = new RheniumWait{nullptr, callback, context, object, milliseconds,
                            (flags & WT_EXECUTEONLYONCE) != 0};
  w->wait = CreateThreadpoolWait(&OnWaitDone, w, nullptr);
  if (!w->wait) {
    delete w;
    return FALSE;
  }
  ArmWait(w);
  *new_wait = reinterpret_cast<HANDLE>(w);
  return TRUE;
}

// Avec INVALID_HANDLE_VALUE, attend la fin des rappels en cours (usage de //base). Sans
// attente demandée, la structure est abandonnée : un rappel peut encore s'en servir.
BOOL WINAPI RheniumUnregisterWaitEx(HANDLE wait_handle, HANDLE completion_event) {
  auto* w = reinterpret_cast<RheniumWait*>(wait_handle);
  if (!w) {
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
  }
  SetThreadpoolWait(w->wait, nullptr, nullptr);
  if (completion_event == nullptr) {
    CloseThreadpoolWait(w->wait);
    return TRUE;
  }
  WaitForThreadpoolWaitCallbacks(w->wait, TRUE);
  CloseThreadpoolWait(w->wait);
  delete w;
  if (completion_event != INVALID_HANDLE_VALUE)
    SetEvent(completion_event);
  return TRUE;
}

}  // namespace

// Sans console, V8 écrit ses messages (dont ceux d'erreur fatale) par OutputDebugStringA,
// invisibles sur le téléphone : la sonde les redirige vers son journal. L'app Rhénium
// (RHENIUM_SHELL) a le journal de content_shell et garde l'import du système.
#if !defined(RHENIUM_SHELL)
extern "C" {
void (*rhenium_debug_sink)(char const* text) = nullptr;
}

namespace {
void WINAPI RheniumOutputDebugStringA(LPCSTR text) {
  if (rhenium_debug_sink && text) rhenium_debug_sink(text);
}
}  // namespace

extern "C" {
decltype(&RheniumOutputDebugStringA) __imp_OutputDebugStringA = &RheniumOutputDebugStringA;
}
#endif

extern "C" {
decltype(&RheniumVerifyVersionInfoW) __imp_VerifyVersionInfoW = &RheniumVerifyVersionInfoW;
decltype(&RheniumCreateSemaphoreA) __imp_CreateSemaphoreA = &RheniumCreateSemaphoreA;
decltype(&RheniumTimeGetTime) __imp_timeGetTime = &RheniumTimeGetTime;
decltype(&RheniumTimeBeginPeriod) __imp_timeBeginPeriod = &RheniumTimeBeginPeriod;
decltype(&RheniumTimeEndPeriod) __imp_timeEndPeriod = &RheniumTimeEndPeriod;
decltype(&RheniumMoveFileW) __imp_MoveFileW = &RheniumMoveFileW;
decltype(&RheniumGetSystemPowerStatus) __imp_GetSystemPowerStatus = &RheniumGetSystemPowerStatus;
decltype(&RheniumRegisterWaitForSingleObject) __imp_RegisterWaitForSingleObject =
    &RheniumRegisterWaitForSingleObject;
decltype(&RheniumUnregisterWaitEx) __imp_UnregisterWaitEx = &RheniumUnregisterWaitEx;
}
