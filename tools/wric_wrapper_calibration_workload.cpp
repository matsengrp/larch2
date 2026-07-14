#include <barrier>
#include <bit>
#include <cstdint>
#include <iostream>
#include <sched.h>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t thread_count = 8;
constexpr std::uint64_t iterations_per_thread = 2'250'000'000ULL;
constexpr std::string_view version =
    "wric-wrapper-calibration-workload-v1";

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view{argv[1]} == "--version") {
    std::cout << version << '\n';
    return 0;
  }
  if (argc != 1) {
    std::cerr << "usage: " << argv[0] << " [--version]\n";
    return 2;
  }

  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  if (::sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
    std::cerr << "error: sched_getaffinity failed\n";
    return 2;
  }
  auto const affinity_count = CPU_COUNT(&affinity);
  if (affinity_count != static_cast<int>(thread_count)) {
    std::cerr << "error: affinity has " << affinity_count << " CPUs, expected "
              << thread_count << '\n';
    return 2;
  }

  std::barrier start_line{static_cast<std::ptrdiff_t>(thread_count + 1)};
  std::vector<std::uint64_t> results(thread_count);
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (std::size_t worker = 0; worker < thread_count; ++worker) {
    workers.emplace_back([&, worker] {
      std::uint64_t value =
          0x9e3779b97f4a7c15ULL ^ ((worker + 1) * 0xd1b54a32d192ed03ULL);
      start_line.arrive_and_wait();
      for (std::uint64_t iteration = 0; iteration < iterations_per_thread;
           ++iteration) {
        value ^= value >> 12;
        value ^= value << 25;
        value ^= value >> 27;
        value *= 0x2545f4914f6cdd1dULL;
        value = std::rotl(value, 17) + iteration +
                0x94d049bb133111ebULL;
      }
      results[worker] = value;
    });
  }
  start_line.arrive_and_wait();
  for (auto& worker : workers) {
    worker.join();
  }

  std::uint64_t checksum = 0x6a09e667f3bcc909ULL;
  for (auto value : results) {
    checksum = std::rotl(checksum ^ value, 11) * 0x9e3779b97f4a7c15ULL;
  }
  std::cout << "schema=wric-wrapper-calibration-workload-v1\n"
            << "threads=" << thread_count << '\n'
            << "iterations_per_thread=" << iterations_per_thread << '\n'
            << "affinity_cpu_count=" << affinity_count << '\n'
            << "checksum=" << checksum << '\n';
  return 0;
}
