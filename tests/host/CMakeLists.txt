cmake_minimum_required(VERSION 3.16)
project(julia_host_tests C)
enable_testing()
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
add_test(NAME voice_playback COMMAND test_voice_playback)

add_executable(test_julia_fsm test_julia_fsm.c ../../main/fsm/julia_fsm.c)
target_include_directories(test_julia_fsm PRIVATE stubs ../../main/fsm)
set_property(TARGET test_julia_fsm PROPERTY C_STANDARD 99)
add_test(NAME julia_fsm COMMAND test_julia_fsm)

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
