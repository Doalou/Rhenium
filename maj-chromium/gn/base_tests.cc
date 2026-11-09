// Exécuteur d'un sous-ensemble de base_unittests pour la sonde Rhénium
// (..\sonde\build.ps1 -BaseTests). Sans processus enfants, un test qui plante
// tue l'app : chaque test est noté dans LocalState\base-tests.txt avant de
// démarrer, et au lancement suivant un test resté « en cours » est compté
// comme planté et sauté. Les lancements successifs reprennent donc là où le
// précédent s'est arrêté, jusqu'à ce qu'il ne reste plus rien à exécuter.

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "base/at_exit.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/logging/logging_settings.h"
#include "base/path_service.h"
#include "base/strings/pattern.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/test/test_timeouts.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

// Tests exclus d'office :
// - sans processus enfants : multiprocessus (les tests de mort sont désactivés
//   dans gtest sous UWP) ;
// - sans objet sur le téléphone : dbghelp n'est pas dans System32, pas de
//   bureau ni de Program Files pour une app UWP, pas de données de test
//   (l'arbre source n'est pas sur le téléphone, et le dossier parent de
//   l'exécutable, WindowsApps, n'est pas lisible).
constexpr char kAlwaysExcluded[] =
    "*Death*:*MultiProcess*:*ChildProcess*:DISABLED_*:*.DISABLED_*:"
    "WindowsVersion.CheckDbgHelpVersion:PathServiceTest.Get:"
    "PathServiceTest.GetProgramFiles:JSONReaderTest.ReadFromFile:"
    "PathServiceTest.DIR_GEN_TEST_DATA_ROOT:"
    "PathServiceTest.SetTestDataRootAsRelativePath";

base::FilePath StatePath() {
  base::FilePath dir;
  base::PathService::Get(base::DIR_LOCAL_APP_DATA, &dir);
  return dir.AppendASCII("base-tests.txt");
}

// base::AppendToFile n'écrit que dans un fichier existant.
void AppendState(const std::string& line) {
  const base::FilePath path = StatePath();
  if (!base::PathExists(path))
    base::WriteFile(path, "");
  base::AppendToFile(path, line + "\n");
}

class Listener : public testing::EmptyTestEventListener {
 public:
  explicit Listener(void (*step)(const char*)) : step_(step) {}

  void OnTestStart(const testing::TestInfo& info) override {
    current_ = std::string(info.test_suite_name()) + "." + info.name();
    first_failure_.clear();
    AppendState("en cours " + current_);
    if (step_)
      step_(("base_unittests : " + current_).c_str());
    start_ = base::TimeTicks::Now();
  }

  void OnTestPartResult(const testing::TestPartResult& result) override {
    if (!result.failed() || !first_failure_.empty())
      return;
    std::string message = result.message() ? result.message() : "";
    base::ReplaceChars(message, "\r\n", " ", &message);
    first_failure_ = base::StringPrintf(
        "%s:%d %s",
        result.file_name() ? base::FilePath::FromASCII(result.file_name())
                                 .BaseName()
                                 .MaybeAsASCII()
                                 .c_str()
                           : "?",
        result.line_number(), message.substr(0, 240).c_str());
  }

  void OnTestEnd(const testing::TestInfo& info) override {
    const int ms = static_cast<int>(
        (base::TimeTicks::Now() - start_).InMilliseconds());
    if (info.result()->Skipped()) {
      AppendState("saute " + current_);
    } else if (info.result()->Passed()) {
      AppendState(base::StringPrintf("ok %s %d", current_.c_str(), ms));
    } else {
      AppendState("echec " + current_ + " " + first_failure_);
    }
  }

 private:
  void (*step_)(const char*);
  std::string current_;
  std::string first_failure_;
  base::TimeTicks start_;
};

struct State {
  std::map<std::string, std::string> status;  // nom -> ok, echec, plantage...
  std::map<std::string, std::string> detail;  // nom -> premier échec
  std::vector<std::string> order;
};

