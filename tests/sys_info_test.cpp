#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

/* =========================================================================
 * cpu_info
 * ========================================================================= */
#include <nei/sys/cpu_info.h>

TEST(SysCpuInfo, GetCpuCountReturnsPositive) {
    int count = nei_get_cpu_count();
    EXPECT_GT(count, 0);
}

TEST(SysCpuInfo, GetCpuPhysicalCountReturnsPositive) {
    int count = nei_get_cpu_physical_count();
    EXPECT_GT(count, 0);
    int logical = nei_get_cpu_count();
    EXPECT_LE(count, logical) << "Physical count should not exceed logical count";
}

TEST(SysCpuInfo, GetCpuArchReturnsNonEmpty) {
    char buf[64];
    int len = nei_get_cpu_arch(buf, sizeof(buf));
    EXPECT_GT(len, 0) << "Should return a non-empty architecture string";
    EXPECT_STREQ(buf, std::string(buf).c_str());  // proper null termination
}

TEST(SysCpuInfo, GetCpuArchSmallBuffer) {
    char buf[4];
    int len = nei_get_cpu_arch(buf, sizeof(buf));
    EXPECT_GT(len, 0) << "Should report required buffer size";
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0') << "Should be null-terminated";
}

TEST(SysCpuInfo, GetCpuArchNullBufferReturnsError) {
    int ret = nei_get_cpu_arch(nullptr, 64);
    EXPECT_LT(ret, 0);
}

TEST(SysCpuInfo, GetCpuArchZeroSizeReturnsError) {
    char buf[64];
    int ret = nei_get_cpu_arch(buf, 0);
    EXPECT_LT(ret, 0);
}

/* C++ wrappers */
TEST(SysCpuInfo, CppWrappersCompile) {
    int count = nei_get_cpu_count();
    EXPECT_GT(count, 0);
    int physical = nei_get_cpu_physical_count();
    EXPECT_GT(physical, 0);
    std::string arch = nei_get_cpu_arch();
    EXPECT_FALSE(arch.empty());
}

/* =========================================================================
 * memory_info
 * ========================================================================= */
#include <nei/sys/memory_info.h>

TEST(SysMemoryInfo, GetTotalPhysicalMemoryPositive) {
    uint64_t total = nei_get_total_physical_memory();
    EXPECT_GT(total, 0u) << "Total physical memory should be > 0";
}

TEST(SysMemoryInfo, GetAvailablePhysicalMemoryPositive) {
    uint64_t avail = nei_get_available_physical_memory();
    EXPECT_GT(avail, 0u) << "Available physical memory should be > 0";
}

TEST(SysMemoryInfo, AvailableNotExceedTotal) {
    uint64_t total = nei_get_total_physical_memory();
    uint64_t avail = nei_get_available_physical_memory();
    EXPECT_LE(avail, total) << "Available memory should not exceed total";
}

TEST(SysMemoryInfo, GetPageSizePositive) {
    int page_size = nei_get_page_size();
    EXPECT_GT(page_size, 0);
    /* Common page sizes are multiples of 4096. */
    EXPECT_EQ(page_size % 4096, 0);
}

/* C++ wrappers */
TEST(SysMemoryInfo, CppWrappersCompile) {
    uint64_t total = nei_get_total_physical_memory();
    EXPECT_GT(total, 0u);
    uint64_t avail = nei_get_available_physical_memory();
    EXPECT_GT(avail, 0u);
    int ps = nei_get_page_size();
    EXPECT_GT(ps, 0);
}

/* =========================================================================
 * os_info
 * ========================================================================= */
#include <nei/sys/os_info.h>

