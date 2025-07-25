//===-- lldb-dap.cpp ------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DAP.h"
#include "DAPLog.h"
#include "EventHelper.h"
#include "Handler/RequestHandler.h"
#include "RunInTerminal.h"
#include "Transport.h"
#include "lldb/API/SBDebugger.h"
#include "lldb/API/SBStream.h"
#include "lldb/Host/Config.h"
#include "lldb/Host/File.h"
#include "lldb/Host/MainLoop.h"
#include "lldb/Host/MainLoopBase.h"
#include "lldb/Host/MemoryMonitor.h"
#include "lldb/Host/Socket.h"
#include "lldb/Utility/Status.h"
#include "lldb/Utility/UriParser.h"
#include "lldb/lldb-forward.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/raw_ostream.h"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
// We need to #define NOMINMAX in order to skip `min()` and `max()` macro
// definitions that conflict with other system headers.
// We also need to #undef GetObject (which is defined to GetObjectW) because
// the JSON code we use also has methods named `GetObject()` and we conflict
// against these.
#define NOMINMAX
#include <windows.h>
#undef GetObject
#include <io.h>
typedef int socklen_t;
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/prctl.h>
#endif

using namespace lldb_dap;
using lldb_private::File;
using lldb_private::IOObject;
using lldb_private::MainLoop;
using lldb_private::MainLoopBase;
using lldb_private::NativeFile;
using lldb_private::Socket;
using lldb_private::Status;

namespace {
using namespace llvm::opt;

enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

#define OPTTABLE_STR_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

static constexpr llvm::opt::OptTable::Info InfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};
class LLDBDAPOptTable : public llvm::opt::GenericOptTable {
public:
  LLDBDAPOptTable()
      : llvm::opt::GenericOptTable(OptionStrTable, OptionPrefixesTable,
                                   InfoTable, true) {}
};
} // anonymous namespace

static void PrintHelp(LLDBDAPOptTable &table, llvm::StringRef tool_name) {
  std::string usage_str = tool_name.str() + " options";
  table.printHelp(llvm::outs(), usage_str.c_str(), "LLDB DAP", false);

  llvm::outs() << R"___(
EXAMPLES:
  The debug adapter can be started in two modes.

  Running lldb-dap without any arguments will start communicating with the
  parent over stdio. Passing a --connection URI will cause lldb-dap to listen
  for a connection in the specified mode.

    lldb-dap --connection listen://localhost:<port>

  Passing --wait-for-debugger will pause the process at startup and wait for a
  debugger to attach to the process.

    lldb-dap -g
)___";
}

static void PrintVersion() {
  llvm::outs() << "lldb-dap: ";
  llvm::cl::PrintVersionMessage();
  llvm::outs() << "liblldb: " << lldb::SBDebugger::GetVersionString() << '\n';
}

// If --launch-target is provided, this instance of lldb-dap becomes a
// runInTerminal launcher. It will ultimately launch the program specified in
// the --launch-target argument, which is the original program the user wanted
// to debug. This is done in such a way that the actual debug adapter can
// place breakpoints at the beginning of the program.
//
// The launcher will communicate with the debug adapter using a fifo file in the
// directory specified in the --comm-file argument.
//
// Regarding the actual flow, this launcher will first notify the debug adapter
// of its pid. Then, the launcher will be in a pending state waiting to be
// attached by the adapter.
//
// Once attached and resumed, the launcher will exec and become the program
// specified by --launch-target, which is the original target the
// user wanted to run.
//
// In case of errors launching the target, a suitable error message will be
// emitted to the debug adapter.
static llvm::Error LaunchRunInTerminalTarget(llvm::opt::Arg &target_arg,
                                             llvm::StringRef comm_file,
                                             lldb::pid_t debugger_pid,
                                             char *argv[]) {
#if defined(_WIN32)
  return llvm::createStringError(
      "runInTerminal is only supported on POSIX systems");
#else

  // On Linux with the Yama security module enabled, a process can only attach
  // to its descendants by default. In the runInTerminal case the target
  // process is launched by the client so we need to allow tracing explicitly.
#if defined(__linux__)
  if (debugger_pid != LLDB_INVALID_PROCESS_ID)
    (void)prctl(PR_SET_PTRACER, debugger_pid, 0, 0, 0);
#endif

  RunInTerminalLauncherCommChannel comm_channel(comm_file);
  if (llvm::Error err = comm_channel.NotifyPid())
    return err;

  // We will wait to be attached with a timeout. We don't wait indefinitely
  // using a signal to prevent being paused forever.

  // This env var should be used only for tests.
  const char *timeout_env_var = getenv("LLDB_DAP_RIT_TIMEOUT_IN_MS");
  int timeout_in_ms =
      timeout_env_var != nullptr ? atoi(timeout_env_var) : 20000;
  if (llvm::Error err = comm_channel.WaitUntilDebugAdapterAttaches(
          std::chrono::milliseconds(timeout_in_ms))) {
    return err;
  }

  const char *target = target_arg.getValue();
  execvp(target, argv);

  std::string error = std::strerror(errno);
  comm_channel.NotifyError(error);
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 std::move(error));
#endif
}

