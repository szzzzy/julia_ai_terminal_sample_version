cmake_minimum_required(VERSION 3.16)
project(julia_host_tests C)
enable_testing()
add_executable(test_wss_auth_policy test_wss_auth_policy.c)
target_include_directories(test_wss_auth_policy PRIVATE ../../main/voice)
add_test(NAME wss_auth_policy COMMAND test_wss_auth_policy)
find_path(CJSON_DIR cJSON.h
    HINTS "${IDF_PATH}/components/json/cJSON" "$ENV{IDF_PATH}/components/json/cJSON")
if(CJSON_DIR)
    add_executable(test_voice_control_guard test_voice_control_guard.c
        ../../main/voice/voice_control_guard.c "${CJSON_DIR}/cJSON.c")
    target_include_directories(test_voice_control_guard PRIVATE ../../main/voice "${CJSON_DIR}")
    if(NOT WIN32)
        target_link_libraries(test_voice_control_guard PRIVATE m)
    endif()
    add_test(NAME voice_control_guard COMMAND test_voice_control_guard)
    add_executable(test_voice_state_sync test_voice_state_sync.c
        ../../main/voice/voice_state_sync.c ../../main/fsm/julia_fsm.c "${CJSON_DIR}/cJSON.c")
    target_include_directories(test_voice_state_sync PRIVATE state_sync_stubs stubs
        ../../main/voice ../../main/fsm ../../main/ota "${CJSON_DIR}")
    set_property(TARGET test_voice_state_sync PROPERTY C_STANDARD 99)
    if(NOT WIN32)
        target_link_libraries(test_voice_state_sync PRIVATE m)
    endif()
    add_test(NAME voice_state_sync COMMAND test_voice_state_sync)
    add_executable(test_voice_state_sync_strict test_voice_state_sync.c
        ../../main/voice/voice_state_sync.c ../../main/fsm/julia_fsm.c "${CJSON_DIR}/cJSON.c")
    target_compile_definitions(test_voice_state_sync_strict PRIVATE CONFIG_JULIA_MULTI_DEVICE_ENABLE=1)
    target_include_directories(test_voice_state_sync_strict PRIVATE state_sync_stubs stubs
        ../../main/voice ../../main/fsm ../../main/ota "${CJSON_DIR}")
    if(NOT WIN32)
        target_link_libraries(test_voice_state_sync_strict PRIVATE m)
    endif()
    add_test(NAME voice_state_sync_strict COMMAND test_voice_state_sync_strict)
else()
    message(STATUS "Set IDF_PATH to include the cloud state sync regression test")
endif()
add_executable(test_pcm_buffer test_pcm_buffer.c ../../main/voice/pcm_buffer.c)
target_include_directories(test_pcm_buffer PRIVATE ../../main/voice)
set_property(TARGET test_pcm_buffer PROPERTY C_STANDARD 99)
add_test(NAME pcm_buffer COMMAND test_pcm_buffer)
add_executable(test_voice_playback test_voice_playback.c
    ../../main/voice/voice_playback.c ../../main/voice/pcm_buffer.c)
target_include_directories(test_voice_playback PRIVATE stubs ../../main/voice
    ../../components/julia_board_audio/include)
set_property(TARGET test_voice_playback PROPERTY C_STANDARD 99)
if(NOT WIN32)
    target_link_libraries(test_voice_playback PRIVATE m)
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
set(JULIA_CLOUD_SOURCE_DIR "" CACHE PATH "Cloud api_server source for offline firmware/cloud interop")
if(Python3_Interpreter_FOUND)
    if(CJSON_DIR)
        add_test(NAME multidevice_routing COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_multidevice_routing.py"
            --cc "${CMAKE_C_COMPILER}" --cjson "${CJSON_DIR}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/multidevice-routing")
        if(JULIA_CLOUD_SOURCE_DIR)
            if(NOT EXISTS "${JULIA_CLOUD_SOURCE_DIR}/server/control_protocol.py")
                message(FATAL_ERROR "JULIA_CLOUD_SOURCE_DIR must contain the control-v1 cloud server")
            endif()
            add_test(NAME cross_control_protocol COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/test_cross_control_protocol.py"
                --cloud-root "${JULIA_CLOUD_SOURCE_DIR}" --cc "${CMAKE_C_COMPILER}"
                --cjson "${CJSON_DIR}" --out "${CMAKE_CURRENT_BINARY_DIR}/cross-control")
        endif()
    endif()
    add_test(NAME fault_restart_prompt
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_fault_restart_prompt.py"
            --cc "${CMAKE_C_COMPILER}" --out "${CMAKE_CURRENT_BINARY_DIR}/fault-prompt")
    add_test(NAME ota_tls_memory
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_ota_tls_memory.py"
            --cc "${CMAKE_C_COMPILER}" --out "${CMAKE_CURRENT_BINARY_DIR}/ota-memory")
    foreach(recovery_case backlight rtc display mqtt fsm wifi_profiles offline_ui boot storage fault voice)
        add_test(NAME recovery_${recovery_case}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_recovery_paths.py"
                --cc "${CMAKE_C_COMPILER}" --out "${CMAKE_CURRENT_BINARY_DIR}/recovery"
                --case "${recovery_case}")
    endforeach()
