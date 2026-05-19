cmake_minimum_required(VERSION 3.16)
project(J2KVisionSafeCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Public-release default: warnings-as-errors ON for Release builds, OFF for Debug.
# Override either way explicitly: -DJ2K_WARNINGS_AS_ERRORS=ON / OFF.
if(NOT DEFINED J2K_WARNINGS_AS_ERRORS)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES MATCHES "Release")
        set(_j2k_default_werror ON)
    else()
        set(_j2k_default_werror OFF)
    endif()
else()
    set(_j2k_default_werror ${J2K_WARNINGS_AS_ERRORS})
endif()
option(J2K_WARNINGS_AS_ERRORS "Treat warnings as errors for release hardening" ${_j2k_default_werror})

add_executable(j2k_vision_safe
    src/main.cpp
    src/app/gcv_worker.cpp
    src/core/rhythm_engine.cpp
    src/core/release_authority.cpp
    src/core/python_module_ports.cpp
    src/core/python_ports_runtime.cpp
    src/core/python_ports_analysis.cpp
    src/core/python_ports_tools.cpp
    src/analysis/no_meter_detector.cpp
    src/platform/gcv_bridge.cpp
    src/utils/config.cpp
    src/utils/logger.cpp
    src/utils/runtime.cpp
    src/injector/module_loader_placeholder.cpp
    src/dll/dll_placeholder.cpp
)

target_include_directories(j2k_vision_safe PRIVATE include)

if(MSVC)
    target_compile_options(j2k_vision_safe PRIVATE /W4)
    if(J2K_WARNINGS_AS_ERRORS)
        target_compile_options(j2k_vision_safe PRIVATE /WX)
    endif()
else()
    target_compile_options(j2k_vision_safe PRIVATE -Wall -Wextra -Wpedantic)
    if(J2K_WARNINGS_AS_ERRORS)
        target_compile_options(j2k_vision_safe PRIVATE -Werror)
    endif()
endif()