/// used only by TestVSCode_redirection_to_console.py
static void redirection_test() {
  printf("stdout message\n");
  fprintf(stderr, "stderr message\n");
  fflush(stdout);
  fflush(stderr);
}

/// Duplicates a file descriptor, setting FD_CLOEXEC if applicable.
static int DuplicateFileDescriptor(int fd) {
#if defined(F_DUPFD_CLOEXEC)
  // Ensure FD_CLOEXEC is set.
  return ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
#else
  return ::dup(fd);
#endif
}

static llvm::Expected<std::pair<Socket::SocketProtocol, std::string>>
validateConnection(llvm::StringRef conn) {
  auto uri = lldb_private::URI::Parse(conn);

  auto make_error = [conn]() -> llvm::Error {
    return llvm::createStringError(
        "Unsupported connection specifier, expected 'accept:///path' or "
        "'listen://[host]:port', got '%s'.",
        conn.str().c_str());
  };

  if (!uri)
    return make_error();

  std::optional<Socket::ProtocolModePair> protocol_and_mode =
      Socket::GetProtocolAndMode(uri->scheme);
  if (!protocol_and_mode || protocol_and_mode->second != Socket::ModeAccept)
    return make_error();

  if (protocol_and_mode->first == Socket::ProtocolTcp) {
    return std::make_pair(
        Socket::ProtocolTcp,
        formatv("[{0}]:{1}", uri->hostname.empty() ? "0.0.0.0" : uri->hostname,
                uri->port.value_or(0)));
  }

  if (protocol_and_mode->first == Socket::ProtocolUnixDomain)
    return std::make_pair(Socket::ProtocolUnixDomain, uri->path.str());

  return make_error();
}

static llvm::Error
serveConnection(const Socket::SocketProtocol &protocol, const std::string &name,
                Log *log, const ReplMode default_repl_mode,
                const std::vector<std::string> &pre_init_commands) {
  Status status;
  static std::unique_ptr<Socket> listener = Socket::Create(protocol, status);
  if (status.Fail()) {
    return status.takeError();
  }

  status = listener->Listen(name, /*backlog=*/5);
  if (status.Fail()) {
    return status.takeError();
  }

  std::string address = llvm::join(listener->GetListeningConnectionURI(), ", ");
  DAP_LOG(log, "started with connection listeners {0}", address);

  llvm::outs() << "Listening for: " << address << "\n";
  // Ensure listening address are flushed for calles to retrieve the resolve
  // address.
  llvm::outs().flush();

  static MainLoop g_loop;
  llvm::sys::SetInterruptFunction([]() {
    g_loop.AddPendingCallback(
        [](MainLoopBase &loop) { loop.RequestTermination(); });
  });
  std::condition_variable dap_sessions_condition;
  std::mutex dap_sessions_mutex;
  std::map<IOObject *, DAP *> dap_sessions;
  unsigned int clientCount = 0;
  auto handle = listener->Accept(g_loop, [=, &dap_sessions_condition,
                                          &dap_sessions_mutex, &dap_sessions,
                                          &clientCount](
                                             std::unique_ptr<Socket> sock) {
    std::string client_name = llvm::formatv("client_{0}", clientCount++).str();
    DAP_LOG(log, "({0}) client connected", client_name);

    lldb::IOObjectSP io(std::move(sock));

    // Move the client into a background thread to unblock accepting the next
    // client.
    std::thread client([=, &dap_sessions_condition, &dap_sessions_mutex,
                        &dap_sessions]() {
      llvm::set_thread_name(client_name + ".runloop");
      Transport transport(client_name, log, io, io);
      DAP dap(log, default_repl_mode, pre_init_commands, transport);

      if (auto Err = dap.ConfigureIO()) {
        llvm::logAllUnhandledErrors(std::move(Err), llvm::errs(),
                                    "Failed to configure stdout redirect: ");
        return;
      }

      {
        std::scoped_lock<std::mutex> lock(dap_sessions_mutex);
        dap_sessions[io.get()] = &dap;
      }

      if (auto Err = dap.Loop()) {
        llvm::logAllUnhandledErrors(std::move(Err), llvm::errs(),
                                    "DAP session (" + client_name +
                                        ") error: ");
      }

      DAP_LOG(log, "({0}) client disconnected", client_name);
      std::unique_lock<std::mutex> lock(dap_sessions_mutex);
      dap_sessions.erase(io.get());
      std::notify_all_at_thread_exit(dap_sessions_condition, std::move(lock));
    });
    client.detach();
  });

  if (auto Err = handle.takeError()) {
    return Err;
  }

  status = g_loop.Run();
  if (status.Fail()) {
    return status.takeError();
  }

  DAP_LOG(
      log,
      "lldb-dap server shutdown requested, disconnecting remaining clients...");

  bool client_failed = false;
  {
    std::scoped_lock<std::mutex> lock(dap_sessions_mutex);
    for (auto [sock, dap] : dap_sessions) {
      if (llvm::Error error = dap->Disconnect()) {
        client_failed = true;
        llvm::errs() << "DAP client " << dap->transport.GetClientName()
                     << " disconnected failed: "
                     << llvm::toString(std::move(error)) << "\n";
      }
      // Close the socket to ensure the DAP::Loop read finishes.
      sock->Close();
    }
  }

  // Wait for all clients to finish disconnecting.
  std::unique_lock<std::mutex> lock(dap_sessions_mutex);
  dap_sessions_condition.wait(lock, [&] { return dap_sessions.empty(); });

  if (client_failed)
    return llvm::make_error<llvm::StringError>(
        "disconnecting all clients failed", llvm::inconvertibleErrorCode());

  return llvm::Error::success();
}

