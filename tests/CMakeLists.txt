# tests/CMakeLists.txt — doctest unit testy
add_executable(ithaca_tests test_log.cpp)
target_link_libraries(ithaca_tests PRIVATE ithaca_core doctest)
add_test(NAME ithaca_tests COMMAND ithaca_tests)

add_executable(test_wav_reader test_wav_reader.cpp)
target_link_libraries(test_wav_reader PRIVATE ithaca_core doctest)
add_test(NAME test_wav_reader COMMAND test_wav_reader)

add_executable(test_wav_range test_wav_range.cpp)
target_link_libraries(test_wav_range PRIVATE ithaca_core doctest)
add_test(NAME test_wav_range COMMAND test_wav_range)

add_executable(test_bank_index test_bank_index.cpp)
target_link_libraries(test_bank_index PRIVATE ithaca_core doctest)
add_test(NAME test_bank_index COMMAND test_bank_index)

add_executable(test_sample_loader test_sample_loader.cpp)
target_link_libraries(test_sample_loader PRIVATE ithaca_core doctest)
add_test(NAME test_sample_loader COMMAND test_sample_loader)

add_executable(test_sample_store test_sample_store.cpp)
target_link_libraries(test_sample_store PRIVATE ithaca_core doctest)
add_test(NAME test_sample_store COMMAND test_sample_store)

add_executable(test_wav_writer test_wav_writer.cpp)
target_link_libraries(test_wav_writer PRIVATE ithaca_core doctest)
add_test(NAME test_wav_writer COMMAND test_wav_writer)

add_executable(test_patch_manager test_patch_manager.cpp)
target_link_libraries(test_patch_manager PRIVATE ithaca_core doctest)
add_test(NAME test_patch_manager COMMAND test_patch_manager)

add_executable(test_voice_pool test_voice_pool.cpp)
target_link_libraries(test_voice_pool PRIVATE ithaca_core doctest)
add_test(NAME test_voice_pool COMMAND test_voice_pool)

add_executable(test_midi_queue test_midi_queue.cpp)
target_link_libraries(test_midi_queue PRIVATE ithaca_core doctest)
add_test(NAME test_midi_queue COMMAND test_midi_queue)

add_executable(test_engine test_engine.cpp)
target_link_libraries(test_engine PRIVATE ithaca_core doctest)
add_test(NAME test_engine COMMAND test_engine)

add_executable(test_batch_renderer test_batch_renderer.cpp)
target_link_libraries(test_batch_renderer PRIVATE ithaca_core doctest)
add_test(NAME test_batch_renderer COMMAND test_batch_renderer)

add_executable(test_stream_engine test_stream_engine.cpp)
target_link_libraries(test_stream_engine PRIVATE ithaca_core doctest)
add_test(NAME test_stream_engine COMMAND test_stream_engine)

add_executable(test_long_sample_stream test_long_sample_stream.cpp)
target_link_libraries(test_long_sample_stream PRIVATE ithaca_core doctest)
add_test(NAME test_long_sample_stream COMMAND test_long_sample_stream)

add_executable(test_pedal_state test_pedal_state.cpp)
target_link_libraries(test_pedal_state PRIVATE ithaca_core doctest)
add_test(NAME test_pedal_state COMMAND test_pedal_state)

add_executable(test_harmonic_proximity test_harmonic_proximity.cpp)
target_link_libraries(test_harmonic_proximity PRIVATE ithaca_core doctest)
add_test(NAME test_harmonic_proximity COMMAND test_harmonic_proximity)

add_executable(test_resonance_engine test_resonance_engine.cpp)
target_link_libraries(test_resonance_engine PRIVATE ithaca_core doctest)
add_test(NAME test_resonance_engine COMMAND test_resonance_engine)

add_executable(test_engine_diagnostics test_engine_diagnostics.cpp)
target_link_libraries(test_engine_diagnostics PRIVATE ithaca_core doctest)
add_test(NAME test_engine_diagnostics COMMAND test_engine_diagnostics)

add_executable(test_master_peak_meter test_master_peak_meter.cpp)
target_link_libraries(test_master_peak_meter PRIVATE ithaca_core doctest)
add_test(NAME test_master_peak_meter COMMAND test_master_peak_meter)

# F8/Task5: GUI LogRingBuffer + Logger Subscriber API. Test linkuje source
# z app/gui/ napred (jen ten jeden soubor — GUI nema vlastni knihovnu, je to
# entry-point binary).
add_executable(test_log_subscriber test_log_subscriber.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/gui/log_subscriber.cpp)
target_include_directories(test_log_subscriber PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/gui)
target_link_libraries(test_log_subscriber PRIVATE ithaca_core doctest)
add_test(NAME test_log_subscriber COMMAND test_log_subscriber)
