# bmclient: the client's core with no Qt. The Qt app reaches it through the wrappers in src/.
add_library(bmclient STATIC
 src/bytes.cpp src/chat_format.cpp src/clock.cpp src/hash.cpp src/json.cpp src/phrase.cpp src/pow.cpp src/protocol.cpp src/random.cpp src/secret.cpp src/text.cpp src/updates.cpp src/wire.cpp)
target_include_directories(bmclient PUBLIC include)
target_compile_features(bmclient PUBLIC cxx_std_20)
target_link_libraries(bmclient PUBLIC notbit_codec secp256k1 OpenSSL::Crypto PkgConfig::SODIUM Threads::Threads)
if(MSVC)
 target_compile_options(bmclient PRIVATE /utf-8 /FI "${CMAKE_SOURCE_DIR}/src/msvc_ssize_t.h")
else()
 target_compile_options(bmclient PRIVATE -Wno-deprecated-declarations -include sys/types.h)
endif()
