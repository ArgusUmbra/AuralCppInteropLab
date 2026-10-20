cmake_minimum_required(VERSION 3.25)

project(AuralCppInteropLab VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)

add_library(aural_project_warnings INTERFACE)
if(MSVC)
    target_compile_options(aural_project_warnings INTERFACE /W4 /WX /permissive-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        aural_project_warnings
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
    )
endif()

add_library(aural_core src/core/range.cpp)
add_library(aural::core ALIAS aural_core)
target_compile_features(aural_core PUBLIC cxx_std_20)
target_include_directories(
    aural_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(aural_core PRIVATE aural_project_warnings)

add_executable(aural_lab src/main.cpp)
target_link_libraries(aural_lab PRIVATE aural_project_warnings)

include(CTest)
if(BUILD_TESTING)
    add_executable(dependency_smoke tests/dependency_smoke.cpp)
    target_link_libraries(
        dependency_smoke
        PRIVATE
            aural_project_warnings
            OpenSSL::Crypto
            ZLIB::ZLIB
    )

    add_executable(frozen_input_verify tests/frozen_input_verify.cpp)
    target_link_libraries(
        frozen_input_verify
        PRIVATE
            aural_project_warnings
            OpenSSL::Crypto
    )

    add_executable(core_range_tests tests/core_range_tests.cpp)
    target_link_libraries(
        core_range_tests
        PRIVATE
            aural::core
            aural_project_warnings
    )

    add_test(NAME executable_startup COMMAND aural_lab)
    set_tests_properties(
        executable_startup
        PROPERTIES PASS_REGULAR_EXPRESSION "verifier not implemented"
    )

    add_test(NAME dependency_known_answers COMMAND dependency_smoke)
    set_tests_properties(
        dependency_known_answers
        PROPERTIES PASS_REGULAR_EXPRESSION "DEPENDENCY_SMOKE_OK"
    )

    add_test(
        NAME frozen_input_checksums
        COMMAND frozen_input_verify "${CMAKE_CURRENT_SOURCE_DIR}/frozen-input"
    )
    set_tests_properties(
        frozen_input_checksums
        PROPERTIES PASS_REGULAR_EXPRESSION "FROZEN_INPUTS_OK"
    )

    add_test(NAME core_range_arithmetic COMMAND core_range_tests)
    set_tests_properties(
        core_range_arithmetic
        PROPERTIES PASS_REGULAR_EXPRESSION "CORE_RANGE_TESTS_OK"
    )
endif()
