// Test de fumée de //base pour la sonde Rhénium (..\sonde\build.ps1 -WithBase).
// Compilé dans le build GN de //base, appelé par la sonde à travers une
// interface C : rhenium_base_smoke écrit un rapport « clé : valeur » par ligne
// et renvoie le nombre d'échecs. step() est appelé avant chaque test, pour que
// la sonde note l'étape en cours si l'app est tuée.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include "base/at_exit.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/logging/logging_settings.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/rand_util.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "base/system/sys_info.h"
#include "base/task/single_thread_task_executor.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "base/values.h"
#include "base/win/registry.h"
#include "base/win/windows_version.h"

namespace {

class Report {
 public:
  explicit Report(void (*step)(const char*)) : step_(step) {}

  void Step(const char* name) {
    if (step_)
      step_(name);
  }
  void Ok(const std::string& key, const std::string& value) {
    text_ += key + " : " + value + "\n";
  }
  void Fail(const std::string& key, const std::string& value) {
    text_ += key + " : ÉCHEC, " + value + "\n";
    ++failures_;
  }
  void Check(bool ok, const std::string& key, const std::string& value) {
    ok ? Ok(key, value) : Fail(key, value);
  }

  const std::string& text() const { return text_; }
  int failures() const { return failures_; }

 private:
  void (*step_)(const char*);
  std::string text_;
  int failures_ = 0;
};

std::string Path(int key) {
  base::FilePath path;
  if (!base::PathService::Get(key, &path))
    return std::string();
  return path.AsUTF8Unsafe();
}

void TestSystem(Report& r) {
  r.Step("base : système");
  const auto version = base::win::OSInfo::GetInstance()->version_number();
  r.Ok("Windows (OSInfo)", base::StringPrintf("%u.%u.%u.%u", version.major,
                                              version.minor, version.build,
                                              version.patch));
  r.Ok("Processeurs", base::NumberToString(base::SysInfo::NumberOfProcessors()));
  r.Ok("Mémoire physique",
       base::NumberToString(
           base::SysInfo::AmountOfTotalPhysicalMemory().InMiB()) + " Mo");
  r.Ok("Thread courant", base::NumberToString(base::PlatformThread::CurrentId().raw()));
  const uint64_t a = base::RandUint64();
  const uint64_t b = base::RandUint64();
  r.Check(a != b, "RandUint64", base::StringPrintf("%016llx", a));
}

void TestTime(Report& r) {
  r.Step("base : horloges");
  const base::TimeTicks start = base::TimeTicks::Now();
  base::PlatformThread::Sleep(base::Milliseconds(20));
  const base::TimeDelta slept = base::TimeTicks::Now() - start;
  r.Check(slept >= base::Milliseconds(15) && slept < base::Seconds(1),
          "Sleep(20 ms)",
          base::StringPrintf("%.1f ms, haute résolution : %s",
                             slept.InMillisecondsF(),
                             base::TimeTicks::IsHighResolution() ? "oui" : "non"));
  base::Time::Exploded now;
  base::Time::Now().LocalExplode(&now);
  r.Check(now.year >= 2024, "Time::Now",
          base::StringPrintf("%04d-%02d-%02d %02d:%02d:%02d", now.year,
                             now.month, now.day_of_month, now.hour,
                             now.minute, now.second));
}

void TestPaths(Report& r) {
  r.Step("base : chemins");
  const std::string exe = Path(base::FILE_EXE);
  r.Check(!exe.empty(), "FILE_EXE", exe);
  const std::string local = Path(base::DIR_LOCAL_APP_DATA);
  r.Check(local.find("LocalState") != std::string::npos, "DIR_LOCAL_APP_DATA",
          local);
  const std::string roaming = Path(base::DIR_ROAMING_APP_DATA);
  r.Check(roaming.find("RoamingState") != std::string::npos,
          "DIR_ROAMING_APP_DATA", roaming);
  const std::string temp = Path(base::DIR_TEMP);
  r.Check(!temp.empty(), "DIR_TEMP", temp);
  r.Ok("GetHomeDir", base::GetHomeDir().AsUTF8Unsafe());
  r.Check(Path(base::DIR_PROGRAM_FILES).empty(), "DIR_PROGRAM_FILES",
          "refusé comme prévu");
}

void TestFiles(Report& r) {
  r.Step("base : fichiers");
  base::FilePath dir;
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &dir)) {
    r.Fail("Fichiers", "pas de DIR_LOCAL_APP_DATA");
    return;
  }
  const base::FilePath file = dir.AppendASCII("base-fumee.txt");
  const std::string written = "Rhénium //base\n";
  const bool wrote = base::WriteFile(file, written);
  std::string read;
  const bool ok = wrote && base::ReadFileToString(file, &read) && read == written;
  r.Check(ok, "WriteFile + ReadFileToString", file.AsUTF8Unsafe());
  const int64_t size = base::GetFileSize(file).value_or(-1);
  r.Check(size == static_cast<int64_t>(written.size()), "GetFileSize",
          base::NumberToString(size));
  r.Check(base::DeleteFile(file) && !base::PathExists(file), "DeleteFile", "ok");
  base::FilePath temp_file;
  const bool temp_ok = base::CreateTemporaryFile(&temp_file);
  r.Check(temp_ok, "CreateTemporaryFile", temp_file.AsUTF8Unsafe());
  if (temp_ok)
    base::DeleteFile(temp_file);
}

