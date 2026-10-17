# Tests of bmclient with no Qt (the harness is bmc/test.h). One program, one ctest entry.
add_executable(bmclient_tests main.cpp foundation_test.cpp text_test.cpp json_test.cpp
    chat_format_test.cpp updates_test.cpp protocol_test.cpp wire_test.cpp pow_test.cpp
    qtstream_test.cpp vault_test.cpp vault_edit_test.cpp mailbox_test.cpp mailbox_query_test.cpp
    mailbox_delivery_test.cpp mailbox_chat_test.cpp sqlite_test.cpp path_test.cpp cache_test.cpp
    delivery_test.cpp client_test.cpp scenario_test.cpp golden_test.cpp
    node_config_test.cpp node_status_test.cpp node_watch_test.cpp)
target_link_libraries(bmclient_tests PRIVATE bmclient ntb_object_db)
target_compile_definitions(bmclient_tests PRIVATE BMC_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
add_test(NAME bmclient COMMAND bmclient_tests)
# The Qt-free half of scripts/compat-check.sh: it writes a vault and a mailbox for the previous
# release to open, and prints a report of what it reads for the two to be compared.
add_executable(storage_gen storage_gen.cpp)
target_link_libraries(storage_gen PRIVATE bmclient)
