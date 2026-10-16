# The Qt app, its tools and its tests: everything that needs Qt. CMakeLists.txt includes this file when YNOTBIT_QT is ON.
# miniupnpc, to open the node's port on a home router (UPnP IGD). Pinned and
# checksummed; built static, nothing installed.
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(miniupnpc
 URL https://github.com/miniupnp/miniupnp/archive/refs/tags/miniupnpc_2_3_3.tar.gz
 URL_HASH SHA256=8cf2c833b3e76fc4893ff29c2a376e3394962449e5970e373c0a91421724d222
 SOURCE_SUBDIR miniupnpc)
FetchContent_MakeAvailable(miniupnpc)
add_library(bmcore src/chat_format.cpp src/chat_store.cpp src/storage.cpp src/mailbox_ops.cpp src/protocol.cpp src/protocol_wire.cpp src/key_search.cpp src/pow.cpp src/gpu_pow.cpp src/gpu_opencl.cpp src/delivery.cpp src/cache.cpp src/scanner.cpp src/updates.cpp src/message_search.cpp src/port_mapper.cpp src/port_mapper_miniupnpc.cpp)
target_include_directories(bmcore PUBLIC src)
target_link_libraries(bmcore PUBLIC Threads::Threads notbit_codec secp256k1 miniupnpc::miniupnpc Qt6::Core Qt6::Network PkgConfig::SODIUM PkgConfig::SQLCIPHER OpenSSL::Crypto ntb_object_db)
if(APPLE)
 # Proof of work on the GPU through Metal (src/gpu_metal.mm).
 enable_language(OBJCXX)
 target_sources(bmcore PRIVATE src/gpu_metal.mm)
 set_source_files_properties(src/gpu_metal.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
 target_link_libraries(bmcore PUBLIC "-framework Metal" "-framework Foundation" "-framework IOKit")
endif()
add_executable(chat_tests tests/chat_tests.cpp)
target_link_libraries(chat_tests PRIVATE bmcore)
add_test(NAME chat COMMAND chat_tests)
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE bmcore)
add_test(NAME core COMMAND core_tests)