int main(int argc, char *argv[]) {
  llvm::InitLLVM IL(argc, argv, /*InstallPipeSignalExitHandler=*/false);
#if !defined(__APPLE__)
  llvm::setBugReportMsg("PLEASE submit a bug report to " LLDB_BUG_REPORT_URL
                        " and include the crash backtrace.\n");
#else
  llvm::setBugReportMsg("PLEASE submit a bug report to " LLDB_BUG_REPORT_URL
                        " and include the crash report from "
                        "~/Library/Logs/DiagnosticReports/.\n");
#endif

  llvm::SmallString<256> program_path(argv[0]);
  llvm::sys::fs::make_absolute(program_path);
  DAP::debug_adapter_path = program_path;

  LLDBDAPOptTable T;
  unsigned MAI, MAC;
  llvm::ArrayRef<const char *> ArgsArr = llvm::ArrayRef(argv + 1, argc);
  llvm::opt::InputArgList input_args = T.ParseArgs(ArgsArr, MAI, MAC);

  if (input_args.hasArg(OPT_help)) {
    PrintHelp(T, llvm::sys::path::filename(argv[0]));
    return EXIT_SUCCESS;
  }

  if (input_args.hasArg(OPT_version)) {
    PrintVersion();
    return EXIT_SUCCESS;
  }

  ReplMode default_repl_mode = ReplMode::Auto;
  if (input_args.hasArg(OPT_repl_mode)) {
    llvm::opt::Arg *repl_mode = input_args.getLastArg(OPT_repl_mode);
    llvm::StringRef repl_mode_value = repl_mode->getValue();
    if (repl_mode_value == "auto") {
      default_repl_mode = ReplMode::Auto;
    } else if (repl_mode_value == "variable") {
      default_repl_mode = ReplMode::Variable;
    } else if (repl_mode_value == "command") {
      default_repl_mode = ReplMode::Command;
    } else {
      llvm::errs() << "'" << repl_mode_value
                   << "' is not a valid option, use 'variable', 'command' or "
                      "'auto'.\n";
      return EXIT_FAILURE;
    }
  }

  if (llvm::opt::Arg *target_arg = input_args.getLastArg(OPT_launch_target)) {
    if (llvm::opt::Arg *comm_file = input_args.getLastArg(OPT_comm_file)) {
      lldb::pid_t pid = LLDB_INVALID_PROCESS_ID;
      llvm::opt::Arg *debugger_pid = input_args.getLastArg(OPT_debugger_pid);
      if (debugger_pid) {
        llvm::StringRef debugger_pid_value = debugger_pid->getValue();
        if (debugger_pid_value.getAsInteger(10, pid)) {
          llvm::errs() << "'" << debugger_pid_value
                       << "' is not a valid "
                          "PID\n";
          return EXIT_FAILURE;
        }
      }
      int target_args_pos = argc;
      for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--launch-target") == 0) {
          target_args_pos = i + 1;
          break;
        }
      }
      if (llvm::Error err =
              LaunchRunInTerminalTarget(*target_arg, comm_file->getValue(), pid,
                                        argv + target_args_pos)) {
        llvm::errs() << llvm::toString(std::move(err)) << '\n';
        return EXIT_FAILURE;
      }
    } else {
      llvm::errs() << "\"--launch-target\" requires \"--comm-file\" to be "
                      "specified\n";
      return EXIT_FAILURE;
    }
  }

  std::string connection;
  if (auto *arg = input_args.getLastArg(OPT_connection)) {
    const auto *path = arg->getValue();
    connection.assign(path);
  }

