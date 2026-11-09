// Copyright 2026 Doalo
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Rhénium : journal de l'app et export vers Downloads. Code Win32 et WinRT
// (ABI) seul, sans //base : il doit encore marcher quand le moteur a planté.

#include "rhenium/log_export_uwp.h"

#include <windows.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.storage.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <exception>
#include <vector>

namespace rhenium {
namespace {

using ABI::Windows::Foundation::AsyncStatus;
using ABI::Windows::Foundation::IAsyncAction;
using ABI::Windows::Foundation::IAsyncInfo;
using ABI::Windows::Foundation::IAsyncOperation;
using ABI::Windows::Storage::IDownloadsFolderStatics;
using ABI::Windows::Storage::IFileIOStatics;
using ABI::Windows::Storage::IStorageFile;
using ABI::Windows::Storage::IStorageItem;
using ABI::Windows::Storage::StorageFile;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

std::wstring g_journal_path;
std::wstring g_engine_log_path;
std::wstring g_stderr_path;    // sortie d'erreur (paniques de Rust)
std::wstring g_previous_path;  // journaux du lancement précédent
ComPtr<IStorageFile> g_target;
ULONGLONG g_exported_size = 0;
wchar_t g_start_stamp[32];  // horodatage du lancement, dans le nom des fichiers
unsigned g_sequence = 0;
CRITICAL_SECTION g_export_lock;
CRITICAL_SECTION g_journal_lock;
CRITICAL_SECTION g_crash_lock;  // une exception à la fois dans le journal

// Attente active d'une opération WinRT, appelée hors du thread UI.
HRESULT Wait(IUnknown* operation, DWORD timeout_ms) {
  ComPtr<IAsyncInfo> info;
  HRESULT hr = operation->QueryInterface(IID_PPV_ARGS(&info));
  if (FAILED(hr))
    return hr;
  AsyncStatus status = AsyncStatus::Started;
  for (DWORD waited = 0; waited < timeout_ms; waited += 10) {
    if (FAILED(info->get_Status(&status)) || status != AsyncStatus::Started)
      break;
    Sleep(10);
  }
  if (status == AsyncStatus::Completed)
    return S_OK;
  HRESULT error = E_FAIL;
  info->get_ErrorCode(&error);
  return FAILED(error) ? error : E_FAIL;
}

// Chaque export crée un nouveau fichier et supprime le précédent : le PC lit
// les fichiers par MTP, qui garde en cache la taille d'un fichier déjà vu et
// en tronque la copie. Un fichier neuf est toujours lu en entier.
HRESULT CreateTarget(ComPtr<IStorageFile>* target) {
  ComPtr<IDownloadsFolderStatics> downloads;
  HRESULT hr = RoGetActivationFactory(
      HStringReference(RuntimeClass_Windows_Storage_DownloadsFolder).Get(),
      IID_PPV_ARGS(&downloads));
  if (FAILED(hr))
    return hr;
  if (!g_start_stamp[0]) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    swprintf_s(g_start_stamp, L"%04u%02u%02u-%02u%02u%02u", now.wYear,
               now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  }
  wchar_t name[64];
  swprintf_s(name, L"rhenium-%s-%04u.log", g_start_stamp, ++g_sequence);
  ComPtr<IAsyncOperation<StorageFile*>> create;
  hr = downloads->CreateFileAsync(HStringReference(name).Get(), &create);
  if (SUCCEEDED(hr))
    hr = Wait(create.Get(), 5000);
  ComPtr<IStorageFile> file;
  if (SUCCEEDED(hr))
    hr = create->GetResults(&file);
  if (SUCCEEDED(hr))
    *target = file;
  return hr;
}

void DeleteTarget(IStorageFile* file, DWORD timeout_ms) {
  ComPtr<IStorageItem> item;
  ComPtr<IAsyncAction> remove;
  if (SUCCEEDED(file->QueryInterface(IID_PPV_ARGS(&item))) &&
      SUCCEEDED(item->DeleteAsyncOverloadDefaultOptions(&remove))) {
    Wait(remove.Get(), timeout_ms);
  }
}

// Ajoute à |bytes| la fin de |path| (2 Mo au plus). Le moteur garde son
// journal ouvert.
void AppendFile(const std::wstring& path, std::vector<BYTE>* bytes,
                LONGLONG kMaxBytes = 2 * 1024 * 1024) {
  HANDLE file = CreateFile2(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING,
      nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  LARGE_INTEGER size = {};
  GetFileSizeEx(file, &size);
  LARGE_INTEGER start = {};
  start.QuadPart = size.QuadPart > kMaxBytes ? size.QuadPart - kMaxBytes : 0;
  const LONGLONG length = size.QuadPart - start.QuadPart;
  if (length > 0 && SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) {
    static const char kTruncated[] = "(début tronqué)\r\n";
    if (start.QuadPart)
      bytes->insert(bytes->end(), kTruncated, kTruncated + sizeof(kTruncated) - 1);
    const size_t offset = bytes->size();
    bytes->resize(offset + static_cast<size_t>(length));
    DWORD read = 0;
    ReadFile(file, bytes->data() + offset, static_cast<DWORD>(length), &read,
             nullptr);
    bytes->resize(offset + read);
  }
  CloseHandle(file);
}

// |title| puis |path|, si le fichier n'est pas vide.
void AppendSection(const char* title, const std::wstring& path,
                   std::vector<BYTE>* bytes) {
  if (path.empty())
    return;
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
      (data.nFileSizeHigh == 0 && data.nFileSizeLow == 0)) {
    return;
  }
  bytes->insert(bytes->end(), title, title + strlen(title));
  AppendFile(path, bytes);
}

// Recopie journal Rhénium puis journal du moteur, s'ils ont changé.
void Export(DWORD timeout_ms) {
  if (g_journal_path.empty())  // StartLogExport n'a pas tourné
    return;
  EnterCriticalSection(&g_export_lock);
  std::vector<BYTE> bytes;
  AppendFile(g_journal_path, &bytes);
  static const char kSeparator[] = "\r\n---- content_shell.log ----\r\n";
  bytes.insert(bytes.end(), kSeparator, kSeparator + sizeof(kSeparator) - 1);
  AppendFile(g_engine_log_path, &bytes);
  AppendSection("\r\n---- sortie d'erreur ----\r\n", g_stderr_path, &bytes);
  AppendSection("\r\n==== lancement précédent ====\r\n", g_previous_path,
                &bytes);
  if (bytes.size() != g_exported_size) {
    ComPtr<IStorageFile> target;
    ComPtr<IFileIOStatics> file_io;
    HRESULT hr = CreateTarget(&target);
    if (SUCCEEDED(hr)) {
      hr = RoGetActivationFactory(
          HStringReference(RuntimeClass_Windows_Storage_FileIO).Get(),
          IID_PPV_ARGS(&file_io));
    }
    ComPtr<IAsyncAction> write;
    if (SUCCEEDED(hr)) {
      hr = file_io->WriteBytesAsync(target.Get(),
                                    static_cast<UINT32>(bytes.size()),
                                    bytes.data(), &write);
    }
    if (SUCCEEDED(hr) && SUCCEEDED(Wait(write.Get(), timeout_ms))) {
      g_exported_size = bytes.size();
      if (g_target)
        DeleteTarget(g_target.Get(), timeout_ms);
      g_target = target;
    } else if (target) {
      DeleteTarget(target.Get(), timeout_ms);
    }
  }
  LeaveCriticalSection(&g_export_lock);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers);
void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                                unsigned, uintptr_t);
void __cdecl OnPureCall();

// Chaque seconde ; un plantage exporte aussitôt. Remet aussi nos gestionnaires
// si quelqu'un les a remplacés (content installe les siens, qui sortent par
// _exit sans rien dire).
DWORD WINAPI ExportThread(void*) {
  RoInitialize(RO_INIT_MULTITHREADED);
  for (;;) {
    LPTOP_LEVEL_EXCEPTION_FILTER previous =
        SetUnhandledExceptionFilter(&OnUnhandledException);
    if (previous != &OnUnhandledException) {
      rhenium::Note("filtre d'exceptions remplacé par 0x%p, remis",
                    reinterpret_cast<void*>(previous));
    }
    if (_set_invalid_parameter_handler(&OnInvalidParameter) !=
        &OnInvalidParameter) {
      rhenium::Note("gestionnaire de paramètre invalide remis");
    }
    if (_set_purecall_handler(&OnPureCall) != &OnPureCall)
      rhenium::Note("gestionnaire d'appel virtuel pur remis");
    Export(5000);
    Sleep(1000);
  }
}

void AppendToJournal(const char* text) {
  if (g_journal_path.empty())
    return;
  EnterCriticalSection(&g_journal_lock);
  HANDLE file = CreateFile2(
      g_journal_path.c_str(), FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_ALWAYS,
      nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    CloseHandle(file);
  }
  LeaveCriticalSection(&g_journal_lock);
}

FARPROC SystemProc(const char* name) {
  for (const wchar_t* module : {L"ntdll.dll", L"kernelbase.dll"}) {
    if (HMODULE handle = GetModuleHandleW(module)) {
      if (FARPROC proc = GetProcAddress(handle, name))
        return proc;
    }
  }
  return nullptr;
}

void DescribeThread(char* out, size_t size) {
  out[0] = 0;
  using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get_description =
      reinterpret_cast<GetDescription>(SystemProc("GetThreadDescription"));
  PWSTR description = nullptr;
  if (get_description &&
      SUCCEEDED(get_description(GetCurrentThread(), &description)) &&
      description) {
    WideCharToMultiByte(CP_UTF8, 0, description, -1, out,
                        static_cast<int>(size), nullptr, nullptr);
    LocalFree(description);
  }
}

#if defined(_M_ARM)
// « nom.dll+0xdécalage » pour une adresse hors de l'exe.
void DescribeAddress(DWORD pc, int depth, char* out, size_t size) {
  using PcToFileHeader = PVOID(NTAPI*)(PVOID, PVOID*);
  static const auto pc_to_file_header =
      reinterpret_cast<PcToFileHeader>(SystemProc("RtlPcToFileHeader"));
  void* module = nullptr;
  wchar_t path[MAX_PATH] = L"";
  if (pc_to_file_header &&
      pc_to_file_header(reinterpret_cast<void*>(pc), &module) && module &&
      GetModuleFileNameW(static_cast<HMODULE>(module), path, MAX_PATH)) {
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    char narrow[64];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr,
                        nullptr);
    sprintf_s(out, size, "  #%02d %s+0x%X\r\n", depth, narrow,
              pc - reinterpret_cast<DWORD>(module));
  } else {
    sprintf_s(out, size, "  #%02d 0x%08X\r\n", depth, pc);
  }
}
#endif

