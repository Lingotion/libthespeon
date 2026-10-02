// This code and software are protected by intellectual property law and is the property of Lingotion AB, reg. no. 559341-4138, Sweden.
// The code and software may only be used and distributed according to the Terms of Service and Use found at https://lingotion.com/terms-of-service/.

#include "bench.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include "core/engine.h"
#include "core/utils/profile.h"

namespace thespeon::cli {
namespace {

namespace fs = std::filesystem;

using Clock = std::chrono::steady_clock;
using Millis = std::chrono::duration<double, std::milli>;

double MillisSince(Clock::time_point start) {
  return Millis(Clock::now() - start).count();
}

std::size_t PeakResidentBytes() {
#if defined(__linux__)
  std::ifstream status("/proc/self/status");
  for (std::string line; std::getline(status, line);) {
    if (line.rfind("VmHWM:", 0) != 0) continue;
    return std::stoull(line.substr(6)) * 1024;
  }
#endif
  return 0;
}

double ProcessCpuSeconds() {
#ifdef _WIN32
  FILETIME creation, exit, kernel, user;
  GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
  const auto seconds = [](const FILETIME& time) {
    return static_cast<double>((static_cast<std::uint64_t>(time.dwHighDateTime)
                                << 32) |
                               time.dwLowDateTime) /
           1e7;
  };
  return seconds(kernel) + seconds(user);
#else
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const auto seconds = [](const timeval& time) {
    return static_cast<double>(time.tv_sec) +
           static_cast<double>(time.tv_usec) / 1e6;
  };
  return seconds(usage.ru_utime) + seconds(usage.ru_stime);
#endif
}

double Median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2;
  return values.size() % 2 == 1 ? values[middle]
                                : (values[middle - 1] + values[middle]) / 2;
}

struct StreamTiming {
  double first_audio_ms = 0;
  double total_ms = 0;
  std::size_t samples = 0;
  std::vector<double> gaps;
};

StreamTiming MeasureStream(Engine& engine, const ThespeonInput& input) {
  StreamTiming timing;
  bool first = true;
  const auto start = Clock::now();
  auto previous = start;
  engine.SynthesizeWithCallbacks(input, [&](metagraph::CallbackPacket packet) {
    if (packet.type != metagraph::CallbackType::Audio) return;
    const auto* samples = std::get_if<std::vector<float>>(&packet.payload);
    if (samples == nullptr || samples->empty()) return;
    const auto now = Clock::now();
    if (first) {
      timing.first_audio_ms = Millis(now - start).count();
      first = false;
    } else {
      timing.gaps.push_back(Millis(now - previous).count());
    }
    previous = now;
    timing.samples += samples->size();
  });
  timing.total_ms = MillisSince(start);
  THESPEON_PROFILE_FRAME("synthesis");
  return timing;
}

std::ostream& Row(const char* label) {
  return std::cerr << "  " << std::left << std::setw(30) << label << std::right;
}

void ReportMillis(const char* label, double value) {
  Row(label) << std::fixed << std::setprecision(1) << std::setw(10) << value
             << " ms\n";
}

void ReportRun(const StreamTiming& timing) {
  ReportMillis("time-to-first-audio", timing.first_audio_ms);
  ReportMillis("total", timing.total_ms);
  Row("audio seconds") << std::fixed << std::setprecision(2) << std::setw(10)
                       << static_cast<double>(timing.samples) / 44100.0
                       << " s\n";
  if (timing.gaps.empty()) return;
  auto sorted = timing.gaps;
  std::sort(sorted.begin(), sorted.end());
  const auto sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
  ReportMillis("inter-chunk mean", sum / static_cast<double>(sorted.size()));
  ReportMillis("inter-chunk median", sorted[sorted.size() / 2]);
  ReportMillis("inter-chunk max", sorted.back());
  Row("chunks") << std::setw(10) << timing.gaps.size() + 1 << '\n';
}

struct SweepResult {
  EngineOptions config;
  double preload_ms = 0;
  double first_audio_ms = 0;
  double total_ms = 0;
  double cpu_ms = 0;
};

std::string Flags(const EngineOptions& config) {
  std::ostringstream flags;
  flags << "--threads " << config.intra_op_threads;
  if (config.allow_spinning) flags << " --spinning";
  if (config.low_memory) flags << " --low-memory";
  return flags.str();
}

std::string AbiFlags(const EngineOptions& config) {
  std::string flags;
  if (config.allow_spinning) flags = "THESPEON_INFERENCE_SPINNING";
  if (config.low_memory)
    flags += (flags.empty() ? "" : " | ") +
             std::string("THESPEON_INFERENCE_LOW_MEMORY");
  return flags.empty() ? "0" : flags;
}

SweepResult Measure(const fs::path& data_directory, const ThespeonInput& input,
                    int repeats, const EngineOptions& config) {
  std::cerr << "\n=== " << Flags(config) << " ===\n";
  SetEngineOptions(config);
  Engine engine(data_directory);
  SweepResult result{config};
  const auto start = Clock::now();
  engine.Preload(input, true);
  result.preload_ms = MillisSince(start);
  ReportMillis("Preload with warmup", result.preload_ms);
  std::vector<double> first_audio;
  std::vector<double> total;
  std::vector<double> cpu;
  for (int run = 0; run < repeats; ++run) {
    std::cerr << "run " << run + 1 << " of " << repeats << '\n';
    const auto cpu_start = ProcessCpuSeconds();
    const auto timing = MeasureStream(engine, input);
    cpu.push_back((ProcessCpuSeconds() - cpu_start) * 1000.0);
    ReportRun(timing);
    ReportMillis("CPU", cpu.back());
    first_audio.push_back(timing.first_audio_ms);
    total.push_back(timing.total_ms);
  }
  result.first_audio_ms = Median(first_audio);
  result.total_ms = Median(total);
  result.cpu_ms = Median(cpu);
  return result;
}

void ReportSweep(const std::vector<SweepResult>& results) {
  std::cerr << "\n=== Summary, medians over the runs ===\n"
            << std::left << std::setw(40) << "  options" << std::right
            << std::setw(10) << "preload" << std::setw(13) << "first audio"
            << std::setw(10) << "total" << std::setw(10) << "CPU" << '\n';
  for (const auto& result : results) {
    std::cerr << "  " << std::left << std::setw(38) << Flags(result.config)
              << std::right << std::fixed << std::setprecision(0)
              << std::setw(7) << result.preload_ms << " ms" << std::setw(10)
              << result.first_audio_ms << " ms" << std::setw(7)
              << result.total_ms << " ms" << std::setw(7) << result.cpu_ms
              << " ms\n";
  }
}

}  // namespace

