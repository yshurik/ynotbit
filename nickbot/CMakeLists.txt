# nickbot: the nicknames service, in Go on libbmbot (the C ABI of bmbot/bmbot.h, through cgo).
# Opt-in (-DYNOTBIT_NICKBOT=ON): the app's own builds need no Go. Three things exist when it is on:
# the target nickbot_go (the binary, nickbot/nickbot in the build tree), the test nickbot-go (the Go
# tests, with the library) and the test nickbot-e2e (the binary over a real bmnode).
find_program(GO_EXECUTABLE go)
if(NOT GO_EXECUTABLE)
 message(FATAL_ERROR "YNOTBIT_NICKBOT is ON but Go was not found")
endif()
# The library is found by -L at build time and by rpath at run time, from where this tree built it.
set(NICKBOT_ENV CGO_ENABLED=1
 "CGO_LDFLAGS=-L$<TARGET_FILE_DIR:bmbot> -Wl,-rpath,$<TARGET_FILE_DIR:bmbot>")
add_custom_target(nickbot_go ALL
 COMMAND ${CMAKE_COMMAND} -E env ${NICKBOT_ENV} ${GO_EXECUTABLE} build -tags libbmbot -trimpath
         -o ${CMAKE_CURRENT_BINARY_DIR}/nickbot ./cmd/nickbot
 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
 DEPENDS bmbot
 COMMENT "Building nickbot (Go)"
 VERBATIM)
add_test(NAME nickbot-go
 COMMAND ${CMAKE_COMMAND} -E env ${NICKBOT_ENV} ${GO_EXECUTABLE} test -tags libbmbot -count=1 ./...
 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
set_tests_properties(nickbot-go PROPERTIES TIMEOUT 600)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
 add_test(NAME nickbot-e2e
  COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_SOURCE_DIR}/tests/nickbot/nickbot_e2e_test.py
          ${CMAKE_CURRENT_BINARY_DIR}/nickbot $<TARGET_FILE:bmnode>)
 set_tests_properties(nickbot-e2e PROPERTIES TIMEOUT 900 ENVIRONMENT "BMBOT_LIB=$<TARGET_FILE:bmbot>")
endif()
