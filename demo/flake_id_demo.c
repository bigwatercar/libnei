#include <nei/utils/flake_id.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <pthread.h>
#endif

#define DEMO_THREADS 8
#define IDS_PER_THREAD 50000

typedef struct demo_thread_ctx_st {
  uint64_t *buffer;
  size_t begin;
  size_t count;
} demo_thread_ctx_st;

#if defined(_WIN32)
DWORD WINAPI demo_thread_proc(LPVOID arg) {
  demo_thread_ctx_st *ctx = (demo_thread_ctx_st *)arg;
  size_t i;
  for (i = 0; i < ctx->count; ++i) {
    ctx->buffer[ctx->begin + i] = nei_flake_next_id();
  }
  return 0;
}
#else
void *demo_thread_proc(void *arg) {
  demo_thread_ctx_st *ctx = (demo_thread_ctx_st *)arg;
  size_t i;
  for (i = 0; i < ctx->count; ++i) {
    ctx->buffer[ctx->begin + i] = nei_flake_next_id();
  }
  return NULL;
}
#endif

static int cmp_u64(const void *a, const void *b) {
  const uint64_t ua = *(const uint64_t *)a;
  const uint64_t ub = *(const uint64_t *)b;
  if (ua < ub) {
    return -1;
  }
  if (ua > ub) {
    return 1;
  }
  return 0;
}

int main(void) {
  const size_t total = (size_t)DEMO_THREADS * (size_t)IDS_PER_THREAD;
  uint64_t *ids = (uint64_t *)malloc(total * sizeof(uint64_t));
  demo_thread_ctx_st ctx[DEMO_THREADS];
  size_t i;
  size_t dup_count = 0;

#if defined(_WIN32)
  HANDLE threads[DEMO_THREADS];
#else
  pthread_t threads[DEMO_THREADS];
#endif

  if (ids == NULL) {
    fprintf(stderr, "allocation failed\n");
    return 1;
  }

  for (i = 0; i < DEMO_THREADS; ++i) {
    ctx[i].buffer = ids;
    ctx[i].begin = i * (size_t)IDS_PER_THREAD;
    ctx[i].count = IDS_PER_THREAD;
#if defined(_WIN32)
    threads[i] = CreateThread(NULL, 0, demo_thread_proc, &ctx[i], 0, NULL);
    if (threads[i] == NULL) {
      fprintf(stderr, "CreateThread failed\n");
      free(ids);
      return 2;
    }
#else
    if (pthread_create(&threads[i], NULL, demo_thread_proc, &ctx[i]) != 0) {
      fprintf(stderr, "pthread_create failed\n");
      free(ids);
      return 2;
    }
#endif
  }

#if defined(_WIN32)
  WaitForMultipleObjects(DEMO_THREADS, threads, TRUE, INFINITE);
  for (i = 0; i < DEMO_THREADS; ++i) {
    CloseHandle(threads[i]);
  }
#else
  for (i = 0; i < DEMO_THREADS; ++i) {
    pthread_join(threads[i], NULL);
  }
#endif

  qsort(ids, total, sizeof(uint64_t), cmp_u64);
  for (i = 1; i < total; ++i) {
    if (ids[i] == ids[i - 1]) {
      ++dup_count;
    }
  }

  printf("flake_id demo\n");
  printf("  threads          : %d\n", DEMO_THREADS);
  printf("  ids/thread       : %d\n", IDS_PER_THREAD);
  printf("  total ids        : %zu\n", total);
  printf("  duplicates       : %zu\n", dup_count);
  printf("  first id         : %" PRIu64 "\n", ids[0]);
  printf("  last id          : %" PRIu64 "\n", ids[total - 1]);

  free(ids);
  return dup_count == 0 ? 0 : 3;
}