void RunSweep(const fs::path& data_directory, const ThespeonInput& input,
              int repeats) {
  constexpr double kTolerance = 1.10;
  std::vector<int> thread_counts{0};
  const int cores = static_cast<int>(std::thread::hardware_concurrency());
  for (int threads = 1; threads <= std::max(cores, 1); threads *= 2)
    thread_counts.push_back(threads);

  std::vector<SweepResult> results;
  for (const bool spinning : {false, true}) {
    for (const int threads : thread_counts) {
      EngineOptions config;
      config.intra_op_threads = threads;
      config.allow_spinning = spinning;
      results.push_back(Measure(data_directory, input, repeats, config));
    }
  }

  const auto fastest = *std::min_element(
      results.begin(), results.end(),
      [](const auto& a, const auto& b) { return a.total_ms < b.total_ms; });
  SweepResult best = fastest;
  for (const auto& result : results) {
    if (result.total_ms <= fastest.total_ms * kTolerance &&
        result.cpu_ms < best.cpu_ms)
      best = result;
  }

  auto low_memory = best.config;
  low_memory.low_memory = true;
  results.push_back(Measure(data_directory, input, repeats, low_memory));
  if (results.back().total_ms <= fastest.total_ms * kTolerance)
    best = results.back();
  SetEngineOptions({});

  ReportSweep(results);
  const auto line = [](const char* label, const SweepResult& result) {
    std::cerr << "  " << std::left << std::setw(14) << label << std::setw(38)
              << Flags(result.config) << std::right << std::fixed
              << std::setprecision(0) << result.total_ms << " ms total, "
              << result.cpu_ms << " ms CPU\n";
  };
  std::cerr << "\n=== Recommendation ===\n";
  line("Fastest:", fastest);
  line("Recommended:", best);
  std::cerr << "  Recommended is the least CPU within 10% of the fastest "
               "total, with low\n  memory if that stays within 10% too. Its "
               "memory saving is not measured\n  here.\n"
            << "  C ABI: thespeon_set_inference_options("
            << best.config.intra_op_threads << ", " << AbiFlags(best.config)
            << ")\n\n";
}

void RunBenchmark(const fs::path& data_directory, const ThespeonInput& input,
                  int repeats) {
  std::cerr << "\n=== Cold engine, no preload ===\n";
  {
    auto start = Clock::now();
    Engine engine(data_directory);
    ReportMillis("Engine construction", MillisSince(start));
    for (int run = 0; run < repeats; ++run) {
      std::cerr << "run " << run + 1 << " of " << repeats << '\n';
      ReportRun(MeasureStream(engine, input));
    }
  }

  for (const bool warmup : {false, true}) {
    std::cerr << "\n=== Preloaded engine, warmup=" << (warmup ? "on" : "off")
              << " ===\n";
    Engine engine(data_directory);
    const auto start = Clock::now();
    engine.Preload(input, warmup);
    ReportMillis("Preload", MillisSince(start));
    Row("resident models") << std::setw(10)
                           << engine.ResidentBytes() / 1024 / 1024 << " MB\n";
    for (int run = 0; run < repeats; ++run) {
      std::cerr << "run " << run + 1 << " of " << repeats << '\n';
      ReportRun(MeasureStream(engine, input));
      Row("resident after run") << std::setw(10)
                                << engine.ResidentBytes() / 1024 / 1024
                                << " MB\n";
    }
  }

  if (const auto peak = PeakResidentBytes(); peak != 0)
    Row("peak RSS") << std::setw(10) << peak / 1024 / 1024 << " MB\n";
  std::cerr << '\n';
}

}  // namespace thespeon::cli
