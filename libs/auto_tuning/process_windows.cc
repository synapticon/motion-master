// clang-format off
#include <windows.h>
// clang-format on

#include <chrono>
#include <string>
#include <vector>

#include "auto_tuning/process_platform.h"

namespace mm::auto_tuning::detail {

namespace {

/// @brief Describes a Windows error code the way errnoMessage describes an errno value.
std::string lastErrorMessage(DWORD error) {
  char* buffer = nullptr;
  const DWORD length = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
  std::string message = length != 0 && buffer != nullptr ? std::string(buffer, length)
                                                         : "error " + std::to_string(error);
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  // FormatMessage ends its text with CRLF, which reads as a broken log line once it is embedded in
  // a longer message.
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();
  }
  return message;
}

/// @brief Quotes one command-line argument the way the C runtime parses it back.
///
/// Windows passes a command line as one string, and every program splits it itself. Nothing here
/// contains a space or a quote today — the arguments are flags, an address and a port — but the
/// executable's path can, so the rule is applied to all of them rather than to the one that needs
/// it. The rule is the one the Microsoft C runtime documents: a backslash run is doubled only when
/// a quote follows it.
std::string quoteArgument(const std::string& argument) {
  std::string quoted = "\"";
  std::size_t backslashes = 0;
  for (const char c : argument) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      quoted.append(backslashes * 2 + 1, '\\');
      backslashes = 0;
    } else {
      quoted.append(backslashes, '\\');
      backslashes = 0;
    }
    quoted.push_back(c);
  }
  quoted.append(backslashes * 2, '\\');
  quoted.push_back('"');
  return quoted;
}

}  // namespace

std::expected<Child, std::string> spawnChild(const std::filesystem::path& binary,
                                             const std::vector<std::string>& args,
                                             const std::filesystem::path& logFile) {
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);

  // The log handle must be inheritable, and CreateProcess must be told to inherit handles at all,
  // for the child's two streams to reach the file.
  HANDLE log = INVALID_HANDLE_VALUE;
  if (!logFile.empty()) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    // FILE_APPEND_DATA rather than GENERIC_WRITE: two writers (a previous run's file, a new one)
    // then append instead of overwriting from offset zero.
    log =
        CreateFileA(logFile.string().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
      return std::unexpected("cannot open " + logFile.string() + ": " +
                             lastErrorMessage(GetLastError()));
    }
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    // stdin is left at NULL rather than inherited: the auto-tuning program reads none, and a child
    // that shares a console's input can consume keystrokes meant for Motion Master.
    startup.hStdInput = nullptr;
  }

  std::string commandLine = quoteArgument(binary.string());
  for (const std::string& arg : args) {
    commandLine += " " + quoteArgument(arg);
  }

  // A job object, so that terminating the child also terminates the program it unpacks and runs.
  // TerminateProcess reaches one process, and the unpacked program is the one holding the port.
  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE also covers a Motion Master that dies without stopping the
  // child: closing the last handle to the job kills everything in it.
  HANDLE job = CreateJobObjectA(nullptr, nullptr);
  if (job == nullptr) {
    if (log != INVALID_HANDLE_VALUE) {
      CloseHandle(log);
    }
    return std::unexpected("cannot create a job object: " + lastErrorMessage(GetLastError()));
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

  PROCESS_INFORMATION info{};
  // CreateProcessA writes into its command-line argument, so it gets a buffer it may modify.
  std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
  mutableCommandLine.push_back('\0');
  // CREATE_NO_WINDOW keeps the child from opening a console window of its own, which it would do
  // when Motion Master itself runs without a console. CREATE_SUSPENDED holds it before its first
  // instruction, so it joins the job before it can start a process of its own outside it.
  const BOOL created =
      CreateProcessA(binary.string().c_str(), mutableCommandLine.data(), nullptr, nullptr,
                     log != INVALID_HANDLE_VALUE ? TRUE : FALSE,
                     CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info);
  const DWORD error = GetLastError();
  if (log != INVALID_HANDLE_VALUE) {
    // The child holds its own copy of the handle, so this one has done its job either way.
    CloseHandle(log);
  }
  if (created == FALSE) {
    CloseHandle(job);
    return std::unexpected(lastErrorMessage(error));
  }
  if (AssignProcessToJobObject(job, info.hProcess) == FALSE) {
    const DWORD assignError = GetLastError();
    TerminateProcess(info.hProcess, 0);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    CloseHandle(job);
    return std::unexpected("cannot put the child in a job object: " +
                           lastErrorMessage(assignError));
  }
  ResumeThread(info.hThread);
  // The thread handle is of no use here, and an open handle keeps a kernel object alive.
  CloseHandle(info.hThread);
  return Child{static_cast<std::int64_t>(info.dwProcessId),
               reinterpret_cast<std::intptr_t>(info.hProcess),
               reinterpret_cast<std::intptr_t>(job)};
}

bool childAlive(const Child& child) {
  if (child.handle == 0) {
    return false;
  }
  auto handle = reinterpret_cast<HANDLE>(child.handle);
  // A zero wait is a test, not a wait. WAIT_TIMEOUT means the process is still running; anything
  // else means it has exited, and the handle is what keeps its exit status readable, so there is no
  // process id to reissue while we hold it.
  return WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
}

void terminateChild(const Child& child, std::chrono::milliseconds grace) {
  if (child.handle == 0) {
    return;
  }
  auto handle = reinterpret_cast<HANDLE>(child.handle);
  auto job = reinterpret_cast<HANDLE>(child.group);
  // Windows has no equivalent of the signal the auto-tuning program handles: a console control
  // event only reaches a child that shares a console, and CREATE_NO_WINDOW means this one does not.
  // So the job is terminated outright, which takes the launcher and the program it unpacked
  // together. Neither writes a file of its own, and the HTTP server holds nothing that a clean exit
  // would flush, so there is nothing lost by skipping the polite step. `grace` is therefore only
  // how long to wait for the kernel to finish.
  if (job != nullptr) {
    TerminateJobObject(job, 0);
  } else {
    TerminateProcess(handle, 0);
  }
  WaitForSingleObject(handle, static_cast<DWORD>(grace.count()));
  CloseHandle(handle);
  if (job != nullptr) {
    CloseHandle(job);
  }
}

}  // namespace mm::auto_tuning::detail