target_compile_definitions(bmcore PUBLIC SQLITE_HAS_CODEC)
if(MSVC)
 target_compile_options(bmcore PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/msvc_ssize_t.h")
else()
 target_compile_options(bmcore PRIVATE -Wno-deprecated-declarations)
endif()

# Names for addresses: coolname's vocabulary, pinned in third_party/coolname.
set(COOLNAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/coolname)
file(GLOB COOLNAME_DATA CONFIGURE_DEPENDS ${COOLNAME_DIR}/data/*)
# coolname's code with its data: a target naming addresses needs both.
function(add_coolname target)
 target_sources(${target} PRIVATE src/coolname.cpp src/coolname.h)
 qt_add_resources(${target} "coolname_data" PREFIX "/coolname" BASE ${COOLNAME_DIR}
  FILES ${COOLNAME_DATA} ${COOLNAME_DIR}/excluded.txt)
endfunction()
qt_add_executable(ynotbit MACOSX_BUNDLE src/main.cpp src/i18n.cpp src/i18n.h src/quoting.cpp src/quoting.h src/letter_document.cpp src/letter_document.h src/letter_document.cpp src/letter_document.h src/session.cpp src/session.h
 assets/icons.qrc
 src/appearance.cpp src/appearance.h
 src/desktop_window.cpp src/desktop_window.h src/settings_pane.cpp src/settings_pane.h src/new_vault_dialog.cpp src/new_vault_dialog.h src/letter_render.cpp src/letter_render.h src/feed_view.cpp src/feed_view.h src/message_model.cpp src/message_model.h
 third_party/qrcodegen/qrcodegen.cpp third_party/qrcodegen/qrcodegen.hpp)
target_include_directories(ynotbit PRIVATE third_party/qrcodegen)
add_coolname(ynotbit)
if(APPLE)
 set_source_files_properties(assets/ynotbit.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
 target_sources(ynotbit PRIVATE assets/ynotbit.icns)
 set_target_properties(ynotbit PROPERTIES MACOSX_BUNDLE_ICON_FILE "ynotbit.icns")
endif()
if(WIN32)
 enable_language(RC)
 configure_file(assets/ynotbit.rc.in ${CMAKE_CURRENT_BINARY_DIR}/ynotbit.rc @ONLY)
 target_sources(ynotbit PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/ynotbit.rc)
 set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/ynotbit.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/ynotbit.ico")
endif()
target_link_libraries(ynotbit PRIVATE bmcore Qt6::Widgets Qt6::Network)
# A windowed app on Windows: no console window opens alongside it.
set_target_properties(ynotbit PROPERTIES WIN32_EXECUTABLE TRUE)
target_sources(ynotbit PRIVATE src/relay_bridge.cpp)
target_link_libraries(ynotbit PRIVATE notbit_engine)
set_target_properties(ynotbit PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "ynotbit" MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.yshurik.ynotbit" MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}" MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
target_compile_definitions(ynotbit PRIVATE YNOTBIT_VERSION="${PROJECT_VERSION}")
install(TARGETS ynotbit BUNDLE DESTINATION . RUNTIME DESTINATION bin)
if(UNIX AND NOT APPLE)
 include(GNUInstallDirs)
 install(FILES assets/ynotbit.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
 foreach(size 16 20 24 32 40 48 64 96 128 256 512 1024)
  install(FILES assets/icons/application/${size}.png
   DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME ynotbit.png)
 endforeach()
 install(FILES assets/ynotbit.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
add_executable(lifecycle_tests tests/lifecycle_tests.cpp)
target_link_libraries(lifecycle_tests PRIVATE bmcore)
add_test(NAME lifecycle COMMAND lifecycle_tests)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
 add_test(NAME peer-connectivity COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/peer_connectivity_test.py $<TARGET_FILE:ynotbit>)
 set_tests_properties(peer-connectivity PROPERTIES TIMEOUT 180)
 add_test(NAME relay COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_test.py $<TARGET_FILE:ynotbit>)
 add_test(NAME relay-verack-first COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_test.py $<TARGET_FILE:ynotbit> verack-first)
 set_tests_properties(relay relay-verack-first PROPERTIES TIMEOUT 60)
add_test(NAME relay-download COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_download_test.py $<TARGET_FILE:ynotbit>)
set_tests_properties(relay-download PROPERTIES TIMEOUT 60)
add_test(NAME tls-interop COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/tls_interop.py $<TARGET_FILE:ynotbit>)
set_tests_properties(tls-interop PROPERTIES TIMEOUT 120)
add_test(NAME listen-fallback COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/listen_fallback_test.py $<TARGET_FILE:ynotbit>)
set_tests_properties(listen-fallback PROPERTIES TIMEOUT 60)
endif()
add_test(NAME gui-smoke COMMAND ynotbit --offline --data-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke-node --smoke-test)
set_tests_properties(gui-smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# A zero or non-numeric retention limit must stop the node, not delete everything.
add_test(NAME node-rejects-bad-retention COMMAND ynotbit --node -D ${CMAKE_CURRENT_BINARY_DIR}/bad-retention-node -R 0)
add_test(NAME node-rejects-bad-age COMMAND ynotbit --node -D ${CMAKE_CURRENT_BINARY_DIR}/bad-retention-node -A x)
set_tests_properties(node-rejects-bad-retention node-rejects-bad-age PROPERTIES WILL_FAIL TRUE TIMEOUT 10)
qt_add_executable(desktop_tests tests/widgets_tests.cpp src/i18n.cpp src/quoting.cpp src/letter_document.cpp src/session.cpp src/session.h src/message_model.cpp src/message_model.h src/desktop_window.cpp src/desktop_window.h src/settings_pane.cpp src/settings_pane.h src/new_vault_dialog.cpp src/new_vault_dialog.h src/letter_render.cpp src/letter_render.h src/feed_view.cpp src/feed_view.h src/appearance.cpp src/appearance.h
 assets/icons.qrc
 third_party/qrcodegen/qrcodegen.cpp third_party/qrcodegen/qrcodegen.hpp)
target_include_directories(desktop_tests PRIVATE third_party/qrcodegen)
add_coolname(desktop_tests)
target_link_libraries(desktop_tests PRIVATE bmcore Qt6::Widgets Qt6::Network Qt6::Test)
add_test(NAME desktop COMMAND desktop_tests)
set_tests_properties(desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 480)
# README screenshots from demo data; built only on request:
#   cmake --build build --target readme_screenshots && build/readme_screenshots docs/images
qt_add_executable(readme_screenshots EXCLUDE_FROM_ALL tools/readme_screenshots.cpp
 src/i18n.cpp src/quoting.cpp src/letter_document.cpp src/session.cpp src/session.h src/message_model.cpp src/message_model.h src/desktop_window.cpp src/settings_pane.cpp src/settings_pane.h src/new_vault_dialog.cpp src/new_vault_dialog.h
 src/desktop_window.h src/letter_render.cpp src/letter_render.h src/feed_view.cpp src/feed_view.h src/appearance.cpp src/appearance.h assets/icons.qrc
 third_party/qrcodegen/qrcodegen.cpp third_party/qrcodegen/qrcodegen.hpp)
target_include_directories(readme_screenshots PRIVATE third_party/qrcodegen)
add_coolname(readme_screenshots)
target_link_libraries(readme_screenshots PRIVATE bmcore Qt6::Widgets Qt6::Network Qt6::Test)
# How smoothly the Subscriptions feed scrolls (frame times); built only on request:
#   cmake --build build --target feed_scroll_bench && build/feed_scroll_bench
qt_add_executable(feed_scroll_bench EXCLUDE_FROM_ALL tools/feed_scroll_bench.cpp
 src/i18n.cpp src/quoting.cpp src/letter_document.cpp src/session.cpp src/session.h src/message_model.cpp src/message_model.h src/desktop_window.cpp src/settings_pane.cpp src/settings_pane.h src/new_vault_dialog.cpp src/new_vault_dialog.h
 src/desktop_window.h src/letter_render.cpp src/letter_render.h src/feed_view.cpp src/feed_view.h src/appearance.cpp src/appearance.h assets/icons.qrc
 third_party/qrcodegen/qrcodegen.cpp third_party/qrcodegen/qrcodegen.hpp)
target_include_directories(feed_scroll_bench PRIVATE third_party/qrcodegen)
add_coolname(feed_scroll_bench)
target_link_libraries(feed_scroll_bench PRIVATE bmcore Qt6::Widgets Qt6::Network)
# How smoothly a long chat opens and scrolls (frame times); built only on request:
#   cmake --build build --target chat_scroll_bench && build/chat_scroll_bench
qt_add_executable(chat_scroll_bench EXCLUDE_FROM_ALL tools/chat_scroll_bench.cpp
 src/chats_pane.cpp src/chats_pane.h src/letter_render.cpp src/letter_render.h
 src/letter_document.cpp src/quoting.cpp src/appearance.cpp src/appearance.h assets/icons.qrc)
target_include_directories(chat_scroll_bench PRIVATE src)
target_link_libraries(chat_scroll_bench PRIVATE Qt6::Widgets)
foreach(desktop_target ynotbit desktop_tests readme_screenshots feed_scroll_bench)
 target_sources(${desktop_target} PRIVATE src/chats_pane.cpp src/chats_pane.h src/new_chat_dialog.cpp src/new_chat_dialog.h
  src/join_channel_dialog.cpp src/join_channel_dialog.h)
endforeach()
# Proof-of-work speed on the CPU and on each GPU backend; built only on request:
#   cmake --build build --target pow_bench && build/pow_bench
add_executable(pow_bench EXCLUDE_FROM_ALL tools/pow_bench.cpp)
target_link_libraries(pow_bench PRIVATE bmcore)
# Release announcement broadcast ("ynotbit X.Y.Z" from the release address);
# built only on request. Dry run unless --send:
#   cmake --build build --target announce_release
#   build/announce_release --keys release_key.dat --version 0.5.1 --notes notes.md --send
# Bitmessage digest issue (the digest address, footer appended); built only on
# request. Dry run unless --send:
#   cmake --build build --target post_digest
#   build/post_digest --keys digest_keys.dat --issue issue.md --send
# A chat peer to try the chats against: it answers every chat it gets; built only
# on request:
#   cmake --build build --target chat_responder
#   build/chat_responder
foreach(tool announce_release post_digest chat_responder)
 qt_add_executable(${tool} EXCLUDE_FROM_ALL tools/${tool}.cpp
  tools/publish_broadcast.cpp tools/publish_broadcast.h
  src/i18n.cpp src/session.cpp src/session.h src/message_model.cpp src/message_model.h
  src/appearance.cpp src/appearance.h src/relay_bridge.cpp)
 target_include_directories(${tool} PRIVATE tools)
 target_link_libraries(${tool} PRIVATE bmcore notbit_engine Qt6::Widgets Qt6::Network)
 target_compile_definitions(${tool} PRIVATE YNOTBIT_VERSION="${PROJECT_VERSION}")
endforeach()
# The engine of chat_responder, apart from it so that it can be tested: what a bot or a service
# made on ynotbit's chats starts from.
target_sources(chat_responder PRIVATE tools/chat_bot.cpp tools/chat_bot.h
 tools/rotating_log.cpp tools/rotating_log.h)
qt_add_executable(chatbot_tests tests/chatbot_tests.cpp tools/chat_bot.cpp tools/chat_bot.h
 tools/rotating_log.cpp tools/rotating_log.h src/i18n.cpp src/session.cpp src/session.h src/message_model.cpp src/message_model.h
 src/appearance.cpp src/appearance.h src/relay_bridge.cpp)
target_include_directories(chatbot_tests PRIVATE tools)
target_link_libraries(chatbot_tests PRIVATE bmcore notbit_engine Qt6::Widgets Qt6::Network)
target_compile_definitions(chatbot_tests PRIVATE YNOTBIT_VERSION="${PROJECT_VERSION}")
add_test(NAME chatbot COMMAND chatbot_tests)
set_tests_properties(chatbot PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
add_executable(outbox_tests tests/outbox_tests.cpp)
target_link_libraries(outbox_tests PRIVATE bmcore)
add_test(NAME outbox COMMAND outbox_tests)

add_executable(wire_tests tests/wire_tests.cpp)
target_link_libraries(wire_tests PRIVATE bmcore)
add_test(NAME wire COMMAND wire_tests)

add_executable(delivery_tests tests/delivery_tests.cpp)
target_link_libraries(delivery_tests PRIVATE bmcore)
add_test(NAME delivery COMMAND delivery_tests)
set_tests_properties(delivery PROPERTIES TIMEOUT 120)


# --- Interface translations ---------------------------------------------------
# translations/ynotbit_<lang>.ts hold the translations; `cmake --build build
# --target update_translations` refreshes them from the sources (including the
# storage/protocol error catalogue). Compiled .qm files, plus Qt's own qtbase
# translations for standard buttons and dialogs, are embedded under :/i18n.
set(YNOTBIT_LANGUAGES zh_CN zh_TW ja ko ru uk)
set(YNOTBIT_TS_FILES)
foreach(lang IN LISTS YNOTBIT_LANGUAGES)
 list(APPEND YNOTBIT_TS_FILES ${CMAKE_CURRENT_SOURCE_DIR}/translations/ynotbit_${lang}.ts)
endforeach()
qt_add_lupdate(TS_FILES ${YNOTBIT_TS_FILES}
 SOURCES src/desktop_window.cpp src/chats_pane.cpp src/new_chat_dialog.cpp src/join_channel_dialog.cpp src/settings_pane.cpp src/new_vault_dialog.cpp src/session.cpp src/feed_view.cpp src/updates.cpp src/i18n/error_catalog.cpp
 LUPDATE_TARGET update_translations
 OPTIONS -no-obsolete -locations none)
qt_add_lrelease(TS_FILES ${YNOTBIT_TS_FILES} QM_FILES_OUTPUT_VARIABLE YNOTBIT_QM_FILES
 LRELEASE_TARGET ynotbit_lrelease)
set(QT_BASE_QM_FILES)
foreach(lang IN LISTS YNOTBIT_LANGUAGES)
 set(qm "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}/qtbase_${lang}.qm")
 if(EXISTS "${qm}")
  list(APPEND QT_BASE_QM_FILES "${qm}")
 else()
  message(WARNING "Qt translation not found, standard dialogs stay English: ${qm}")
 endif()
endforeach()
qt_add_executable(quoting_tests tests/quoting_tests.cpp src/quoting.cpp src/quoting.h)
target_include_directories(quoting_tests PRIVATE src)
target_link_libraries(quoting_tests PRIVATE Qt6::Gui)
add_test(NAME quoting COMMAND quoting_tests)
set_tests_properties(quoting PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
qt_add_executable(letter_document_tests tests/letter_document_tests.cpp src/letter_document.cpp
 src/quoting.cpp)
target_include_directories(letter_document_tests PRIVATE src)
target_link_libraries(letter_document_tests PRIVATE Qt6::Gui)
add_test(NAME letter-document COMMAND letter_document_tests)
set_tests_properties(letter-document PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
qt_add_executable(picture_tests tests/picture_tests.cpp src/letter_render.cpp src/letter_render.h
 src/letter_document.cpp src/quoting.cpp src/appearance.cpp src/appearance.h)
target_include_directories(picture_tests PRIVATE src)
target_link_libraries(picture_tests PRIVATE Qt6::Widgets)
add_test(NAME pictures COMMAND picture_tests)
# As on a Retina screen: pictures are kept at twice their size in points.
set_tests_properties(pictures PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2")
qt_add_executable(i18n_tests tests/i18n_tests.cpp src/i18n.cpp src/i18n.h)
target_include_directories(i18n_tests PRIVATE src)
target_link_libraries(i18n_tests PRIVATE Qt6::Core)
add_test(NAME i18n COMMAND i18n_tests)
qt_add_executable(coolname_tests tests/coolname_tests.cpp)
target_include_directories(coolname_tests PRIVATE src)
target_link_libraries(coolname_tests PRIVATE Qt6::Core)
add_coolname(coolname_tests)
add_test(NAME coolname COMMAND coolname_tests ${CMAKE_CURRENT_SOURCE_DIR})
if(Python3_Interpreter_FOUND)
 add_test(NAME translations COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-translations.py)
endif()
foreach(target ynotbit desktop_tests readme_screenshots i18n_tests)
 add_dependencies(${target} ynotbit_lrelease)
 qt_add_resources(${target} "ynotbit_translations" PREFIX "/i18n"
  BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${YNOTBIT_QM_FILES})
 if(QT_BASE_QM_FILES)
  qt_add_resources(${target} "qt_translations" PREFIX "/i18n"
   BASE "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}" FILES ${QT_BASE_QM_FILES})
 endif()
endforeach()


# --- The old code beside bmclient ----------------------------------------------------------------
# tests/legacy is a snapshot of the sources slice 1 ports (scripts/legacy-snapshot.sh), in the
# namespace `legacy`. The parity tests run it and bmclient on the same input. It goes away at the
# end of the slice, when tests/golden holds what it answered.
add_library(bm_legacy STATIC tests/legacy/protocol.cpp tests/legacy/protocol_wire.cpp
 tests/legacy/pow.cpp tests/legacy/phrase.cpp tests/legacy/oracle.cpp)
target_include_directories(bm_legacy PRIVATE tests/legacy)
target_link_libraries(bm_legacy PUBLIC bmclient Qt6::Core)
if(MSVC)
 target_compile_options(bm_legacy PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/msvc_ssize_t.h")
else()
 target_compile_options(bm_legacy PRIVATE -Wno-deprecated-declarations)
endif()
file(GLOB BMC_PARITY_TESTS CONFIGURE_DEPENDS tests/bmclient/parity/*_test.cpp)
add_executable(bmclient_parity_tests tests/bmclient/main.cpp ${BMC_PARITY_TESTS})
target_include_directories(bmclient_parity_tests PRIVATE tests)
target_link_libraries(bmclient_parity_tests PRIVATE bmclient bm_legacy Qt6::Core)
add_test(NAME bmclient-parity COMMAND bmclient_parity_tests)
set_tests_properties(bmclient-parity PROPERTIES TIMEOUT 600)
