// Sonde Rhénium : mesure ce que Windows 10 Mobile accorde réellement à une app UWP
// (mémoire, génération de code pour le JIT de V8, Direct3D 11). Voir README.md.
//
// Les résultats s'affichent à l'écran et sont écrits dans LocalState\resultats.txt.
// Avant chaque étape risquée, LocalState\progression.txt note l'étape en cours :
// si l'app est tuée, ce fichier dit où.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Security.ExchangeActiveSyncProvisioning.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Display.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Media.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <vector>

#ifndef SONDE_CODEGEN
#define SONDE_CODEGEN 1
#endif

// Première configuration V8 d'un cycle (build.ps1 -V8First N, base 0) : après une installation,
// la sonde commence à cette configuration au lieu de la première.
#ifndef SONDE_V8_FIRST
#define SONDE_V8_FIRST 0
#endif

#if SONDE_A32
#if !defined(_M_ARM)
#error "Le test A32 (-WithA32) n'a de sens que sur ARM32"
#endif
#include <atomic>
#include <chrono>
#include <thread>
#include "a32_probe.h"  // ..\j1\app : code A32 assemblé depuis a32_probe.s
#endif

#if SONDE_V8 || SONDE_BASE
#include <chrono>
#endif

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

namespace {

std::wstring g_report;
std::wstring g_folder;  // LocalState de l'app
Windows::System::Display::DisplayRequest g_keepScreenOn{nullptr};  // sinon le verrouillage suspend les tests

// ---------------------------------------------------------------------------
// Rapport

std::string ToUtf8(std::wstring const& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

void SaveText(wchar_t const* name, std::wstring const& content) {
  if (g_folder.empty()) return;
  std::wstring path = g_folder + L"\\" + name;
  HANDLE h = CreateFile2(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  std::string bytes = ToUtf8(content);
  DWORD written = 0;
  ::WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
}

void Section(wchar_t const* title) {
  g_report += L"\n== ";
  g_report += title;
  g_report += L" ==\n";
}

void Line(std::wstring const& key, std::wstring const& value) {
  g_report += key + L" : " + value + L"\n";
}

// Lit un petit fichier UTF-8 de LocalState (vide s'il n'existe pas).
std::wstring ReadText(wchar_t const* name) {
  std::wstring path = g_folder + L"\\" + name;
  HANDLE h = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
  if (h == INVALID_HANDLE_VALUE) return {};
  std::string bytes(64 * 1024, '\0');
  DWORD read = 0;
  ::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
  CloseHandle(h);
  int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(read), nullptr, 0);
  std::wstring text(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(read), text.data(), n);
  return text;
}

std::wstring g_step;
std::wstring g_previous;  // progression.txt laissé par le lancement précédent

// Note l'étape en cours sur disque avant de la lancer.
void Step(std::wstring const& what) {
  g_step = what;
  SaveText(L"progression.txt", L"Étape en cours : " + what + L"\n" + g_report);
}

std::wstring Hex(uint32_t v) {
  wchar_t buf[16];
  swprintf_s(buf, L"0x%08X", v);
  return buf;
}

std::wstring LastError() {
  DWORD e = GetLastError();
  return L"échec, GetLastError = " + std::to_wstring(e) + L" (" + Hex(e) + L")";
}

std::wstring Mo(uint64_t bytes) {
  return std::to_wstring(bytes / (1024 * 1024)) + L" Mo";
}

template <typename F>
void Guarded(wchar_t const* name, F&& f) {
  Step(std::wstring(L"test ") + name);
  try {
    f();
  } catch (hresult_error const& e) {
    Line(name, L"exception WinRT " + Hex(static_cast<uint32_t>(static_cast<int32_t>(e.code()))) + L" " +
                   std::wstring(e.message()));
  } catch (std::exception const& e) {
    Line(name, L"exception C++ " + std::wstring(to_hstring(e.what())));
  }
}

// ---------------------------------------------------------------------------
// Appareil

void TestDevice() {
  Section(L"Appareil");
  auto vi = Windows::System::Profile::AnalyticsInfo::VersionInfo();
  Line(L"Famille", std::wstring(vi.DeviceFamily()));
  uint64_t v = std::stoull(std::wstring(vi.DeviceFamilyVersion()));
  Line(L"Version", std::to_wstring(v >> 48) + L"." + std::to_wstring((v >> 32) & 0xFFFF) + L"." +
                       std::to_wstring((v >> 16) & 0xFFFF) + L"." + std::to_wstring(v & 0xFFFF));

  Windows::Security::ExchangeActiveSyncProvisioning::EasClientDeviceInformation eas;
  Line(L"Fabricant", std::wstring(eas.SystemManufacturer()));
  Line(L"Modèle", std::wstring(eas.SystemProductName()));
  Line(L"SKU", std::wstring(eas.SystemSku()));

  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  wchar_t const* arch = L"autre";
  switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_ARM: arch = L"ARM"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: arch = L"ARM64"; break;
    case PROCESSOR_ARCHITECTURE_AMD64: arch = L"x64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: arch = L"x86"; break;
  }
  Line(L"Architecture", arch);
  Line(L"Cœurs", std::to_wstring(si.dwNumberOfProcessors));
  Line(L"Taille de page", std::to_wstring(si.dwPageSize) + L" octets");
}

// ---------------------------------------------------------------------------
// Api sets : une DLL importée mais absente du système empêche l'app de démarrer, avant
// tout code. On interroge le système pour ceux que V8 ajoute aux imports de la sonde.
// IsApiSetImplemented est cherché dynamiquement, pour ne rien importer de risqué.

// Absente des en-têtes « app » mais exportée et déjà importée par la CRT de la sonde.
extern "C" __declspec(dllimport) HMODULE WINAPI GetModuleHandleW(LPCWSTR module_name);

// Schéma des api sets (format 6, Windows 10), tel que le chargeur le lit dans le PEB.
struct ApiSetNamespace {
  ULONG Version, Size, Flags, Count, EntryOffset, HashOffset, HashFactor;
};
struct ApiSetNamespaceEntry {
  ULONG Flags, NameOffset, NameLength, HashedLength, ValueOffset, ValueCount;
};
struct ApiSetValueEntry {
  ULONG Flags, NameOffset, NameLength, ValueOffset, ValueLength;
};

// Hôte du set dans le schéma : "" si le set n'y est pas, "(vide)" s'il n'a pas d'hôte.
// Le chargeur compare les noms sans le dernier numéro de version (« -0 » de l1-2-0).
std::wstring ApiSetHost(ApiSetNamespace const* map, std::wstring name) {
  name = name.substr(0, name.rfind(L'-'));
  auto const* base = reinterpret_cast<uint8_t const*>(map);
  auto const* entries = reinterpret_cast<ApiSetNamespaceEntry const*>(base + map->EntryOffset);
  for (ULONG i = 0; i < map->Count; ++i) {
    std::wstring entry(reinterpret_cast<wchar_t const*>(base + entries[i].NameOffset),
                       entries[i].NameLength / sizeof(wchar_t));
    entry = entry.substr(0, entry.rfind(L'-'));
    if (_wcsicmp(entry.c_str(), name.c_str()) != 0) continue;
    if (entries[i].ValueCount == 0) return L"(vide)";
    auto const* values = reinterpret_cast<ApiSetValueEntry const*>(base + entries[i].ValueOffset);
    auto const& v = values[entries[i].ValueCount - 1];
    if (v.ValueLength == 0) return L"(vide)";
    return std::wstring(reinterpret_cast<wchar_t const*>(base + v.ValueOffset), v.ValueLength / sizeof(wchar_t));
  }
  return L"";
}

void TestApiSets() {
  Section(L"Api sets importés par V8");
  // TEB+0x30 -> PEB, PEB+0x38 -> ApiSetMap (disposition 32 bits).
  auto const* teb = reinterpret_cast<uint8_t const*>(NtCurrentTeb());
  auto const* peb = *reinterpret_cast<uint8_t const* const*>(teb + 0x30);
  auto const* map = *reinterpret_cast<ApiSetNamespace const* const*>(peb + 0x38);
  Line(L"Schéma", L"version " + std::to_wstring(map->Version) + L", " + std::to_wstring(map->Count) + L" sets");
  if (map->Version != 6) return;
  char const* const sets[] = {
      "api-ms-win-core-debug-l1-1-0",           "api-ms-win-core-file-l1-2-2",
      "api-ms-win-core-kernel32-legacy-l1-1-1", "api-ms-win-core-libraryloader-l1-2-1",
      "api-ms-win-core-processenvironment-l1-1-0", "api-ms-win-core-profile-l1-1-0",
      "api-ms-win-core-synch-ansi-l1-1-0",      "api-ms-win-core-synch-l1-2-0",
      "api-ms-win-core-timezone-l1-1-0",        "api-ms-win-crt-filesystem-l1-1-0",
      "api-ms-win-crt-time-l1-1-0",             "api-ms-win-crt-utility-l1-1-0",
      "api-ms-win-mm-time-l1-1-0",
  };
  int missing = 0;
  for (char const* set : sets) {
    std::wstring const host = ApiSetHost(map, std::wstring(to_hstring(set)));
    bool const ok = !host.empty() && host != L"(vide)";
    if (!ok) ++missing;
    Line(std::wstring(to_hstring(set)), host.empty() ? L"ABSENT du schéma" : ok ? L"-> " + host : L"ABSENT (hôte vide)");
  }
  Line(L"Absents", std::to_wstring(missing) + L"/" + std::to_wstring(ARRAYSIZE(sets)));

  // Fonctions dont l'api set est douteux : exportées par kernelbase ou kernel32 ?
  HMODULE const modules[] = {GetModuleHandleW(L"kernelbase.dll"), GetModuleHandleW(L"kernel32.dll")};
  for (char const* fn : {"VerifyVersionInfoW", "LoadLibraryW", "timeGetTime", "CreateSemaphoreA", "GetTempPathA"}) {
    std::wstring where;
    if (modules[0] && GetProcAddress(modules[0], fn)) where += L"kernelbase ";
    if (modules[1] && GetProcAddress(modules[1], fn)) where += L"kernel32 ";
    Line(std::wstring(to_hstring(fn)), where.empty() ? L"absente de kernelbase et kernel32" : where);
  }
}

#if SONDE_IMPORT_CHECK
#include "import_check.h"  // généré par build.ps1 -ImportCheck

// Vérifie chaque import d'un autre exe (build V8 ou //base) : DLL hôte (via le schéma des api
// sets), chargement, puis export. Un manque hors chargement différé empêche cet exe de démarrer.
void TestImports() {
  Section(L"Imports de l'exe vérifié");
  auto const* teb = reinterpret_cast<uint8_t const*>(NtCurrentTeb());
  auto const* peb = *reinterpret_cast<uint8_t const* const*>(teb + 0x30);
  auto const* map = *reinterpret_cast<ApiSetNamespace const* const*>(peb + 0x38);
  using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
  auto loadLibrary = reinterpret_cast<LoadLibraryWFn>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "LoadLibraryW"));

  int missing = 0;
  int missingDelayed = 0;
  std::wstring lastDll;
  for (auto const& import : kImportsToCheck) {
    std::wstring dll(to_hstring(import.dll));
    std::wstring const label = import.delayed ? L"(différé) " + dll : dll;
    int& count = import.delayed ? missingDelayed : missing;
    std::wstring host = dll;
    if (_wcsnicmp(dll.c_str(), L"api-", 4) == 0 || _wcsnicmp(dll.c_str(), L"ext-", 4) == 0) {
      host = ApiSetHost(map, dll.substr(0, dll.size() - 4));  // sans « .dll »
      if (host.empty() || host == L"(vide)") {
        if (dll != lastDll) Line(label, L"ABSENT du schéma ou sans hôte");
        lastDll = dll;
        ++count;
        continue;
      }
    }
    HMODULE module = GetModuleHandleW(host.c_str());
    if (!module && loadLibrary) module = loadLibrary(host.c_str());
    if (!module) {
      if (dll != lastDll) Line(label + L" -> " + host, L"hôte NON CHARGEABLE (" + std::to_wstring(GetLastError()) + L")");
      lastDll = dll;
      ++count;
      continue;
    }
    char const* function = import.function[0] == '#' ? MAKEINTRESOURCEA(atoi(import.function + 1)) : import.function;
    if (!GetProcAddress(module, function)) {
      Line(label + L" -> " + host, L"export ABSENT : " + std::wstring(to_hstring(import.function)));
      ++count;
    }
  }
  Line(L"Imports vérifiés", std::to_wstring(ARRAYSIZE(kImportsToCheck)) + L", manquants : " + std::to_wstring(missing) +
                                L" bloquants, " + std::to_wstring(missingDelayed) + L" en chargement différé");
}
#endif

