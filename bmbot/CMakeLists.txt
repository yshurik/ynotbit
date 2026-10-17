cmake_minimum_required(VERSION 3.18)
project(bmbot LANGUAGES CXX)

# bmbot: C ABI library for bot runtime — shared so Python ctypes can load it.
# depends on bmclient (C++20, std only) and the notbit C code
set(LIBBMBOT_SOURCES
    bmbot.cpp
)

# The OpenSSL and other dependencies are provided by the parent CMake
# (find_package in the top-level CMakeLists.txt).  We re-express the
# requirements here so the imported targets are visible in this scope.
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SQLCIPHER REQUIRED IMPORTED_TARGET sqlcipher)

add_library(libbmbot SHARED ${LIBBMBOT_SOURCES})
target_include_directories(libbmbot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(libbmbot PRIVATE OpenSSL::SSL Threads::Threads PkgConfig::SODIUM PkgConfig::SQLCIPHER)

# Export the C linkage so nothing mangles the names
set_target_properties(libbmbot PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
set_target_properties(libbmbot PROPERTIES OUTPUT_NAME "bmbot")