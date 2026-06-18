// neixx AsyncFile throughput benchmark
//
// Measures sequential write and read throughput at various chunk sizes.
//
// Build: cmake --build build/<config> --target async_file_bench --config Release
// Run:   build/<config>/bench/Release/async_file_bench.exe

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <neixx/common/location.h>
#include <neixx/io/async_file.h>
#include <neixx/io/io_buffer.h>
#include <neixx/memory/ref_counted.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/message_loop/message_pump_type.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace {

using Clock = std::chrono::high_resolution_clock;

std::filesystem::path MakeTempPath() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto seed = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
  std::filesystem::path p = std::filesystem::temp_directory_path();
  p /= "nei_async_file_bench_" + std::to_string(seed) + ".bin";
  return p;
}

std::vector<std::uint8_t> MakeFill(std::size_t size, std::uint8_t salt) {
  std::vector<std::uint8_t> data(size);
  for (std::size_t i = 0; i < size; ++i)
    data[i] = static_cast<std::uint8_t>((i * 131u + salt) & 0xFFu);
  return data;
}

std::string FormatSize(std::size_t bytes) {
  if (bytes >= 1024 * 1024 * 1024)
    return std::to_string(bytes / (1024 * 1024 * 1024)) + " GB";
  if (bytes >= 1024 * 1024)
    return std::to_string(bytes / (1024 * 1024)) + " MB";
  if (bytes >= 1024)
    return std::to_string(bytes / 1024) + " KB";
  return std::to_string(bytes) + " B";
}

struct BenchEntry {
  const char* label = "";
  std::size_t chunk_size = 0;
  std::size_t total_bytes = 0;
  std::uint64_t elapsed_us = 0;
  double throughput_mbps = 0.0;
};