void TestJson(Report& r) {
  r.Step("base : JSON");
  auto value = base::JSONReader::Read(
      R"({"nom": "Rhénium", "liste": [1, 2.5, true, null], "objet": {}})",
      base::JSON_PARSE_RFC);
  if (!value || !value->is_dict()) {
    r.Fail("JSONReader", "lecture impossible");
    return;
  }
  const std::string* name = value->GetDict().FindString("nom");
  r.Check(name && *name == "Rhénium", "JSONReader", name ? *name : "absent");
  std::string out;
  r.Check(base::JSONWriter::Write(*value, &out), "JSONWriter", out);
}

void TestRegistry(Report& r) {
  r.Step("base : registre");
  base::win::RegKey key;
  LONG result = key.Create(HKEY_CURRENT_USER, L"Software\\Rhenium",
                           KEY_READ | KEY_WRITE);
  if (result != ERROR_SUCCESS) {
    r.Fail("RegKey::Create (HKCU)", base::StringPrintf("erreur %ld", result));
    return;
  }
  result = key.WriteValue(L"fumee", 42);
  DWORD read = 0;
  if (result == ERROR_SUCCESS)
    result = key.ReadValueDW(L"fumee", &read);
  r.Check(result == ERROR_SUCCESS && read == 42, "HKCU écriture + lecture",
          base::StringPrintf("erreur %ld, valeur %lu", result, read));
  base::FilePath hive;
  base::PathService::Get(base::DIR_LOCAL_APP_DATA, &hive);
  hive = hive.AppendASCII("hkcu.dat");
  r.Check(base::PathExists(hive), "Ruche privée", hive.AsUTF8Unsafe());

  base::win::RegKey hklm;
  result = hklm.Open(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     KEY_READ);
  std::wstring product;
  if (result == ERROR_SUCCESS)
    result = hklm.ReadValue(L"ProductName", &product);
  r.Ok("HKLM ProductName", result == ERROR_SUCCESS
                               ? base::WideToUTF8(product)
                               : base::StringPrintf("erreur %ld", result));
}

void TestProcess(Report& r) {
  r.Step("base : processus");
  base::Process process = base::LaunchProcess(
      base::CommandLine(base::FilePath(L"cmd.exe")), base::LaunchOptions());
  r.Check(!process.IsValid(), "LaunchProcess", "refusé comme prévu");
}

void TestRunLoop(Report& r) {
  r.Step("base : RunLoop");
  base::SingleThreadTaskExecutor executor;
  base::RunLoop run_loop;
  int ran = 0;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce([](int* ran) { ++*ran; }, &ran));
  const base::TimeTicks start = base::TimeTicks::Now();
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, run_loop.QuitClosure(), base::Milliseconds(30));
  run_loop.Run();
  const base::TimeDelta elapsed = base::TimeTicks::Now() - start;
  r.Check(ran == 1 && elapsed >= base::Milliseconds(25), "PostTask + PostDelayedTask",
          base::StringPrintf("%.1f ms pour 30 ms", elapsed.InMillisecondsF()));
}

void TestThread(Report& r) {
  r.Step("base : Thread");
  base::Thread thread("RheniumFumee");
  if (!thread.Start()) {
    r.Fail("Thread::Start", "échec");
    return;
  }
  base::WaitableEvent done;
  base::PlatformThreadId id;
  thread.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::PlatformThreadId* id, base::WaitableEvent* done) {
                       *id = base::PlatformThread::CurrentId();
                       done->Signal();
                     },
                     &id, &done));
  const bool signaled = done.TimedWait(base::Seconds(5));
  r.Check(signaled && id != base::PlatformThread::CurrentId(), "Thread + PostTask",
          base::StringPrintf("thread %lu", static_cast<unsigned long>(id.raw())));
  thread.Stop();
}

void TestThreadPool(Report& r) {
  r.Step("base : ThreadPool");
  base::ThreadPoolInstance::CreateAndStartWithDefaultParams("Rhenium");
  constexpr int kTasks = 16;
  base::WaitableEvent done;
  std::atomic<int> remaining{kTasks};
  for (int i = 0; i < kTasks; ++i) {
    base::ThreadPool::PostTask(
        FROM_HERE, base::BindOnce(
                       [](std::atomic<int>* remaining, base::WaitableEvent* done) {
                         if (--*remaining == 0)
                           done->Signal();
                       },
                       &remaining, &done));
  }
  const bool signaled = done.TimedWait(base::Seconds(10));
  r.Check(signaled, "ThreadPool : 16 tâches",
          signaled ? "terminées" : "délai dépassé");
}

