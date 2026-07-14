#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr int runner_error_status = 125;
constexpr int timeout_status = 124;
constexpr int rss_limit_status = 123;
constexpr auto sample_period = std::chrono::milliseconds{10};
constexpr auto terminate_grace = std::chrono::milliseconds{250};

std::string monitored_proc_root = "/proc";
bool report_monitor_errors = true;
bool monitor_error_observed = false;
std::uint64_t monitor_error_count = 0;

[[nodiscard]] bool process_is_gone_zombie_or_reused(
    pid_t pid, std::uint64_t expected_start_time_ticks) {
  // Consult the real procfs even when --proc-root injects a test failure. A
  // process can become a zombie between two monitored-proc reads; losing a
  // file in that transition is ordinary exit, not degraded observation of a
  // live process. Conversely, a live identity under a broken injected root
  // must still fail closed.
  std::ifstream stat{"/proc/" + std::to_string(pid) + "/stat"};
  std::string line;
  if (stat && std::getline(stat, line)) {
    auto close = line.rfind(") ");
    if (close != std::string::npos && close + 2 < line.size()) {
      std::string_view rest{line};
      rest.remove_prefix(close + 2);
      std::string_view token;
      char state = '\0';
      std::uint64_t start_time_ticks = 0;
      for (int field = 3; field <= 22; ++field) {
        while (!rest.empty() && rest.front() == ' ') {
          rest.remove_prefix(1);
        }
        if (rest.empty()) {
          return false;
        }
        auto end = rest.find(' ');
        token = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{}
                                             : rest.substr(end + 1);
        if (field == 3 && token.size() == 1) {
          state = token.front();
        } else if (field == 22) {
          auto parsed = std::from_chars(
              token.data(), token.data() + token.size(), start_time_ticks);
          if (parsed.ec != std::errc{} ||
              parsed.ptr != token.data() + token.size()) {
            return false;
          }
        }
      }
      if (expected_start_time_ticks != 0 &&
          start_time_ticks != expected_start_time_ticks) {
        return true;
      }
      return state == 'Z' || state == 'X';
    }
    return false;
  }
  if (::kill(pid, 0) == 0 || errno == EPERM) {
    return false;
  }
  return errno == ESRCH;
}

void record_monitor_error(pid_t pid,
                          std::uint64_t expected_start_time_ticks = 0) {
  if (!report_monitor_errors ||
      process_is_gone_zombie_or_reused(pid, expected_start_time_ticks)) {
    return;
  }
  monitor_error_observed = true;
  ++monitor_error_count;
}

struct options {
  std::string stdout_path;
  std::string stderr_path;
  std::string metrics_path;
  std::chrono::nanoseconds timeout{};
  bool timeout_enabled = false;
  std::uint64_t rss_limit_bytes = 0;
  bool rss_limit_enabled = false;
  std::string proc_root = "/proc";
  int command_index = -1;
};

struct proc_peaks {
  std::uint64_t peak_rss_kb = 0;
  std::uint64_t peak_swap_kb = 0;
  std::uint64_t status_samples = 0;
  std::uint64_t rss_samples = 0;
  std::uint64_t swap_samples = 0;
  std::uint64_t process_group_samples = 0;
  std::uint64_t peak_process_count = 0;
};

struct proc_sample {
  std::uint64_t rss_kb = 0;
  std::uint64_t swap_kb = 0;
  std::uint64_t process_count = 0;
};

enum class termination_cause {
  none,
  timeout,
  rss_limit,
  descendant_leak,
  monitor_error
};

struct process_identity {
  pid_t pid = 0;
  std::uint64_t start_time_ticks = 0;
  pid_t process_group = 0;
  char state = '\0';
};

struct tree_observation {
  std::vector<process_identity> identities;
  std::uint64_t live_count = 0;
  bool original_group_alive = false;
};

struct sampled_process_tree {
  proc_sample sample;
  tree_observation tree;
};

[[nodiscard]] bool process_state_is_live(char state) {
  return state != 'Z' && state != 'X';
}

enum class child_error_stage : int {
  none = 0,
  restore_sigchld = 1,
  set_process_group = 2,
  redirect_stdout = 3,
  redirect_stderr = 4,
  exec = 5,
};

struct child_error {
  child_error_stage stage = child_error_stage::none;
  int error_number = 0;
};

[[nodiscard]] char const* stage_name(child_error_stage stage) {
  switch (stage) {
    case child_error_stage::none:
      return "none";
    case child_error_stage::restore_sigchld:
      return "restore_sigchld";
    case child_error_stage::set_process_group:
      return "set_process_group";
    case child_error_stage::redirect_stdout:
      return "redirect_stdout";
    case child_error_stage::redirect_stderr:
      return "redirect_stderr";
    case child_error_stage::exec:
      return "exec";
  }
  return "unknown";
}

void usage(std::ostream& stream, char const* program) {
  stream << "usage: " << program
         << " [--timeout-seconds SECONDS] [--rss-limit-bytes BYTES]"
            " [--proc-root PATH]"
            " --stdout PATH --stderr PATH"
            " [--metrics PATH] -- COMMAND [ARG ...]\n"
            "\n"
            "Run COMMAND and emit deterministic key=value process metrics.\n"
            "SECONDS may be fractional; zero disables the timeout. BYTES is"
            " a strict positive integer; omission disables RSS enforcement."
            " Aggregate Linux descendant-tree RSS is sampled every 10 ms;"
            " enforcement is reactive and may overshoot by one interval. Child"
            " stdout and stderr are truncated before execution. Metrics go"
            " to this process's stdout unless --metrics is supplied.\n";
}

