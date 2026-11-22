# Can the OpenSSL that was found do what the node's TLS needs (ntb-tls-setup.h in the node's src)?
# One that cannot still builds: the node then offers no TLS and talks plain Bitmessage to PyBitmessage
# peers, and nothing says so until a peer or tests/tls_interop.py notices. Homebrew's OpenSSL 4.1.0 is
# such a one: it refuses the secp256k1 TLS group. The check runs the node's own calls once
# (check_node_tls.c). It stops the configure only when that program ran and said no: one that cannot
# run here (a cross build, a DLL off the path) is skipped, and the node's log still tells.
function(check_node_tls include_dir)
 if(CMAKE_CROSSCOMPILING)
  message(STATUS "Node TLS check skipped: cross compiling")
  return()
 endif()
 # try_run keeps its result in the cache: a rerun after -DOPENSSL_ROOT_DIR must not read the old one.
 unset(node_tls_run CACHE)
 unset(node_tls_compiled CACHE)
 try_run(node_tls_run node_tls_compiled
  ${CMAKE_BINARY_DIR}/CMakeFiles/check-node-tls ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_node_tls.c
  CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${include_dir}"
  LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto
  COMPILE_OUTPUT_VARIABLE compile_output
  RUN_OUTPUT_VARIABLE run_output)
 string(STRIP "${run_output}" run_output)
 if(NOT node_tls_compiled)
  message(STATUS "Node TLS check skipped: its program did not compile")
  message(VERBOSE "${compile_output}")
 elseif(node_tls_run STREQUAL "0")
  message(STATUS "Node TLS check: ${run_output} takes the node's TLS setup")
 elseif(node_tls_run STREQUAL "2")
  message(FATAL_ERROR
   "The OpenSSL found (${OPENSSL_SSL_LIBRARY}) cannot set up the node's TLS:\n"
   "  ${run_output}\n"
   "A node built with it would offer no TLS and talk plain Bitmessage to PyBitmessage peers.\n"
   "Build against an OpenSSL 3: -DOPENSSL_ROOT_DIR=<its prefix> and PKG_CONFIG_PATH=<its prefix>/lib/pkgconfig "
   "(Homebrew: brew install openssl@3, the prefix is brew --prefix openssl@3).")
 else()
  message(STATUS "Node TLS check skipped: its program did not run (${node_tls_run})")
 endif()
endfunction()
