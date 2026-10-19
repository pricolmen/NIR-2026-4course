set(TELEMETRY_JSON_VERSION 3.12.0)
if(TELEMETRY_USE_SYSTEM_JSON)
    find_package(nlohmann_json ${TELEMETRY_JSON_VERSION} EXACT CONFIG REQUIRED)
else()
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    if(POLICY CMP0168)
        # Avoid ExternalProject's sub-build path conversions on recent CMake.
        cmake_policy(SET CMP0168 NEW)
    endif()
    set(JSON_BuildTests OFF CACHE BOOL "Disable dependency tests" FORCE)
    set(JSON_Install OFF CACHE BOOL "Disable dependency installation" FORCE)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        TIMEOUT 60 INACTIVITY_TIMEOUT 15)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

function(telemetry_use_json target)
    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json)
    # Suppress third-party-header warnings without weakening our own warnings.
    target_include_directories(${target} SYSTEM PRIVATE
        "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
endfunction()
