# tests/CMakeLists.txt — doctest unit testy
add_executable(ithaca_tests test_log.cpp)
target_link_libraries(ithaca_tests PRIVATE ithaca_core doctest)
add_test(NAME ithaca_tests COMMAND ithaca_tests)

add_executable(test_wav_reader test_wav_reader.cpp)
target_link_libraries(test_wav_reader PRIVATE ithaca_core doctest)
add_test(NAME test_wav_reader COMMAND test_wav_reader)

add_executable(test_bank_index test_bank_index.cpp)
target_link_libraries(test_bank_index PRIVATE ithaca_core doctest)
add_test(NAME test_bank_index COMMAND test_bank_index)

add_executable(test_sample_loader test_sample_loader.cpp)
target_link_libraries(test_sample_loader PRIVATE ithaca_core doctest)
add_test(NAME test_sample_loader COMMAND test_sample_loader)