endif()
add_test(NAME voice_playback COMMAND test_voice_playback)

add_executable(test_julia_fsm test_julia_fsm.c ../../main/fsm/julia_fsm.c)
target_include_directories(test_julia_fsm PRIVATE stubs ../../main/fsm)
set_property(TARGET test_julia_fsm PROPERTY C_STANDARD 99)
add_test(NAME julia_fsm COMMAND test_julia_fsm)

add_executable(test_transport_disconnect_fsm test_transport_disconnect_fsm.c
    ../../main/fsm/julia_fsm.c)
target_include_directories(test_transport_disconnect_fsm PRIVATE stubs ../../main/fsm)
set_property(TARGET test_transport_disconnect_fsm PROPERTY C_STANDARD 99)
add_test(NAME transport_disconnect_fsm COMMAND test_transport_disconnect_fsm)

add_executable(test_wss_tx_writer test_wss_tx_writer.c
    ../../main/voice/wss_tx_writer.c)
target_include_directories(test_wss_tx_writer PRIVATE ../../main/voice)
set_property(TARGET test_wss_tx_writer PROPERTY C_STANDARD 99)
add_test(NAME wss_tx_writer COMMAND test_wss_tx_writer)

add_executable(test_voice_uplink_ring test_voice_uplink_ring.c
    ../../main/voice/voice_uplink_ring.c)
target_include_directories(test_voice_uplink_ring PRIVATE ../../main/voice)
set_property(TARGET test_voice_uplink_ring PROPERTY C_STANDARD 99)
add_test(NAME voice_uplink_ring COMMAND test_voice_uplink_ring)

add_executable(test_voice_uplink_pump test_voice_uplink_pump.c
    ../../main/voice/voice_uplink_pump.c
    ../../main/voice/voice_uplink_ring.c)
target_include_directories(test_voice_uplink_pump PRIVATE ../../main/voice)
set_property(TARGET test_voice_uplink_pump PROPERTY C_STANDARD 99)
add_test(NAME voice_uplink_pump COMMAND test_voice_uplink_pump)

add_executable(test_voice_session_recovery test_voice_session_recovery.c
    ../../main/voice/voice_uplink_pump.c
    ../../main/voice/voice_uplink_ring.c)
target_include_directories(test_voice_session_recovery PRIVATE ../../main/voice)
set_property(TARGET test_voice_session_recovery PROPERTY C_STANDARD 99)
add_test(NAME voice_session_recovery COMMAND test_voice_session_recovery)

add_executable(test_display_ownership test_display_ownership.c)
target_compile_definitions(test_display_ownership PRIVATE
    REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")
set_property(TARGET test_display_ownership PROPERTY C_STANDARD 99)
add_test(NAME display_ownership COMMAND test_display_ownership)

add_executable(test_rtc_calendar test_rtc_calendar.c ../../main/hardware/rtc_calendar.c)
target_include_directories(test_rtc_calendar PRIVATE stubs ../../main/hardware)
set_property(TARGET test_rtc_calendar PROPERTY C_STANDARD 99)
add_test(NAME rtc_calendar COMMAND test_rtc_calendar)

find_path(HTTP_PARSER_DIR http_parser.h
    HINTS "${IDF_PATH}/components/http_parser" "$ENV{IDF_PATH}/components/http_parser")
if(HTTP_PARSER_DIR)
    add_executable(test_download_protocol test_download_protocol.c
        ../../main/network/download_protocol.c "${HTTP_PARSER_DIR}/http_parser.c")
    target_include_directories(test_download_protocol PRIVATE ../../main/network "${HTTP_PARSER_DIR}")
    # TinyCC provides stdint.h; select it instead of http_parser's old MSVC typedef fallback.
    if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "TinyCC")
        target_compile_definitions(test_download_protocol PRIVATE __MINGW32__)
    endif()
    set_property(TARGET test_download_protocol PROPERTY C_STANDARD 99)
    add_test(NAME download_protocol COMMAND test_download_protocol)
else()
    message(STATUS "Set IDF_PATH to include the download protocol regression test")
endif()