#if !defined(_WIN32)
  if (input_args.hasArg(OPT_wait_for_debugger)) {
    printf("Paused waiting for debugger to attach (pid = %i)...\n", getpid());
    pause();
  }
#endif

  std::unique_ptr<Log> log = nullptr;
  const char *log_file_path = getenv("LLDBDAP_LOG");
  if (log_file_path) {
    std::error_code EC;
    log = std::make_unique<Log>(log_file_path, EC);
    if (EC) {
      llvm::logAllUnhandledErrors(llvm::errorCodeToError(EC), llvm::errs(),
                                  "Failed to create log file: ");
      return EXIT_FAILURE;
    }
  }

  // Initialize LLDB first before we do anything.
  lldb::SBError error = lldb::SBDebugger::InitializeWithErrorHandling();
  if (error.Fail()) {
    lldb::SBStream os;
    error.GetDescription(os);
    llvm::errs() << "lldb initialize failed: " << os.GetData() << "\n";
    return EXIT_FAILURE;
  }

  // Create a memory monitor. This can return nullptr if the host platform is
  // not supported.
  std::unique_ptr<lldb_private::MemoryMonitor> memory_monitor =
      lldb_private::MemoryMonitor::Create([log = log.get()]() {
        DAP_LOG(log, "memory pressure detected");
        lldb::SBDebugger::MemoryPressureDetected();
      });

  if (memory_monitor)
    memory_monitor->Start();

  // Terminate the debugger before the C++ destructor chain kicks in.
  auto terminate_debugger = llvm::make_scope_exit([&] {
    if (memory_monitor)
      memory_monitor->Stop();
    lldb::SBDebugger::Terminate();
  });

  std::vector<std::string> pre_init_commands;
  for (const std::string &arg :
       input_args.getAllArgValues(OPT_pre_init_command)) {
    pre_init_commands.push_back(arg);
  }

  if (!connection.empty()) {
    auto maybeProtoclAndName = validateConnection(connection);
    if (auto Err = maybeProtoclAndName.takeError()) {
      llvm::logAllUnhandledErrors(std::move(Err), llvm::errs(),
                                  "Invalid connection: ");
      return EXIT_FAILURE;
    }

    Socket::SocketProtocol protocol;
    std::string name;
    std::tie(protocol, name) = *maybeProtoclAndName;
    if (auto Err = serveConnection(protocol, name, log.get(), default_repl_mode,
                                   pre_init_commands)) {
      llvm::logAllUnhandledErrors(std::move(Err), llvm::errs(),
                                  "Connection failed: ");
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  }

#if defined(_WIN32)
  // Windows opens stdout and stdin in text mode which converts \n to 13,10
  // while the value is just 10 on Darwin/Linux. Setting the file mode to
  // binary fixes this.
  int result = _setmode(fileno(stdout), _O_BINARY);
  assert(result);
  result = _setmode(fileno(stdin), _O_BINARY);
  UNUSED_IF_ASSERT_DISABLED(result);
  assert(result);
#endif

  int stdout_fd = DuplicateFileDescriptor(fileno(stdout));
  if (stdout_fd == -1) {
    llvm::logAllUnhandledErrors(
        llvm::errorCodeToError(llvm::errnoAsErrorCode()), llvm::errs(),
        "Failed to configure stdout redirect: ");
    return EXIT_FAILURE;
  }

  lldb::IOObjectSP input = std::make_shared<NativeFile>(
      fileno(stdin), File::eOpenOptionReadOnly, NativeFile::Unowned);
  lldb::IOObjectSP output = std::make_shared<NativeFile>(
      stdout_fd, File::eOpenOptionWriteOnly, NativeFile::Unowned);

  constexpr llvm::StringLiteral client_name = "stdio";
  Transport transport(client_name, log.get(), input, output);
  DAP dap(log.get(), default_repl_mode, pre_init_commands, transport);

  // stdout/stderr redirection to the IDE's console
  if (auto Err = dap.ConfigureIO(stdout, stderr)) {
    llvm::logAllUnhandledErrors(std::move(Err), llvm::errs(),
                                "Failed to configure stdout redirect: ");
    return EXIT_FAILURE;
  }

  // used only by TestVSCode_redirection_to_console.py
  if (getenv("LLDB_DAP_TEST_STDOUT_STDERR_REDIRECTION") != nullptr)
    redirection_test();

  if (auto Err = dap.Loop()) {
    DAP_LOG(log.get(), "({0}) DAP session error: {1}", client_name,
            llvm::toStringWithoutConsuming(Err));
    llvm::logAllUnhandledErrors(std::move(Err), llvm::errs(),
                                "DAP session error: ");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