// ---------------------------------------------------------------------------
// Mémoire

std::wstring LevelName(Windows::System::AppMemoryUsageLevel level) {
  using Windows::System::AppMemoryUsageLevel;
  switch (level) {
    case AppMemoryUsageLevel::Low: return L"Low";
    case AppMemoryUsageLevel::Medium: return L"Medium";
    case AppMemoryUsageLevel::High: return L"High";
    case AppMemoryUsageLevel::OverLimit: return L"OverLimit";
  }
  return L"inconnu";
}

void TestMemory() {
  using Windows::System::MemoryManager;
  Section(L"Mémoire");
  Line(L"AppMemoryUsageLimit", Mo(MemoryManager::AppMemoryUsageLimit()));
  Line(L"AppMemoryUsage", Mo(MemoryManager::AppMemoryUsage()));
  Line(L"Niveau", LevelName(MemoryManager::AppMemoryUsageLevel()));
  auto report = MemoryManager::GetAppMemoryReport();
  Line(L"Commit privé", Mo(report.PrivateCommitUsage()));
  Line(L"Pic de commit privé", Mo(report.PeakPrivateCommitUsage()));
  Line(L"Limite de commit totale", Mo(report.TotalCommitLimit()));
}

// Alloue et touche des blocs de 8 Mo jusqu'à 90 % de la limite, puis libère tout.
// Au-delà de 4 Go de limite (un PC), la cible est plafonnée à 1 Go pour ne pas remplir la RAM.
void StressMemory() {
  using Windows::System::MemoryManager;
  Section(L"Remplissage mémoire");
  uint64_t const limit = MemoryManager::AppMemoryUsageLimit();
  uint64_t const cap = limit >= 4096ull * 1024 * 1024 ? 1024ull * 1024 * 1024 : limit;
  uint64_t const target = (limit / 10 * 9 < cap) ? limit / 10 * 9 : cap;
  size_t const chunk = 8 * 1024 * 1024;
  Line(L"Cible", Mo(target));

  std::vector<std::unique_ptr<char[]>> blocks;
  Step(L"remplissage mémoire jusqu'à " + Mo(target));
  while (MemoryManager::AppMemoryUsage() + chunk < target) {
    char* p = new (std::nothrow) char[chunk];
    if (!p) {
      Line(L"Allocation", L"refusée avant la cible");
      break;
    }
    std::memset(p, 0x5A, chunk);
    blocks.emplace_back(p);
  }
  Line(L"Alloué", Mo(blocks.size() * chunk) + L" (" + std::to_wstring(blocks.size()) + L" blocs de 8 Mo)");
  Line(L"AppMemoryUsage au pic", Mo(MemoryManager::AppMemoryUsage()));
  Line(L"Niveau au pic", LevelName(MemoryManager::AppMemoryUsageLevel()));
  blocks.clear();
}