#if defined(_M_ARM)
// Pile d'appels déroulée par les tables .pdata, adresses relatives à l'exe
// quand elles y tombent (adresses de retour : situer l'instruction d'avant).
void AppendStack(const CONTEXT& start) {
  using LookupFn = PRUNTIME_FUNCTION(NTAPI*)(DWORD, PDWORD, void*);
  using UnwindFn = PEXCEPTION_ROUTINE(NTAPI*)(DWORD, DWORD, DWORD,
                                              PRUNTIME_FUNCTION, PCONTEXT,
                                              PVOID*, PDWORD, void*);
  static const auto lookup =
      reinterpret_cast<LookupFn>(SystemProc("RtlLookupFunctionEntry"));
  static const auto unwind =
      reinterpret_cast<UnwindFn>(SystemProc("RtlVirtualUnwind"));
  if (!lookup || !unwind) {
    AppendToJournal("  (pas de déroulage de pile)\r\n");
    return;
  }
  const auto base = reinterpret_cast<DWORD>(GetModuleHandle(nullptr));
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const DWORD end = base + nt->OptionalHeader.SizeOfImage;
  CONTEXT context = start;
  for (int depth = 0; depth < 32 && context.Pc; ++depth) {
    const DWORD pc = context.Pc & ~1u;
    char line[96];
    if (pc >= base && pc < end)
      sprintf_s(line, "  #%02d exe+0x%X\r\n", depth, pc - base);
    else
      DescribeAddress(pc, depth, line, sizeof(line));
    AppendToJournal(line);
    DWORD image_base = 0;
    PRUNTIME_FUNCTION function = lookup(pc, &image_base, nullptr);
    const DWORD sp = context.Sp;
    if (function) {
      void* handler_data = nullptr;
      DWORD establisher = 0;
      unwind(0, image_base, pc, function, &context, &handler_data,
             &establisher, nullptr);
    } else {
      context.Pc = context.Lr;  // fonction feuille
    }
    if ((context.Pc & ~1u) == pc && context.Sp == sp)
      break;
  }
}
#endif

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) {
  EnterCriticalSection(&g_crash_lock);
  const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
  char thread[64];
  DescribeThread(thread, sizeof(thread));
  char line[256];
  sprintf_s(line,
            "\r\n[Rhenium] exception 0x%08lX (adresse 0x%p) thread %lu "
            "« %s »\r\n",
            record->ExceptionCode,
            record->NumberParameters >= 2
                ? reinterpret_cast<void*>(record->ExceptionInformation[1])
                : nullptr,
            GetCurrentThreadId(), thread);
  AppendToJournal(line);
#if defined(_M_ARM)
  AppendStack(*pointers->ContextRecord);
#endif
  Export(3000);
  LeaveCriticalSection(&g_crash_lock);
  return EXCEPTION_CONTINUE_SEARCH;
}

