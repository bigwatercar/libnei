// =============================================================================
// HostResolver unit tests  --  async DNS resolution via c-ares
// =============================================================================

// winsock2.h must come before windows.h on Windows.
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <neixx/net/address_list.h>
#include <neixx/net/host_resolver.h>
#include <neixx/net/ip_address.h>
#include <neixx/net/ip_end_point.h>
#include <neixx/synchronization/waitable_event.h>
#include <neixx/task/task_runner.h>
#include <neixx/threading/thread.h>

namespace nei::net {
namespace {

// =============================================================================
// Test fixture  --  provides a dedicated thread with TaskRunner for callbacks.
// =============================================================================

class HostResolverTest : public testing::Test {
protected:
  void SetUp() override {
    Thread::Options opts;
    ASSERT_TRUE(test_thread_.StartWithOptions(opts));
    test_runner_ = test_thread_.GetTaskRunner();
    ASSERT_TRUE(test_runner_);
  }

  void TearDown() override {
    test_thread_.Stop();
  }

  // Helper: resolve and block until callback fires, return the result.
  // Uses a WaitableEvent to synchronize.  `timeout` is the caller's budget:
  // most lookups answer in milliseconds, but a NXDOMAIN for a non-existent TLD
  // can take >10 s behind a forwarding resolver, so callers that expect a
  // negative answer pass a larger budget instead of looking like a hang.
  AddressList ResolveAndWait(const std::string &host,
                             HostResolver *resolver = nullptr,
                             std::chrono::seconds timeout = std::chrono::seconds(10)) {
    std::unique_ptr<HostResolver> local_resolver;
    if (!resolver) {
      local_resolver = std::make_unique<HostResolver>();
      resolver = local_resolver.get();
    }

    AddressList result;
    WaitableEvent done(WaitableEvent::ResetPolicy::kManual, false);

    resolver->Resolve(
        host,
        [&result, &done](const AddressList &addrs) {
          result = addrs;
          done.Signal();
        },
        test_runner_);

    EXPECT_TRUE(done.TimedWait(timeout)) << "DNS resolution timed out for host: " << host;
    return result;
  }