[[nodiscard]] bool parse_positive_uint64(std::string_view text,
                                         std::uint64_t& result) {
  if (text.empty()) {
    return false;
  }
  for (char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
         result != 0;
}

[[nodiscard]] bool parse_timeout(std::string_view text,
                                 std::chrono::nanoseconds& result) {
  double seconds = 0.0;
  auto const* first = text.data();
  auto const* last = first + text.size();
  auto parsed =
      std::from_chars(first, last, seconds, std::chars_format::general);
  if (parsed.ec != std::errc{} || parsed.ptr != last ||
      !std::isfinite(seconds) || seconds < 0.0) {
    return false;
  }

  constexpr auto max_nanoseconds =
      static_cast<long double>(std::numeric_limits<std::int64_t>::max());
  auto nanoseconds = static_cast<long double>(seconds) * 1'000'000'000.0L;
  if (nanoseconds > max_nanoseconds) {
    return false;
  }
  result = std::chrono::nanoseconds{static_cast<std::int64_t>(nanoseconds)};
  return true;
}

[[nodiscard]] bool parse_options(int argc, char** argv, options& result) {
  for (int index = 1; index < argc; ++index) {
    std::string_view argument{argv[index]};
    if (argument == "--") {
      result.command_index = index + 1;
      break;
    }
    if (argument == "--help" || argument == "-h") {
      usage(std::cout, argv[0]);
      std::exit(0);
    }

    auto take_value = [&](std::string& destination) {
      if (++index >= argc) {
        std::cerr << "error: " << argument << " requires a value\n";
        return false;
      }
      destination = argv[index];
      return true;
    };

    if (argument == "--stdout") {
      if (!take_value(result.stdout_path)) {
        return false;
      }
    } else if (argument == "--stderr") {
      if (!take_value(result.stderr_path)) {
        return false;
      }
    } else if (argument == "--metrics") {
      if (!take_value(result.metrics_path)) {
        return false;
      }
    } else if (argument == "--timeout-seconds" || argument == "--timeout") {
      if (++index >= argc) {
        std::cerr << "error: " << argument << " requires a value\n";
        return false;
      }
      if (!parse_timeout(argv[index], result.timeout)) {
        std::cerr << "error: invalid nonnegative timeout in seconds: '"
                  << argv[index] << "'\n";
        return false;
      }
      result.timeout_enabled = result.timeout.count() != 0;
    } else if (argument == "--rss-limit-bytes") {
      if (++index >= argc) {
        std::cerr << "error: " << argument << " requires a value\n";
        return false;
      }
      if (!parse_positive_uint64(argv[index], result.rss_limit_bytes)) {
        std::cerr << "error: invalid positive RSS limit in bytes: '"
                  << argv[index] << "'\n";
        return false;
      }
      result.rss_limit_enabled = true;
    } else if (argument == "--proc-root") {
      if (!take_value(result.proc_root)) {
        return false;
      }
      if (result.proc_root.empty() || result.proc_root.front() != '/') {
        std::cerr << "error: --proc-root requires an absolute path\n";
        return false;
      }
      while (result.proc_root.size() > 1 && result.proc_root.back() == '/') {
        result.proc_root.pop_back();
      }
    } else {
      std::cerr << "error: unknown option before --: " << argument << "\n";
      return false;
    }
  }

  if (result.stdout_path.empty()) {
    std::cerr << "error: --stdout PATH is required\n";
    return false;
  }
  if (result.stderr_path.empty()) {
    std::cerr << "error: --stderr PATH is required\n";
    return false;
  }
  if (result.command_index < 0 || result.command_index >= argc) {
    std::cerr << "error: a command is required after --\n";
    return false;
  }
  if (!result.metrics_path.empty() &&
      (result.metrics_path == result.stdout_path ||
       result.metrics_path == result.stderr_path)) {
    std::cerr << "error: --metrics must differ from child output paths\n";
    return false;
  }
  return true;
}

[[nodiscard]] int open_output(std::string const& path) {
  return ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
}

[[nodiscard]] bool same_file_identity(int left, int right) {
  struct stat left_stat{};
  struct stat right_stat{};
  return ::fstat(left, &left_stat) == 0 && ::fstat(right, &right_stat) == 0 &&
         left_stat.st_dev == right_stat.st_dev &&
         left_stat.st_ino == right_stat.st_ino;
}

[[nodiscard]] bool truncate_output(int fd, std::string_view description) {
  struct stat status{};
  if (::fstat(fd, &status) != 0) {
    std::cerr << "error: cannot inspect " << description << ": "
              << std::strerror(errno) << '\n';
    return false;
  }
  if (!S_ISREG(status.st_mode)) {
    return true;
  }
  if (::ftruncate(fd, 0) == 0) {
    return true;
  }
  std::cerr << "error: cannot truncate " << description << ": "
            << std::strerror(errno) << '\n';
  return false;
}

[[nodiscard]] std::uint64_t monotonic_nanoseconds() {
  timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    std::cerr << "error: clock_gettime(CLOCK_MONOTONIC): "
              << std::strerror(errno) << '\n';
    std::exit(runner_error_status);
  }
  return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(value.tv_nsec);
}

[[nodiscard]] std::string format_nanoseconds(std::uint64_t nanoseconds) {
  auto seconds = nanoseconds / 1'000'000'000ULL;
  auto fraction = nanoseconds % 1'000'000'000ULL;
  auto fraction_text = std::to_string(fraction);
  return std::to_string(seconds) + "." +
         std::string(9 - fraction_text.size(), '0') + fraction_text;
}

[[nodiscard]] std::string format_timeval(timeval value) {
  auto seconds = static_cast<std::uint64_t>(value.tv_sec);
  auto micros = static_cast<std::uint64_t>(value.tv_usec);
  auto fraction_text = std::to_string(micros);
  return std::to_string(seconds) + "." +
         std::string(6 - fraction_text.size(), '0') + fraction_text;
}

[[nodiscard]] bool read_process_identity(pid_t pid, pid_t& process_group,
                                         std::uint64_t& start_time_ticks,
                                         char* process_state = nullptr,
                                         std::uint64_t expected_start_time_ticks =
                                             0) {
  std::ifstream stat{monitored_proc_root + "/" + std::to_string(pid) + "/stat"};
  std::string line;
  if (!stat || !std::getline(stat, line)) {
    record_monitor_error(pid, expected_start_time_ticks);
    return false;
  }
  // comm is parenthesized and may contain spaces. Fields after its final ')'
  // begin with state, ppid, then pgrp.
  auto close = line.rfind(") ");
  if (close == std::string::npos) {
    record_monitor_error(pid, expected_start_time_ticks);
    return false;
  }
  std::string_view rest{line};
  rest.remove_prefix(close + 2);
  auto take_token = [&](std::string_view& token) {
    while (!rest.empty() && rest.front() == ' ') {
      rest.remove_prefix(1);
    }
    if (rest.empty()) {
      return false;
    }
    auto end = rest.find(' ');
    token = rest.substr(0, end);
    rest = end == std::string_view::npos ? std::string_view{}
                                         : rest.substr(end + 1);
    return true;
  };
  // `rest` begins at field 3. pgrp is field 5 and starttime is field 22.
  std::string_view token;
  std::string_view state;
  std::string_view group;
  std::string_view start_time;
  for (int field = 3; field <= 22; ++field) {
    if (!take_token(token)) {
      record_monitor_error(pid, expected_start_time_ticks);
      return false;
    }
    if (field == 3) {
      state = token;
    } else if (field == 5) {
      group = token;
    } else if (field == 22) {
      start_time = token;
    }
  }
  std::uint64_t parsed_group = 0;
  auto parsed =
      std::from_chars(group.data(), group.data() + group.size(), parsed_group);
  if (parsed.ec != std::errc{} || parsed.ptr != group.data() + group.size() ||
      parsed_group >
          static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
    record_monitor_error(pid, expected_start_time_ticks);
    return false;
  }
  std::uint64_t parsed_start_time = 0;
  parsed =
      std::from_chars(start_time.data(), start_time.data() + start_time.size(),
                      parsed_start_time);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != start_time.data() + start_time.size()) {
    record_monitor_error(pid, expected_start_time_ticks);
    return false;
  }
  process_group = static_cast<pid_t>(parsed_group);
  start_time_ticks = parsed_start_time;
  if (state.size() != 1) {
    record_monitor_error(pid, expected_start_time_ticks);
    return false;
  }
  if (process_state != nullptr) {
    *process_state = state.front();
  }
  return true;
}