State ReadState() {
  State state;
  std::string text;
  base::ReadFileToString(StatePath(), &text);
  std::string running;
  for (const std::string& line : base::SplitString(
           text, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    std::string kind;
    std::string rest;
    // Messages d'erreur et exceptions : rattachés au test en cours.
    if (base::StartsWith(line, "log ") || base::StartsWith(line, "exception ")) {
      if (!running.empty() && state.detail[running].size() < 600)
        state.detail[running] += (state.detail[running].empty() ? "" : " | ") + line;
      continue;
    }
    if (base::StartsWith(line, "en cours ")) {
      kind = "en cours";
      rest = line.substr(9);
    } else {
      const size_t space = line.find(' ');
      if (space == std::string::npos)
        continue;
      kind = line.substr(0, space);
      rest = line.substr(space + 1);
    }
    const size_t space = rest.find(' ');
    const std::string name = rest.substr(0, space);
    if (!state.status.count(name))
      state.order.push_back(name);
    state.status[name] = kind;
    running = kind == "en cours" ? name : std::string();
    if (space != std::string::npos)
      state.detail[name] = rest.substr(space + 1);
  }
  // Un test resté « en cours » a tué l'app.
  for (auto& [name, kind] : state.status) {
    if (kind == "en cours") {
      kind = "plantage";
      AppendState("plantage " + name);
    }
  }
  return state;
}

std::string Summary(const State& state, int total_in_filter) {
  int ok = 0, failed = 0, crashed = 0, skipped = 0;
  std::string details;
  for (const std::string& name : state.order) {
    const std::string& kind = state.status.at(name);
    if (kind == "ok") {
      ++ok;
    } else if (kind == "saute") {
      ++skipped;
    } else if (kind == "echec") {
      ++failed;
      if (details.size() < 12000) {
        auto it = state.detail.find(name);
        details += "ÉCHEC " + name + " : " +
                   (it != state.detail.end() ? it->second : "") + "\n";
      }
    } else if (kind == "plantage") {
      ++crashed;
      if (details.size() < 12000) {
        auto it = state.detail.find(name);
        details += "PLANTAGE " + name +
                   (it != state.detail.end() ? " : " + it->second : "") + "\n";
      }
    }
  }
  const int done = ok + failed + crashed + skipped;
  return base::StringPrintf(
             "Tests exécutés : %d sur %d (réussis %d, échecs %d, plantages "
             "%d, sautés %d)\n",
             done, total_in_filter, ok, failed, crashed, skipped) +
         (done < total_in_filter ? "Reste à exécuter : relancer la sonde\n"
                                 : "Tous les tests sont passés\n") +
         details;
}

// Messages d'erreur de //base (CHECK compris), notés avant que l'app ne meure.
bool OnLogMessage(int severity,
                  const char* file,
                  int line,
                  size_t message_start,
                  const std::string& message) {
  if (severity >= logging::LOGGING_ERROR) {
    std::string text = message.substr(0, 300);
    base::ReplaceChars(text, "\r\n", " ", &text);
    AppendState("log " + text);
  }
  return false;
}

// Exceptions matérielles (dont __debugbreak et udf des CHECK), avec l'adresse
// relative à l'exe pour la retrouver dans RheniumSonde.map.
LONG WINAPI OnException(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
      code == EXCEPTION_ACCESS_VIOLATION || code == STATUS_STACK_BUFFER_OVERRUN ||
      code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
    const uintptr_t pc =
        reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    const uintptr_t exe = reinterpret_cast<uintptr_t>(::GetModuleHandle(nullptr));
    const uintptr_t lr = info->ContextRecord->Lr;
    AppendState(base::StringPrintf(
        "exception %08lx pc=%08x (exe+%x) lr=exe+%x", code,
        static_cast<unsigned>(pc), static_cast<unsigned>(pc - exe),
        static_cast<unsigned>(lr - exe)));
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void InitOnce() {
  static bool initialized = false;
  if (initialized)
    return;
  initialized = true;
  ::AddVectoredExceptionHandler(1, &OnException);
  static base::AtExitManager* at_exit = new base::AtExitManager;
  (void)at_exit;
  base::CommandLine::Init(0, nullptr);
  logging::LoggingSettings settings;
  settings.logging_dest = logging::LOG_TO_SYSTEM_DEBUG_LOG;
  logging::InitLogging(settings);
  logging::SetLogMessageHandler(&OnLogMessage);
  TestTimeouts::Initialize();
  int argc = 1;
  static char arg0[] = "rhenium";
  char* argv[] = {arg0, nullptr};
  testing::InitGoogleTest(&argc, argv);
}

// Nombre de tests du sous-ensemble, hors exclusions permanentes.
int CountTotal() {
  const std::vector<std::string> patterns =
      base::SplitString(kAlwaysExcluded, ":", base::TRIM_WHITESPACE,
                        base::SPLIT_WANT_NONEMPTY);
  int total = 0;
  testing::UnitTest& unit = *testing::UnitTest::GetInstance();
  for (int i = 0; i < unit.total_test_suite_count(); ++i) {
    const testing::TestSuite* suite = unit.GetTestSuite(i);
    for (int j = 0; j < suite->total_test_count(); ++j) {
      const testing::TestInfo* info = suite->GetTestInfo(j);
      const std::string name =
          std::string(info->test_suite_name()) + "." + info->name();
      bool excluded = false;
      for (const std::string& pattern : patterns)
        excluded |= base::MatchPattern(name, pattern);
      total += !excluded;
    }
  }
  return total;
}

int CopyOut(const State& state, int total, char* out, int out_size) {
  const std::string text = Summary(state, total);
  if (out && out_size > 0) {
    const size_t n = std::min(text.size(), static_cast<size_t>(out_size - 1));
    memcpy(out, text.data(), n);
    out[n] = '\0';
  }
  int failures = 0;
  for (const auto& [name, kind] : state.status)
    failures += kind == "echec" || kind == "plantage";
  return failures;
}

}  // namespace

// Bilan des lancements précédents, sans rien exécuter : la sonde l'exporte
// avant de lancer les tests, pour qu'il reste lisible si l'app est tuée.
extern "C" int rhenium_base_tests_state(char* out, int out_size) {
  InitOnce();
  return CopyOut(ReadState(), CountTotal(), out, out_size);
}

extern "C" int rhenium_base_tests(void (*step)(const char*),
                                  char* out,
                                  int out_size) {
  InitOnce();
  State state = ReadState();
  const int total = CountTotal();

  // Tout ce qui a déjà un résultat est exclu de ce lancement.
  std::string negative = kAlwaysExcluded;
  for (const std::string& name : state.order)
    negative += ":" + name;
  GTEST_FLAG_SET(filter, "*-" + negative);

  testing::TestEventListeners& listeners =
      testing::UnitTest::GetInstance()->listeners();
  delete listeners.Release(listeners.default_result_printer());
  listeners.Append(new Listener(step));

  if (static_cast<int>(state.order.size()) < total) {
    (void)RUN_ALL_TESTS();
    state = ReadState();
  }
  return CopyOut(state, total, out, out_size);
}