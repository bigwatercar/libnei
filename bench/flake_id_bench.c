#include <nei/utils/flake_id.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#define BENCH_THREADS 8
#define IDS_PER_THREAD 1000000

#if defined(_WIN32)
static uint64_t monotonic_ns_now(void) {
  static LARGE_INTEGER freq;
  static volatile LONG inited = 0;
  LARGE_INTEGER now;

  if (inited == 0 && InterlockedCompareExchange(&inited, 1, 0) == 0) {
    QueryPerformanceFrequency(&freq);
  }
  QueryPerformanceCounter(&now);
  return (uint64_t)((now.QuadPart * 1000000000ULL) / (uint64_t)freq.QuadPart);
}
#else
static uint64_t monotonic_ns_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

typedef struct bench_thread_ctx_st {
  uint64_t rounds;
  uint64_t checksum;
} bench_thread_ctx_st;

#if defined(_WIN32)
DWORD WINAPI bench_thread_proc(LPVOID arg) {
  bench_thread_ctx_st *ctx = (bench_thread_ctx_st *)arg;
  uint64_t i;
  uint64_t sum = 0;
  for (i = 0; i < ctx->rounds; ++i) {
    sum ^= nei_flake_next_id();
  }
  ctx->checksum = sum;
  return 0;
}
#else
void *bench_thread_proc(void *arg) {
  bench_thread_ctx_st *ctx = (bench_thread_ctx_st *)arg;
  uint64_t i;
  uint64_t sum = 0;
  for (i = 0; i < ctx->rounds; ++i) {
    sum ^= nei_flake_next_id();
  }
  ctx->checksum = sum;
  return NULL;
}
#endif

int main(void) {
  bench_thread_ctx_st ctx[BENCH_THREADS];
  uint64_t t0;
  uint64_t t1;
  uint64_t elapsed_ns;
  uint64_t total_ids;
  uint64_t checksum = 0;
  int i;

#if defined(_WIN32)
  HANDLE threads[BENCH_THREADS];
#else
  pthread_t threads[BENCH_THREADS];
#endif

  for (i = 0; i < BENCH_THREADS; ++i) {
    ctx[i].rounds = IDS_PER_THREAD;
    ctx[i].checksum = 0;
  }

  t0 = monotonic_ns_now();

  for (i = 0; i < BENCH_THREADS; ++i) {
#if defined(_WIN32)
    threads[i] = CreateThread(NULL, 0, bench_thread_proc, &ctx[i], 0, NULL);
    if (threads[i] == NULL) {
      fprintf(stderr, "CreateThread failed\n");
      return 2;
    }
#else
    if (pthread_create(&threads[i], NULL, bench_thread_proc, &ctx[i]) != 0) {
      fprintf(stderr, "pthread_create failed\n");
      return 2;
    }
#endif
  }

#if defined(_WIN32)
  WaitForMultipleObjects(BENCH_THREADS, threads, TRUE, INFINITE);
  for (i = 0; i < BENCH_THREADS; ++i) {
    CloseHandle(threads[i]);
  }
#else
  for (i = 0; i < BENCH_THREADS; ++i) {
    pthread_join(threads[i], NULL);
  }
#endif

  t1 = monotonic_ns_now();
  elapsed_ns = t1 - t0;

  for (i = 0; i < BENCH_THREADS; ++i) {
    checksum ^= ctx[i].checksum;
  }

  total_ids = (uint64_t)BENCH_THREADS * (uint64_t)IDS_PER_THREAD;

  printf("flake_id bench\n");
  printf("  threads          : %d\n", BENCH_THREADS);
  printf("  ids/thread       : %d\n", IDS_PER_THREAD);
  printf("  total ids        : %" PRIu64 "\n", total_ids);
  printf("  elapsed ms       : %.3f\n", (double)elapsed_ns / 1000000.0);
  printf("  ids/sec          : %.0f\n", (double)total_ids * 1000000000.0 / (double)elapsed_ns);
  printf("  checksum         : %" PRIu64 "\n", checksum);

  return 0;
}
