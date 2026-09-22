cmake_minimum_required(VERSION 3.21)

add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE symcore)

add_test(NAME core COMMAND test_core)

add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE symcore)

add_test(NAME integration COMMAND test_integration)
