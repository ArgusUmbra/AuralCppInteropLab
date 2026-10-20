#include <iostream>

consteval int cpp_standard_generation() {
    return 20;
}

int main() {
    static_assert(__cplusplus >= 202002L);
    std::cout << "AURAL C++ Interoperability Laboratory\n"
              << "status: foundation only; verifier not implemented\n"
              << "language: C++" << cpp_standard_generation() << '\n';
    return 0;
}