// ---------------------------------------------------------------------------
// JIT : V8 a besoin d'écrire du code machine, de le rendre exécutable, de l'exécuter,
// puis de le repasser en écriture pour le modifier (W^X).

#if defined(_M_ARM)
uint8_t const kCode[] = {0x2A, 0x20, 0x70, 0x47};  // movs r0, #42 ; bx lr (Thumb-2)
uintptr_t const kEntryBit = 1;                       // bit Thumb de l'adresse d'appel
size_t const kPatchOffset = 0;
uint8_t const kPatchValue = 0x2B;                    // movs r0, #43
#elif defined(_M_ARM64)
uint8_t const kCode[] = {0x40, 0x05, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6};  // mov w0, #42 ; ret
uintptr_t const kEntryBit = 0;
size_t const kPatchOffset = 0;
uint8_t const kPatchValue = 0x60;                                           // mov w0, #43
#else
uint8_t const kCode[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};  // mov eax, 42 ; ret
uintptr_t const kEntryBit = 0;
size_t const kPatchOffset = 1;
uint8_t const kPatchValue = 0x2B;                               // mov eax, 43
#endif

bool RunGenerated(void* page, size_t size, int expected) {
  FlushInstructionCache(GetCurrentProcess(), page, size);
  auto fn = reinterpret_cast<int (*)()>(reinterpret_cast<uintptr_t>(page) | kEntryBit);
  Step(L"exécution du code généré (attendu " + std::to_wstring(expected) + L")");
  int got = fn();
  Line(L"Exécution", got == expected ? L"OK (" + std::to_wstring(got) + L")"
                                      : L"valeur inattendue " + std::to_wstring(got));
  return got == expected;
}

