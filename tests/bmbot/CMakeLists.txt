# Tests of bmbot and of the chat bot, with no Qt (the harness is bmc/test.h). One program, one
# ctest entry; the node and responder tests are scripts.
add_executable(bmbot_tests ../bmclient/main.cpp bot_test.cpp abi_test.cpp chatbot_test.cpp
    rotating_log_test.cpp)
target_include_directories(bmbot_tests PRIVATE ${CMAKE_SOURCE_DIR}/bmbot ${CMAKE_SOURCE_DIR}/tests/bmclient)
target_link_libraries(bmbot_tests PRIVATE bmbot_static chat_bot ntb_object_db)
target_compile_definitions(bmbot_tests PRIVATE BMBOT_TEST_NODE="$<TARGET_FILE:bmnode>")
add_dependencies(bmbot_tests bmnode)
add_test(NAME bmbot COMMAND bmbot_tests)
set_tests_properties(bmbot PROPERTIES TIMEOUT 600)

# The responder over real nodes, and bmbot from Python. Python is optional in the Qt-free tree:
# without it these tests simply do not exist.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
 add_test(NAME responder-e2e COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/responder_e2e_test.py $<TARGET_FILE:chat_responder> $<TARGET_FILE:bmnode>)
 add_test(NAME responder-keys COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/responder_keys_test.py $<TARGET_FILE:chat_responder>)
 add_test(NAME bmbot-ctypes COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/ctypes_test.py)
 set_tests_properties(bmbot-ctypes PROPERTIES ENVIRONMENT
  "BMBOT_LIB=$<TARGET_FILE:bmbot>;CHAT_RESPONDER=$<TARGET_FILE:chat_responder>;BMNODE=$<TARGET_FILE:bmnode>")
 set_tests_properties(responder-e2e bmbot-ctypes PROPERTIES TIMEOUT 600)
 set_tests_properties(responder-keys PROPERTIES TIMEOUT 300)
endif()
