cmake_minimum_required(VERSION 3.22)
project(ynotbit VERSION 0.6.0 LANGUAGES C CXX) # the one place the version is set
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Test LinguistTools)
# Qt 6.8 references AGL, which Apple removed from the macOS 26 SDK.
# The Widgets desktop does not use this obsolete framework.
if(APPLE AND CMAKE_OSX_SYSROOT MATCHES "MacOSX26")
 get_target_property(qt_gui_links WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
 list(FILTER qt_gui_links EXCLUDE REGEX "AGL")
 set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${qt_gui_links}")
endif()
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SQLCIPHER REQUIRED IMPORTED_TARGET sqlcipher)
set(NTB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/notbit)
add_library(notbit_codec STATIC
 ${NTB}/src/ntb-address.c ${NTB}/src/ntb-base58.c ${NTB}/src/ntb-buffer.c
 ${NTB}/src/ntb-util.c ${NTB}/src/ntb-error.c ${NTB}/src/ntb-ecc.c
 ${NTB}/src/ntb-proto.c ${NTB}/src/ntb-netaddress.c)
target_include_directories(notbit_codec PUBLIC ${NTB}/src ${NTB})
target_link_libraries(notbit_codec PUBLIC OpenSSL::Crypto)
if(WIN32)
 target_link_libraries(notbit_codec PUBLIC ws2_32)
