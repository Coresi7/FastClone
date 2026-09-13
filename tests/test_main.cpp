#include <exception>
#include <iostream>
#include <system_error>
#include <typeinfo>

#if defined(FASTCLONE_ENABLE_CLI_TESTS)
void RunCliTests();
#endif
void RunHashMemCacheTests();
void RunDeltaTests();
void RunStreamingBuildPlanTests();
void RunDiskIoAlignTests();
void RunDiskIoDriverTests();
void RunReadGateTests();
void RunFileIndexTests();
void RunManifestDirentTests();
void RunSyncUtilTests();
void RunLongPathTests();
void RunReconnectClassifierTests();
void RunRouteSelectionTests();
void RunLinkSchedulerTests();
void RunWanTuningTests();
void RunWritePathAccountingTests();
void RunComparePhaseTests();
void RunComparePipelineTests();
void RunProtocolCodecTests();
// unify-probe-extra-shared: shared probe / extra-scan / source-gate tests.
namespace fc::test {
void RunSharedProbeTestsFastCloneSide();
void RunSharedExtraScanTestsFastCloneSide();
void RunSharedSourceGateTests();
}  // namespace fc::test

namespace {
// Diagnostics only (test harness, never shipped in the product): remember which
// test group is executing. On a CI runner the only evidence is stdout/stderr, so
// a bare "Test failed: unknown error" is undiagnosable - naming the group plus the
// exception type / error code turns the next run into an actionable report.
const char* g_currentTest = "<startup>";

template <typename Fn>
void RunTest(const char* name, Fn&& fn) {
    g_currentTest = name;
    fn();
    g_currentTest = "<between-groups>";
}
}  // namespace

int main() {
    try {
#if defined(FASTCLONE_ENABLE_CLI_TESTS)
        RunTest("RunCliTests", RunCliTests);
#endif
        RunTest("RunReconnectClassifierTests", RunReconnectClassifierTests);
        RunTest("RunHashMemCacheTests", RunHashMemCacheTests);
        RunTest("RunDeltaTests", RunDeltaTests);
        RunTest("RunStreamingBuildPlanTests", RunStreamingBuildPlanTests);
        RunTest("RunDiskIoAlignTests", RunDiskIoAlignTests);
        RunTest("RunDiskIoDriverTests", RunDiskIoDriverTests);
        RunTest("RunReadGateTests", RunReadGateTests);
        RunTest("RunFileIndexTests", RunFileIndexTests);
        RunTest("RunManifestDirentTests", RunManifestDirentTests);
        RunTest("RunSyncUtilTests", RunSyncUtilTests);
        RunTest("RunLongPathTests", RunLongPathTests);
        RunTest("RunRouteSelectionTests", RunRouteSelectionTests);
        RunTest("RunLinkSchedulerTests", RunLinkSchedulerTests);
        RunTest("RunWanTuningTests", RunWanTuningTests);
        RunTest("RunWritePathAccountingTests", RunWritePathAccountingTests);
        RunTest("RunComparePhaseTests", RunComparePhaseTests);
        RunTest("RunComparePipelineTests", RunComparePipelineTests);
        RunTest("RunProtocolCodecTests", RunProtocolCodecTests);
        RunTest("fc::test::RunSharedProbeTestsFastCloneSide", fc::test::RunSharedProbeTestsFastCloneSide);
        RunTest("fc::test::RunSharedExtraScanTestsFastCloneSide", fc::test::RunSharedExtraScanTestsFastCloneSide);
        RunTest("fc::test::RunSharedSourceGateTests", fc::test::RunSharedSourceGateTests);
        std::cout << "All FastClone tests passed." << std::endl;
        return 0;
    } catch (const std::system_error& ex) {
        std::cerr << "Test failed in " << g_currentTest << ": " << ex.what()
                  << " [type=std::system_error code=" << ex.code().value()
                  << " category=" << ex.code().category().name()
                  << " msg=" << ex.code().message() << "]" << std::endl;
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed in " << g_currentTest << ": " << ex.what()
                  << " [type=" << typeid(ex).name() << "]" << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Test failed in " << g_currentTest
                  << ": unknown non-std exception" << std::endl;
        return 1;
    }
}
