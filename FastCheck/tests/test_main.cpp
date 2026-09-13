#include <exception>
#include <iostream>
#include <system_error>
#include <typeinfo>

void RunCheckCliTests();
void RunCheckReportTests();
void RunCheckEngineTests();
// unify-probe-extra-shared: shared probe / extra-scan tests (FastCheck side).
namespace fc::test {
void RunSharedProbeTestsFastCheckSide();
void RunSharedExtraScanTestsFastCheckSide();
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
        RunTest("RunCheckCliTests", RunCheckCliTests);
        RunTest("RunCheckReportTests", RunCheckReportTests);
        RunTest("RunCheckEngineTests", RunCheckEngineTests);
        RunTest("fc::test::RunSharedProbeTestsFastCheckSide", fc::test::RunSharedProbeTestsFastCheckSide);
        RunTest("fc::test::RunSharedExtraScanTestsFastCheckSide", fc::test::RunSharedExtraScanTestsFastCheckSide);
        std::cout << "All FastCheck tests passed." << std::endl;
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