TEST(SysOsInfo, GetOsNameReturnsNonEmpty) {
    char buf[128];
    int len = nei_get_os_name(buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STRNE(buf, "");
}

TEST(SysOsInfo, GetOsNameSmallBuffer) {
    char buf[4];
    int len = nei_get_os_name(buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

TEST(SysOsInfo, GetOsNameNullBufferReturnsError) {
    EXPECT_LT(nei_get_os_name(nullptr, 128), 0);
}

TEST(SysOsInfo, GetOsVersionReturnsNonEmpty) {
    char buf[128];
    int len = nei_get_os_version(buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STRNE(buf, "");
}

TEST(SysOsInfo, GetKernelVersionReturnsNonEmpty) {
    char buf[128];
    int len = nei_get_kernel_version(buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STRNE(buf, "");
}

TEST(SysOsInfo, NullBufferReturnsError) {
    EXPECT_LT(nei_get_os_version(nullptr, 128), 0);
    EXPECT_LT(nei_get_kernel_version(nullptr, 128), 0);
}

/* C++ wrappers */
TEST(SysOsInfo, CppWrappersCompile) {
    std::string name = nei_get_os_name();
    EXPECT_FALSE(name.empty());
    std::string ver = nei_get_os_version();
    EXPECT_FALSE(ver.empty());
    std::string kver = nei_get_kernel_version();
    EXPECT_FALSE(kver.empty());
}

/* =========================================================================
 * host_info
 * ========================================================================= */
#include <nei/sys/host_info.h>

TEST(SysHostInfo, GetHostnameReturnsNonEmpty) {
    char buf[256];
    int len = nei_get_hostname(buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STRNE(buf, "");
}

TEST(SysHostInfo, GetHostnameSmallBuffer) {
    char buf[4];
    nei_get_hostname(buf, sizeof(buf));  // return value unused; test checks null-termination
    // Small buffer: the return value is the required size (or -1 on
    // POSIX). Either way the buffer must be safely null-terminated.
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

TEST(SysHostInfo, GetUsernameReturnsNonEmpty) {
    char buf[256];
    int len = nei_get_username(buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STRNE(buf, "");
}

TEST(SysHostInfo, GetHomeDirReturnsNonEmpty) {
    char buf[4096];
    int len = nei_get_home_dir(buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STRNE(buf, "");
}

TEST(SysHostInfo, GetTempDirReturnsNonEmpty) {
    char buf[4096];
    int len = nei_get_temp_dir(buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STRNE(buf, "");
}

TEST(SysHostInfo, NullBufferReturnsError) {
    EXPECT_LT(nei_get_hostname(nullptr, 256), 0);
    EXPECT_LT(nei_get_username(nullptr, 256), 0);
    EXPECT_LT(nei_get_home_dir(nullptr, 4096), 0);
    EXPECT_LT(nei_get_temp_dir(nullptr, 4096), 0);
}

/* C++ wrappers */
TEST(SysHostInfo, CppWrappersCompile) {
    std::string hn = nei_get_hostname();
    EXPECT_FALSE(hn.empty());
    std::string un = nei_get_username();
    EXPECT_FALSE(un.empty());
    std::string home = nei_get_home_dir();
    EXPECT_FALSE(home.empty());
    auto home_path = nei_get_home_dir_path();
    EXPECT_FALSE(home_path.empty());
    std::string tmp = nei_get_temp_dir();
    EXPECT_FALSE(tmp.empty());
    auto tmp_path = nei_get_temp_dir_path();
    EXPECT_FALSE(tmp_path.empty());
}

/* =========================================================================
 * process_info
 * ========================================================================= */
#include <nei/sys/process_info.h>

TEST(SysProcessInfo, GetPidReturnsPositive) {
    int64_t pid = nei_get_pid();
    EXPECT_GT(pid, 0);
}

TEST(SysProcessInfo, GetParentPidReturnsPositive) {
    int64_t ppid = nei_get_parent_pid();
    EXPECT_GT(ppid, 0);
}

TEST(SysProcessInfo, PidAndParentPidDiffer) {
    int64_t pid = nei_get_pid();
    int64_t ppid = nei_get_parent_pid();
    EXPECT_NE(pid, ppid) << "Parent PID should differ from current PID";
}

TEST(SysProcessInfo, GetProcessUptimeMsReturnsPositive) {
    int64_t uptime = nei_get_process_uptime_ms();
    EXPECT_GT(uptime, 0) << "Process uptime should be > 0 ms";
}

TEST(SysProcessInfo, GetCurrentDirectoryReturnsNonEmpty) {
    char buf[4096];
    int len = nei_get_current_directory(buf, sizeof(buf));
    EXPECT_GT(len, 0);
    EXPECT_STRNE(buf, "");
}

TEST(SysProcessInfo, NullBufferReturnsError) {
    EXPECT_LT(nei_get_current_directory(nullptr, 4096), 0);
}

TEST(SysProcessInfo, GetProcessMemoryInfoReturnsValid) {
    nei_process_memory_info_st info;
    int ret = nei_get_process_memory_info(&info);
    ASSERT_EQ(ret, 0);
    EXPECT_GT(info.virtual_bytes, 0u) << "Virtual memory should be > 0";
    EXPECT_GT(info.resident_bytes, 0u) << "Resident memory should be > 0";
    EXPECT_GE(info.peak_virtual_bytes, info.virtual_bytes)
        << "Peak virtual >= current";
    EXPECT_GE(info.peak_resident_bytes, info.resident_bytes)
        << "Peak resident >= current";
}

TEST(SysProcessInfo, GetProcessMemoryInfoNullReturnsError) {
    EXPECT_LT(nei_get_process_memory_info(nullptr), 0);
}

/* C++ wrappers */
TEST(SysProcessInfo, CppWrappersCompile) {
    int64_t pid = nei_get_pid();
    EXPECT_GT(pid, 0);
    int64_t ppid = nei_get_parent_pid();
    EXPECT_GT(ppid, 0);
    int64_t uptime = nei_get_process_uptime_ms();
    EXPECT_GT(uptime, 0);
    std::string cwd = nei_get_current_directory();
    EXPECT_FALSE(cwd.empty());
    auto cwd_path = nei_get_current_directory_path();
    EXPECT_FALSE(cwd_path.empty());
}

/* =========================================================================
 * disk_info
 * ========================================================================= */
#include <nei/sys/disk_info.h>

TEST(SysDiskInfo, GetDiskTotalSpacePositive) {
    uint64_t total = nei_get_disk_total_space(nullptr);
    EXPECT_GT(total, 0u) << "Disk total space should be > 0";
}

TEST(SysDiskInfo, GetDiskFreeSpacePositive) {
    uint64_t free = nei_get_disk_free_space(nullptr);
    EXPECT_GT(free, 0u) << "Disk free space should be > 0";
}

TEST(SysDiskInfo, GetDiskAvailableSpacePositive) {
    uint64_t avail = nei_get_disk_available_space(nullptr);
    EXPECT_GT(avail, 0u) << "Disk available space should be > 0";
}

TEST(SysDiskInfo, FreeNotExceedTotal) {
    uint64_t total = nei_get_disk_total_space(nullptr);
    uint64_t free = nei_get_disk_free_space(nullptr);
    uint64_t avail = nei_get_disk_available_space(nullptr);
    EXPECT_LE(free, total);
    EXPECT_LE(avail, total);
}

TEST(SysDiskInfo, QueryWithPath) {
    uint64_t total = nei_get_disk_total_space(".");
    EXPECT_GT(total, 0u);
}

/* C++ wrappers */
TEST(SysDiskInfo, CppWrappersCompile) {
    uint64_t total = nei_get_disk_total_space(nullptr);
    EXPECT_GT(total, 0u);
    uint64_t free = nei_get_disk_free_space(nullptr);
    EXPECT_GT(free, 0u);
    uint64_t avail = nei_get_disk_available_space(nullptr);
    EXPECT_GT(avail, 0u);
    uint64_t total2 = nei_get_disk_total_space(".");
    EXPECT_GT(total2, 0u);
}
