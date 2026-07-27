// LSan suppression for phosphor-logging's CommitDeviceError async queue.
//
// nv::lg2::CommitDeviceError posts an async_method_call onto a function-
// local-static io_context owned by phosphor::logging::AsioConnection.
// That io_context is never run from these tests, so the completion lambda
// — which captures messageId/redfishArgs by value — sits in the queue
// until process exit and LeakSanitizer reports the captures as indirect
// leaks.
//
// Pre-sync, sd_bus_default failed in the test env, the static-init threw,
// CommitDeviceError threw, the test caught and SKIPped — no queued work,
// no leak.  After the upstream sync the static-init succeeds and the
// queued work persists; the drift-fix in 561e026 had to rewrite
// EXPECT_THROW into try/catch GTEST_SKIP for the same reason.
//
// A drain-based fix would need phosphor-logging/asio_connection.hpp to
// reach the singleton's io_context, but that header is internal to the
// subproject and is not installed by the docker image's phosphor-logging
// package, so it isn't visible in CI builds.  Instead, register a weak-
// symbol LSan suppression scoped to CommitDeviceError.  This is a
// suppression, not a fix; the real fix needs phosphor-logging to expose
// either a public io_context accessor or a Shutdown() entry point.
//
// Link this file into any test binary whose tests transitively call
// CommitDeviceError.

#if defined(__SANITIZE_ADDRESS__)
#define DRAIN_LSAN_ON 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DRAIN_LSAN_ON 1
#endif
#endif

#ifdef DRAIN_LSAN_ON
// Two suppression rules:
//   1. CommitDeviceError → singleton AsioConnection's async_method_call;
//      the queued completion lambdas + captured strings persist to exit
//      because the singleton io_context is never run from tests.
//   2. async_method_call_timed → any test path that posts an async_method_call
//      via sdbusplus and routes it through the sd_bus_call_async __wrap
//      (PendingAsync).  Entries that the test does not driveAsyncCall*()
//      stay in gPendingAsyncCalls at process exit; the captured lambda +
//      its arguments are reported as leaked.  Both are pending-async-work
//      issues in the test harness, not product defects.
extern "C" const char* __lsan_default_suppressions() // NOLINT
{
    return "leak:nv::lg2::CommitDeviceError\n"
           "leak:async_method_call_timed\n"
           // Indirect leaks reachable only from the suppressed async work:
           // the per-test conn shared_ptr / dev shared_ptr held by captured
           // lambdas in gPendingAsyncCalls or the singleton io_context queue.
           // Fixture- / test-body-owned, test-only — never reaches
           // production builds.
           "leak:FakeConnFixture::SetUp\n"
           "leak:AsyncFixture::SetUp\n"
           "leak:TestUSBMCTPDDevice\n";
}
#endif
