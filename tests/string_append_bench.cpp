#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <neixx/strings/string_util.h>

namespace {

using Clock = std::chrono::high_resolution_clock;

struct BenchResult {
  const char *name = "";
  std::uint64_t iterations = 0;
  std::uint64_t elapsed_us = 0;
  std::size_t output_size = 0;
  std::uint64_t checksum = 0;
};

std::uint64_t ComputeChecksum(const std::string &s) {
  std::uint64_t sum = 1469598103934665603ull;
  for (unsigned char ch : s) {
    sum ^= static_cast<std::uint64_t>(ch);
    sum *= 1099511628211ull;
  }
  return sum;
}

BenchResult BenchStringAppendF(std::uint64_t iterations) {
  std::string out;
  out.reserve(static_cast<std::size_t>(iterations) * 48u);

  const auto begin = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    nei::StringAppendF(
        &out, "{\"id\":%llu,\"ok\":%d,\"name\":\"user-%llu\"}", i, (i & 1ull) ? 1 : 0, i);
  }
  const auto end = Clock::now();

  BenchResult result;
  result.name = "StringAppendF";
  result.iterations = iterations;
  result.elapsed_us =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
  result.output_size = out.size();
  result.checksum = ComputeChecksum(out);
  return result;
}

BenchResult BenchPrintfPlusAppend(std::uint64_t iterations) {
  std::string out;
  out.reserve(static_cast<std::size_t>(iterations) * 48u);

  const auto begin = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    out += nei::StringPrintf("{\"id\":%llu,\"ok\":%d,\"name\":\"user-%llu\"}", i, (i & 1ull) ? 1 : 0, i);
  }
  const auto end = Clock::now();

  BenchResult result;
  result.name = "StringPrintf + operator+=";
  result.iterations = iterations;
  result.elapsed_us =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
  result.output_size = out.size();
  result.checksum = ComputeChecksum(out);
  return result;
}

BenchResult BenchStringStream(std::uint64_t iterations) {
  std::stringstream ss;

  const auto begin = Clock::now();
  for (std::uint64_t i = 0; i < iterations; ++i) {
    ss << "{\"id\":" << i << ",\"ok\":" << ((i & 1ull) ? 1 : 0) << ",\"name\":\"user-" << i << "\"}";
  }
  const auto end = Clock::now();

  const std::string out = ss.str();

  BenchResult result;
  result.name = "std::stringstream";
  result.iterations = iterations;
  result.elapsed_us =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
  result.output_size = out.size();
  result.checksum = ComputeChecksum(out);
  return result;
}

void PrintResult(const BenchResult &result) {
  const double us_per_op = result.iterations == 0 ? 0.0 : static_cast<double>(result.elapsed_us) / result.iterations;
  const double ops_per_sec = us_per_op <= 0.0 ? 0.0 : 1000000.0 / us_per_op;

  std::cout << result.name << "\n";
  std::cout << "  iterations : " << result.iterations << "\n";
  std::cout << "  elapsed_us : " << result.elapsed_us << "\n";
  std::cout << "  us/op      : " << std::fixed << std::setprecision(3) << us_per_op << "\n";
  std::cout << "  ops/sec    : " << std::fixed << std::setprecision(0) << ops_per_sec << "\n";
  std::cout << "  out.size   : " << result.output_size << "\n";
  std::cout << "  checksum   : " << result.checksum << "\n\n";
}

} // namespace

int main(int argc, char **argv) {
  std::uint64_t iterations = 100000;
  if (argc > 1) {
    const long long parsed = std::atoll(argv[1]);
    if (parsed > 0) {
      iterations = static_cast<std::uint64_t>(parsed);
    }
  }

  std::cout << "String Append Benchmark (JSON Fragments)\n";
  std::cout << "=======================================\n";
  std::cout << "iterations: " << iterations << "\n\n";

  const BenchResult appendf = BenchStringAppendF(iterations);
  const BenchResult printf_append = BenchPrintfPlusAppend(iterations);
  const BenchResult stringstream_append = BenchStringStream(iterations);

  PrintResult(appendf);
  PrintResult(printf_append);
  PrintResult(stringstream_append);

  if (appendf.output_size != printf_append.output_size || appendf.checksum != printf_append.checksum
      || appendf.output_size != stringstream_append.output_size || appendf.checksum != stringstream_append.checksum) {
    std::cerr << "ERROR: outputs differ between implementations\n";
    return 2;
  }

  const double speedup_vs_printf = appendf.elapsed_us == 0
                                       ? 0.0
                                       : static_cast<double>(printf_append.elapsed_us)
                                             / static_cast<double>(appendf.elapsed_us);
  const double speedup_vs_sstream = appendf.elapsed_us == 0
                                        ? 0.0
                                        : static_cast<double>(stringstream_append.elapsed_us)
                                              / static_cast<double>(appendf.elapsed_us);

  std::cout << "speedup (StringAppendF vs StringPrintf+operator+=): " << std::fixed << std::setprecision(2)
            << speedup_vs_printf << "x\n";
  std::cout << "speedup (StringAppendF vs std::stringstream): " << std::fixed << std::setprecision(2)
            << speedup_vs_sstream << "x\n";

  return 0;
}