void TestJit() {
  Section(L"JIT / codeGeneration");
  Line(L"Capability codeGeneration déclarée", SONDE_CODEGEN ? L"oui" : L"non");

  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  size_t const size = si.dwPageSize;

  Step(L"VirtualAllocFromApp RWX");
  void* rwx = VirtualAllocFromApp(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  Line(L"VirtualAllocFromApp(RWX), refus attendu", rwx ? L"accepté" : LastError());
  if (rwx) VirtualFree(rwx, 0, MEM_RELEASE);

  Step(L"VirtualAllocFromApp RW");
  void* page = VirtualAllocFromApp(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  Line(L"VirtualAllocFromApp(RW)", page ? L"OK" : LastError());
  if (!page) return;
  std::memcpy(page, kCode, sizeof(kCode));

  ULONG old = 0;
  Step(L"VirtualProtectFromApp RW -> RX");
  BOOL ok = VirtualProtectFromApp(page, size, PAGE_EXECUTE_READ, &old);
  Line(L"VirtualProtectFromApp(RX)", ok ? L"OK" : LastError());
  if (ok && RunGenerated(page, size, 42)) {
    Step(L"VirtualProtectFromApp RX -> RW");
    ok = VirtualProtectFromApp(page, size, PAGE_READWRITE, &old);
    Line(L"Retour en écriture (RX -> RW)", ok ? L"OK" : LastError());
    if (ok) {
      static_cast<uint8_t*>(page)[kPatchOffset] = kPatchValue;
      ok = VirtualProtectFromApp(page, size, PAGE_EXECUTE_READ, &old);
      Line(L"Code modifié repassé en RX", ok ? L"OK" : LastError());
      if (ok) RunGenerated(page, size, 43);
    }
  }
  VirtualFree(page, 0, MEM_RELEASE);

  // Double mappage (piste écartée pour V8, voir ..\j1\README.md) : une même mémoire vue en RW
  // pour écrire et en RX pour exécuter. Fonctions cherchées dans kernelbase, sans import.
  using CreateMappingFn = HANDLE(WINAPI*)(HANDLE, PSECURITY_ATTRIBUTES, ULONG, ULONG64, PCWSTR);
  using MapViewFn = PVOID(WINAPI*)(HANDLE, ULONG, ULONG64, SIZE_T);
  HMODULE const kernelbase = GetModuleHandleW(L"kernelbase.dll");
  auto createMapping = reinterpret_cast<CreateMappingFn>(GetProcAddress(kernelbase, "CreateFileMappingFromApp"));
  auto mapView = reinterpret_cast<MapViewFn>(GetProcAddress(kernelbase, "MapViewOfFileFromApp"));
  if (!createMapping || !mapView) {
    Line(L"Double mappage", L"CreateFileMappingFromApp ou MapViewOfFileFromApp introuvable");
    return;
  }
  Step(L"CreateFileMappingFromApp RWX");
  HANDLE mapping = createMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, size, nullptr);
  Line(L"CreateFileMappingFromApp(PAGE_EXECUTE_READWRITE)", mapping ? L"OK" : LastError());
  if (!mapping) return;
  void* rw = mapView(mapping, FILE_MAP_WRITE, 0, size);
  Line(L"Vue RW (MapViewOfFileFromApp)", rw ? L"OK" : LastError());
  void* rx = mapView(mapping, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, size);
  Line(L"Vue RX (MapViewOfFileFromApp)", rx ? L"OK" : LastError());
  if (rw && rx) {
    std::memcpy(rw, kCode, sizeof(kCode));
    if (RunGenerated(rx, size, 42)) {
      static_cast<uint8_t*>(rw)[kPatchOffset] = kPatchValue;  // écrit par la vue RW…
      RunGenerated(rx, size, 43);                              // …exécuté par la vue RX
    }
  }
  if (rw) UnmapViewOfFile(rw);
  if (rx) UnmapViewOfFile(rx);
  CloseHandle(mapping);
}

// ---------------------------------------------------------------------------
// Direct3D 11 : ANGLE (rendu GPU de Chromium) s'appuie dessus.

std::wstring FeatureLevelName(D3D_FEATURE_LEVEL level) {
  switch (level) {
    case D3D_FEATURE_LEVEL_11_1: return L"11_1";
    case D3D_FEATURE_LEVEL_11_0: return L"11_0";
    case D3D_FEATURE_LEVEL_10_1: return L"10_1";
    case D3D_FEATURE_LEVEL_10_0: return L"10_0";
    case D3D_FEATURE_LEVEL_9_3: return L"9_3";
    case D3D_FEATURE_LEVEL_9_2: return L"9_2";
    case D3D_FEATURE_LEVEL_9_1: return L"9_1";
    default: return Hex(static_cast<uint32_t>(level));
  }
}

void TestD3D() {
  Section(L"Direct3D 11");
  D3D_FEATURE_LEVEL const levels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1,
  };
  struct Driver {
    D3D_DRIVER_TYPE type;
    wchar_t const* name;
  };
  Driver const drivers[] = {{D3D_DRIVER_TYPE_HARDWARE, L"Matériel"}, {D3D_DRIVER_TYPE_WARP, L"WARP (logiciel)"}};

  for (auto const& driver : drivers) {
    Step(std::wstring(L"création d'un device D3D11 ") + driver.name);
    com_ptr<ID3D11Device> device;
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, driver.type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), &got, nullptr);
    if (hr == E_INVALIDARG) {  // runtime sans 11_1 : on retente sans
      hr = D3D11CreateDevice(nullptr, driver.type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1,
                             ARRAYSIZE(levels) - 1, D3D11_SDK_VERSION, device.put(), &got, nullptr);
    }
    if (FAILED(hr)) {
      Line(driver.name, L"échec " + Hex(static_cast<uint32_t>(hr)));
      continue;
    }
    Line(driver.name, L"feature level " + FeatureLevelName(got));
    if (driver.type != D3D_DRIVER_TYPE_HARDWARE) continue;

    com_ptr<IDXGIAdapter> adapter;
    if (SUCCEEDED(device.as<IDXGIDevice>()->GetAdapter(adapter.put()))) {
      DXGI_ADAPTER_DESC desc{};
      adapter->GetDesc(&desc);
      Line(L"GPU", desc.Description);
      Line(L"VendorId / DeviceId", Hex(desc.VendorId) + L" / " + Hex(desc.DeviceId));
      Line(L"Mémoire vidéo dédiée", Mo(desc.DedicatedVideoMemory));
      Line(L"Mémoire système partagée", Mo(desc.SharedSystemMemory));
    }
  }
}

// ---------------------------------------------------------------------------

// Module clang-cl (src\clang_probe.cpp), lié quand build.ps1 reçoit -Compiler clang.

#if SONDE_CLANG
extern "C" uint32_t rhenium_clang_sum_squares();
extern "C" uint64_t rhenium_clang_div(uint64_t a, uint32_t b);
extern "C" char const* rhenium_clang_version();

void TestClang() {
  Section(L"Module clang-cl (sans exceptions)");
  Line(L"Version", std::wstring(to_hstring(rhenium_clang_version())));
  Step(L"appel du module clang");
  uint32_t const sum = rhenium_clang_sum_squares();
  Line(L"Somme des carrés 1..100", sum == 338350 ? L"OK (338350)" : L"valeur inattendue " + std::to_wstring(sum));
  uint64_t const a = 0x0123456789ABCDEFull;
  uint32_t volatile b = 12345;  // volatile : MSVC calcule la référence à l'exécution
  uint64_t const expected = a / b;
  uint64_t const got = rhenium_clang_div(a, b);
  Line(L"Division 64 bits", got == expected ? L"OK (" + std::to_wstring(got) + L")"
                                            : L"écart : " + std::to_wstring(got) + L" au lieu de " + std::to_wstring(expected));
}
#endif

// Mode A32 (-WithA32) : le JIT et les builtins de V8 ARM32 émettent de l'A32 (« mode ARM »),
// alors que Windows on ARM est documenté « Thumb-2 uniquement ». Code de ..\j1\app\a32_probe.s,
// copié dans une page RX et appelé par une adresse paire : BLX bascule alors en mode ARM.

#if SONDE_A32
int ThumbAddOne(int x) {
  return x + 1;
}