[[nodiscard]] std::vector<pid_t> read_process_children(
    pid_t pid, std::uint64_t expected_start_time_ticks = 0) {
  std::vector<pid_t> children;
  auto task_path = monitored_proc_root + "/" + std::to_string(pid) + "/task";
  DIR* tasks = ::opendir(task_path.c_str());
  if (tasks == nullptr) {
    record_monitor_error(pid, expected_start_time_ticks);
    return children;
  }
  while (true) {
    errno = 0;
    auto* entry = ::readdir(tasks);
    if (entry == nullptr) {
      if (errno != 0) {
        record_monitor_error(pid, expected_start_time_ticks);
      }
      break;
    }
    std::string_view name{entry->d_name};
    if (name.empty() ||
        !std::all_of(name.begin(), name.end(), [](char character) {
          return character >= '0' && character <= '9';
        })) {
      continue;
    }
    auto children_path = task_path + "/" + std::string{name} + "/children";
    std::ifstream file{children_path};
    if (!file) {
      auto task = task_path + "/" + std::string{name};
      if (::access(task.c_str(), F_OK) == 0) {
        record_monitor_error(pid, expected_start_time_ticks);
      }
      continue;
    }
    std::string contents{std::istreambuf_iterator<char>{file},
                         std::istreambuf_iterator<char>{}};
    if (file.bad()) {
      record_monitor_error(pid, expected_start_time_ticks);
      continue;
    }
    std::string_view remaining{contents};
    while (true) {
      while (!remaining.empty() &&
             (remaining.front() == ' ' || remaining.front() == '\t' ||
              remaining.front() == '\n' || remaining.front() == '\r')) {
        remaining.remove_prefix(1);
      }
      if (remaining.empty()) {
        break;
      }
      auto end = remaining.find_first_of(" \t\n\r");
      auto token = remaining.substr(0, end);
      remaining = end == std::string_view::npos
                      ? std::string_view{}
                      : remaining.substr(end + 1);
      std::uint64_t parsed_pid = 0;
      auto parsed = std::from_chars(token.data(), token.data() + token.size(),
                                    parsed_pid);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != token.data() + token.size() || parsed_pid == 0 ||
          parsed_pid > static_cast<std::uint64_t>(
                           std::numeric_limits<pid_t>::max())) {
        record_monitor_error(pid, expected_start_time_ticks);
        break;
      }
      children.push_back(static_cast<pid_t>(parsed_pid));
    }
  }
  (void)::closedir(tasks);
  return children;
}

[[nodiscard]] std::vector<process_identity> discover_descendants(
    process_identity leader, pid_t runner,
    std::map<pid_t, std::uint64_t>& known_descendants) {
  struct pending_process {
    pid_t pid = 0;
    // A PID read from a current children file may legitimately be a reuse of
    // an older retained PID. Leader and retained-only seeds require their
    // exact frozen identity instead.
    bool currently_discovered = false;
    std::uint64_t exact_start_time_ticks = 0;
  };

  std::vector<pending_process> pending;
  pending.reserve(known_descendants.size() + 2);
  pending.push_back({.pid = leader.pid,
                     .currently_discovered = false,
                     .exact_start_time_ticks = leader.start_time_ticks});
  auto adopted = read_process_children(runner);
  pending.reserve(pending.size() + adopted.size() + known_descendants.size());
  for (auto pid : adopted) {
    pending.push_back({.pid = pid, .currently_discovered = true});
  }

  // Retained (pid,starttime) identities close the short reparenting window
  // when a process exits between two child-tree traversals. A reused PID is
  // never accepted unless it is rediscovered as a current descendant.
  for (auto const& [pid, start_time] : known_descendants) {
    pending.push_back({.pid = pid,
                       .currently_discovered = false,
                       .exact_start_time_ticks = start_time});
  }

  std::set<pid_t> visited;
  std::vector<process_identity> result;
  result.reserve(pending.size());
  for (std::size_t index = 0; index < pending.size(); ++index) {
    auto const candidate = pending[index];
    auto const pid = candidate.pid;
    if (pid <= 0 || pid == runner || visited.contains(pid)) {
      continue;
    }
    pid_t process_group = 0;
    std::uint64_t start_time = 0;
    char process_state = '\0';
    auto const known = known_descendants.find(pid);
    auto const expected_start_time =
        known == known_descendants.end() ? candidate.exact_start_time_ticks
                                         : known->second;
    if (!read_process_identity(pid, process_group, start_time, &process_state,
                               expected_start_time)) {
      if (!candidate.currently_discovered &&
          known != known_descendants.end() &&
          known->second == candidate.exact_start_time_ticks) {
        known_descendants.erase(known);
      }
      continue;
    }
    if (!candidate.currently_discovered &&
        candidate.exact_start_time_ticks != 0 &&
        start_time != candidate.exact_start_time_ticks) {
      if (known != known_descendants.end() &&
          known->second == candidate.exact_start_time_ticks) {
        known_descendants.erase(known);
      }
      continue;
    }
    visited.insert(pid);
    known_descendants[pid] = start_time;
    result.push_back({.pid = pid,
                      .start_time_ticks = start_time,
                      .process_group = process_group,
                      .state = process_state});
    auto children = read_process_children(pid, start_time);
    pending.reserve(pending.size() + children.size());
    for (auto child : children) {
      pending.push_back({.pid = child, .currently_discovered = true});
    }
  }
  return result;
}

[[nodiscard]] std::uint64_t count_live_processes(
    std::vector<process_identity> const& identities) {
  return static_cast<std::uint64_t>(std::count_if(
      identities.begin(), identities.end(),
      [](process_identity const& identity) {
        return process_state_is_live(identity.state);
      }));
}

[[nodiscard]] tree_observation observe_process_tree(
    process_identity leader, pid_t runner,
    std::map<pid_t, std::uint64_t>& known_descendants) {
  tree_observation result;
  result.identities =
      discover_descendants(leader, runner, known_descendants);
  result.live_count = count_live_processes(result.identities);
  result.original_group_alive =
      std::any_of(result.identities.begin(), result.identities.end(),
                  [&](process_identity const& identity) {
                    return process_state_is_live(identity.state) &&
                           identity.process_group == leader.pid;
                  });
  return result;
}

[[nodiscard]] std::vector<process_identity> discover_direct_children(
    pid_t runner, std::map<pid_t, std::uint64_t>& known_descendants) {
  std::vector<process_identity> result;
  for (auto pid : read_process_children(runner)) {
    pid_t process_group = 0;
    std::uint64_t start_time = 0;
    char process_state = '\0';
    auto const known = known_descendants.find(pid);
    auto const expected_start_time =
        known == known_descendants.end() ? 0 : known->second;
    if (!read_process_identity(pid, process_group, start_time, &process_state,
                               expected_start_time)) {
      continue;
    }
    known_descendants[pid] = start_time;
    result.push_back({.pid = pid,
                      .start_time_ticks = start_time,
                      .process_group = process_group,
                      .state = process_state});
  }
  return result;
}

