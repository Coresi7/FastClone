#include <exception>
#include <iostream>

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
void RunReconnectClassifierTests();
void RunRouteSelectionTests();
void RunLinkSchedulerTests();
void RunWanTuningTests();
void RunComparePhaseTests();
void RunProtocolCodecTests();

int main() {
    try {
#if defined(FASTCLONE_ENABLE_CLI_TESTS)
        RunCliTests();
#endif
        RunReconnectClassifierTests();
        RunHashMemCacheTests();
        RunDeltaTests();
        RunStreamingBuildPlanTests();
        RunDiskIoAlignTests();
        RunDiskIoDriverTests();
        RunReadGateTests();
        RunFileIndexTests();
        RunManifestDirentTests();
        RunSyncUtilTests();
        RunRouteSelectionTests();
        RunLinkSchedulerTests();
        RunWanTuningTests();
        RunComparePhaseTests();
        RunProtocolCodecTests();
        std::cout << "All FastClone tests passed." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed: " << ex.what() << std::endl;
        return 1;
    }
}