void TestA32() {
  Section(L"Mode A32 (V8 ARM32)");
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  size_t const size = si.dwPageSize;
  void* page = VirtualAllocFromApp(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!page) {
    Line(L"VirtualAllocFromApp", LastError());
    return;
  }
  std::memcpy(page, kA32Code, sizeof(kA32Code));
  ULONG old = 0;
  if (!VirtualProtectFromApp(page, size, PAGE_EXECUTE_READ, &old)) {
    Line(L"VirtualProtectFromApp", LastError());
    VirtualFree(page, 0, MEM_RELEASE);
    return;
  }
  FlushInstructionCache(GetCurrentProcess(), page, size);
  auto* base = static_cast<uint8_t*>(page);
  auto ret42 = reinterpret_cast<int (*)()>(base + k_a32_ret42);
  auto call = reinterpret_cast<int (*)(int (*)(int), int)>(base + k_a32_call);
  auto loop = reinterpret_cast<uint32_t (*)(uint32_t)>(base + k_a32_loop);

  Step(L"A32 : retour simple (a32_ret42)");
  int const r = ret42();
  Line(L"a32_ret42", r == 42 ? L"OK (42)" : L"valeur inattendue " + std::to_wstring(r));

  Step(L"A32 : appel Thumb depuis A32 (a32_call)");
  int const c = call(&ThumbAddOne, 41);
  Line(L"a32_call (A32 -> Thumb -> A32)", c == 43 ? L"OK (43)" : L"valeur inattendue " + std::to_wstring(c));

  // Des centaines de millions d'additions VFP en A32 sur 8 threads : interruptions et
  // préemptions tombent forcément pendant que le cœur est en mode ARM.
  Step(L"A32 : 8 threads de calcul VFP (a32_loop)");
  uint32_t const n = 1u << 28;
  std::atomic<int> good{0};
  auto const start = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      if (loop(n) == n) ++good;
    });
  }
  for (auto& t : threads) t.join();
  auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  Line(L"a32_loop sur 8 threads", std::to_wstring(good.load()) + L"/8 corrects en " + std::to_wstring(ms) + L" ms");

  // Défauts de page en A32 : 64 Mo engagés mais jamais touchés, une page à zéro à la demande
  // par accès.
  auto touch = reinterpret_cast<uint32_t (*)(uint8_t*, uint32_t)>(base + k_a32_touch);
  uint32_t const pages = 16384;
  auto* zone = static_cast<uint8_t*>(VirtualAllocFromApp(nullptr, size_t{pages} * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!zone) {
    Line(L"a32_touch", L"allocation " + LastError());
  } else {
    Step(L"A32 : défauts de page sur 64 Mo (a32_touch)");
    uint32_t const sum = touch(zone, pages);
    uint32_t written = 0;
    for (uint32_t i = 0; i < pages; ++i) written += zone[size_t{i} * 4096];
    Line(L"a32_touch (16384 défauts de page)", sum == 0 && written == pages ? L"OK" :
         L"lus " + std::to_wstring(sum) + L", écrits " + std::to_wstring(written) + L"/" + std::to_wstring(pages));
    VirtualFree(zone, 0, MEM_RELEASE);
  }

  // Pages de garde de la pile en A32 : sur un thread neuf (pile engagée à la demande), la
  // descente franchit une page de garde à chaque page.
  auto stack = reinterpret_cast<uint32_t (*)(uint32_t)>(base + k_a32_stack);
  Step(L"A32 : croissance de la pile par pages de garde (a32_stack)");
  uint32_t stackPages = 0;
  std::thread([&] { stackPages = stack(192); }).join();
  Line(L"a32_stack (192 pages de pile)", stackPages == 192 ? L"OK" : L"valeur inattendue " + std::to_wstring(stackPages));

  VirtualFree(page, 0, MEM_RELEASE);
}
#endif

// ---------------------------------------------------------------------------
// V8 (-WithV8) : v8_monolith.lib (..\v8) derrière la cale C ..\j1\app\v8_shim.cc.

#if SONDE_V8
extern "C" char const* rhenium_v8_version();
extern "C" int rhenium_v8_run(char const* flags, char const* source, char* out, int out_size,
                              void (*progress)(char const* step));

// Petits tests indépendants (chacun dans son try/catch, pour localiser une panne), puis un
// banc chronométré : calcul, JSON et RegExp.
char const kV8Script[] = R"JS((function () {
  const out = [];
  function t(nom, f) {
    try { out.push(nom + ' = ' + String(f())); } catch (e) { out.push(nom + ' : ERREUR ' + e); }
  }
  t('fib(20)', () => { function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } return fib(20); });
  t('0.1 + 0.2', () => 0.1 + 0.2);
  t('Math.sqrt(2)', () => Math.sqrt(2));
  t('parseFloat("3.25") * 2', () => parseFloat('3.25') * 2);
  t('(1e21).toString()', () => (1e21).toString());
  t('typeof JSON.stringify', () => typeof JSON.stringify);
  t('JSON.stringify(1)', () => JSON.stringify(1));
  t('JSON.stringify(1.5)', () => JSON.stringify(1.5));
  t('JSON.stringify("é")', () => JSON.stringify('é'));
  t('JSON.stringify([1, 2])', () => JSON.stringify([1, 2]));
  t('JSON.stringify({a: 1})', () => JSON.stringify({ a: 1 }));
  t('JSON.stringify([{a: 1.5}])', () => JSON.stringify([{ a: 1.5 }]));
  t('JSON.parse', () => JSON.parse('{"a":[1,2,3]}').a.length);
  t('RegExp', () => 'a1-b2-c3'.match(/(\w)(\d)/g).join(','));
  // Banc : six épreuves, trois tours (le JIT accélère au fil des tours), gc() complet avant
  // chaque tour pour que le balayage concurrent des pages de code croise l'exécution du JS.
  t('banc', () => {
    const now = () => Date.now();
    function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
    function crible(n) {
      const composé = new Uint8Array(n + 1);
      let premiers = 0;
      for (let i = 2; i <= n; i++) {
        if (composé[i]) continue;
        premiers++;
        for (let j = i * i; j <= n; j += i) composé[j] = 1;
      }
      return premiers;
    }
    function objets(n) {
      const liste = [];
      for (let i = 0; i < n; i++) liste.push({ x: i, y: i * 2, z: 'p' + (i % 100) });
      let somme = 0;
      for (const o of liste) somme += o.x + o.y + o.z.length;
      return somme;
    }
    function tri(n) {
      let graine = 12345;
      const valeurs = [];
      for (let i = 0; i < n; i++) { graine = (graine * 1103515245 + 12345) % 2147483648; valeurs.push(graine); }
      valeurs.sort((a, b) => a - b);
      return valeurs[n >> 1];
    }
    function json(n) {
      const liste = [];
      for (let i = 0; i < n; i++) liste.push({ id: i, nom: 'n' + i, valeur: i * 1.5 });
      return JSON.parse(JSON.stringify(liste)).length;
    }
    function chaînes(n) {
      let texte = '';
      for (let i = 0; i < n; i++) texte += i + '-abc ';
      return texte.match(/(\d+)-(\w+)/g).length;
    }
    const épreuves = [['fib(27)', () => fib(27)], ['crible 1e6', () => crible(1000000)],
                      ['objets 2e5', () => objets(200000)], ['tri 5e4', () => tri(50000)],
                      ['JSON 2e4', () => json(20000)], ['RegExp 2e4', () => chaînes(20000)]];
    const lignes = [];
    for (let tour = 1; tour <= 3; tour++) {
      if (typeof gc === 'function') gc();
      const temps = [];
      let total = 0;
      for (const [nom, f] of épreuves) {
        const t0 = now();
        f();
        const ms = now() - t0;
        total += ms;
        temps.push(nom + ' ' + ms);
      }
      lignes.push('tour ' + tour + ' : ' + temps.join(' | ') + ' | total ' + total + ' ms');
    }
    return '\n' + lignes.join('\n');
  });
  return out.join('\n');
})())JS";