endif()
if(MSVC)
 target_compile_options(notbit_codec PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/msvc_ssize_t.h")
else()
 target_compile_options(notbit_codec PRIVATE -Wno-deprecated-declarations -include sys/types.h)
endif()
# The node's network objects in one SQLite file (objects.sqlite): the engine
# writes it; the app reads it, and its tests write fixtures through it.
add_library(ntb_object_db STATIC ${NTB}/src/ntb-object-db.c)
set_target_properties(ntb_object_db PROPERTIES C_STANDARD 11)
target_include_directories(ntb_object_db PUBLIC ${NTB}/src ${SQLCIPHER_INCLUDE_DIRS}/..)
target_link_libraries(ntb_object_db PUBLIC PkgConfig::SQLCIPHER OpenSSL::Crypto)
if(MSVC)
 target_compile_definitions(ntb_object_db PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
endif()
# libsecp256k1, for the ECDH that decrypting tries on every incoming message
# against every identity: 10-20x OpenSSL's generic curve code. Pinned and
# checksummed; built static, nothing installed.
include(FetchContent)
# Its CMake declares BUILD_SHARED_LIBS (default ON) for the whole project,
# which would turn our own libraries shared; they stay static.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SECP256K1_DISABLE_SHARED ON CACHE BOOL "" FORCE)
set(SECP256K1_INSTALL OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH ON CACHE BOOL "" FORCE)
foreach(off BUILD_TESTS BUILD_EXHAUSTIVE_TESTS BUILD_BENCHMARK BUILD_CTIME_TESTS BUILD_EXAMPLES)
 set(SECP256K1_${off} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(secp256k1
 URL https://github.com/bitcoin-core/secp256k1/archive/refs/tags/v0.6.0.tar.gz
 URL_HASH SHA256=785bb98e7d6705c51c8dfa8ac3af6aa2ccfa3774714d51c0b9e28fac1146e9f1)
FetchContent_MakeAvailable(secp256k1)
add_library(bmcore src/storage.cpp src/mailbox_ops.cpp src/protocol.cpp src/protocol_wire.cpp src/pow.cpp src/gpu_pow.cpp src/gpu_opencl.cpp src/delivery.cpp src/cache.cpp src/scanner.cpp src/updates.cpp src/message_search.cpp)
target_include_directories(bmcore PUBLIC src)
target_link_libraries(bmcore PUBLIC Threads::Threads notbit_codec secp256k1 Qt6::Core PkgConfig::SODIUM PkgConfig::SQLCIPHER OpenSSL::Crypto ntb_object_db)
if(APPLE)
 # Proof of work on the GPU through Metal (src/gpu_metal.mm).
 enable_language(OBJCXX)
 target_sources(bmcore PRIVATE src/gpu_metal.mm)
 set_source_files_properties(src/gpu_metal.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
 target_link_libraries(bmcore PUBLIC "-framework Metal" "-framework Foundation" "-framework IOKit")
endif()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE bmcore)
enable_testing()
add_executable(peer_policy_tests tests/peer_policy_tests.c)
target_include_directories(peer_policy_tests PRIVATE ${NTB}/src)
add_test(NAME peer-policy COMMAND peer_policy_tests)
add_executable(object_db_tests tests/object_db_tests.c)
target_link_libraries(object_db_tests PRIVATE ntb_object_db)
add_test(NAME object-db COMMAND object_db_tests ${CMAKE_CURRENT_BINARY_DIR}/object-db-test)
add_test(NAME core COMMAND core_tests)

target_compile_definitions(bmcore PUBLIC SQLITE_HAS_CODEC)
if(MSVC)
 target_compile_options(bmcore PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/msvc_ssize_t.h")
else()
 target_compile_options(bmcore PRIVATE -Wno-deprecated-declarations)
endif()

file(GLOB NTB_ENGINE_SOURCES CONFIGURE_DEPENDS ${NTB}/src/*.c)
list(FILTER NTB_ENGINE_SOURCES EXCLUDE REGEX "/(main|ntb-object-db|ntb-address|ntb-base58|ntb-buffer|ntb-util|ntb-error|ntb-ecc|ntb-proto|ntb-netaddress)\\.c$")
if(WIN32)
 # The IPC socket and its sendmail/keygen clients are Unix-socket only, and
 # ynotbit uses neither: the desktop talks to the engine through files.
 list(FILTER NTB_ENGINE_SOURCES EXCLUDE REGEX "/(ntb-ipc[a-z-]*|ntb-sendmail|ntb-keygen)\\.c$")
endif()
add_library(notbit_engine STATIC ${NTB_ENGINE_SOURCES})
target_link_libraries(notbit_engine PUBLIC ntb_object_db)
set_target_properties(notbit_engine PROPERTIES C_STANDARD 11)
# The network user agent, "/ynotbit:<version>/".
target_compile_definitions(notbit_engine PRIVATE YNOTBIT_VERSION="${PROJECT_VERSION}")
target_link_libraries(notbit_engine PUBLIC notbit_codec)
if(WIN32)
 find_package(PThreads4W REQUIRED)
 find_path(GETOPT_INCLUDE_DIR getopt.h REQUIRED)
 find_library(GETOPT_LIBRARY NAMES getopt REQUIRED)
 target_include_directories(notbit_engine PRIVATE ${GETOPT_INCLUDE_DIR})
 target_link_libraries(notbit_engine PUBLIC PThreads4W::PThreads4W ${GETOPT_LIBRARY} ws2_32)
else()
 target_link_libraries(notbit_engine PUBLIC Threads::Threads)
endif()
if(MSVC)
 target_compile_definitions(notbit_engine PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
 target_compile_options(notbit_engine PRIVATE /FI "${CMAKE_CURRENT_SOURCE_DIR}/src/msvc_ssize_t.h")
else()
 target_compile_definitions(notbit_engine PRIVATE _GNU_SOURCE)
 target_compile_options(notbit_engine PRIVATE -Wno-deprecated-declarations -include sys/types.h)
endif()
qt_add_executable(ynotbit MACOSX_BUNDLE src/main.cpp src/i18n.cpp src/i18n.h src/quoting.cpp src/quoting.h src/letter_document.cpp src/letter_document.h src/letter_document.cpp src/letter_document.h src/session.cpp src/session.h
 assets/icons.qrc
 src/appearance.cpp src/appearance.h
 src/desktop_window.cpp src/desktop_window.h src/settings_window.cpp src/settings_window.h src/letter_render.cpp src/letter_render.h src/feed_view.cpp src/feed_view.h src/message_model.cpp src/message_model.h
 third_party/qrcodegen/qrcodegen.cpp third_party/qrcodegen/qrcodegen.hpp)
target_include_directories(ynotbit PRIVATE third_party/qrcodegen)
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
endif()
add_test(NAME gui-smoke COMMAND ynotbit --offline --data-dir ${CMAKE_CURRENT_BINARY_DIR}/smoke-node --smoke-test)
set_tests_properties(gui-smoke PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# A zero or non-numeric retention limit must stop the node, not delete everything.
add_test(NAME node-rejects-bad-retention COMMAND ynotbit --node -D ${CMAKE_CURRENT_BINARY_DIR}/bad-retention-node -R 0)
add_test(NAME node-rejects-bad-age COMMAND ynotbit --node -D ${CMAKE_CURRENT_BINARY_DIR}/bad-retention-node -A x)
set_tests_properties(node-rejects-bad-retention node-rejects-bad-age PROPERTIES WILL_FAIL TRUE TIMEOUT 10)
qt_add_executable(desktop_tests tests/widgets_tests.cpp src/i18n.cpp src/quoting.cpp src/letter_document.cpp src/session.cpp src/session.h src/message_model.cpp src/message_model.h src/desktop_window.cpp src/desktop_window.h src/settings_window.cpp src/settings_window.h src/letter_render.cpp src/letter_render.h src/feed_view.cpp src/feed_view.h src/appearance.cpp src/appearance.h
 assets/icons.qrc
 third_party/qrcodegen/qrcodegen.cpp third_party/qrcodegen/qrcodegen.hpp)
target_include_directories(desktop_tests PRIVATE third_party/qrcodegen)
target_link_libraries(desktop_tests PRIVATE bmcore Qt6::Widgets Qt6::Network Qt6::Test)
add_test(NAME desktop COMMAND desktop_tests)
set_tests_properties(desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 480)
# README screenshots from demo data; built only on request:
#   cmake --build build --target readme_screenshots && build/readme_screenshots docs/images
qt_add_executable(readme_screenshots EXCLUDE_FROM_ALL tools/readme_screenshots.cpp
 src/i18n.cpp src/quoting.cpp src/letter_document.cpp src/session.cpp src/session.h src/message_model.cpp src/message_model.h src/desktop_window.cpp src/settings_window.cpp src/settings_window.h
 src/desktop_window.h src/letter_render.cpp src/letter_render.h src/feed_view.cpp src/feed_view.h src/appearance.cpp src/appearance.h assets/icons.qrc
 third_party/qrcodegen/qrcodegen.cpp third_party/qrcodegen/qrcodegen.hpp)
target_include_directories(readme_screenshots PRIVATE third_party/qrcodegen)
target_link_libraries(readme_screenshots PRIVATE bmcore Qt6::Widgets Qt6::Network Qt6::Test)
# How smoothly the Subscriptions feed scrolls (frame times); built only on request:
#   cmake --build build --target feed_scroll_bench && build/feed_scroll_bench
qt_add_executable(feed_scroll_bench EXCLUDE_FROM_ALL tools/feed_scroll_bench.cpp
 src/i18n.cpp src/quoting.cpp src/letter_document.cpp src/session.cpp src/session.h src/message_model.cpp src/message_model.h src/desktop_window.cpp src/settings_window.cpp src/settings_window.h
 src/desktop_window.h src/letter_render.cpp src/letter_render.h src/feed_view.cpp src/feed_view.h src/appearance.cpp src/appearance.h assets/icons.qrc
 third_party/qrcodegen/qrcodegen.cpp third_party/qrcodegen/qrcodegen.hpp)
target_include_directories(feed_scroll_bench PRIVATE third_party/qrcodegen)
target_link_libraries(feed_scroll_bench PRIVATE bmcore Qt6::Widgets Qt6::Network)
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
foreach(tool announce_release post_digest)
 qt_add_executable(${tool} EXCLUDE_FROM_ALL tools/${tool}.cpp
  tools/publish_broadcast.cpp tools/publish_broadcast.h
  src/i18n.cpp src/session.cpp src/session.h src/message_model.cpp src/message_model.h
  src/appearance.cpp src/appearance.h src/relay_bridge.cpp)
 target_include_directories(${tool} PRIVATE tools)
 target_link_libraries(${tool} PRIVATE bmcore notbit_engine Qt6::Widgets Qt6::Network)
 target_compile_definitions(${tool} PRIVATE YNOTBIT_VERSION="${PROJECT_VERSION}")
endforeach()
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

add_executable(netaddress_tests tests/netaddress_tests.cpp)
target_link_libraries(netaddress_tests PRIVATE notbit_codec)
add_test(NAME netaddress COMMAND netaddress_tests)

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
 SOURCES src/desktop_window.cpp src/settings_window.cpp src/session.cpp src/feed_view.cpp src/updates.cpp src/i18n/error_catalog.cpp
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
