# bm_vanity: a fast search for a Bitmessage address with a word in it (see vanity.h). It needs
# only OpenSSL. For the full speed of a machine, build it there with -O3 -march=native.
find_package(Threads REQUIRED)
# The wide hashes (avx512.h): built with the AVX-512 flags where the compiler is for x86-64, and an
# empty stand-in elsewhere. Whether to use them is decided when the program runs.
add_library(vanity_avx512 STATIC avx512.cpp)
target_include_directories(vanity_avx512 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NOT MSVC)
 target_compile_options(vanity_avx512 PRIVATE -mavx512f -mavx512bw -mavx512dq -mavx512vl)
endif()
add_executable(bm_vanity bm_vanity.cpp)
target_link_libraries(bm_vanity PRIVATE vanity_avx512 OpenSSL::Crypto Threads::Threads)
target_compile_options(bm_vanity PRIVATE -Wno-deprecated-declarations)
