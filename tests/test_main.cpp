#include <exception>
#include <iostream>

#if defined(FASTCLONE_ENABLE_CLI_TESTS)
void RunCliTests();
#endif
void RunHashMemCacheTests();
void RunFileIndexTests();
void RunReconnectClassifierTests();
void RunRouteSelectionTests();

int main() {
    try {
#if defined(FASTCLONE_ENABLE_CLI_TESTS)
        RunCliTests();
#endif
        RunReconnectClassifierTests();
        RunHashMemCacheTests();
        RunFileIndexTests();
        RunRouteSelectionTests();
        std::cout << "All FastClone tests passed." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed: " << ex.what() << std::endl;
        return 1;
    }
}