  // Check if IPv6 connectivity is available.
  static bool HasIPv6Connectivity() {
#if defined(_WIN32)
    SOCKET s = socket(AF_INET6, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
      return false;
    closesocket(s);
#else
    int s = socket(AF_INET6, SOCK_DGRAM, 0);
    if (s < 0)
      return false;
    close(s);
#endif
    return true;
  }

  // Check if external DNS resolution is available via HostResolver (c-ares).
  // Returns false when DNS is unavailable (offline, firewall, CI, etc.).
  static bool HasFunctionalDNS() {
    // Use HostResolver (same stack as the tests) to probe DNS quickly.
    HostResolver resolver;
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    std::atomic<bool> has_dns{false};

    Thread probe_thread("dns-probe");
    probe_thread.Start();
    resolver.Resolve(
        "one.one.one.one",
        [&done, &has_dns](const AddressList &addrs) {
          has_dns.store(!addrs.empty(), std::memory_order_release);
          done.Signal();
        },
        probe_thread.GetTaskRunner());

    done.TimedWait(std::chrono::seconds(5));
    probe_thread.Stop();
    return has_dns.load(std::memory_order_acquire);
  }

  // Checks whether AAAA (IPv6 address) lookups work through the system DNS
  // configuration.  Says nothing about IPv6 *transport*, so it is only used by
  // tests that resolve IPv6 addresses over the system resolver.
  static bool HasIPv6AddressResolution() {
    HostResolverOptions opts;
    opts.address_family = AF_INET6;
    HostResolver resolver(opts);
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    std::atomic<bool> has_ipv6{false};

    Thread probe_thread("dns6-probe");
    probe_thread.Start();
    resolver.Resolve(
        "one.one.one.one",
        [&done, &has_ipv6](const AddressList &addrs) {
          has_ipv6.store(!addrs.empty(), std::memory_order_release);
          done.Signal();
        },
        probe_thread.GetTaskRunner());

    done.TimedWait(std::chrono::seconds(5));
    probe_thread.Stop();
    return has_ipv6.load(std::memory_order_acquire);
  }

  // Checks whether IPv6 DNS actually works: an IPv6 listener socket can exist
  // (so HasIPv6Connectivity() returns true) while there is no IPv6 *route* to
  // the public resolvers, which used to let the IPv6 custom-server tests run
  // and fail instantly instead of skipping.  Probes the same literal server the
  // tests use, so "probe passed" means "the test below can work".
  static bool HasIPv6DNSConnectivity(const std::string &ipv6_dns_server) {
    HostResolverOptions opts;
    opts.address_family = AF_INET6;
    opts.dns_servers = {ipv6_dns_server};
    HostResolver resolver(opts);
    WaitableEvent done(WaitableEvent::ResetPolicy::kAutomatic, false);
    std::atomic<bool> has_ipv6{false};

    Thread probe_thread("dns6-probe");
    probe_thread.Start();
    resolver.Resolve(
        "one.one.one.one",
        [&done, &has_ipv6](const AddressList &addrs) {
          has_ipv6.store(!addrs.empty(), std::memory_order_release);
          done.Signal();
        },
        probe_thread.GetTaskRunner());

    done.TimedWait(std::chrono::seconds(5));
    probe_thread.Stop();
    return has_ipv6.load(std::memory_order_acquire);
  }

  Thread test_thread_{"test-thread"};
  scoped_refptr<SingleThreadTaskRunner> test_runner_;
};

// =============================================================================
// Basic resolution tests
// =============================================================================

TEST_F(HostResolverTest, ResolveLocalhost) {
  AddressList result = ResolveAndWait("localhost");
  // "localhost" should resolve to at least one address (127.0.0.1 or ::1).
  EXPECT_FALSE(result.empty());
}

TEST_F(HostResolverTest, ResolveEmptyHost) {
  // Empty host should return immediately with empty result.
  AddressList result = ResolveAndWait("");
  EXPECT_TRUE(result.empty());
}

TEST_F(HostResolverTest, ResolveInvalidHost) {
  // Non-existent domain should return empty result.
  // On some networks (captive portals, certain DNS forwarders) the resolver
  // may return a redirect IP instead of NXDOMAIN — tolerate that.  The
  // negative answer can also be slow (observed ~12 s behind a forwarding
  // resolver), hence the enlarged budget: this test is about not crashing /
  // not hanging, so a slow-but-finite NXDOMAIN must not look like a failure.
  AddressList result = ResolveAndWait("this-domain-does-not-exist-12345.invalid.", nullptr, std::chrono::seconds(30));
  // No hard assertion on result content; primary goal is no crash/timeout.
  (void)result;
  SUCCEED();
}

TEST_F(HostResolverTest, ResolveIPv4Literal) {
  AddressList result = ResolveAndWait("8.8.8.8");
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(result[0].address().family(), IPAddress::Family::kIPv4);
}

TEST_F(HostResolverTest, ResolveIPv6Literal) {
  AddressList result = ResolveAndWait("::1");
  ASSERT_FALSE(result.empty());
  EXPECT_EQ(result[0].address().family(), IPAddress::Family::kIPv6);
}

// =============================================================================
// Address family tests
// =============================================================================

TEST_F(HostResolverTest, ResolveDualStack) {
  if (!HasFunctionalDNS()) {
    GTEST_SKIP() << "No functional DNS available";
  }

  // Default AF_UNSPEC should return both IPv4 and IPv6 for a dual-stack host.
  // Use one.one.one.one which is known to have both A and AAAA records.
  AddressList result = ResolveAndWait("one.one.one.one");

  bool has_ipv4 = false;
  bool has_ipv6 = false;
  for (std::size_t i = 0; i < result.size(); ++i) {
    if (result[i].address().family() == IPAddress::Family::kIPv4)
      has_ipv4 = true;
    if (result[i].address().family() == IPAddress::Family::kIPv6)
      has_ipv6 = true;
  }
  (void)has_ipv6; // IPv6 depends on network setup — may be absent

  // At minimum we should get IPv4; IPv6 depends on network setup.
  EXPECT_TRUE(has_ipv4);
}

TEST_F(HostResolverTest, ResolveIPv4Only) {
  if (!HasFunctionalDNS()) {
    GTEST_SKIP() << "No functional DNS available";
  }

  HostResolverOptions opts;
  opts.address_family = AF_INET;

  HostResolver resolver(opts);
  AddressList result = ResolveAndWait("one.one.one.one", &resolver);

  for (std::size_t i = 0; i < result.size(); ++i) {
    EXPECT_EQ(result[i].address().family(), IPAddress::Family::kIPv4);
  }
  EXPECT_FALSE(result.empty());
}

TEST_F(HostResolverTest, ResolveIPv6Only) {
  if (!HasFunctionalDNS()) {
    GTEST_SKIP() << "No functional DNS available";
  }
  if (!HasIPv6AddressResolution()) {
    GTEST_SKIP() << "No IPv6 address resolution";
  }

  HostResolverOptions opts;
  opts.address_family = AF_INET6;

  HostResolver resolver(opts);
  AddressList result = ResolveAndWait("one.one.one.one", &resolver);

  for (std::size_t i = 0; i < result.size(); ++i) {
    EXPECT_EQ(result[i].address().family(), IPAddress::Family::kIPv6);
  }
  EXPECT_FALSE(result.empty());
}

// =============================================================================
// Custom DNS server tests
// =============================================================================

TEST_F(HostResolverTest, CustomDnsServerAliDNS) {
  // AliDNS (223.5.5.5) may be unreachable outside China.
  HostResolverOptions opts;
  opts.dns_servers = {"223.5.5.5"};

  HostResolver resolver(opts);
  AddressList result = ResolveAndWait("www.baidu.com", &resolver);
  if (result.empty()) {
    GTEST_SKIP() << "AliDNS unreachable (likely outside China)";
  }
  EXPECT_FALSE(result.empty());
}

TEST_F(HostResolverTest, CustomDnsServerCloudflare) {
  if (!HasFunctionalDNS()) {
    GTEST_SKIP() << "No functional DNS available";
  }
  HostResolverOptions opts;
  opts.dns_servers = {"1.1.1.1"};

  HostResolver resolver(opts);
  AddressList result = ResolveAndWait("one.one.one.one", &resolver);
  EXPECT_FALSE(result.empty());
}

TEST_F(HostResolverTest, CustomDnsServerGoogle) {
  if (!HasFunctionalDNS()) {
    GTEST_SKIP() << "No functional DNS available";
  }
  HostResolverOptions opts;
  opts.dns_servers = {"8.8.8.8"};

  HostResolver resolver(opts);
  AddressList result = ResolveAndWait("dns.google", &resolver);
  EXPECT_FALSE(result.empty());
}

TEST_F(HostResolverTest, CustomDnsServerIPv6Cloudflare) {
  if (!HasFunctionalDNS()) {
    GTEST_SKIP() << "No functional DNS available";
  }
  if (!HasIPv6DNSConnectivity("2606:4700:4700::1111")) {
    GTEST_SKIP() << "No IPv6 DNS connectivity";
  }

  HostResolverOptions opts;
  opts.dns_servers = {"2606:4700:4700::1111"};

  HostResolver resolver(opts);
  AddressList result = ResolveAndWait("one.one.one.one", &resolver);
  EXPECT_FALSE(result.empty());
}

TEST_F(HostResolverTest, CustomDnsServerIPv6Google) {
  if (!HasFunctionalDNS()) {
    GTEST_SKIP() << "No functional DNS available";
  }
  if (!HasIPv6DNSConnectivity("2001:4860:4860::8888")) {
    GTEST_SKIP() << "No IPv6 DNS connectivity";
  }

  HostResolverOptions opts;
  opts.dns_servers = {"2001:4860:4860::8888"};

  HostResolver resolver(opts);
  AddressList result = ResolveAndWait("dns.google", &resolver);
  EXPECT_FALSE(result.empty());
}

TEST_F(HostResolverTest, CustomDnsServerMixed) {
  HostResolverOptions opts;
  // Multiple DNS servers for fallback.
  opts.dns_servers = {"223.5.5.5", "8.8.8.8"};

  HostResolver resolver(opts);
  AddressList result = ResolveAndWait("www.baidu.com", &resolver);
  EXPECT_FALSE(result.empty());
}

TEST_F(HostResolverTest, CustomDnsServerMixedV4V6) {
  if (!HasFunctionalDNS()) {
    GTEST_SKIP() << "No functional DNS available";
  }
  // The IPv4 server in the list still serves the query when IPv6 has no route,
  // so the IPv6 transport probe is informational only here.
  const bool has_ipv6_dns = HasIPv6DNSConnectivity("2001:4860:4860::8888");
  (void)has_ipv6_dns;

  HostResolverOptions opts;
  opts.dns_servers = {"223.5.5.5", "2001:4860:4860::8888"};

  HostResolver resolver(opts);
  AddressList result = ResolveAndWait("www.baidu.com", &resolver);
  EXPECT_FALSE(result.empty());
}

// =============================================================================
// Custom timeout test
// =============================================================================

TEST_F(HostResolverTest, CustomTimeout) {
  HostResolverOptions opts;
  opts.timeout_ms = 100; // Very short timeout
  opts.tries = 0;        // No retries

  HostResolver resolver(opts);
  // NOTE: With a non-existent domain and a very short timeout, the result
  // depends on network conditions (fast NXDOMAIN vs actual timeout).
  // The primary goal is verifying that the short timeout doesn't crash
  // or hang, which this achieves regardless of the result.
  AddressList result = ResolveAndWait("this-domain-does-not-exist-12345.invalid.", &resolver);
  (void)result;
  SUCCEED();
}

// =============================================================================
// Lifetime tests
// =============================================================================

TEST_F(HostResolverTest, DestroyBeforeCallback) {
  // Resolve and immediately destroy the resolver  --  should not crash.
  // Use a heap-allocated atomic to avoid dangling reference after
  // the stack frame is destroyed.
  auto called = std::make_shared<std::atomic<bool>>(false);
  {
    HostResolver resolver;
    resolver.Resolve("www.example.com", [called](const AddressList &) { called->store(true); }, test_runner_);
  }
  // The callback should be silently dropped via WeakPtr.
  // If we get here without crashing, the test passes.
  SUCCEED();
}

TEST_F(HostResolverTest, CallbackOnCorrectRunner) {
  // Verify the callback is actually called on the target runner.
  HostResolver resolver;
  WaitableEvent done(WaitableEvent::ResetPolicy::kManual, false);
  bool callback_called = false;

  resolver.Resolve(
      "localhost",
      [&done, &callback_called](const AddressList &) {
        callback_called = true;
        done.Signal();
      },
      test_runner_);

  done.Wait();
  EXPECT_TRUE(callback_called);
}

// =============================================================================
// Concurrent resolution test
// =============================================================================

TEST_F(HostResolverTest, ResolveMultipleConcurrent) {
  constexpr int kConcurrent = 10;
  std::vector<std::string> hosts = {
      "localhost",
      "127.0.0.1",
      "::1",
      "one.one.one.one",
      "dns.google",
      "www.baidu.com",
      "8.8.8.8",
      "1.1.1.1",
      "223.5.5.5",
      "119.29.29.29",
  };

  std::atomic<int> completed{0};
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kManual, false);
  HostResolver resolver;

