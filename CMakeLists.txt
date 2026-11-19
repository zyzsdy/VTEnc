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

# Link the installed libraries, never invoke the ffmpeg CLI at runtime.
# A custom installation can be selected using CMAKE_PREFIX_PATH.
find_path(VTENC_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
foreach(component avformat avcodec avutil swresample)
    find_library(VTENC_${component}_LIBRARY NAMES ${component} REQUIRED)
endforeach()

find_path(VTENC_ASS_INCLUDE_DIR ass/ass.h REQUIRED)
find_library(VTENC_ASS_LIBRARY NAMES ass REQUIRED)
find_package(Freetype REQUIRED)

add_library(vtenc_options INTERFACE)
target_compile_options(vtenc_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Werror=unguarded-availability
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")

add_library(vtenc_core STATIC src/core/command_line.cpp src/core/help.cpp src/core/colorspace.cpp src/core/status.cpp src/core/subtitle.cpp)
target_include_directories(vtenc_core PUBLIC include)
target_link_libraries(vtenc_core PRIVATE vtenc_options)

# Embed source so installed CLI needs neither shader files nor the offline Metal compiler.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/apple/shaders/filter.metal src/apple/shaders/colorspace.metal src/apple/shaders/ivtc.metal src/apple/shaders/subtitle.metal src/apple/shaders/yadif.metal)
file(READ src/apple/shaders/filter.metal VTENC_GPU_SHADER)
file(READ src/apple/shaders/colorspace.metal VTENC_COLOR_SHADER)
file(READ src/apple/shaders/ivtc.metal VTENC_IVTC_SHADER)
file(READ src/apple/shaders/subtitle.metal VTENC_SUBTITLE_SHADER)
file(READ src/apple/shaders/yadif.metal VTENC_YADIF_SHADER)
string(APPEND VTENC_GPU_SHADER "\n${VTENC_IVTC_SHADER}\n${VTENC_SUBTITLE_SHADER}\n${VTENC_YADIF_SHADER}")
string(PREPEND VTENC_GPU_SHADER "${VTENC_COLOR_SHADER}\n")
configure_file(src/apple/shaders/filter_shader.hpp.in generated/filter_shader.hpp @ONLY)

add_library(vtenc_apple STATIC
    src/apple/encoder.cpp
    src/apple/encoder_controls.cpp
    src/apple/subtitle.cpp
    src/apple/output_metadata.cpp
    src/apple/transcode.cpp
    src/apple/timeline.cpp
    src/apple/audio_transcode.cpp
    src/apple/aac_edit.cpp
    src/apple/software_decode.cpp
    src/apple/filter_plan.cpp
    src/apple/bob.cpp
    src/apple/yadif.cpp
    src/apple/yadif.mm
    src/apple/ivtc.cpp
    src/apple/rff.cpp
    src/apple/field_frames.cpp
    src/apple/cadence_parser.cpp
    src/apple/hevc_cadence.cpp
    src/apple/gpu_filter.mm
    src/apple/source_format.cpp
    src/apple/avformat_input.cpp
    src/apple/avformat_output.cpp
    src/apple/bitstream_output.cpp
    src/apple/capability_probe.cpp
    src/apple/metal_device.mm)
target_include_directories(vtenc_apple PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(vtenc_apple SYSTEM PUBLIC "${VTENC_FFMPEG_INCLUDE_DIR}" "${VTENC_ASS_INCLUDE_DIR}")
target_link_libraries(vtenc_apple PRIVATE vtenc_options vtenc_core Freetype::Freetype "${VTENC_ASS_LIBRARY}"
    "${VTENC_avformat_LIBRARY}" "${VTENC_avcodec_LIBRARY}" "${VTENC_avutil_LIBRARY}" "${VTENC_swresample_LIBRARY}")
foreach(framework CoreFoundation CoreMedia CoreVideo VideoToolbox AudioToolbox Foundation Metal)
    find_library(VTENC_${framework}_LIBRARY ${framework} REQUIRED)
    target_link_libraries(vtenc_apple PRIVATE "${VTENC_${framework}_LIBRARY}")
endforeach()

add_executable(vtenc src/cli/main.cpp)
target_compile_definitions(vtenc PRIVATE VTENC_VERSION="${PROJECT_VERSION}")
target_compile_definitions(vtenc PRIVATE
    VTENC_BUILD_INFO="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, C++${CMAKE_CXX_STANDARD}, $<CONFIG>")
target_link_libraries(vtenc PRIVATE vtenc_apple vtenc_options)
install(TARGETS vtenc RUNTIME DESTINATION bin)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION share/doc/vtenc)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME vtenc.cli
        COMMAND "${CMAKE_COMMAND}" -DVTENC=$<TARGET_FILE:vtenc>
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_smoke.cmake")
    set_tests_properties(vtenc.cli PROPERTIES LABELS "cli" TIMEOUT 15)
    add_executable(vtenc_encoder_controls_test tests/encoder_controls_test.cpp)
    target_include_directories(vtenc_encoder_controls_test PRIVATE src/apple)
    target_link_libraries(vtenc_encoder_controls_test PRIVATE vtenc_apple vtenc_options)
    add_test(NAME vtenc.encoder-controls COMMAND vtenc_encoder_controls_test)
    set_tests_properties(vtenc.encoder-controls PROPERTIES LABELS "encoder" TIMEOUT 15)
    add_executable(vtenc_output_metadata_test tests/output_metadata_test.cpp)
    target_include_directories(vtenc_output_metadata_test PRIVATE src/apple)
    target_link_libraries(vtenc_output_metadata_test PRIVATE vtenc_apple vtenc_options)
    add_test(NAME vtenc.output-metadata COMMAND vtenc_output_metadata_test)
    set_tests_properties(vtenc.output-metadata PROPERTIES LABELS "metadata" TIMEOUT 15)
    add_executable(vtenc_status_test tests/status_test.cpp)
    target_link_libraries(vtenc_status_test PRIVATE vtenc_core vtenc_options)
    add_test(NAME vtenc.status COMMAND vtenc_status_test)
    set_tests_properties(vtenc.status PROPERTIES LABELS "status" TIMEOUT 15)
    add_executable(vtenc_decode_policy_test tests/decode_policy_test.cpp)
    target_include_directories(vtenc_decode_policy_test PRIVATE src/apple)
    target_link_libraries(vtenc_decode_policy_test PRIVATE vtenc_core vtenc_options)
    add_test(NAME vtenc.decode-policy COMMAND vtenc_decode_policy_test)
    set_tests_properties(vtenc.decode-policy PROPERTIES LABELS "decode" TIMEOUT 15)
    add_executable(vtenc_bitstream_test tests/bitstream_test.cpp)
    target_include_directories(vtenc_bitstream_test PRIVATE src/apple)
    target_link_libraries(vtenc_bitstream_test PRIVATE vtenc_apple vtenc_options)
    add_test(NAME vtenc.bitstream COMMAND vtenc_bitstream_test "${CMAKE_CURRENT_BINARY_DIR}/test-output")
    set_tests_properties(vtenc.bitstream PROPERTIES LABELS "bitstream" TIMEOUT 15)
    add_executable(vtenc_colorspace_test tests/colorspace_test.cpp)
    target_include_directories(vtenc_colorspace_test PRIVATE src/apple)
    target_link_libraries(vtenc_colorspace_test PRIVATE vtenc_apple vtenc_options)
    add_test(NAME vtenc.colorspace COMMAND vtenc_colorspace_test)
    set_tests_properties(vtenc.colorspace PROPERTIES LABELS "colorspace" TIMEOUT 15)
    # Explicit hardware executable: intentionally excluded from default CTest.
    add_executable(vtenc_subtitle_test tests/subtitle_hardware.cpp)
    target_include_directories(vtenc_subtitle_test PRIVATE src/apple)
    target_link_libraries(vtenc_subtitle_test PRIVATE vtenc_apple vtenc_options)
    add_executable(vtenc_gpu_filter_test tests/gpu_filter_hardware.cpp)
    target_include_directories(vtenc_gpu_filter_test PRIVATE src/apple)
    target_link_libraries(vtenc_gpu_filter_test PRIVATE vtenc_apple vtenc_options)
    add_executable(vtenc_yadif_config_test tests/yadif_config_test.cpp)
    target_include_directories(vtenc_yadif_config_test PRIVATE src/apple)
    target_link_libraries(vtenc_yadif_config_test PRIVATE vtenc_apple vtenc_options)
    add_test(NAME vtenc.yadif-config COMMAND vtenc_yadif_config_test)
    set_tests_properties(vtenc.yadif-config PROPERTIES LABELS "timeline" TIMEOUT 15)
    add_executable(vtenc_yadif_test tests/yadif_hardware.cpp)
    target_include_directories(vtenc_yadif_test PRIVATE src/apple)
    target_link_libraries(vtenc_yadif_test PRIVATE vtenc_apple vtenc_options)
    add_executable(vtenc_ivtc_test tests/ivtc_hardware.cpp)
    target_include_directories(vtenc_ivtc_test PRIVATE src/apple)
    target_link_libraries(vtenc_ivtc_test PRIVATE vtenc_apple vtenc_options)
    # Explicit fixture generator, no video encoder or automatic hardware tests.
    add_executable(vtenc_hevc_rff_remux tests/hevc_rff_remux.cpp)
    target_include_directories(vtenc_hevc_rff_remux PRIVATE src/apple)
    target_link_libraries(vtenc_hevc_rff_remux PRIVATE vtenc_apple vtenc_options)
    add_executable(vtenc_hevc_cadence_test tests/hevc_cadence_test.cpp)
    target_include_directories(vtenc_hevc_cadence_test PRIVATE src/apple)
    target_link_libraries(vtenc_hevc_cadence_test PRIVATE vtenc_apple vtenc_options)
    add_test(NAME vtenc.hevc-cadence COMMAND vtenc_hevc_cadence_test)
    set_tests_properties(vtenc.hevc-cadence PROPERTIES LABELS "cadence" TIMEOUT 15)
    add_executable(vtenc_cfr_test tests/cfr_test.cpp)
    target_include_directories(vtenc_cfr_test PRIVATE src/apple)
    target_link_libraries(vtenc_cfr_test PRIVATE vtenc_apple vtenc_options)
    add_test(NAME vtenc.cfr COMMAND vtenc_cfr_test)
    set_tests_properties(vtenc.cfr PROPERTIES LABELS "timeline" TIMEOUT 15)
    add_executable(vtenc_timeline_test tests/timeline_test.cpp)
    target_include_directories(vtenc_timeline_test PRIVATE src/apple)
    target_link_libraries(vtenc_timeline_test PRIVATE vtenc_apple vtenc_options)
    add_test(NAME vtenc.timeline COMMAND vtenc_timeline_test)
    set_tests_properties(vtenc.timeline PROPERTIES LABELS "timeline" TIMEOUT 15)
endif()
