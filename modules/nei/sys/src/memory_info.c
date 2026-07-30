#include <nei/sys/memory_info.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <unistd.h>
#else /* Linux / other Unix */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#endif

uint64_t nei_get_total_physical_memory(void) {
#ifdef _WIN32
  MEMORYSTATUSEX mem;
  mem.dwLength = sizeof(mem);
  if (!GlobalMemoryStatusEx(&mem)) {
    return 0;
  }
  return (uint64_t)mem.ullTotalPhys;
#elif defined(__APPLE__)
  uint64_t memsize = 0;
  size_t len = sizeof(memsize);
  if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) != 0) {
    return 0;
  }
  return memsize;
#else
  long pages = sysconf(_SC_PHYS_PAGES);
  long page_size = sysconf(_SC_PAGE_SIZE);
  if (pages < 0 || page_size < 0) {
    return 0;
  }
  return (uint64_t)pages * (uint64_t)page_size;
#endif
}

uint64_t nei_get_available_physical_memory(void) {
#ifdef _WIN32
  MEMORYSTATUSEX mem;
  mem.dwLength = sizeof(mem);
  if (!GlobalMemoryStatusEx(&mem)) {
    return 0;
  }
  return (uint64_t)mem.ullAvailPhys;
#elif defined(__APPLE__)
  /*
   * Use host_statistics64 to get free + inactive memory.
   * This gives a reasonable estimate of memory that can be made
   * available to applications without swapping.
   */
  mach_port_t host = mach_host_self();
  vm_statistics64_data_t vm_stat;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(host, HOST_VM_INFO64, (host_info_t)&vm_stat, &count) != KERN_SUCCESS) {
    return 0;
  }
  int page_size = nei_get_page_size();
  if (page_size < 0) {
    return 0;
  }
  uint64_t free_mem = (uint64_t)vm_stat.free_count * (uint64_t)page_size;
  uint64_t inactive_mem = (uint64_t)vm_stat.inactive_count * (uint64_t)page_size;
  return free_mem + inactive_mem;
#else
  /*
   * Read MemAvailable from /proc/meminfo.
   * This field is available since Linux 3.14 and provides an estimate
   * of how much memory is available for starting new applications.
   */
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == NULL) {
    /* Fallback: use sysconf */
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages < 0 || page_size < 0) {
      return 0;
    }
    return (uint64_t)pages * (uint64_t)page_size;
  }

  uint64_t available = 0;
  char line[256];
  while (fgets(line, sizeof(line), f) != NULL) {
    uint64_t kb = 0;
    if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1) {
      available = kb * 1024;
      break;
    }
    /* Fallback: try MemFree + Buffers + Cached if MemAvailable not found */
    /* We only try this if the first pass didn't find MemAvailable. */
  }

  if (available == 0) {
    /* Rewind and try MemFree + Buffers + Cached */
    rewind(f);
    uint64_t mem_free = 0, buffers = 0, cached = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
      uint64_t kb = 0;
      if (sscanf(line, "MemFree: %lu kB", &kb) == 1) {
        mem_free = kb;
      } else if (sscanf(line, "Buffers: %lu kB", &kb) == 1) {
        buffers = kb;
      } else if (sscanf(line, "Cached: %lu kB", &kb) == 1) {
        cached = kb;
      }
    }
    available = (mem_free + buffers + cached) * 1024;
  }

  fclose(f);
  return available;
#endif
}

int nei_get_page_size(void) {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return (int)si.dwPageSize;
#elif defined(__APPLE__)
  int page_size = 0;
  size_t len = sizeof(page_size);
  if (sysctlbyname("vm.pagesize", &page_size, &len, NULL, 0) != 0) {
    /* Fallback: hw.pagesize */
    if (sysctlbyname("hw.pagesize", &page_size, &len, NULL, 0) != 0) {
      return -1;
    }
  }
  return page_size;
#else
  long page_size = sysconf(_SC_PAGE_SIZE);
  if (page_size < 0) {
    return -1;
  }
  return (int)page_size;
#endif
}