extern "C" void (*rhenium_debug_sink)(char const* text);  // ..\j1\app\w10m_imports.cc
// Attentes d'un thread sur une page de code en cours d'écriture (W^X multi-thread) :
// ..\v8\src\base\platform\platform-win32.cc, WaitForCodePage.
extern "C" void rhenium_wx_stats(uint32_t* waits, double* total_ms, double* max_ms);

// Journal des étapes de V8 dans Téléchargements, écrit de façon synchrone : complet au moment
// d'un plantage, contrairement au rapport. Appelé depuis des threads non STA (.get() permis).
Windows::Storage::StorageFile g_v8Log{nullptr};
std::chrono::steady_clock::time_point g_v8Start;
SRWLOCK g_v8LogLock = SRWLOCK_INIT;

void V8Log(std::wstring const& text) {
  AcquireSRWLockExclusive(&g_v8LogLock);
  if (g_v8Log) {
    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_v8Start).count();
    try {
      Windows::Storage::FileIO::AppendTextAsync(g_v8Log, L"[" + std::to_wstring(ms) + L" ms] " + text + L"\n").get();
    } catch (...) {
    }
  }
  ReleaseSRWLockExclusive(&g_v8LogLock);
}

void V8Progress(char const* step) {
  Step(std::wstring(to_hstring(step)));
  V8Log(std::wstring(to_hstring(step)));
}

// Exceptions matérielles (violation d'accès, instruction illégale…) : V8 meurt sans message.
// Ce gestionnaire vectorisé note le contexte dans le journal, puis laisse la suite se dérouler.
extern "C" const uint8_t v8_Default_embedded_blob_code_[];  // début du blob des builtins (A32)
extern "C" uint32_t v8_Default_embedded_blob_code_size_;

LONG WINAPI V8ExceptionLogger(EXCEPTION_POINTERS* info) {
  DWORD const code = info->ExceptionRecord->ExceptionCode;
  // Exceptions C++, nom de thread, OutputDebugString : normales, ignorées.
  if (code == 0xE06D7363 || code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  static LONG count = 0;
  if (InterlockedIncrement(&count) > 4) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT const& c = *info->ContextRecord;
  auto const* rec = info->ExceptionRecord;
  uintptr_t const blob = reinterpret_cast<uintptr_t>(v8_Default_embedded_blob_code_) & ~uintptr_t{1};
  wchar_t text[768];
  swprintf_s(text,
             L"EXCEPTION 0x%08lX à %p | PC=%08lX (%ls) LR=%08lX SP=%08lX R11=%08lX | "
             L"R0=%08lX R1=%08lX R2=%08lX R3=%08lX R4=%08lX R5=%08lX R6=%08lX R7=%08lX R8=%08lX "
             L"R9=%08lX R10=%08lX R12=%08lX | infos %Iu : %08IX %08IX | PC-blob=%ld",
             code, rec->ExceptionAddress, c.Pc, (c.Cpsr & 0x20) ? L"Thumb" : L"A32", c.Lr, c.Sp, c.R11,
             c.R0, c.R1, c.R2, c.R3, c.R4, c.R5, c.R6, c.R7, c.R8, c.R9, c.R10, c.R12,
             static_cast<size_t>(rec->NumberParameters),
             rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : 0,
             rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0,
             static_cast<long>(c.Pc - blob));
  V8Log(text);
  return EXCEPTION_CONTINUE_SEARCH;
}

void V8DebugSink(char const* text) {
  std::wstring line(to_hstring(text));
  while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
  if (!line.empty()) V8Log(L"[V8] " + line);
}

void Finish();

struct V8Job {
  char const* flags = nullptr;
  wchar_t const* next = L"";  // configuration du lancement suivant
  int status = -1;
  long long ms = 0;
  std::wstring wx;  // attentes W^X
  char out[4096] = {};
  Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
  TextBlock results{nullptr};
};

// Complète le rapport sur le thread d'interface, une fois V8 terminé.
void ReportV8(V8Job const& job) {
  wchar_t const* status = job.status == 0 ? L"OK" : job.status == 1 ? L"exception JavaScript"
                        : job.status == 2 ? L"le script ne compile pas" : L"inconnu";
  Line(L"Statut", status);
  Line(L"Résultat", std::wstring(to_hstring(job.out)));
  Line(L"Temps total (initialisation de V8 + script)", std::to_wstring(job.ms) + L" ms");
  Line(L"Attentes W^X (page de code en écriture)", job.wx);
  Line(L"Lancement suivant", job.next + std::wstring(L" (la sonde se ferme dans 3 s)"));
  job.results.Text(g_report);
  Finish();

  // W10M suspend une app au lieu de la fermer : sans fermeture, le lancement suivant
  // reprendrait cette instance sans refaire les tests. 3 s laissent finir l'export.
  auto dispatcher = job.dispatcher;
  Windows::System::Threading::ThreadPoolTimer::CreateTimer(
      [dispatcher](auto&&) {
        dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [] { Application::Current().Exit(); });
      },
      std::chrono::seconds(3));
}

DWORD WINAPI V8Thread(void* arg) {
  auto* job = static_cast<V8Job*>(arg);
  auto const start = std::chrono::steady_clock::now();
  g_v8Start = start;
  try {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"v8-journal-%02u%02u%02u.txt", t.wHour, t.wMinute, t.wSecond);
    g_v8Log = Windows::Storage::DownloadsFolder::CreateFileAsync(
                  name, Windows::Storage::CreationCollisionOption::GenerateUniqueName).get();
  } catch (...) {
  }
  V8Log(std::wstring(L"début, drapeaux : ") + std::wstring(to_hstring(job->flags)));
  {
    wchar_t text[256];
    swprintf_s(text, L"exe chargé à %p ; blob des builtins à %p (%lu octets)", GetModuleHandleW(nullptr),
               v8_Default_embedded_blob_code_, static_cast<unsigned long>(v8_Default_embedded_blob_code_size_));
    V8Log(text);
  }
  // Absente des en-têtes « app » : cherchée dans kernelbase, pour n'ajouter aucun import.
  using AddVehFn = PVOID(WINAPI*)(ULONG, LONG(WINAPI*)(EXCEPTION_POINTERS*));
  if (auto addVeh = reinterpret_cast<AddVehFn>(
          GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "AddVectoredExceptionHandler"))) {
    addVeh(1, &V8ExceptionLogger);
  } else {
    V8Log(L"AddVectoredExceptionHandler introuvable : exceptions non journalisées");
  }
  rhenium_debug_sink = &V8DebugSink;
  job->status = rhenium_v8_run(job->flags, kV8Script, job->out, sizeof(job->out), &V8Progress);
  job->ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  V8Log(L"fin, statut " + std::to_wstring(job->status) + L" : " + std::wstring(to_hstring(job->out)));
  {
    uint32_t waits = 0;
    double total = 0, max = 0;
    rhenium_wx_stats(&waits, &total, &max);
    wchar_t text[128];
    swprintf_s(text, L"%u, cumul %.3f ms, plus longue %.3f ms", waits, total, max);
    job->wx = text;
    V8Log(std::wstring(L"attentes W^X : ") + text);
  }
  job->dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [job] {
    ReportV8(*job);
    delete job;
  });
  return 0;
}

