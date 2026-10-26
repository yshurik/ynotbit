# bm_vanity: a fast search for a Bitmessage address with a word in it (see vanity.h). It needs
# only OpenSSL. For the full speed of a machine, build it there with -O3 -march=native.
find_package(Threads REQUIRED)
add_executable(bm_vanity bm_vanity.cpp)
target_link_libraries(bm_vanity PRIVATE OpenSSL::Crypto Threads::Threads)
target_compile_options(bm_vanity PRIVATE -Wno-deprecated-declarations)
