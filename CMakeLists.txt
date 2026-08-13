cmake_minimum_required(VERSION 3.25)

project(CppOptimizer VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "CppOptimizer currently supports Windows only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "x86 is not a release target; use an x64 generator")
endif()

option(OPTIMIZER_BUILD_TESTS "Build unit tests" ON)

add_executable(CppOptimizer
    source/main.cpp
    source/common/error.cpp
    source/logger/logger.cpp
    source/memory/memory_tuner.cpp
    source/metrics/memory_metrics.cpp
    source/platform/native_api.cpp
)

target_include_directories(CppOptimizer PRIVATE include)
target_compile_features(CppOptimizer PRIVATE cxx_std_20)
target_compile_definitions(CppOptimizer PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(CppOptimizer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus>
)

if(MINGW)
    # Auxiliary portability build only. The supported toolchain remains MSVC.
    target_link_options(CppOptimizer PRIVATE -municode)
endif()

set_target_properties(CppOptimizer PROPERTIES
    CXX_EXTENSIONS OFF
    VS_DEBUGGER_COMMAND_ARGUMENTS "--diagnose"
)

include(CTest)
if(BUILD_TESTING AND OPTIMIZER_BUILD_TESTS)
    add_executable(CppOptimizerCommonTests
        tests/common_tests.cpp
        source/common/error.cpp
    )
    target_include_directories(CppOptimizerCommonTests PRIVATE include)
    target_compile_features(CppOptimizerCommonTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerCommonTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerCommonTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus>
    )
    if(MINGW)
        target_link_options(CppOptimizerCommonTests PRIVATE -municode)
    endif()
    add_test(NAME CommonTests COMMAND CppOptimizerCommonTests)

    add_executable(CppOptimizerMemoryTests
        tests/memory_tests.cpp
        source/common/error.cpp
        source/memory/memory_tuner.cpp
    )
    target_include_directories(CppOptimizerMemoryTests PRIVATE include)
    target_compile_features(CppOptimizerMemoryTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerMemoryTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerMemoryTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus>
    )
    if(MINGW)
        target_link_options(CppOptimizerMemoryTests PRIVATE -municode)
    endif()
    add_test(NAME MemoryTests COMMAND CppOptimizerMemoryTests)

    add_executable(CppOptimizerMetricsTests
        tests/memory_metrics_tests.cpp
        source/common/error.cpp
        source/metrics/memory_metrics.cpp
    )
    target_include_directories(CppOptimizerMetricsTests PRIVATE include)
    target_compile_features(CppOptimizerMetricsTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerMetricsTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerMetricsTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus>
    )
    if(MINGW)
        target_link_options(CppOptimizerMetricsTests PRIVATE -municode)
    endif()
    add_test(NAME MetricsTests COMMAND CppOptimizerMetricsTests)

    add_executable(CppOptimizerLoggerTests
        tests/logger_tests.cpp
        source/common/error.cpp
        source/logger/logger.cpp
    )
    target_include_directories(CppOptimizerLoggerTests PRIVATE include)
    target_compile_features(CppOptimizerLoggerTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerLoggerTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerLoggerTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus>
    )
    if(MINGW)
        target_link_options(CppOptimizerLoggerTests PRIVATE -municode)
    endif()
    add_test(NAME LoggerTests COMMAND CppOptimizerLoggerTests)
endif()