// Lance V8 en arrière-plan, après l'affichage de la fenêtre : W10M tue une app qui tarde à
// afficher sa première image. Le rapport est exporté avant, pour qu'un plantage de V8 laisse
// une trace (le lancement suivant indique l'étape interrompue).
void StartV8(TextBlock const& results) {
  Section(L"V8 (JavaScript)");
  Line(L"Version", std::wstring(to_hstring(rhenium_v8_version())));

  // Les drapeaux de V8 sont figés après son initialisation : une configuration par lancement,
  // à tour de rôle. W^X : jamais de page RWX ; V8 bascule une page en RW pour y écrire. Un
  // thread qui exécute du code de cette page pendant ce temps attend la fin de l'écriture
  // (WaitForCodePage dans V8) : compilation et GC concurrents sont donc permis.
  struct Config {
    wchar_t const* name;
    char const* flags;
  };
  // --write-protect-code-memory : sans lui, V8 écrit le code dans des pages RW et ne les
  // repasse jamais en RX (il compte sur des pages RWX, refusées par W10M).
#define RHENIUM_WX "--no-write-code-using-rwx --write-protect-code-memory --expose-gc "
  static Config const configs[] = {
      {L"JIT complet multi-thread", RHENIUM_WX "--sparkplug --maglev --turbofan"},
      {L"JIT complet mono-thread", RHENIUM_WX "--single-threaded --sparkplug --maglev --turbofan"},
      {L"jitless (Ignition seul)", "--jitless --expose-gc"},
      {L"Sparkplug seul (multi-thread)", RHENIUM_WX "--sparkplug --no-maglev --no-turbofan"},
      {L"Maglev seul (multi-thread)", RHENIUM_WX "--maglev --no-sparkplug --no-turbofan"},
      {L"TurboFan seul (multi-thread)", RHENIUM_WX "--turbofan --no-sparkplug --no-maglev"},
  };
#undef RHENIUM_WX
  int const count = static_cast<int>(ARRAYSIZE(configs));
  std::wstring saved = ReadText(L"v8-mode-2.txt");  // -2 : liste multi-thread du 2026-10-15
  int index = saved.empty() ? SONDE_V8_FIRST : _wtoi(saved.c_str());
  if (index < 0 || index >= count) index = 0;
  SaveText(L"v8-mode-2.txt", std::to_wstring((index + 1) % count));

  auto* job = new V8Job;
  job->flags = configs[index].flags;
  job->next = configs[(index + 1) % count].name;
  job->dispatcher = Window::Current().Dispatcher();
  job->results = results;
  Line(L"Mode", std::to_wstring(index + 1) + L"/" + std::to_wstring(count) + L" : " + configs[index].name);
  Line(L"Drapeaux", std::wstring(to_hstring(job->flags)));
  results.Text(g_report + L"Statut : en cours…\n");
  Finish();

  // Pile de 4 Mo engagée d'avance : le code ARM de V8 ne sonde pas les pages de garde.
  HANDLE thread = CreateThread(nullptr, 4 * 1024 * 1024, &V8Thread, job, 0, nullptr);
  if (!thread) {
    Line(L"CreateThread", LastError());
    results.Text(g_report);
    Finish();
    delete job;
    return;
  }
  CloseHandle(thread);
}
#endif

// ---------------------------------------------------------------------------
// Chromium //base : test de fumée (..\j1-base\gn\base_smoke.cc) lié quand build.ps1 reçoit
// -WithBase. Lancé sur un thread après l'affichage de la fenêtre, comme V8.

#if SONDE_BASE
extern "C" char const* rhenium_base_version();
#if SONDE_BASE_TESTS
// ..\j1-base\gn\base_tests.cc : sous-ensemble de base_unittests, repris d'un lancement à l'autre.
extern "C" int rhenium_base_tests(void (*step)(char const*), char* out, int out_size);
extern "C" int rhenium_base_tests_state(char* out, int out_size);
#else
extern "C" int rhenium_base_smoke(void (*step)(char const*), char* out, int out_size);
extern "C" void rhenium_base_ui_test(void (*step)(char const*), void (*done)(char const*, int));
#endif
void Finish();

TextBlock g_baseResults{nullptr};

#if !SONDE_BASE_TESTS
// Appelé sur le thread UI par une tâche //base, à la fin du test UI.
void BaseUiDone(char const* text, int failures) {
  Section(L"Chromium //base sur le thread UI (CoreDispatcher)");
  g_report += std::wstring(to_hstring(text));
  Line(L"Échecs", std::to_wstring(failures));
  if (g_baseResults) g_baseResults.Text(g_report);
  Finish();
}
#endif

struct BaseJob {
  Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
  TextBlock results{nullptr};
  char out[64 * 1024] = {};
  int failures = 0;
  long long ms = 0;
};

void BaseStep(char const* what) {
  Step(std::wstring(to_hstring(what)));
}

DWORD WINAPI BaseThread(void* param) {
  auto* job = static_cast<BaseJob*>(param);
  auto const start = std::chrono::steady_clock::now();
#if SONDE_BASE_TESTS
  job->failures = rhenium_base_tests(&BaseStep, job->out, sizeof(job->out));
#else
  job->failures = rhenium_base_smoke(&BaseStep, job->out, sizeof(job->out));
#endif
  job->ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  job->dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [job] {
    g_report += std::wstring(to_hstring(job->out));
    Line(L"Échecs", std::to_wstring(job->failures) + L" (" + std::to_wstring(job->ms) + L" ms)");
    job->results.Text(g_report);
    Finish();
    g_baseResults = job->results;
    delete job;
#if !SONDE_BASE_TESTS
    Step(L"base UI");
    rhenium_base_ui_test(&BaseStep, &BaseUiDone);
#endif
  });
  return 0;
}

void StartBase(TextBlock const& results) {
#if SONDE_BASE_TESTS
  Section(L"Chromium //base : base_unittests (sous-ensemble)");
  {
    // Bilan des lancements précédents, exporté avant que les tests ne puissent tuer l'app.
    static char state[32 * 1024];
    Step(L"bilan des tests précédents");
    rhenium_base_tests_state(state, sizeof(state));
    g_report += L"Avant ce lancement :\n" + std::wstring(to_hstring(state)) + L"Ce lancement :\n";
  }
#else
  Section(L"Chromium //base");
  Line(L"Version", std::wstring(to_hstring(rhenium_base_version())));
#endif
  auto* job = new BaseJob;
  job->dispatcher = Window::Current().Dispatcher();
  job->results = results;
  results.Text(g_report + L"Statut : en cours…\n");
  Finish();
  HANDLE thread = CreateThread(nullptr, 4 * 1024 * 1024, &BaseThread, job, 0, nullptr);
  if (!thread) {
    Line(L"CreateThread", LastError());
    results.Text(g_report);
    Finish();
    delete job;
    return;
  }
  CloseHandle(thread);
}
#endif