// Messages de OutputDebugString (V8 y écrit ses erreurs fatales faute de
// console) : le système les fait passer par une exception que l'on lit ici.
// Les lignes du journal du moteur (« [pid:tid:... ») y sont déjà.
LONG WINAPI OnDebugString(EXCEPTION_POINTERS* pointers) {
  const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
  const char* text = nullptr;
  if (record->ExceptionCode == 0x4001000A && record->NumberParameters >= 4)
    text = reinterpret_cast<const char*>(record->ExceptionInformation[3]);
  else if (record->ExceptionCode == 0x40010006 && record->NumberParameters >= 2)
    text = reinterpret_cast<const char*>(record->ExceptionInformation[1]);
  if (!text) {
    // Exceptions graves, notées dès leur levée (même rattrapées ensuite), les
    // vingt premières seulement.
    static LONG noted = 0;
    // Plus les points d'arrêt (__debugbreak de ImmediateCrash) et le manque
    // de mémoire de PartitionAlloc.
    const DWORD code = record->ExceptionCode;
    const bool grave = (code & 0xF0000000) == 0xC0000000 ||
                       code == 0x80000003 || code == 0xE0000008;
    const LONG index = grave ? InterlockedIncrement(&noted) : 0;
    if (index > 0 && index <= 20) {
      const auto base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
      const auto at = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
      char thread[64];
      DescribeThread(thread, sizeof(thread));
      char line[256];
      sprintf_s(line,
                "[levée] exception 0x%08lX à exe+0x%X (adresse 0x%p) thread "
                "%lu « %s »\r\n",
                record->ExceptionCode, static_cast<unsigned>(at - base),
                record->NumberParameters >= 2
                    ? reinterpret_cast<void*>(record->ExceptionInformation[1])
                    : nullptr,
                GetCurrentThreadId(), thread);
      AppendToJournal(line);
#if defined(_M_ARM)
      if (index <= 5)
        AppendStack(*pointers->ContextRecord);
#endif
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (text[0] != '[') {
    AppendToJournal("[debug] ");
    AppendToJournal(text);
    if (text[0] && text[strlen(text) - 1] != '\n')
      AppendToJournal("\r\n");
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// Arrêts qui ne passent pas par une exception : on note la pile avant que le
// CRT n'appelle __fastfail ou _exit.
void NoteFatal(const char* what) {
  EnterCriticalSection(&g_crash_lock);
  char thread[64];
  DescribeThread(thread, sizeof(thread));
  char line[192];
  snprintf(line, sizeof(line), "\r\n[Rhenium] %s thread %lu « %s »\r\n", what,
           GetCurrentThreadId(), thread);
  AppendToJournal(line);
#if defined(_M_ARM)
  CONTEXT context = {};
  RtlCaptureContext(&context);
  AppendStack(context);
#endif
  Export(3000);
  LeaveCriticalSection(&g_crash_lock);
}

void OnAbort(int) {
  NoteFatal("abort()");
}

void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                                unsigned, uintptr_t) {
  NoteFatal("paramètre invalide (CRT)");
}

void __cdecl OnPureCall() {
  NoteFatal("appel virtuel pur");
}

void OnTerminate() {
  NoteFatal("std::terminate");
  abort();
}

void __cdecl OnExit() {
  NoteFatal("exit()");
}

}  // namespace

void StartLogExport(const std::wstring& journal_path,
                    const std::wstring& engine_log_path) {
  static bool started = false;  // coquille puis moteur : une seule fois
  if (started)
    return;
  started = true;
  g_journal_path = journal_path;
  g_engine_log_path = engine_log_path;
  InitializeCriticalSectionEx(&g_export_lock, 0, 0);
  InitializeCriticalSectionEx(&g_journal_lock, 0, 0);
  InitializeCriticalSectionEx(&g_crash_lock, 0, 0);

  // Un arrêt sans exception ne laisse pas le temps d'exporter : les journaux
  // du lancement précédent, lus avant que le moteur n'efface le sien, partent
  // avec ceux de celui-ci.
  const std::wstring dir =
      journal_path.substr(0, journal_path.find_last_of(L'\\') + 1);
  g_stderr_path = dir + L"stderr.txt";
  g_previous_path = dir + L"precedent.log";
  {
    std::vector<BYTE> previous;
    AppendFile(journal_path, &previous, 256 * 1024);
    static const char kEngine[] = "\r\n---- content_shell.log ----\r\n";
    previous.insert(previous.end(), kEngine, kEngine + sizeof(kEngine) - 1);
    AppendFile(engine_log_path, &previous, 256 * 1024);
    static const char kStderr[] = "\r\n---- sortie d'erreur ----\r\n";
    previous.insert(previous.end(), kStderr, kStderr + sizeof(kStderr) - 1);
    AppendFile(g_stderr_path, &previous, 64 * 1024);
    HANDLE file = CreateFile2(g_previous_path.c_str(), GENERIC_WRITE, 0,
                              CREATE_ALWAYS, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(file, previous.data(), static_cast<DWORD>(previous.size()),
                &written, nullptr);
      CloseHandle(file);
    }
  }

  DeleteFileW(journal_path.c_str());
  // Rust écrit ses paniques sur la sortie d'erreur avant d'abandonner.
  HANDLE error_file = CreateFile2(
      g_stderr_path.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_ALWAYS,
      nullptr);
  if (error_file != INVALID_HANDLE_VALUE)
    SetStdHandle(STD_ERROR_HANDLE, error_file);
  SetUnhandledExceptionFilter(&OnUnhandledException);
  AddVectoredExceptionHandler(0, &OnDebugString);
  signal(SIGABRT, &OnAbort);
  _set_invalid_parameter_handler(&OnInvalidParameter);
  _set_purecall_handler(&OnPureCall);
  std::set_terminate(&OnTerminate);
  atexit(&OnExit);
  HANDLE thread = CreateThread(nullptr, 0, &ExportThread, nullptr, 0, nullptr);
  if (thread)
    CloseHandle(thread);
}

void FlushNow() {
  Export(3000);
}

void Note(const char* format, ...) {
  SYSTEMTIME now;
  GetLocalTime(&now);
  // Tronquée au besoin : les variantes _s du CRT arrêtent le processus quand
  // le tampon est trop court.
  char line[2048];
  int length = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u ", now.wHour,
                        now.wMinute, now.wSecond, now.wMilliseconds);
  va_list args;
  va_start(args, format);
  vsnprintf(line + length, sizeof(line) - length - 2, format, args);
  va_end(args);
  const size_t end = strlen(line);
  line[end] = '\r';
  line[end + 1] = '\n';
  line[end + 2] = 0;
  AppendToJournal(line);
}

}  // namespace rhenium