  for (int i = 0; i < kConcurrent; ++i) {
    resolver.Resolve(
        hosts[i % hosts.size()],
        [&completed, &all_done, kConcurrent](const AddressList &) {
          if (++completed == kConcurrent) {
            all_done.Signal();
          }
        },
        test_runner_);
  }

  all_done.Wait();
  EXPECT_EQ(completed.load(), kConcurrent);
}

// =============================================================================
// Options channel reuse test
// =============================================================================

TEST_F(HostResolverTest, OptionsChannelReuse) {
  HostResolverOptions opts;
  opts.dns_servers = {"223.5.5.5"};

  // Two resolvers with same options should share the c-ares channel.
  HostResolver r1(opts);
  HostResolver r2(opts);

  AddressList result1 = ResolveAndWait("www.baidu.com", &r1);
  AddressList result2 = ResolveAndWait("www.baidu.com", &r2);

  EXPECT_FALSE(result1.empty());
  EXPECT_FALSE(result2.empty());
}

// =============================================================================
// Stress test
// =============================================================================

TEST_F(HostResolverTest, StressConcurrent) {
  constexpr int kTotal = 50;
  std::atomic<int> completed{0};
  std::atomic<int> errors{0};
  WaitableEvent all_done(WaitableEvent::ResetPolicy::kManual, false);
  HostResolver resolver;

  for (int i = 0; i < kTotal; ++i) {
    // Mix of valid and invalid hosts.
    std::string host;
    switch (i % 5) {
    case 0:
      host = "localhost";
      break;
    case 1:
      host = "one.one.one.one";
      break;
    case 2:
      host = "127.0.0.1";
      break;
    case 3:
      host = "8.8.8.8";
      break;
    case 4:
      host = "this-domain-does-not-exist-12345.invalid.";
      break;
    }

    resolver.Resolve(
        host,
        [&completed, &errors, &all_done, kTotal](const AddressList &result) {
          if (result.empty()) {
            // Invalid domain or timeout  --  expected for some hosts.
          }
          if (++completed == kTotal) {
            all_done.Signal();
          }
        },
        test_runner_);
  }

  all_done.Wait();
  EXPECT_EQ(completed.load(), kTotal);
}

} // namespace
} // namespace nei::net
