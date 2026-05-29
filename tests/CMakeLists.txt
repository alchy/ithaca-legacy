# tests/CMakeLists.txt — doctest unit testy
add_executable(ithaca_tests test_log.cpp)
target_link_libraries(ithaca_tests PRIVATE ithaca_core doctest)
add_test(NAME ithaca_tests COMMAND ithaca_tests)