BenchEntry BenchWrite(nei::AsyncFile& file,
                      const nei::scoped_refptr<nei::TaskRunner>& bg,
                      const std::filesystem::path& path,
                      std::size_t chunk_size,
                      std::size_t total_bytes,
                      const std::vector<std::uint8_t>& fill) {
  const std::string path_str = path.u8string();
  const std::size_t nchunks = (total_bytes + chunk_size - 1) / chunk_size;

  nei::WaitableEvent ev(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};
  file.OpenAsync(path_str, nei::AsyncFile::OpenMode::kWriteOnly,
                 nei::AsyncFile::OpenDisposition::kCreateAlways, bg,
                 [&](bool s, nei::AsyncFile::Error e) {
                   ok.store(s && e.ok(), std::memory_order_release);
                   ev.Signal();
                 });
  ev.Wait();
  if (!ok.load(std::memory_order_acquire)) return {};

  // warm-up
  {
    nei::WaitableEvent w(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
    auto b = nei::scoped_refptr<nei::IOBuffer>(new nei::IOBufferWithSize(4096));
    std::memcpy(b->data(), fill.data(), 4096);
    file.WriteAsync(b, 4096, 0, [&](bool, std::size_t, nei::AsyncFile::Error) { w.Signal(); });
    w.Wait();
  }

  const std::size_t kMaxInFlight = 256;
  std::atomic<std::size_t> in_flight{0};
  std::atomic<std::size_t> done{0};
  nei::WaitableEvent all(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  const auto t0 = Clock::now();

  for (std::size_t i = 0; i < nchunks; ++i) {
    while (in_flight.load(std::memory_order_acquire) >= kMaxInFlight) {
      std::this_thread::yield();
    }
    in_flight.fetch_add(1, std::memory_order_acq_rel);

    const std::size_t off = i * chunk_size;
    const std::size_t sz = (std::min)(chunk_size, total_bytes - off);
    auto b = nei::scoped_refptr<nei::IOBuffer>(new nei::IOBufferWithSize(sz));
    // Copy from fill with wrap-around (fill may be smaller than chunk).
    std::size_t copied = 0;
    std::size_t src_off = off % fill.size();
    while (copied < sz) {
      const std::size_t part = (std::min)(sz - copied, fill.size() - src_off);
      std::memcpy(b->data() + copied, fill.data() + src_off, part);
      copied += part;
      src_off = 0;  // subsequent wraps start from fill[0]
    }
    file.WriteAsync(b, sz, off,
                    [&](bool, std::size_t, nei::AsyncFile::Error) {
                      in_flight.fetch_sub(1, std::memory_order_acq_rel);
                      if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == nchunks)
                        all.Signal();
                    });
  }

  all.Wait();
  const auto t1 = Clock::now();
  nei::WaitableEvent close_ev(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  file.Close([&]() { close_ev.Signal(); });
  close_ev.Wait();

  BenchEntry e;
  e.label = "sequential-write";
  e.chunk_size = chunk_size;
  e.total_bytes = total_bytes;
  e.elapsed_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  e.throughput_mbps = static_cast<double>(total_bytes) / (1024.0 * 1024.0) /
                      (static_cast<double>(e.elapsed_us) / 1'000'000.0);
  return e;
}

BenchEntry BenchRead(nei::AsyncFile& file,
                     const nei::scoped_refptr<nei::TaskRunner>& bg,
                     const std::filesystem::path& path,
                     std::size_t chunk_size,
                     std::size_t total_bytes) {
  const std::string path_str = path.u8string();
  const std::size_t nchunks = (total_bytes + chunk_size - 1) / chunk_size;

  nei::WaitableEvent ev(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  std::atomic<bool> ok{false};
  file.OpenAsync(path_str, nei::AsyncFile::OpenMode::kReadOnly,
                 nei::AsyncFile::OpenDisposition::kOpenExisting, bg,
                 [&](bool s, nei::AsyncFile::Error e) {
                   ok.store(s && e.ok(), std::memory_order_release);
                   ev.Signal();
                 });
  ev.Wait();
  if (!ok.load(std::memory_order_acquire)) return {};

  const std::size_t kMaxInFlight = 256;
  std::atomic<std::size_t> in_flight{0};
  std::atomic<std::size_t> done{0};
  nei::WaitableEvent all(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  const auto t0 = Clock::now();

  for (std::size_t i = 0; i < nchunks; ++i) {
    while (in_flight.load(std::memory_order_acquire) >= kMaxInFlight) {
      std::this_thread::yield();
    }
    in_flight.fetch_add(1, std::memory_order_acq_rel);

    const std::size_t off = i * chunk_size;
    const std::size_t sz = (std::min)(chunk_size, total_bytes - off);
    auto b = nei::scoped_refptr<nei::IOBuffer>(new nei::IOBufferWithSize(sz));
    file.ReadAsync(b, sz, off,
                   [&](bool, std::size_t, nei::AsyncFile::Error) {
                     in_flight.fetch_sub(1, std::memory_order_acq_rel);
                     if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == nchunks)
                       all.Signal();
                   });
  }

  all.Wait();
  const auto t1 = Clock::now();
  nei::WaitableEvent close_ev(nei::WaitableEvent::ResetPolicy::kAutomatic, false);
  file.Close([&]() { close_ev.Signal(); });
  close_ev.Wait();

  BenchEntry e;
  e.label = "sequential-read";
  e.chunk_size = chunk_size;
  e.total_bytes = total_bytes;
  e.elapsed_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  e.throughput_mbps = static_cast<double>(total_bytes) / (1024.0 * 1024.0) /
                      (static_cast<double>(e.elapsed_us) / 1'000'000.0);
  return e;
}

void PrintHeader() {
  std::cout << std::left
            << std::setw(18) << "Operation"
            << std::setw(12) << "Chunk"
            << std::setw(14) << "Total"
            << std::setw(12) << "Time(ms)"
            << std::setw(14) << "Throughput" << std::endl;
  std::cout << std::string(70, '-') << std::endl;
}

void PrintRow(const BenchEntry& e) {
  std::cout << std::left
            << std::setw(18) << e.label
            << std::setw(12) << FormatSize(e.chunk_size)
            << std::setw(14) << FormatSize(e.total_bytes)
            << std::setw(12) << std::fixed << std::setprecision(2)
            << (static_cast<double>(e.elapsed_us) / 1000.0)
            << std::setw(14) << std::fixed << std::setprecision(1)
            << e.throughput_mbps << " MB/s" << std::endl;
}
}  // namespace

int main() {
#if defined(_WIN32)
  const std::vector<std::size_t> kChunks = {
      4 * 1024, 16 * 1024, 64 * 1024, 256 * 1024,
      1 * 1024 * 1024, 4 * 1024 * 1024};
#else
  // POSIX: skip 4KB/16KB �?the single background thread serializes
  // all I/O, making 16384+ tiny ops impractically slow on WSL.
  const std::vector<std::size_t> kChunks = {
      64 * 1024, 256 * 1024, 1 * 1024 * 1024, 4 * 1024 * 1024};
#endif
  const std::size_t kTotal = 64 * 1024 * 1024;

  std::cout << "=== neixx AsyncFile Throughput Benchmark ===" << std::endl;
  std::cout << "Total per run: " << FormatSize(kTotal) << std::endl;
  std::cout << "Chunk sizes: ";
  for (std::size_t i = 0; i < kChunks.size(); ++i) {
    if (i) std::cout << ", ";
    std::cout << FormatSize(kChunks[i]);
  }
  std::cout << std::endl << std::endl;

  const std::filesystem::path base = MakeTempPath();
  std::cout << "Temp file: " << base.u8string() << std::endl;

  nei::Thread io{"async-bench-io"};
  nei::Thread bg{"async-bench-bg"};
  {
    nei::Thread::Options opts;
    opts.message_pump_type = nei::MessagePumpType::IO;
    io.StartWithOptions(opts);
    bg.Start();
  }
  auto io_r = io.GetTaskRunner();
  auto bg_r = bg.GetTaskRunner();

  const std::vector<std::uint8_t> fill = MakeFill(1 * 1024 * 1024, 42);
  std::vector<BenchEntry> results;
  results.reserve(kChunks.size() * 2);

  std::cout << "Running write benchmarks..." << std::flush;
  for (auto cs : kChunks) {
    auto f = nei::AsyncFile::Create(io_r);
    auto p = base.parent_path() /
             (base.stem().string() + "_w_" + std::to_string(cs) + ".bin");
    auto e = BenchWrite(*f, bg_r, p, cs, kTotal, fill);
    if (e.elapsed_us) results.push_back(e);
    std::cout << "." << std::flush;
    std::error_code ec;
    std::filesystem::remove(p, ec);
  }
  std::cout << " done" << std::endl;

  const auto rpath = base.parent_path() /
                     (base.stem().string() + "_r_src.bin");
  {
    auto wf = nei::AsyncFile::Create(io_r);
    (void)BenchWrite(*wf, bg_r, rpath, 256 * 1024, kTotal, fill);
  }

  std::cout << "Running read benchmarks..." << std::flush;
  for (auto cs : kChunks) {
    auto f = nei::AsyncFile::Create(io_r);
    auto e = BenchRead(*f, bg_r, rpath, cs, kTotal);
    if (e.elapsed_us) results.push_back(e);
    std::cout << "." << std::flush;
  }
  std::cout << " done" << std::endl;

  std::error_code ec;
  std::filesystem::remove(rpath, ec);
  std::filesystem::remove(base, ec);

  std::cout << std::endl;
  PrintHeader();
  for (const auto& e : results) PrintRow(e);
  std::cout << std::endl;

  double bw = 0, br = 0;
  std::size_t cw = 0, cr = 0;
  for (const auto& e : results) {
    if (e.label == std::string("sequential-write") && e.throughput_mbps > bw)
      { bw = e.throughput_mbps; cw = e.chunk_size; }
    if (e.label == std::string("sequential-read") && e.throughput_mbps > br)
      { br = e.throughput_mbps; cr = e.chunk_size; }
  }
  std::cout << "Best write: " << std::fixed << std::setprecision(1)
            << bw << " MB/s at " << FormatSize(cw) << " chunk" << std::endl;
  std::cout << "Best read:  " << br << " MB/s at " << FormatSize(cr)
            << " chunk" << std::endl;

  bg.Stop();
  io.Stop();
  return 0;
}
