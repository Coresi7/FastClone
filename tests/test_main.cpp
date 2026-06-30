#include <exception>
#include <iostream>

#if defined(FASTCLONE_ENABLE_CLI_TESTS)
void RunCliTests();
#endif
void RunHashMemCacheTests();
void RunDeltaTests();
void RunFileIndexTests();
void RunReconnectClassifierTests();
void RunRouteSelectionTests();
void RunLinkSchedulerTests();
void RunWanTuningTests();

int main() {
    try {
#if defined(FASTCLONE_ENABLE_CLI_TESTS)
        RunCliTests();
#endif
        RunReconnectClassifierTests();
        RunHashMemCacheTests();
        RunDeltaTests();
        RunFileIndexTests();
        RunRouteSelectionTests();
        RunLinkSchedulerTests();
        RunWanTuningTests();
        std::cout << "All FastClone tests passed." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed: " << ex.what() << std::endl;
        return 1;
    }
}