// Test du thread UI : la pompe UWP s'attache au CoreDispatcher, les tâches
// arrivent donc entre deux événements XAML. Tout est asynchrone : le rapport
// est rendu par done() sur le thread UI.
struct UiTest {
  explicit UiTest(void (*step)(const char*)) : report(step) {}
  Report report;
  void (*done)(const char*, int) = nullptr;
  base::PlatformThreadId ui_thread;
  base::TimeTicks start;
};

void UiFinish(UiTest* test) {
  test->done(test->report.text().c_str(), test->report.failures());
  delete test;
}

void UiNestedRunLoop(UiTest* test) {
  test->report.Step("base UI : RunLoop imbriqué");
  base::RunLoop run_loop(base::RunLoop::Type::kNestableTasksAllowed);
  bool inner_ran = false;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce([](bool* ran) { *ran = true; }, &inner_ran));
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, run_loop.QuitClosure(), base::Milliseconds(20));
  const base::TimeTicks start = base::TimeTicks::Now();
  run_loop.Run();
  const base::TimeDelta elapsed = base::TimeTicks::Now() - start;
  test->report.Check(
      inner_ran && elapsed >= base::Milliseconds(15),
      "RunLoop imbriqué (ProcessEvents)",
      base::StringPrintf("tâche interne %s, %.1f ms pour 20 ms",
                         inner_ran ? "exécutée" : "NON exécutée",
                         elapsed.InMillisecondsF()));
  UiFinish(test);
}

void UiDelayed(UiTest* test) {
  const base::TimeDelta elapsed = base::TimeTicks::Now() - test->start;
  test->report.Check(elapsed >= base::Milliseconds(45) &&
                         elapsed < base::Milliseconds(500),
                     "PostDelayedTask 50 ms (ThreadPoolTimer)",
                     base::StringPrintf("%.1f ms", elapsed.InMillisecondsF()));
  // Le RunLoop imbriqué part d'une tâche, comme une boîte de dialogue modale.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&UiNestedRunLoop, test));
}

void UiFromWorker(UiTest* test) {
  const base::PlatformThreadId id = base::PlatformThread::CurrentId();
  test->report.Check(id == test->ui_thread, "PostTask ThreadPool -> UI",
                     base::StringPrintf("thread %lu, attendu %lu",
                                        static_cast<unsigned long>(id.raw()),
                                        static_cast<unsigned long>(
                                            test->ui_thread.raw())));
  test->report.Step("base UI : tâche différée");
  test->start = base::TimeTicks::Now();
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce(&UiDelayed, test), base::Milliseconds(50));
}

}  // namespace

// À appeler sur le thread XAML, après rhenium_base_smoke (qui initialise //base
// et le ThreadPool).
extern "C" void rhenium_base_ui_test(void (*step)(const char*),
                                     void (*done)(const char*, int)) {
  auto* test = new UiTest(step);
  test->done = done;
  test->report.Step("base UI : SingleThreadTaskExecutor(UI)");
  // Gardé jusqu'à la fin de l'app : il reste attaché au CoreDispatcher.
  static base::SingleThreadTaskExecutor* executor =
      new base::SingleThreadTaskExecutor(base::MessagePumpType::UI);
  (void)executor;
  test->ui_thread = base::PlatformThread::CurrentId();
  test->report.Ok("Pompe UI", "attachée au CoreDispatcher");
  test->report.Step("base UI : PostTask depuis le ThreadPool");
  base::ThreadPool::PostTask(
      FROM_HERE,
      base::BindOnce(
          [](scoped_refptr<base::SingleThreadTaskRunner> ui, UiTest* test) {
            ui->PostTask(FROM_HERE, base::BindOnce(&UiFromWorker, test));
          },
          base::SingleThreadTaskRunner::GetCurrentDefault(), test));
}

extern "C" const char* rhenium_base_version() {
  return "Chromium //base 156.0.8078.88";
}

extern "C" int rhenium_base_smoke(void (*step)(const char*),
                                  char* out,
                                  int out_size) {
  static base::AtExitManager* at_exit = new base::AtExitManager;
  (void)at_exit;
  base::CommandLine::Init(0, nullptr);
  logging::LoggingSettings settings;
  settings.logging_dest = logging::LOG_TO_SYSTEM_DEBUG_LOG;
  logging::InitLogging(settings);

  Report r(step);
  TestSystem(r);
  TestTime(r);
  TestPaths(r);
  TestFiles(r);
  TestJson(r);
  TestRegistry(r);
  TestProcess(r);
  TestRunLoop(r);
  TestThread(r);
  TestThreadPool(r);

  if (out && out_size > 0) {
    const size_t n = std::min(r.text().size(), static_cast<size_t>(out_size - 1));
    memcpy(out, r.text().data(), n);
    out[n] = '\0';
  }
  return r.failures();
}
