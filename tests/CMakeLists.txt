# tests/CMakeLists.txt — doctest unit testy
add_executable(ithaca_tests test_log.cpp)
target_link_libraries(ithaca_tests PRIVATE ithaca_core doctest)
add_test(NAME ithaca_tests COMMAND ithaca_tests)

add_executable(test_wav_reader test_wav_reader.cpp)
target_link_libraries(test_wav_reader PRIVATE ithaca_core doctest)
add_test(NAME test_wav_reader COMMAND test_wav_reader)
