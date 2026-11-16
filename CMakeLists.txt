cmake_minimum_required(VERSION 3.25)

# Set the deployment target before compiler detection; newer APIs require guards.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architecture")
project(VTEnc VERSION 0.1.0 LANGUAGES CXX OBJCXX)

if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "VTEnc requires macOS and the Apple SDK.")
endif()
if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "VTEnc supports only native Apple Silicon arm64 builds.")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR
   NOT CMAKE_OBJCXX_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR "Use Apple Clang from Xcode or Command Line Tools.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(vtenc_options INTERFACE)
target_compile_options(vtenc_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Werror=unguarded-availability
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")

add_library(vtenc_core STATIC src/core/command_line.cpp)
target_include_directories(vtenc_core PUBLIC include)
target_link_libraries(vtenc_core PRIVATE vtenc_options)

add_library(vtenc_apple STATIC
    src/apple/encoder.cpp
    src/apple/bitstream_output.cpp
    src/apple/capability_probe.cpp
    src/apple/metal_device.mm)
target_include_directories(vtenc_apple PUBLIC include)
target_link_libraries(vtenc_apple PRIVATE vtenc_options)
foreach(framework CoreFoundation CoreMedia CoreVideo VideoToolbox Foundation Metal)
    find_library(VTENC_${framework}_LIBRARY ${framework} REQUIRED)
    target_link_libraries(vtenc_apple PRIVATE "${VTENC_${framework}_LIBRARY}")
endforeach()

add_executable(vtenc src/cli/main.cpp)
target_compile_definitions(vtenc PRIVATE VTENC_VERSION="${PROJECT_VERSION}")
target_link_libraries(vtenc PRIVATE vtenc_core vtenc_apple vtenc_options)
install(TARGETS vtenc RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME vtenc.cli
        COMMAND "${CMAKE_COMMAND}" -DVTENC=$<TARGET_FILE:vtenc>
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake")
    set_tests_properties(vtenc.cli PROPERTIES LABELS "cli" TIMEOUT 15)
    add_executable(vtenc_bitstream_test tests/bitstream_test.cpp)
    target_include_directories(vtenc_bitstream_test PRIVATE src/apple)
    target_link_libraries(vtenc_bitstream_test PRIVATE vtenc_apple vtenc_core vtenc_options)
    add_test(NAME vtenc.bitstream COMMAND vtenc_bitstream_test "${CMAKE_CURRENT_BINARY_DIR}/test-output")
    set_tests_properties(vtenc.bitstream PROPERTIES LABELS "bitstream" TIMEOUT 15)
endif()