// ---------------------------------------------------------------------------
// Rust : bibliothèque statique (rust\src\lib.rs) liée quand build.ps1 reçoit -WithRust.

#if SONDE_RUST
extern "C" uint32_t rhenium_rust_probe();

void TestRust() {
  Section(L"Rust");
  Step(L"appel de rhenium_rust_probe");
  uint32_t const got = rhenium_rust_probe();
  Line(L"rhenium_rust_probe", got == 5050 ? L"OK (5050 : Vec, tri, format!, thread)"
                                          : L"valeur inattendue " + std::to_wstring(got));
}
#endif

// ---------------------------------------------------------------------------

// Copie horodatée dans Téléchargements : contrairement à LocalState, ce dossier est visible
// depuis un PC par USB (Phone\Downloads\Sonde Rhénium).
fire_and_forget ExportToDownloads(std::wstring text) {
  try {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"sonde-%04u%02u%02u-%02u%02u%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    auto file = co_await Windows::Storage::DownloadsFolder::CreateFileAsync(
        name, Windows::Storage::CreationCollisionOption::GenerateUniqueName);
    co_await Windows::Storage::FileIO::WriteTextAsync(file, text);
  } catch (...) {
    SaveText(L"export-telechargements.txt", L"Échec de la copie dans Téléchargements\n");
  }
}

void Finish() {
  SaveText(L"resultats.txt", g_report);
  SaveText(L"progression.txt", L"Terminé\n");
  ExportToDownloads(g_report);
}

void RunAll() {
  g_report = L"Sonde Rhénium 1.0\n";
#if defined(__clang__)
  Line(L"Compilateur", L"clang " + std::to_wstring(__clang_major__) + L"." + std::to_wstring(__clang_minor__));
#else
  Line(L"Compilateur", L"MSVC " + std::to_wstring(_MSC_FULL_VER));
#endif
  Line(L"Résultats", L"Téléchargements\\sonde-*.txt et " + g_folder + L"\\resultats.txt");
  if (!g_previous.empty() && g_previous.rfind(L"Terminé", 0) != 0) {
    Line(L"Lancement précédent", L"interrompu : " + g_previous.substr(0, g_previous.find(L'\n')));
  }

  Guarded(L"Appareil", TestDevice);
  Guarded(L"Api sets", TestApiSets);
#if SONDE_IMPORT_CHECK
  Guarded(L"Imports", TestImports);
#endif
  Guarded(L"Mémoire", TestMemory);
  Guarded(L"Direct3D", TestD3D);
#if SONDE_CLANG
  Guarded(L"clang", TestClang);
#endif
#if SONDE_RUST
  Guarded(L"Rust", TestRust);
#endif
  Guarded(L"JIT", TestJit);
#if SONDE_A32
  Guarded(L"A32", TestA32);
#endif
  Finish();
}

TextBlock BuildUi(Window const& window) {
  TextBlock title;
  title.Text(L"Sonde Rhénium");
  title.FontSize(24);

  TextBlock results;
  results.Text(g_report);
  results.TextWrapping(TextWrapping::Wrap);
  results.IsTextSelectionEnabled(true);
  results.FontFamily(Media::FontFamily(L"Consolas"));
  results.FontSize(13);

  Button stress;
  stress.Content(box_value(L"Tester le remplissage mémoire"));
  stress.Margin(Thickness{0, 8, 0, 0});
  stress.Click([results](auto&&, auto&&) {
    Guarded(L"Remplissage", StressMemory);
    Finish();
    results.Text(g_report);
  });

  Button copy;
  copy.Content(box_value(L"Copier les résultats"));
  copy.Margin(Thickness{0, 8, 0, 8});
  copy.Click([](auto&&, auto&&) {
    Windows::ApplicationModel::DataTransfer::DataPackage package;
    package.SetText(g_report);
    Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
    Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
  });

  StackPanel panel;
  panel.Margin(Thickness{12, 12, 12, 12});
  panel.Children().Append(title);
  panel.Children().Append(stress);
  panel.Children().Append(copy);
  panel.Children().Append(results);

  ScrollViewer scroll;
  scroll.Content(panel);
  window.Content(scroll);
  return results;
}

struct App : ApplicationT<App> {
  App() { Step(L"App construit"); }

  void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&) {
    Step(L"OnLaunched");
    Window window = Window::Current();
    TextBlock results{nullptr};
    if (!window.Content()) {
      try {
        g_keepScreenOn = Windows::System::Display::DisplayRequest();
        g_keepScreenOn.RequestActive();
        RunAll();
        Step(L"construction de l'interface");
        results = BuildUi(window);
        Finish();
      } catch (hresult_error const& e) {
        SaveText(L"progression.txt", L"Erreur fatale WinRT " + Hex(static_cast<uint32_t>(static_cast<int32_t>(e.code()))) +
                                         L" " + std::wstring(e.message()) + L"\n" + g_report);
        throw;
      }
    }
    window.Activate();
#if SONDE_BASE
    if (results) Guarded(L"base", [&] { StartBase(results); });
#endif
#if SONDE_V8
    if (results) Guarded(L"V8", [&] { StartV8(results); });  // en dernier : V8 peut tuer l'app
#endif
  }
};

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  init_apartment();  // XAML exige un thread MTA avant Application::Start
  g_folder = Windows::Storage::ApplicationData::Current().LocalFolder().Path();
  g_previous = ReadText(L"progression.txt");  // avant que Step ne l'écrase
  std::set_terminate([] {
    SaveText(L"progression.txt", L"std::terminate appelé pendant : " + g_step + L"\n" + g_report);
    std::abort();
  });
  Step(L"démarrage de XAML");
  try {
    Application::Start([](auto&&) {
      Step(L"callback XAML, avant make<App>");
      try {
        make<App>();
      } catch (hresult_error const& e) {
        Step(L"make<App> a levé " + Hex(static_cast<uint32_t>(static_cast<int32_t>(e.code()))) + L" " +
             std::wstring(e.message()));
        throw;
      }
    });
  } catch (hresult_error const& e) {
    SaveText(L"progression.txt", L"Échec du démarrage XAML " + Hex(static_cast<uint32_t>(static_cast<int32_t>(e.code()))) +
                                     L" " + std::wstring(e.message()) + L"\n");
    return 1;
  }
  return 0;
}
