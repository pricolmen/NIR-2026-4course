cmake_minimum_required(VERSION 3.20)
project(telemetry_sim VERSION 0.1.0 LANGUAGES CXX)
set(TELEMETRY_IMPLEMENTATION_STAGE 15)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build/debug")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(CTest)

option(TELEMETRY_ENABLE_ASAN "Enable AddressSanitizer for project targets" OFF)
option(TELEMETRY_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for project targets" OFF)
option(TELEMETRY_USE_SYSTEM_JSON "Require installed nlohmann_json 3.12.0" OFF)

if(UNIX AND (CMAKE_SOURCE_DIR MATCHES ":" OR CMAKE_BINARY_DIR MATCHES ":"))
    if(CMAKE_GENERATOR MATCHES "Makefiles")
        message(FATAL_ERROR "Paths containing ':' require Ninja: configure a new build with -G Ninja")
    endif()
    if(CMAKE_VERSION VERSION_LESS 3.30 AND NOT TELEMETRY_USE_SYSTEM_JSON
            AND NOT FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON)
        message(FATAL_ERROR "FetchContent with ':' in paths requires CMake >= 3.30, or an installed/offline JSON source")
    endif()
endif()

include(cmake/ProjectOptions.cmake)
include(cmake/Dependencies.cmake)

configure_file(cmake/build_config.hpp.in
    generated/telemetry/common/build_config.hpp @ONLY)

# Common contains the numerical helpers; algorithmic targets below acquire
# compiled sources at their own implementation stages.
add_library(telemetry_common STATIC src/common/time.cpp)
add_library(telemetry::common ALIAS telemetry_common)
target_compile_features(telemetry_common PUBLIC cxx_std_20)
telemetry_configure_target(telemetry_common)
target_include_directories(telemetry_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_library(telemetry_core STATIC src/core/task_store.cpp src/core/scheduling.cpp src/core/dispatcher.cpp src/core/detector.cpp)
add_library(telemetry::core ALIAS telemetry_core)
target_link_libraries(telemetry_core PUBLIC telemetry_common)
telemetry_configure_target(telemetry_core)

add_library(telemetry_engine STATIC src/engine/event_loop.cpp)
add_library(telemetry::engine ALIAS telemetry_engine)
target_link_libraries(telemetry_engine PUBLIC telemetry_common)
telemetry_configure_target(telemetry_engine)

add_library(telemetry_simulation STATIC src/simulation/physical_trace.cpp src/simulation/physical_environment.cpp src/simulation/scenario.cpp src/simulation/simulation.cpp)
add_library(telemetry::simulation ALIAS telemetry_simulation)
target_link_libraries(telemetry_simulation PUBLIC telemetry_core telemetry_engine)
telemetry_configure_target(telemetry_simulation)

add_library(telemetry_metrics STATIC src/metrics/analysis.cpp)
add_library(telemetry::metrics ALIAS telemetry_metrics)
target_link_libraries(telemetry_metrics PUBLIC telemetry_simulation)
telemetry_configure_target(telemetry_metrics)

add_library(telemetry_io STATIC src/io/build_info.cpp src/io/scenario_loader.cpp src/io/journal.cpp src/io/run_output.cpp src/io/generation.cpp src/io/batch.cpp src/io/output_directory.cpp)
add_library(telemetry::io ALIAS telemetry_io)
target_link_libraries(telemetry_io PUBLIC telemetry_common telemetry_metrics)
telemetry_use_json(telemetry_io)
telemetry_configure_target(telemetry_io)

add_executable(telemetry_sim app/main.cpp)
target_link_libraries(telemetry_sim PRIVATE telemetry_io)
telemetry_configure_target(telemetry_sim)

# Fail at configure time if JSON or engine accidentally leaks into the core,
# or vice versa. Future module dependencies must preserve this boundary.
get_target_property(common_dependencies telemetry_common INTERFACE_LINK_LIBRARIES)
if(common_dependencies)
    message(FATAL_ERROR "telemetry_common must not introduce module dependencies")
endif()
foreach(module IN ITEMS telemetry_core telemetry_engine)
    get_target_property(dependencies ${module} INTERFACE_LINK_LIBRARIES)
    if(NOT "${dependencies}" STREQUAL "telemetry_common")
        message(FATAL_ERROR "${module} must depend only on telemetry_common")
    endif()
endforeach()

if(BUILD_TESTING)
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_test(NAME final_load_smoke COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/final_verification.py"
        "$<TARGET_FILE:telemetry_sim>" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(final_load_smoke PROPERTIES TIMEOUT 180)

    foreach(group IN ITEMS catalog long_blackout reordered simultaneous no_alive unfinished reload generator batch)
        add_test(NAME examples_${group} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/example_tests.py"
            "$<TARGET_FILE:telemetry_sim>" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" ${group})
        set_tests_properties(examples_${group} PROPERTIES TIMEOUT 90)
    endforeach()

    foreach(group IN ITEMS parsing validate run compare metrics generate batch overwrite failures determinism)
        add_test(NAME commands_${group} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_tests.py"
            "$<TARGET_FILE:telemetry_sim>" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" ${group})
        set_tests_properties(commands_${group} PROPERTIES TIMEOUT 90)
    endforeach()

    add_executable(telemetry_metrics_tests tests/metrics_tests.cpp)
    target_link_libraries(telemetry_metrics_tests PRIVATE telemetry_io)
    telemetry_configure_target(telemetry_metrics_tests)
    target_compile_definitions(telemetry_metrics_tests PRIVATE TELEMETRY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" TELEMETRY_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    foreach(group IN ITEMS baseline recovery classification boundaries retries journal files errors reproducibility)
        add_test(NAME metrics_${group} COMMAND telemetry_metrics_tests ${group})
        set_tests_properties(metrics_${group} PROPERTIES TIMEOUT 30)
    endforeach()

    add_executable(telemetry_integration_tests tests/integration_tests.cpp)
    target_link_libraries(telemetry_integration_tests PRIVATE telemetry_io)
    target_compile_definitions(telemetry_integration_tests PRIVATE TELEMETRY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    telemetry_configure_target(telemetry_integration_tests)
    foreach(group IN ITEMS baseline crash blackout simultaneous boundaries isolation errors)
        add_test(NAME integration_${group} COMMAND telemetry_integration_tests ${group})
        set_tests_properties(integration_${group} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(telemetry_scenario_tests tests/scenario_tests.cpp)
    target_link_libraries(telemetry_scenario_tests PRIVATE telemetry_io)
    telemetry_use_json(telemetry_scenario_tests)
    target_compile_definitions(telemetry_scenario_tests PRIVATE TELEMETRY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    telemetry_configure_target(telemetry_scenario_tests)
    foreach(group IN ITEMS valid json csv ranges immutable examples)
        add_test(NAME scenario_${group} COMMAND telemetry_scenario_tests ${group})
        set_tests_properties(scenario_${group} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(telemetry_physical_tests tests/physical_tests.cpp)
    target_link_libraries(telemetry_physical_tests PRIVATE telemetry_simulation)
    telemetry_configure_target(telemetry_physical_tests)
    foreach(group IN ITEMS fifo crash channel exclusion horizon inputs)
        add_test(NAME physical_${group} COMMAND telemetry_physical_tests ${group})
        set_tests_properties(physical_${group} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(telemetry_engine_tests tests/engine_tests.cpp)
    target_link_libraries(telemetry_engine_tests PRIVATE telemetry_engine)
    telemetry_configure_target(telemetry_engine_tests)
    foreach(group IN ITEMS ordering phases horizon failures limits determinism)
        add_test(NAME engine_${group} COMMAND telemetry_engine_tests ${group})
        set_tests_properties(engine_${group} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(telemetry_reconfiguration_tests tests/reconfiguration_tests.cpp)
    target_link_libraries(telemetry_reconfiguration_tests PRIVATE telemetry_core)
    telemetry_configure_target(telemetry_reconfiguration_tests)
    foreach(group IN ITEMS batch results availability audit inputs)
        add_test(NAME reconfiguration_${group} COMMAND telemetry_reconfiguration_tests ${group})
        set_tests_properties(reconfiguration_${group} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(telemetry_detector_tests tests/detector_tests.cpp)
    target_link_libraries(telemetry_detector_tests PRIVATE telemetry_core)
    telemetry_configure_target(telemetry_detector_tests)
    foreach(group IN ITEMS fixed adaptive window generations boundaries inputs)
        add_test(NAME detector_${group} COMMAND telemetry_detector_tests ${group})
        set_tests_properties(detector_${group} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(telemetry_scheduling_tests tests/scheduling_tests.cpp)
    target_link_libraries(telemetry_scheduling_tests PRIVATE telemetry_core)
    telemetry_configure_target(telemetry_scheduling_tests)
    foreach(group IN ITEMS rr ll integration availability numeric)
        add_test(NAME scheduling_${group} COMMAND telemetry_scheduling_tests ${group})
        set_tests_properties(scheduling_${group} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(telemetry_task_store_tests tests/task_store_tests.cpp)
    target_link_libraries(telemetry_task_store_tests PRIVATE telemetry_core)
    telemetry_configure_target(telemetry_task_store_tests)
    foreach(group IN ITEMS queue results retries inputs audit)
        add_test(NAME tasks_${group} COMMAND telemetry_task_store_tests ${group})
        set_tests_properties(tasks_${group} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(telemetry_common_tests tests/common_tests.cpp)
    target_link_libraries(telemetry_common_tests PRIVATE telemetry_common)
    telemetry_configure_target(telemetry_common_tests)
    foreach(group IN ITEMS time numeric ids)
        add_test(NAME common_${group} COMMAND telemetry_common_tests ${group})
        set_tests_properties(common_${group} PROPERTIES TIMEOUT 20)
    endforeach()

    add_executable(telemetry_build_smoke tests/build_smoke.cpp)
    target_link_libraries(telemetry_build_smoke PRIVATE telemetry_io)
    telemetry_use_json(telemetry_build_smoke)
    telemetry_configure_target(telemetry_build_smoke)
    add_test(NAME build_smoke COMMAND telemetry_build_smoke)
    set_tests_properties(build_smoke PROPERTIES TIMEOUT 20)

    function(telemetry_cli_test name argument expected_exit expected_pattern)
        add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=$<TARGET_FILE:telemetry_sim>"
            "-DARGUMENT=${argument}"
            "-DEXPECTED_EXIT=${expected_exit}"
            "-DEXPECTED_PATTERN=${expected_pattern}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli.cmake")
        set_tests_properties(${name} PROPERTIES TIMEOUT 15)
    endfunction()
    telemetry_cli_test(cli_help --help 0 "stage ${TELEMETRY_IMPLEMENTATION_STAGE}")
    telemetry_cli_test(cli_version --version 0 "telemetry_sim ${PROJECT_VERSION}")
    telemetry_cli_test(cli_build_info --build-info 0 "nlohmann_json")
    telemetry_cli_test(cli_no_arguments "" 2 "Usage:")
    telemetry_cli_test(cli_unknown_option --unknown 2 "Unknown or unsupported")
    telemetry_cli_test(cli_missing_run_options run 2 "Missing required option")
    add_test(NAME cli_extra_argument COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:telemetry_sim>"
        "-DARGUMENT=--help;extra" "-DEXPECTED_EXIT=2"
        "-DEXPECTED_PATTERN=Unknown or unsupported"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli.cmake")
    set_tests_properties(cli_extra_argument PROPERTIES TIMEOUT 15)
endif()