[[nodiscard]] std::vector<process_identity> discover_descendants_for_cleanup(
    process_identity leader, pid_t runner,
    std::map<pid_t, std::uint64_t>& known_descendants) {
  auto saved_root = monitored_proc_root;
  bool const saved_reporting = report_monitor_errors;
  monitored_proc_root = "/proc";
  report_monitor_errors = false;
  auto result = discover_descendants(leader, runner, known_descendants);
  monitored_proc_root = std::move(saved_root);
  report_monitor_errors = saved_reporting;
  return result;
}

[[nodiscard]] bool sample_proc_status(process_identity identity,
                                      proc_peaks& peaks,
                                      std::uint64_t& rss_kb,
                                      std::uint64_t& swap_kb) {
  auto const pid = identity.pid;
  // A process can enter zombie state while procfs is producing one status
  // snapshot: the State line can still say running even though later memory
  // lines have disappeared. Retry that transient once before deciding a live
  // process is unmonitorable. A broken injected proc root fails both reads and
  // is still reported because the identity remains live in real /proc.
  for (int attempt = 0; attempt < 2; ++attempt) {
    pid_t observed_group = 0;
    std::uint64_t observed_start_time = 0;
    char observed_state = '\0';
    // Discovery just validated this identity. Reuse that first bracket and
    // retain the post-status identity read; only a retry needs another leading
    // stat read. This removes one procfs read per live process and sample
    // without weakening PID-reuse detection.
    if (attempt != 0) {
      if (!read_process_identity(pid, observed_group, observed_start_time,
                                 &observed_state,
                                 identity.start_time_ticks)) {
        continue;
      }
      if (observed_start_time != identity.start_time_ticks ||
          !process_state_is_live(observed_state)) {
        return false;
      }
    }
    std::ifstream status{monitored_proc_root + "/" + std::to_string(pid) +
                         "/status"};
    if (!status) {
      continue;
    }
    ++peaks.status_samples;

    bool saw_rss = false;
    bool saw_swap = false;
    bool saw_state = false;
    bool zombie = false;
    bool malformed = false;
    std::uint64_t sampled_rss_kb = 0;
    std::uint64_t sampled_swap_kb = 0;
    std::string line;
    while (std::getline(status, line)) {
      auto parse_kb = [&](std::string_view prefix, std::uint64_t& value,
                          std::uint64_t& samples, bool& saw_value) {
        if (!line.starts_with(prefix)) {
          return;
        }
        std::string_view rest{line};
        rest.remove_prefix(prefix.size());
        while (!rest.empty() &&
               (rest.front() == ' ' || rest.front() == '\t')) {
          rest.remove_prefix(1);
        }
        std::uint64_t parsed_value = 0;
        auto parsed = std::from_chars(rest.data(),
                                      rest.data() + rest.size(), parsed_value);
        if (parsed.ec == std::errc{}) {
          value = parsed_value;
          ++samples;
          saw_value = true;
        } else {
          malformed = true;
        }
      };
      if (line.starts_with("State:")) {
        std::string_view state{line};
        state.remove_prefix(std::string_view{"State:"}.size());
        while (!state.empty() &&
               (state.front() == ' ' || state.front() == '\t')) {
          state.remove_prefix(1);
        }
        if (state.empty()) {
          malformed = true;
        } else {
          saw_state = true;
          zombie = state.front() == 'Z' || state.front() == 'X';
        }
      }
      parse_kb("VmRSS:", sampled_rss_kb, peaks.rss_samples, saw_rss);
      parse_kb("VmSwap:", sampled_swap_kb, peaks.swap_samples, saw_swap);
    }
    if (zombie) {
      return false;
    }
    // Linux legitimately omits VmRSS/VmSwap after a task has detached its mm
    // during exit, before State necessarily becomes Z. A complete status with
    // a valid State line therefore represents a zero-memory sample, not a
    // monitoring failure.
    if (!malformed && saw_state) {
      if (!read_process_identity(pid, observed_group, observed_start_time,
                                 &observed_state,
                                 identity.start_time_ticks)) {
        continue;
      }
      if (observed_start_time != identity.start_time_ticks ||
          !process_state_is_live(observed_state)) {
        return false;
      }
      rss_kb = sampled_rss_kb;
      swap_kb = sampled_swap_kb;
      return true;
    }
  }
  record_monitor_error(pid, identity.start_time_ticks);
  return false;
}

void saturating_add(std::uint64_t& destination, std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() - destination) {
    destination = std::numeric_limits<std::uint64_t>::max();
  } else {
    destination += value;
  }
}

[[nodiscard]] sampled_process_tree sample_process_tree(
    process_identity leader, pid_t runner,
    std::map<pid_t, std::uint64_t>& known_descendants, proc_peaks& peaks) {
  sampled_process_tree result;
  result.tree = observe_process_tree(leader, runner, known_descendants);
  for (auto const& identity : result.tree.identities) {
    if (!process_state_is_live(identity.state)) {
      continue;
    }
    std::uint64_t rss_kb = 0;
    std::uint64_t swap_kb = 0;
    if (!sample_proc_status(identity, peaks, rss_kb, swap_kb)) {
      continue;
    }
    ++result.sample.process_count;
    saturating_add(result.sample.rss_kb, rss_kb);
    saturating_add(result.sample.swap_kb, swap_kb);
  }
  ++peaks.process_group_samples;
  peaks.peak_rss_kb = std::max(peaks.peak_rss_kb, result.sample.rss_kb);
  peaks.peak_swap_kb =
      std::max(peaks.peak_swap_kb, result.sample.swap_kb);
  peaks.peak_process_count =
      std::max(peaks.peak_process_count, result.sample.process_count);
  return result;
}

[[nodiscard]] std::uint64_t kibibytes_to_bytes(std::uint64_t kibibytes) {
  constexpr std::uint64_t bytes_per_kibibyte = 1024;
  if (kibibytes >
      std::numeric_limits<std::uint64_t>::max() / bytes_per_kibibyte) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return kibibytes * bytes_per_kibibyte;
}

