# The fast vanity search's parts (tools/vanity/vanity.h) against OpenSSL and the core, and the
# program itself, with no Qt (the harness is bmc/test.h).
add_executable(vanity_tests ../bmclient/main.cpp vanity_test.cpp)
target_include_directories(vanity_tests PRIVATE ${CMAKE_SOURCE_DIR}/tools/vanity)
target_link_libraries(vanity_tests PRIVATE bmclient)
target_compile_options(vanity_tests PRIVATE -Wno-deprecated-declarations)
target_compile_definitions(vanity_tests PRIVATE BM_VANITY_BIN="$<TARGET_FILE:bm_vanity>")
add_dependencies(vanity_tests bm_vanity)
add_test(NAME vanity COMMAND vanity_tests)
set_tests_properties(vanity PROPERTIES TIMEOUT 300)
