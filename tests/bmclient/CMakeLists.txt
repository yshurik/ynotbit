# Tests of bmclient with no Qt (the harness is bmc/test.h). One program, one ctest entry.
add_executable(bmclient_tests main.cpp foundation_test.cpp text_test.cpp)
target_link_libraries(bmclient_tests PRIVATE bmclient)
add_test(NAME bmclient COMMAND bmclient_tests)
