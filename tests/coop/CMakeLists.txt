cmake_minimum_required(VERSION 3.12)
project(pvz_coop_session_tests LANGUAGES CXX)

enable_testing()
add_executable(coop-session-tests CoopSessionTests.cpp ../../src/Coop/CoopSession.cpp)
target_compile_features(coop-session-tests PRIVATE cxx_std_20)
set_target_properties(coop-session-tests PROPERTIES CXX_EXTENSIONS OFF)
add_test(NAME coop-session-tests COMMAND coop-session-tests)
