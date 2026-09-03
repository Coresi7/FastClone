#include <exception>
#include <iostream>

void RunCheckCliTests();
void RunCheckReportTests();
void RunCheckEngineTests();
// unify-probe-extra-shared: shared probe / extra-scan tests (FastCheck side).
namespace fc::test {
void RunSharedProbeTestsFastCheckSide();
void RunSharedExtraScanTestsFastCheckSide();
}

int main() {
    try {
        RunCheckCliTests();
        RunCheckReportTests();
        RunCheckEngineTests();
        fc::test::RunSharedProbeTestsFastCheckSide();
        fc::test::RunSharedExtraScanTestsFastCheckSide();
        std::cout << "All FastCheck tests passed." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed: " << ex.what() << std::endl;
        return 1;
    }
}
