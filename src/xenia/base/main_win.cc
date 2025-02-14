/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <fcntl.h>
#include <io.h>

#include <cstdlib>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/main.h"
#include "xenia/base/platform_win.h"
#include "xenia/base/string.h"

// Autogenerated by `xb premake`.
#include "build/version.h"

// For RequestHighPerformance.
#include <winternl.h>

// Includes Windows headers, so it goes here.
#include "third_party/xbyak/xbyak/xbyak_util.h"

DEFINE_bool(win32_high_freq, true,
            "Requests high performance from the NT kernel", "Kernel");
DEFINE_bool(enable_console, false, "Open a console window with the main window",
            "General");

namespace xe {

bool has_console_attached_ = true;

bool has_console_attached() { return has_console_attached_; }

bool has_shell_environment_variable() {
  size_t size = 0;
  // Check if SHELL exists
  // If it doesn't, then we are in a Windows Terminal
  auto error = getenv_s(&size, nullptr, 0, "SHELL");
  if (error) {
    return false;
  }
  return !!size;
}

void AttachConsole() {
  bool has_console = ::AttachConsole(ATTACH_PARENT_PROCESS) == TRUE;
  if (!has_console || !has_shell_environment_variable()) {
    // We weren't launched from a console, so just return.
    has_console_attached_ = false;
    return;
  }

  AllocConsole();

  has_console_attached_ = true;

  auto std_handle = (intptr_t)GetStdHandle(STD_OUTPUT_HANDLE);
  auto con_handle = _open_osfhandle(std_handle, _O_TEXT);
  auto fp = _fdopen(con_handle, "w");
  freopen_s(&fp, "CONOUT$", "w", stdout);

  std_handle = (intptr_t)GetStdHandle(STD_ERROR_HANDLE);
  con_handle = _open_osfhandle(std_handle, _O_TEXT);
  fp = _fdopen(con_handle, "w");
  freopen_s(&fp, "CONOUT$", "w", stderr);
}

static void RequestHighPerformance() {
#if XE_PLATFORM_WIN32
  NTSTATUS(*NtQueryTimerResolution)
  (OUT PULONG MinimumResolution, OUT PULONG MaximumResolution,
   OUT PULONG CurrentResolution);

  NTSTATUS(*NtSetTimerResolution)
  (IN ULONG DesiredResolution, IN BOOLEAN SetResolution,
   OUT PULONG CurrentResolution);

  NtQueryTimerResolution = (decltype(NtQueryTimerResolution))GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution");
  NtSetTimerResolution = (decltype(NtSetTimerResolution))GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution");
  if (!NtQueryTimerResolution || !NtSetTimerResolution) {
    return;
  }

  ULONG minimum_resolution, maximum_resolution, current_resolution;
  NtQueryTimerResolution(&minimum_resolution, &maximum_resolution,
                         &current_resolution);
  NtSetTimerResolution(maximum_resolution, TRUE, &current_resolution);
#endif
}

static bool parse_launch_arguments(const xe::EntryInfo& entry_info,
                                   std::vector<std::string>& args) {
  auto command_line = GetCommandLineW();

  int wargc;
  wchar_t** wargv = CommandLineToArgvW(command_line, &wargc);
  if (!wargv) {
    return false;
  }

  // Convert all args to narrow, as cxxopts doesn't support wchar.
  int argc = wargc;
  char** argv = reinterpret_cast<char**>(alloca(sizeof(char*) * argc));
  for (int n = 0; n < argc; n++) {
    size_t len = std::wcstombs(nullptr, wargv[n], 0);
    argv[n] = reinterpret_cast<char*>(alloca(sizeof(char) * (len + 1)));
    std::wcstombs(argv[n], wargv[n], len + 1);
  }

  LocalFree(wargv);

  if (!entry_info.transparent_options) {
    cvar::ParseLaunchArguments(argc, argv, entry_info.positional_usage.value(),
                               entry_info.positional_options.value());
  }

  args.clear();
  for (int n = 0; n < argc; n++) {
    args.push_back(std::string(argv[n]));
  }

  return true;
}

int Main() {
  auto entry_info = xe::GetEntryInfo();

  std::vector<std::string> args;
  if (!parse_launch_arguments(entry_info, args)) {
    return 1;
  }

  // Attach a console so we can write output to stdout. If the user hasn't
  // redirected output themselves it'll pop up a window.
  if (cvars::enable_console) {
    xe::AttachConsole();
  }

  // Setup COM on the main thread.
  // NOTE: this may fail if COM has already been initialized - that's OK.
#pragma warning(suppress : 6031)
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  // Initialize logging. Needs parsed FLAGS.
  xe::InitializeLogging(entry_info.name);

  Xbyak::util::Cpu cpu;
  if (!cpu.has(Xbyak::util::Cpu::tAVX)) {
    xe::FatalError(
        "Your CPU does not support AVX, which is required by Xenia. See the "
        "FAQ for system requirements at https://xenia.jp");
    return -1;
  }

  // Print version info.
  XELOGI(
      "Build: "
#ifdef XE_BUILD_IS_PR
      "PR#" XE_BUILD_PR_NUMBER " " XE_BUILD_PR_REPO " " XE_BUILD_PR_BRANCH
      "@" XE_BUILD_PR_COMMIT_SHORT " against "
#endif
      XE_BUILD_BRANCH "@" XE_BUILD_COMMIT_SHORT " on " XE_BUILD_DATE);

  // Request high performance timing.
  if (cvars::win32_high_freq) {
    RequestHighPerformance();
  }

  // Call app-provided entry point.
  int result = entry_info.entry_point(args);

  xe::ShutdownLogging();
  return result;
}

}  // namespace xe

// Used in console mode apps; automatically picked based on subsystem.
int main(int argc_ignored, char** argv_ignored) { return xe::Main(); }

// Used in windowed apps; automatically picked based on subsystem.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR command_line, int) {
  // Run normal entry point.
  return xe::Main();
}

#if defined _M_IX86
#pragma comment( \
    linker,      \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")  // NOLINT(whitespace/line_length)
#elif defined _M_IA64
#pragma comment( \
    linker,      \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='ia64' publicKeyToken='6595b64144ccf1df' language='*'\"")  // NOLINT(whitespace/line_length)
#elif defined _M_X64
#pragma comment( \
    linker,      \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")  // NOLINT(whitespace/line_length)
#else
#pragma comment( \
    linker,      \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")  // NOLINT(whitespace/line_length)
#endif