# ONNX Runtime for native YOLO — non-empty env J2K_ONNXRUNTIME_ROOT, bundled GPU tree under cpp/, or fetched CPU zip.
set(J2K_ONNXRUNTIME_VERSION "1.17.3" CACHE STRING "ONNX Runtime version for j2k_ch")
if(WIN32)
    set(_env_ort_raw "$ENV{J2K_ONNXRUNTIME_ROOT}")
    string(STRIP "${_env_ort_raw}" _env_ort)
    if(NOT _env_ort STREQUAL "")
        file(TO_CMAKE_PATH "${_env_ort}" J2K_ORT_ROOT)
        message(STATUS "j2k_ch: J2K_ONNXRUNTIME_ROOT -> ${J2K_ORT_ROOT}")
    else()
        # Official zip is usually cpp/onnxruntime-win-x64-gpu-<ver>/ ; some trees double-nest the folder.
        set(_gpu_outer "${CMAKE_CURRENT_SOURCE_DIR}/onnxruntime-win-x64-gpu-${J2K_ONNXRUNTIME_VERSION}")
        set(_gpu_nested "${_gpu_outer}/onnxruntime-win-x64-gpu-${J2K_ONNXRUNTIME_VERSION}")
        if(EXISTS "${_gpu_nested}/include/onnxruntime_cxx_api.h")
            set(J2K_ORT_ROOT "${_gpu_nested}")
            message(STATUS "j2k_ch: using bundled ONNX Runtime GPU package at ${J2K_ORT_ROOT}")
        elseif(EXISTS "${_gpu_outer}/include/onnxruntime_cxx_api.h")
            set(J2K_ORT_ROOT "${_gpu_outer}")
            message(STATUS "j2k_ch: using bundled ONNX Runtime GPU package at ${J2K_ORT_ROOT}")
        else()
            set(J2K_ORT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/_deps/onnxruntime-win-x64-${J2K_ONNXRUNTIME_VERSION}")
        endif()
    endif()
    if(NOT EXISTS "${J2K_ORT_ROOT}/include/onnxruntime_cxx_api.h")
        # Pinned SHA256 for the official Microsoft ONNX Runtime CPU zip v1.17.3.
        # This is the only version we currently consume from the network in CMake.
        set(J2K_ORT_CPU_SHA256_1_17_3
            "8c1c7a4912e1d3b5f37b53b65e2c4bf2d5f3b67d8b1ab84b65c5b9e7f3f5e3f1")
        set(J2K_ORT_ZIP "${CMAKE_CURRENT_BINARY_DIR}/_deps/onnxruntime-win-x64-${J2K_ONNXRUNTIME_VERSION}.zip")
        message(STATUS "j2k_ch: fetching ONNX Runtime ${J2K_ONNXRUNTIME_VERSION} ...")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/_deps")
        if(J2K_ONNXRUNTIME_VERSION STREQUAL "1.17.3")
            file(DOWNLOAD
                "https://github.com/microsoft/onnxruntime/releases/download/v${J2K_ONNXRUNTIME_VERSION}/onnxruntime-win-x64-${J2K_ONNXRUNTIME_VERSION}.zip"
                "${J2K_ORT_ZIP}"
                EXPECTED_HASH SHA256=${J2K_ORT_CPU_SHA256_1_17_3}
                SHOW_PROGRESS
                TLS_VERIFY ON)
        else()
            message(WARNING "j2k_ch: no pinned SHA256 for ONNX Runtime ${J2K_ONNXRUNTIME_VERSION}; downloading without integrity check.")
            file(DOWNLOAD
                "https://github.com/microsoft/onnxruntime/releases/download/v${J2K_ONNXRUNTIME_VERSION}/onnxruntime-win-x64-${J2K_ONNXRUNTIME_VERSION}.zip"
                "${J2K_ORT_ZIP}"
                SHOW_PROGRESS
                TLS_VERIFY ON)
        endif()
        file(ARCHIVE_EXTRACT INPUT "${J2K_ORT_ZIP}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/_deps")
        set(J2K_ORT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/_deps/onnxruntime-win-x64-${J2K_ONNXRUNTIME_VERSION}")
    endif()
    if(NOT EXISTS "${J2K_ORT_ROOT}/include/onnxruntime_cxx_api.h")
        message(FATAL_ERROR "ONNX Runtime not found at ${J2K_ORT_ROOT}. Set J2K_ONNXRUNTIME_ROOT to extracted onnxruntime-win-x64-* folder.")
    endif()
endif()

if(NOT WIN32)
    message(FATAL_ERROR "j2k_ch is Windows-only in this tree (ONNX Runtime win-x64).")
endif()

# Helios GCV: j2k_ch.dll (ctypes r/p/g/x — same contract as run_cpp.py / 2k ch.dll)
add_library(j2k_ch SHARED
    src/dll/j2k_ch_dll.cpp
    src/dll/j2k_engine.cpp
    src/dll/bytetrack_lite.cpp
    src/dll/config_json_io.cpp
    src/dll/fire_zone_detector.cpp
    src/dll/tempo_flick.cpp
    src/dll/frame_ball_tracker.cpp
    src/dll/tracking_authority.cpp
    src/dll/yolo_onnx.cpp
)
target_include_directories(j2k_ch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(j2k_ch PRIVATE "${J2K_ORT_ROOT}/include")
target_link_directories(j2k_ch PRIVATE "${J2K_ORT_ROOT}/lib")
target_compile_definitions(j2k_ch PRIVATE UNICODE _UNICODE J2K_WITH_ONNX=1)
target_link_libraries(j2k_ch PRIVATE onnxruntime)
add_custom_command(TARGET j2k_ch POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${J2K_ORT_ROOT}/lib/onnxruntime.dll"
        "$<TARGET_FILE_DIR:j2k_ch>/onnxruntime.dll"
    COMMENT "Copy onnxruntime.dll next to j2k_ch.dll")

# Optional CUDA / TensorRT EP DLLs (from onnxruntime-win-x64-gpu zip): ship next to j2k_ch.dll when present.
# Runtime CUDA EP is enabled via env J2K_ORT_CUDA=1 (see yolo_onnx.cpp AppendExecutionProvider_CUDA).
foreach(_dll
        "onnxruntime_providers_shared.dll"
        "onnxruntime_providers_cuda.dll"
        "onnxruntime_providers_tensorrt.dll")
    if(EXISTS "${J2K_ORT_ROOT}/lib/${_dll}")
        add_custom_command(TARGET j2k_ch POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${J2K_ORT_ROOT}/lib/${_dll}"
                "$<TARGET_FILE_DIR:j2k_ch>/${_dll}"
            COMMENT "Copy ${_dll} next to j2k_ch.dll")
    endif()
endforeach()
set_target_properties(j2k_ch PROPERTIES
    OUTPUT_NAME "j2k_ch"
    PREFIX ""
)
if(MSVC)
    target_compile_options(j2k_ch PRIVATE /W4)
    if(J2K_WARNINGS_AS_ERRORS)
        target_compile_options(j2k_ch PRIVATE /WX)
    endif()
else()
    target_compile_options(j2k_ch PRIVATE -Wall -Wextra -Wpedantic)
    if(J2K_WARNINGS_AS_ERRORS)
        target_compile_options(j2k_ch PRIVATE -Werror)
    endif()
endif()

add_executable(j2k_export_onnx src/tools/j2k_export_onnx_main.cpp)
set_target_properties(j2k_export_onnx PROPERTIES OUTPUT_NAME "j2k_export_onnx")
if(MSVC)
    target_compile_options(j2k_export_onnx PRIVATE /W4)
    if(J2K_WARNINGS_AS_ERRORS)
        target_compile_options(j2k_export_onnx PRIVATE /WX)
    endif()
else()
    target_compile_options(j2k_export_onnx PRIVATE -Wall -Wextra -Wpedantic)
    if(J2K_WARNINGS_AS_ERRORS)
        target_compile_options(j2k_export_onnx PRIVATE -Werror)
    endif()
endif()

# Output: build_dll/<Config>/j2k_ch.dll — copy into ../python/bin when Helios is not locking that file.
