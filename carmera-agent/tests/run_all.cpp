#include "test_harness.h"

#include <exception>

int main() {
    int failed = 0;
    int total = 0;
    for (const auto& c : test::registry()) {
        ++total;
        std::cout << "[ RUN ] " << c.name << std::endl;
        bool ok = false;
        try {
            ok = c.fn();
        } catch (const std::exception& e) {
            std::cout << "  EXCEPTION: " << e.what() << std::endl;
            ok = false;
        } catch (...) {
            std::cout << "  EXCEPTION: unknown (non-std)" << std::endl;
            ok = false;
        }
        if (ok) {
            std::cout << "[ OK  ] " << c.name << std::endl;
        } else {
            std::cout << "[FAIL ] " << c.name << std::endl;
            ++failed;
        }
    }
    std::cout << "\n==== " << (total - failed) << "/" << total
              << " tests passed ====\n"
              << std::flush;
    return failed == 0 ? 0 : 1;
}