void sleep_for_sample_period(std::uint64_t now_ns, std::uint64_t wake_ns) {
  if (wake_ns <= now_ns) {
    return;
  }
  auto duration = std::min<std::uint64_t>(
      wake_ns - now_ns,
      static_cast<std::uint64_t>(sample_period.count()) * 1'000'000ULL);
  timespec request{
      .tv_sec = static_cast<time_t>(duration / 1'000'000'000ULL),
      .tv_nsec = static_cast<long>(duration % 1'000'000'000ULL),
  };
  while (::nanosleep(&request, &request) != 0 && errno == EINTR) {
  }
}

[[nodiscard]] bool tracked_process_group_exists(
    process_identity leader, pid_t runner,
    std::map<pid_t, std::uint64_t>& known_descendants) {
  auto descendants = discover_descendants(leader, runner, known_descendants);
  for (auto const& identity : descendants) {
    pid_t process_group = 0;
    std::uint64_t start_time = 0;
    char process_state = '\0';
    if (read_process_identity(identity.pid, process_group, start_time,
                              &process_state, identity.start_time_ticks) &&
        start_time == identity.start_time_ticks &&
        process_state_is_live(process_state) &&
        process_group == leader.pid) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool signal_process_tree(
    process_identity leader, pid_t runner,
    std::map<pid_t, std::uint64_t>& known_descendants, int signal_number) {
  bool sent = false;
  auto saved_root = monitored_proc_root;
  bool const saved_reporting = report_monitor_errors;
  monitored_proc_root = "/proc";
  report_monitor_errors = false;
  auto descendants = discover_descendants(leader, runner, known_descendants);
  bool tracked_group_member = false;
  for (auto const& identity : descendants) {
    pid_t process_group = 0;
    std::uint64_t start_time = 0;
    char process_state = '\0';
    if (read_process_identity(identity.pid, process_group, start_time,
                              &process_state, identity.start_time_ticks) &&
        start_time == identity.start_time_ticks &&
        process_state_is_live(process_state) &&
        process_group == leader.pid) {
      tracked_group_member = true;
      break;
    }
  }
  if (tracked_group_member &&
      (::kill(-leader.pid, signal_number) == 0 || errno == EPERM)) {
    sent = true;
  }
  for (auto const& identity : descendants) {
    pid_t process_group = 0;
    std::uint64_t start_time = 0;
    char process_state = '\0';
    bool const identity_matches =
        read_process_identity(identity.pid, process_group, start_time,
                              &process_state, identity.start_time_ticks) &&
        start_time == identity.start_time_ticks &&
        process_state_is_live(process_state);
    if (!identity_matches) {
      continue;
    }
    if (::kill(identity.pid, signal_number) == 0 || errno == EPERM) {
      sent = true;
    }
  }
  monitored_proc_root = std::move(saved_root);
  report_monitor_errors = saved_reporting;
  return sent;
}

void cleanup_unfrozen_child_tree(pid_t child) {
  auto saved_root = monitored_proc_root;
  bool const saved_reporting = report_monitor_errors;
  monitored_proc_root = "/proc";
  report_monitor_errors = false;

  auto const runner = ::getpid();
  std::map<pid_t, std::uint64_t> known_descendants;
  process_identity const unfrozen_leader{
      .pid = child,
      .start_time_ticks = 0,
      .process_group = child,
  };

  // The unreaped child PID cannot yet be reused. Address its original group
  // and PID immediately, then repeatedly discover adopted/late-forked members.
  if (::getpgid(child) == child) {
    (void)::kill(-child, SIGKILL);
  }
  (void)::kill(child, SIGKILL);

  bool saw_echild = false;
  while (true) {
    (void)signal_process_tree(unfrozen_leader, runner, known_descendants,
                              SIGKILL);

    saw_echild = false;
    while (true) {
      auto waited = ::wait4(-1, nullptr, WNOHANG, nullptr);
      if (waited > 0) {
        known_descendants.erase(waited);
        continue;
      }
      if (waited == 0) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == ECHILD) {
        saw_echild = true;
      }
      break;
    }

    bool reaped_zombie = false;
    for (auto const& identity :
         discover_direct_children(runner, known_descendants)) {
      if (process_state_is_live(identity.state)) {
        continue;
      }
      while (true) {
        auto waited = ::wait4(identity.pid, nullptr, 0, nullptr);
        if (waited > 0) {
          known_descendants.erase(waited);
          reaped_zombie = true;
          break;
        }
        if (waited < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
      if (reaped_zombie) {
        break;
      }
    }
    if (reaped_zombie) {
      continue;
    }

    auto descendants =
        discover_descendants(unfrozen_leader, runner, known_descendants);
    bool const group_alive = tracked_process_group_exists(
        unfrozen_leader, runner, known_descendants);
    if (saw_echild && descendants.empty() && !group_alive) {
      break;
    }

    auto const pause_start = monotonic_nanoseconds();
    sleep_for_sample_period(pause_start, pause_start + 1'000'000ULL);
  }

  monitored_proc_root = std::move(saved_root);
  report_monitor_errors = saved_reporting;
}

void add_timeval(timeval& destination, timeval value) {
  destination.tv_sec += value.tv_sec;
  destination.tv_usec += value.tv_usec;
  if (destination.tv_usec >= 1'000'000) {
    destination.tv_sec += destination.tv_usec / 1'000'000;
    destination.tv_usec %= 1'000'000;
  }
}

void add_usage(rusage& destination, rusage const& value) {
  add_timeval(destination.ru_utime, value.ru_utime);
  add_timeval(destination.ru_stime, value.ru_stime);
  destination.ru_maxrss = std::max(destination.ru_maxrss, value.ru_maxrss);
}

void report_child_error(int fd, child_error_stage stage, int error_number) {
  child_error error{.stage = stage, .error_number = error_number};
  auto const* bytes = reinterpret_cast<char const*>(&error);
  std::size_t written = 0;
  while (written < sizeof(error)) {
    auto result = ::write(fd, bytes + written, sizeof(error) - written);
    if (result > 0) {
      written += static_cast<std::size_t>(result);
    } else if (result < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
}

[[nodiscard]] bool write_all(int fd, std::string_view text) {
  std::size_t written = 0;
  while (written < text.size()) {
    auto result = ::write(fd, text.data() + written, text.size() - written);
    if (result > 0) {
      written += static_cast<std::size_t>(result);
    } else if (result < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  options opts;
  if (!parse_options(argc, argv, opts)) {
    usage(std::cerr, argv[0]);
    return 2;
  }

  int metrics_fd = STDOUT_FILENO;
  bool close_metrics = false;
  if (!opts.metrics_path.empty()) {
    metrics_fd = open_output(opts.metrics_path);
    if (metrics_fd < 0) {
      std::cerr << "error: cannot open metrics path '" << opts.metrics_path
                << "': " << std::strerror(errno) << '\n';
      return runner_error_status;
    }
    close_metrics = true;
  }

  int stdout_fd = open_output(opts.stdout_path);
  if (stdout_fd < 0) {
    std::cerr << "error: cannot open child stdout path '" << opts.stdout_path
              << "': " << std::strerror(errno) << '\n';
    if (close_metrics) {
      ::close(metrics_fd);
    }
    return runner_error_status;
  }
  int stderr_fd = -1;
  if (opts.stderr_path == opts.stdout_path) {
    stderr_fd = stdout_fd;
  } else {
    stderr_fd = open_output(opts.stderr_path);
    if (stderr_fd < 0) {
      std::cerr << "error: cannot open child stderr path '" << opts.stderr_path
                << "': " << std::strerror(errno) << '\n';
      ::close(stdout_fd);
      if (close_metrics) {
        ::close(metrics_fd);
      }
      return runner_error_status;
    }
  }

  // Compare opened objects, not path spellings: hardlinks must not allow the
  // machine-readable metrics stream to alias a child output. Open without
  // O_TRUNC above so an invalid alias cannot destroy either file before this
  // check. Two child-output hardlinks are intentionally treated like one
  // shared stdout/stderr destination.
  if (same_file_identity(metrics_fd, stdout_fd) ||
      same_file_identity(metrics_fd, stderr_fd)) {
    std::cerr << "error: metrics output aliases child stdout/stderr\n";
    ::close(stdout_fd);
    if (stderr_fd != stdout_fd) {
      ::close(stderr_fd);
    }
    if (close_metrics) {
      ::close(metrics_fd);
    }
    return runner_error_status;
  }
  if (stderr_fd != stdout_fd && same_file_identity(stdout_fd, stderr_fd)) {
    ::close(stderr_fd);
    stderr_fd = stdout_fd;
  }
  if ((close_metrics && !truncate_output(metrics_fd, "metrics output")) ||
      !truncate_output(stdout_fd, "child stdout") ||
      (stderr_fd != stdout_fd && !truncate_output(stderr_fd, "child stderr"))) {
    ::close(stdout_fd);
    if (stderr_fd != stdout_fd) {
      ::close(stderr_fd);
    }
    if (close_metrics) {
      ::close(metrics_fd);
    }
    return runner_error_status;
  }

  if (::prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
    std::cerr << "error: prctl(PR_SET_CHILD_SUBREAPER): "
              << std::strerror(errno) << '\n';
    ::close(stdout_fd);
    if (stderr_fd != stdout_fd) {
      ::close(stderr_fd);
    }
    if (close_metrics) {
      ::close(metrics_fd);
    }
    return runner_error_status;
  }

  // SIG_IGN/SA_NOCLDWAIT is inherited across exec and would let the kernel
  // auto-reap children before wait4 can collect their status or usage.
  struct sigaction inherited_sigchld {};
  struct sigaction default_sigchld {};
  default_sigchld.sa_handler = SIG_DFL;
  if (::sigaction(SIGCHLD, nullptr, &inherited_sigchld) != 0 ||
      ::sigemptyset(&default_sigchld.sa_mask) != 0 ||
      ::sigaction(SIGCHLD, &default_sigchld, nullptr) != 0) {
    std::cerr << "error: cannot establish waitable SIGCHLD disposition: "
              << std::strerror(errno) << '\n';
    ::close(stdout_fd);
    if (stderr_fd != stdout_fd) {
      ::close(stderr_fd);
    }
    if (close_metrics) {
      ::close(metrics_fd);
    }
    return runner_error_status;
  }

  int error_pipe[2]{};
  if (::pipe2(error_pipe, O_CLOEXEC) != 0) {
    std::cerr << "error: pipe2: " << std::strerror(errno) << '\n';
    if (stdout_fd > STDERR_FILENO) {
      ::close(stdout_fd);
    }
    if (stderr_fd != stdout_fd && stderr_fd > STDERR_FILENO) {
      ::close(stderr_fd);
    }
    if (close_metrics && metrics_fd > STDERR_FILENO) {
      ::close(metrics_fd);
    }
    return runner_error_status;
  }

  auto start_ns = monotonic_nanoseconds();
  pid_t child = ::fork();
  if (child < 0) {
    std::cerr << "error: fork: " << std::strerror(errno) << '\n';
    ::close(error_pipe[0]);
    ::close(error_pipe[1]);
    ::close(stdout_fd);
    if (stderr_fd != stdout_fd) {
      ::close(stderr_fd);
    }
    if (close_metrics) {
      ::close(metrics_fd);
    }
    return runner_error_status;
  }

  if (child == 0) {
    ::close(error_pipe[0]);
    if (::sigaction(SIGCHLD, &inherited_sigchld, nullptr) != 0) {
      report_child_error(error_pipe[1], child_error_stage::restore_sigchld,
                         errno);
      _exit(126);
    }
    if (::setpgid(0, 0) != 0) {
      report_child_error(error_pipe[1], child_error_stage::set_process_group,
                         errno);
      _exit(126);
    }
    if (::dup2(stdout_fd, STDOUT_FILENO) < 0) {
      report_child_error(error_pipe[1], child_error_stage::redirect_stdout,
                         errno);
      _exit(126);
    }
    if (::dup2(stderr_fd, STDERR_FILENO) < 0) {
      report_child_error(error_pipe[1], child_error_stage::redirect_stderr,
                         errno);
      _exit(126);
    }
    if (stdout_fd > STDERR_FILENO) {
      ::close(stdout_fd);
    }
    if (stderr_fd != stdout_fd && stderr_fd > STDERR_FILENO) {
      ::close(stderr_fd);
    }
    if (close_metrics && metrics_fd > STDERR_FILENO) {
      ::close(metrics_fd);
    }

    ::execvp(argv[opts.command_index], argv + opts.command_index);
    auto error_number = errno;
    report_child_error(error_pipe[1], child_error_stage::exec, error_number);
    _exit(127);
  }

  ::close(error_pipe[1]);
  ::close(stdout_fd);
  if (stderr_fd != stdout_fd) {
    ::close(stderr_fd);
  }

  // The child also calls setpgid before exec. The parent-side call closes the
  // fork-to-setpgid race so a timeout can always address the whole command
  // process group rather than only its leader.
  if (::setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
    std::cerr << "warning: setpgid(" << child << "): " << std::strerror(errno)
              << '\n';
  }
  pid_t initial_leader_group = 0;
  std::uint64_t leader_start_time = 0;
  if (!read_process_identity(child, initial_leader_group, leader_start_time)) {
    std::cerr << "error: cannot freeze child process identity\n";
    cleanup_unfrozen_child_tree(child);
    ::close(error_pipe[0]);
    if (close_metrics) {
      ::close(metrics_fd);
    }
    return runner_error_status;
  }
  process_identity const leader_identity{
      .pid = child,
      .start_time_ticks = leader_start_time,
      .process_group = initial_leader_group,
  };
  monitored_proc_root = opts.proc_root;

  proc_peaks peaks;
  int wait_status = 0;
  rusage usage{};
  bool wait_collected = false;
  bool wait_failed = false;
  int wait_error = 0;
  bool wait4_echild_at_return = false;
  std::uint64_t descendants_reaped = 0;
  std::uint64_t post_leader_descendants = 0;
  bool descendant_cleanup_kill_sent = false;
  std::map<pid_t, std::uint64_t> known_descendants;
  auto const runner_pid = ::getpid();
  termination_cause termination = termination_cause::none;
  bool termination_term_sent = false;
  bool termination_kill_sent = false;
  bool rss_limit_observed = false;
  std::uint64_t rss_limit_trigger_bytes = 0;
  std::uint64_t timeout_at = 0;
  if (opts.timeout_enabled) {
    timeout_at = start_ns + static_cast<std::uint64_t>(opts.timeout.count());
  }
  std::uint64_t terminate_at = 0;

  auto collect_waited = [&](pid_t waited, int current_status,
                            rusage const& current_usage) {
    add_usage(usage, current_usage);
    known_descendants.erase(waited);
    wait4_echild_at_return = false;
    if (waited == child) {
      wait_status = current_status;
      wait_collected = true;
    } else {
      ++descendants_reaped;
    }
  };

  auto reap_available = [&] {
    wait4_echild_at_return = false;
    while (true) {
      int current_status = 0;
      rusage current_usage{};
      auto waited = ::wait4(-1, &current_status, WNOHANG, &current_usage);
      if (waited > 0) {
        collect_waited(waited, current_status, current_usage);
        continue;
      }
      if (waited == 0) {
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == ECHILD) {
        wait4_echild_at_return = true;
        return;
      }
      wait_failed = true;
      if (wait_error == 0) {
        wait_error = errno;
      }
      return;
    }
  };

  auto observe_tree = [&] {
    return observe_process_tree(leader_identity, runner_pid,
                                known_descendants);
  };

  auto reap_direct_zombie = [&] {
    auto direct = discover_direct_children(runner_pid, known_descendants);
    for (auto const& identity : direct) {
      if (process_state_is_live(identity.state)) {
        continue;
      }
      while (true) {
        int current_status = 0;
        rusage current_usage{};
        auto waited =
            ::wait4(identity.pid, &current_status, 0, &current_usage);
        if (waited > 0) {
          collect_waited(waited, current_status, current_usage);
          return true;
        }
        if (waited < 0 && errno == EINTR) {
          continue;
        }
        if (waited < 0 && errno == ECHILD) {
          known_descendants.erase(identity.pid);
          return false;
        }
        if (waited < 0) {
          wait_failed = true;
          if (wait_error == 0) {
            wait_error = errno;
          }
        }
        return false;
      }
    }
    return false;
  };

  // Once the leader has been collected, a tracked Z/X-only chain is already
  // dead. Reap a verified direct zombie synchronously; this cannot block, and
  // reaping each root exposes the next adopted generation. Thus completion is
  // decided before a crossed timeout deadline, without a timing grace period.
  auto settle_dead_chain = [&](tree_observation tree) {
    int unexplained_snapshots = 0;
    while (wait_collected && !wait_failed && !monitor_error_observed &&
           tree.live_count == 0 && !tree.original_group_alive) {
      if (wait4_echild_at_return && tree.identities.empty()) {
        return tree;
      }
      if (reap_direct_zombie()) {
        reap_available();
        tree = observe_tree();
        unexplained_snapshots = 0;
        continue;
      }
      reap_available();
      tree = observe_tree();
      if (tree.live_count != 0 || tree.original_group_alive ||
          (wait4_echild_at_return && tree.identities.empty())) {
        return tree;
      }
      if (++unexplained_snapshots >= 2) {
        wait_failed = true;
        if (wait_error == 0) {
          wait_error = EPROTO;
        }
        return tree;
      }
    }
    return tree;
  };

  while (true) {
    auto const sample_started_ns = monotonic_nanoseconds();
    auto sampled = sample_process_tree(leader_identity, runner_pid,
                                       known_descendants, peaks);
    auto const& sample = sampled.sample;
    auto now_ns = monotonic_nanoseconds();
    if (!rss_limit_observed && opts.rss_limit_enabled &&
        sample.rss_kb > opts.rss_limit_bytes / 1024) {
      rss_limit_observed = true;
      rss_limit_trigger_bytes = kibibytes_to_bytes(sample.rss_kb);
    }

    reap_available();
    // The sampled traversal is a complete liveness snapshot for a still-live
    // leader. If wait4 collected the leader, refresh immediately: this closes
    // the fork/reparent window between sampling and collection before any
    // completion, timeout, RSS, or leak decision is made.
    auto tree = std::move(sampled.tree);
    if (wait_collected) {
      tree = observe_tree();
    }
    if (wait_collected && !wait_failed && !monitor_error_observed) {
      tree = settle_dead_chain(std::move(tree));
    }
    now_ns = monotonic_nanoseconds();

    if (monitor_error_observed &&
        termination != termination_cause::monitor_error) {
      termination = termination_cause::monitor_error;
      termination_kill_sent = signal_process_tree(
          leader_identity, runner_pid, known_descendants, SIGKILL);
    }
    if (wait_failed) {
      break;
    }

    bool const completed = wait_collected && wait4_echild_at_return &&
                           tree.identities.empty() &&
                           !tree.original_group_alive;

    // A wholly dead tree completes before timeout classification. RSS keeps
    // its existing first-over-limit-sample semantics. For a genuinely live
    // tree, deterministic cause precedence is timeout > RSS > leak.
    if (termination == termination_cause::none && completed &&
        !rss_limit_observed) {
      break;
    }
    if (termination == termination_cause::none && opts.timeout_enabled &&
        now_ns >= timeout_at &&
        (tree.live_count != 0 || tree.original_group_alive)) {
      termination = termination_cause::timeout;
      termination_term_sent = signal_process_tree(
          leader_identity, runner_pid, known_descendants, SIGTERM);
      terminate_at =
          now_ns +
          static_cast<std::uint64_t>(terminate_grace.count()) * 1'000'000ULL;
    } else if (termination == termination_cause::none && rss_limit_observed) {
      termination = termination_cause::rss_limit;
      termination_kill_sent = signal_process_tree(
          leader_identity, runner_pid, known_descendants, SIGKILL);
    } else if (termination == termination_cause::none && wait_collected &&
               (tree.live_count != 0 || tree.original_group_alive)) {
      termination = termination_cause::descendant_leak;
      post_leader_descendants = std::max<std::uint64_t>(
          post_leader_descendants,
          tree.live_count == 0 ? 1 : tree.live_count);
      descendant_cleanup_kill_sent = signal_process_tree(
          leader_identity, runner_pid, known_descendants, SIGKILL);
      termination_kill_sent = descendant_cleanup_kill_sent;
    }

    if (termination == termination_cause::timeout && !termination_kill_sent &&
        now_ns >= terminate_at) {
      termination_kill_sent = signal_process_tree(
          leader_identity, runner_pid, known_descendants, SIGKILL);
    } else if (termination == termination_cause::rss_limit ||
               termination == termination_cause::descendant_leak ||
               termination == termination_cause::monitor_error) {
      bool const sent = signal_process_tree(
          leader_identity, runner_pid, known_descendants, SIGKILL);
      termination_kill_sent = termination_kill_sent || sent;
      if (termination == termination_cause::descendant_leak) {
        descendant_cleanup_kill_sent = descendant_cleanup_kill_sent || sent;
      }
    }

    if (termination != termination_cause::none && completed) {
      break;
    }

    std::uint64_t wake_ns =
        sample_started_ns +
        static_cast<std::uint64_t>(sample_period.count()) * 1'000'000ULL;
    if (opts.timeout_enabled && termination == termination_cause::none) {
      wake_ns = std::min(wake_ns, timeout_at);
    } else if (termination == termination_cause::timeout &&
               !termination_kill_sent) {
      wake_ns = std::min(wake_ns, terminate_at);
    }
    sleep_for_sample_period(monotonic_nanoseconds(), wake_ns);
  }

  // Every path, including wait/monitor/setup failures and late setsid forks,
  // converges on the same real-procfs kill/reap loop. Never block on a live
  // child: kill identity-checked processes first, then synchronously reap only
  // verified direct zombies, repeating until ECHILD and an empty tree prove
  // that no descendant or old process-group member can escape.
  auto saved_proc_root = monitored_proc_root;
  bool const saved_monitor_reporting = report_monitor_errors;
  monitored_proc_root = "/proc";
  report_monitor_errors = false;
  while (true) {
    reap_available();
    auto tree = observe_tree();
    if (wait4_echild_at_return && tree.identities.empty() &&
        !tree.original_group_alive) {
      break;
    }

    bool const sent = signal_process_tree(leader_identity, runner_pid,
                                          known_descendants, SIGKILL);
    termination_kill_sent = termination_kill_sent || sent;
    if (termination == termination_cause::descendant_leak) {
      descendant_cleanup_kill_sent = descendant_cleanup_kill_sent || sent;
    }

    reap_available();
    if (reap_direct_zombie()) {
      continue;
    }

    // A live SIGKILL-pending task may need a scheduling turn before becoming
    // waitable; repeat discovery rather than performing a blocking wait4.
    auto const pause_start = monotonic_nanoseconds();
    sleep_for_sample_period(pause_start, pause_start + 1'000'000ULL);
  }

  auto final_tree = observe_tree();
  auto const live_at_return = count_live_processes(final_tree.identities);
  bool const group_alive_at_return = final_tree.original_group_alive;
  if (!wait4_echild_at_return || !final_tree.identities.empty() ||
      live_at_return != 0 || group_alive_at_return) {
    wait_failed = true;
    if (wait_error == 0) {
      wait_error = EBUSY;
    }
  }
  monitored_proc_root = std::move(saved_proc_root);
  report_monitor_errors = saved_monitor_reporting;

  auto end_ns = monotonic_nanoseconds();

  child_error child_failure{};
  ssize_t bytes_read = -1;
  do {
    bytes_read = ::read(error_pipe[0], &child_failure, sizeof(child_failure));
  } while (bytes_read < 0 && errno == EINTR);
  if (bytes_read < 0 && errno != EINTR) {
    std::cerr << "warning: cannot read child setup status: "
              << std::strerror(errno) << '\n';
  }
  ::close(error_pipe[0]);

  int child_exit_code = -1;
  int child_term_signal = 0;
  int core_dumped = 0;
  if (wait_collected && WIFEXITED(wait_status)) {
    child_exit_code = WEXITSTATUS(wait_status);
  } else if (wait_collected && WIFSIGNALED(wait_status)) {
    child_term_signal = WTERMSIG(wait_status);
#ifdef WCOREDUMP
    core_dumped = WCOREDUMP(wait_status) ? 1 : 0;
#endif
  }

  int runner_status = runner_error_status;
  std::string_view outcome = "wait_error";
  if (wait_failed) {
    runner_status = runner_error_status;
  } else if (termination == termination_cause::timeout) {
    runner_status = timeout_status;
    outcome = "timeout";
  } else if (termination == termination_cause::rss_limit) {
    runner_status = rss_limit_status;
    outcome = "rss_limit";
  } else if (termination == termination_cause::descendant_leak) {
    runner_status = runner_error_status;
    outcome = "descendant_leak";
  } else if (termination == termination_cause::monitor_error) {
    runner_status = runner_error_status;
    outcome = "monitor_error";
  } else if (child_failure.stage != child_error_stage::none) {
    runner_status =
        child_exit_code >= 0 ? child_exit_code : runner_error_status;
    outcome = child_failure.stage == child_error_stage::exec ? "exec_error"
                                                             : "setup_error";
  } else if (child_exit_code >= 0) {
    runner_status = child_exit_code;
    outcome = "exited";
  } else if (child_term_signal != 0) {
    runner_status = 128 + child_term_signal;
    outcome = "signaled";
  }

  std::string metrics;
  metrics.reserve(768);
  auto add_metric = [&](std::string_view key, auto const& value) {
    metrics += key;
    metrics += '=';
    metrics += value;
    metrics += '\n';
  };
  auto add_integer = [&](std::string_view key, auto value) {
    add_metric(key, std::to_string(value));
  };

  add_integer("schema_version", 2);
  add_metric("outcome", outcome);
  add_integer("exit_code", child_exit_code);
  add_integer("term_signal", child_term_signal);
  add_integer("timed_out", outcome == "timeout" ? 1 : 0);
  add_integer("runner_exit_code", runner_status);
  add_metric("wall_seconds", format_nanoseconds(end_ns - start_ns));
  add_metric("user_seconds", format_timeval(usage.ru_utime));
  add_metric("system_seconds", format_timeval(usage.ru_stime));
  // Linux documents ru_maxrss and /proc status memory values in KiB despite
  // spelling the unit "kB". The explicit unit row prevents byte conversion
  // ambiguity in benchmark consumers.
  add_integer("max_rss_kb", std::max<long>(usage.ru_maxrss, 0));
  add_integer("peak_sampled_rss_kb", peaks.peak_rss_kb);
  add_integer("peak_sampled_swap_kb", peaks.peak_swap_kb);
  add_metric("rss_kb_unit", "1024_bytes");
  add_integer("proc_status_samples", peaks.status_samples);
  add_integer("proc_rss_samples", peaks.rss_samples);
  add_integer("proc_swap_samples", peaks.swap_samples);
  add_integer("proc_group_samples", peaks.process_group_samples);
  add_integer("peak_sampled_process_count", peaks.peak_process_count);
  add_integer("subreaper_enabled", 1);
  add_integer("descendants_reaped", descendants_reaped);
  add_integer("post_leader_descendants", post_leader_descendants);
  add_integer("descendant_cleanup_kill_sent",
              descendant_cleanup_kill_sent ? 1 : 0);
  add_integer("live_descendants_at_return", live_at_return);
  add_integer("process_group_alive_at_return", group_alive_at_return ? 1 : 0);
  add_integer("wait4_echild_at_return", wait4_echild_at_return ? 1 : 0);
  add_integer("monitor_error", outcome == "monitor_error" ? 1 : 0);
  add_integer("monitor_error_count",
              outcome == "monitor_error" ? monitor_error_count : 0);
  add_integer("wait4_collected", wait_collected ? 1 : 0);
  add_integer("wait_errno", wait_error);
  add_metric("child_error_stage", stage_name(child_failure.stage));
  add_integer("child_error_errno", child_failure.error_number);
  add_integer("core_dumped", core_dumped);
  add_integer("timeout_term_sent",
              outcome == "timeout" && termination_term_sent ? 1 : 0);
  add_integer("timeout_kill_sent",
              outcome == "timeout" && termination_kill_sent ? 1 : 0);
  add_integer("rss_limit_bytes", opts.rss_limit_bytes);
  add_integer("rss_limit_enabled", opts.rss_limit_enabled ? 1 : 0);
  add_integer("rss_limit_observed", rss_limit_observed ? 1 : 0);
  add_integer("rss_limit_exceeded", outcome == "rss_limit" ? 1 : 0);
  add_integer("rss_limit_trigger_bytes", rss_limit_trigger_bytes);
  add_integer(
      "rss_limit_term_sent",
      outcome == "rss_limit" && termination_term_sent ? 1 : 0);
  add_integer(
      "rss_limit_kill_sent",
      outcome == "rss_limit" && termination_kill_sent ? 1 : 0);

  bool metrics_written = write_all(metrics_fd, metrics);
  if (close_metrics && ::close(metrics_fd) != 0) {
    metrics_written = false;
  }
  if (!metrics_written) {
    std::cerr << "error: could not write complete process metrics: "
              << std::strerror(errno) << '\n';
    return runner_error_status;
  }
  return runner_status;
}
