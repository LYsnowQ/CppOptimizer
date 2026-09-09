cmake_minimum_required(VERSION 3.25)

project(CppOptimizer VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "CppOptimizer currently supports Windows only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "x86 is not a release target; use an x64 generator")
endif()

option(OPTIMIZER_BUILD_TESTS "Build unit tests" ON)

# Third-party libraries live under thirdParty/<lib-name>/. Convention: compiled
# artifacts (static/dynamic libs) are stored back inside the library's own
# folder (thirdParty/<lib-name>/lib/), headers under thirdParty/<lib-name>/include/,
# and unneeded sources are removed after building. They are never mixed into the
# project output tree. spdlog is linked as a prebuilt static lib.
# Include lookup roots: "thirdParty" is the public root (code references the
# full path like spdlog/include/spdlog/spdlog.h); the library's own include dir
# is also on the path because spdlog headers reference each other via
# <spdlog/...> which resolves relative to that root.
set(SPDLOG_LIB_DIR ${CMAKE_SOURCE_DIR}/thirdParty/spdlog/lib)
set(SPDLOG_LIB_DEBUG ${SPDLOG_LIB_DIR}/spdlogd.lib)
set(SPDLOG_LIB_RELEASE ${SPDLOG_LIB_DIR}/spdlog.lib)
set(SPDLOG_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/thirdParty/spdlog/include)
set(TOMLPLUSPLUS_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/thirdParty/tomlplusplus/include)
set(THIRDPARTY_DIR ${CMAKE_SOURCE_DIR}/thirdParty)

add_executable(CppOptimizer
    source/main.cpp
    source/common/error.cpp
    source/common/console_output.cpp
    source/config/config_manager.cpp
    source/logger/logger.cpp
    source/memory/memory_tuner.cpp
    source/metrics/memory_metrics.cpp
    source/metrics/pdh_metrics.cpp
    source/platform/native_api.cpp
    source/policy/policy_engine.cpp
    source/policy/policy_executor.cpp
    source/power/power_locker.cpp
    source/priority/priority_booster.cpp
    source/process/process_watcher.cpp
    source/activity/user_activity.cpp
    source/service/service_host.cpp
    source/service/recovery_marker.cpp
    source/service/tray_host.cpp
    source/service/presence.cpp
    source/ipc/ipc_protocol.cpp
    source/ipc/ipc_transport.cpp
    source/ipc/ipc_session.cpp
    source/ipc/ipc_facts.cpp
    source/ipc/ipc_credentials.cpp
)

target_include_directories(CppOptimizer PRIVATE include ${THIRDPARTY_DIR} ${SPDLOG_INCLUDE_DIR} ${TOMLPLUSPLUS_INCLUDE_DIR})
target_link_libraries(CppOptimizer PRIVATE
    pdh
    user32
    psapi
    advapi32
    wtsapi32
    shell32
    $<$<CONFIG:Debug>:${SPDLOG_LIB_DEBUG}>
    $<$<NOT:$<CONFIG:Debug>>:${SPDLOG_LIB_RELEASE}>
)
target_compile_features(CppOptimizer PRIVATE cxx_std_20)
target_compile_definitions(CppOptimizer PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SPDLOG_COMPILED_LIB
)

target_compile_options(CppOptimizer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
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
        source/common/console_output.cpp
    )
    target_include_directories(CppOptimizerCommonTests PRIVATE include)
    target_compile_features(CppOptimizerCommonTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerCommonTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerCommonTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerCommonTests PRIVATE -municode)
    endif()
    add_test(NAME CommonTests COMMAND CppOptimizerCommonTests)

    add_executable(CppOptimizerPlatformTests
        tests/platform_tests.cpp
        source/common/error.cpp
        source/platform/native_api.cpp
    )
    target_include_directories(CppOptimizerPlatformTests PRIVATE include)
    target_compile_features(CppOptimizerPlatformTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerPlatformTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerPlatformTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerPlatformTests PRIVATE -municode)
    endif()
    add_test(NAME PlatformTests COMMAND CppOptimizerPlatformTests)

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
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
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
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
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
    target_include_directories(CppOptimizerLoggerTests PRIVATE include ${THIRDPARTY_DIR} ${SPDLOG_INCLUDE_DIR})
    target_link_libraries(CppOptimizerLoggerTests PRIVATE
        $<$<CONFIG:Debug>:${SPDLOG_LIB_DEBUG}>
        $<$<NOT:$<CONFIG:Debug>>:${SPDLOG_LIB_RELEASE}>
    )
    target_compile_features(CppOptimizerLoggerTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerLoggerTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX SPDLOG_COMPILED_LIB
    )
    target_compile_options(CppOptimizerLoggerTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerLoggerTests PRIVATE -municode)
    endif()
    add_test(NAME LoggerTests COMMAND CppOptimizerLoggerTests)

    add_executable(CppOptimizerConfigTests
        tests/config_tests.cpp
        source/common/error.cpp
        source/config/config_manager.cpp
    )
    target_include_directories(CppOptimizerConfigTests PRIVATE include ${TOMLPLUSPLUS_INCLUDE_DIR})
    target_compile_features(CppOptimizerConfigTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerConfigTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerConfigTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerConfigTests PRIVATE -municode)
    endif()
    add_test(NAME ConfigTests COMMAND CppOptimizerConfigTests)

    add_executable(CppOptimizerPdhMetricsTests
        tests/pdh_metrics_tests.cpp
        source/common/error.cpp
        source/metrics/pdh_metrics.cpp
    )
    target_include_directories(CppOptimizerPdhMetricsTests PRIVATE include)
    target_link_libraries(CppOptimizerPdhMetricsTests PRIVATE pdh)
    target_compile_features(CppOptimizerPdhMetricsTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerPdhMetricsTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerPdhMetricsTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerPdhMetricsTests PRIVATE -municode)
    endif()
    add_test(NAME PdhMetricsTests COMMAND CppOptimizerPdhMetricsTests)

    add_executable(CppOptimizerProcessTests
        tests/process_tests.cpp
        source/common/error.cpp
        source/common/console_output.cpp
        source/process/process_watcher.cpp
    )
    target_include_directories(CppOptimizerProcessTests PRIVATE include)
    target_link_libraries(CppOptimizerProcessTests PRIVATE user32 psapi)
    target_compile_features(CppOptimizerProcessTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerProcessTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerProcessTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerProcessTests PRIVATE -municode)
    endif()
    add_test(NAME ProcessTests COMMAND CppOptimizerProcessTests)

    add_executable(CppOptimizerPolicyTests
        tests/policy_tests.cpp
        source/common/error.cpp
        source/policy/policy_engine.cpp
    )
    target_include_directories(CppOptimizerPolicyTests PRIVATE include)
    target_compile_features(CppOptimizerPolicyTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerPolicyTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerPolicyTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerPolicyTests PRIVATE -municode)
    endif()
    add_test(NAME PolicyTests COMMAND CppOptimizerPolicyTests)

    add_executable(CppOptimizerPowerTests
        tests/power_tests.cpp
        source/common/error.cpp
        source/power/power_locker.cpp
    )
    target_include_directories(CppOptimizerPowerTests PRIVATE include)
    target_compile_features(CppOptimizerPowerTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerPowerTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerPowerTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerPowerTests PRIVATE -municode)
    endif()
    add_test(NAME PowerTests COMMAND CppOptimizerPowerTests)

    add_executable(CppOptimizerPriorityTests
        tests/priority_tests.cpp
        source/common/error.cpp
        source/priority/priority_booster.cpp
    )
    target_include_directories(CppOptimizerPriorityTests PRIVATE include)
    target_compile_features(CppOptimizerPriorityTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerPriorityTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerPriorityTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerPriorityTests PRIVATE -municode)
    endif()
    add_test(NAME PriorityTests COMMAND CppOptimizerPriorityTests)

    add_executable(CppOptimizerPolicyExecutorTests
        tests/policy_executor_tests.cpp
        source/common/error.cpp
        source/policy/policy_engine.cpp
        source/policy/policy_executor.cpp
        source/power/power_locker.cpp
        source/priority/priority_booster.cpp
    )
    target_include_directories(CppOptimizerPolicyExecutorTests PRIVATE include)
    target_compile_features(CppOptimizerPolicyExecutorTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerPolicyExecutorTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerPolicyExecutorTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerPolicyExecutorTests PRIVATE -municode)
    endif()
    add_test(NAME PolicyExecutorTests COMMAND CppOptimizerPolicyExecutorTests)

    add_executable(CppOptimizerServiceTests
        tests/service_tests.cpp
        source/common/error.cpp
        source/service/service_host.cpp
        source/service/recovery_marker.cpp
        source/service/tray_host.cpp
        source/service/presence.cpp
    )
    target_include_directories(CppOptimizerServiceTests PRIVATE include)
    target_link_libraries(CppOptimizerServiceTests PRIVATE advapi32 shell32)
    target_compile_features(CppOptimizerServiceTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerServiceTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerServiceTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerServiceTests PRIVATE -municode)
    endif()
    add_test(NAME ServiceTests COMMAND CppOptimizerServiceTests)

    add_executable(CppOptimizerIpcTests
        tests/ipc_tests.cpp
        source/common/error.cpp
        source/ipc/ipc_protocol.cpp
        source/ipc/ipc_transport.cpp
        source/ipc/ipc_session.cpp
        source/ipc/ipc_facts.cpp
        source/ipc/ipc_credentials.cpp
    )
    target_include_directories(CppOptimizerIpcTests PRIVATE include)
    target_compile_features(CppOptimizerIpcTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerIpcTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerIpcTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerIpcTests PRIVATE -municode)
    endif()
    add_test(NAME IpcTests COMMAND CppOptimizerIpcTests)

    add_executable(CppOptimizerActivityTests
        tests/activity_tests.cpp
        source/common/error.cpp
        source/activity/user_activity.cpp
    )
    target_include_directories(CppOptimizerActivityTests PRIVATE include)
    target_link_libraries(CppOptimizerActivityTests PRIVATE user32 wtsapi32)
    target_compile_features(CppOptimizerActivityTests PRIVATE cxx_std_20)
    target_compile_definitions(CppOptimizerActivityTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(CppOptimizerActivityTests PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /sdl /Zc:__cplusplus /utf-8>
    )
    if(MINGW)
        target_link_options(CppOptimizerActivityTests PRIVATE -municode)
    endif()
    add_test(NAME ActivityTests COMMAND CppOptimizerActivityTests)
endif()
