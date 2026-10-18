# bmbot: a bot library with a C ABI over bmclient, and the Qt-free chat_responder on top of it.
# POSIX only for now: the node supervisor and the data directory lock are.
# The runtime and the ABI, once, for the shared library and the static one.
add_library(bmbot_objects OBJECT bot.cpp bmbot.cpp)
target_include_directories(bmbot_objects PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bmbot_objects PUBLIC bmclient ${CMAKE_DL_LIBS})
target_compile_options(bmbot_objects PRIVATE -fvisibility=hidden)

add_library(bmbot SHARED $<TARGET_OBJECTS:bmbot_objects>)
target_link_libraries(bmbot PRIVATE bmbot_objects)
target_include_directories(bmbot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(bmbot PROPERTIES OUTPUT_NAME bmbot VERSION 1.0.0 SOVERSION 1)

add_library(bmbot_static STATIC $<TARGET_OBJECTS:bmbot_objects>)
target_link_libraries(bmbot_static PUBLIC bmbot_objects)
set_target_properties(bmbot_static PROPERTIES OUTPUT_NAME bmbot)

# cmake --install: the shared library (it holds bmclient; the static one does not hold the
# libraries below it), its header and a pkg-config file. chat_responder and bmnode are installed by
# the top-level file.
include(GNUInstallDirs)
install(TARGETS bmbot LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES bmbot.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
configure_file(bmbot.pc.in ${CMAKE_CURRENT_BINARY_DIR}/bmbot.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bmbot.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
