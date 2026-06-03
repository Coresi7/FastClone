#include <exception>
#include <iostream>

void RunCliTests();
void RunHashMemCacheTests();
void RunFileIndexTests();

int main() {
    try {
        RunCliTests();
        RunHashMemCacheTests();
        RunFileIndexTests();
        std::cout << "All FastClone tests passed." << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed: " << ex.what() << std::endl;
        return 1;
    }
}
