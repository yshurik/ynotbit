#!/usr/bin/env python3
"""Example: bmbot echo — load the library, open a config, and demonstrate the ABI.
This is a minimal self-contained example that runs without a real node;
it exercises the C ABI bindings from Python."""
import ctypes
import os

# Load the shared library
lib_path = os.environ.get("BMBOT_LIB", "")
if not lib_path:
    # try relative to this script
    test_dir = os.path.dirname(os.path.abspath(__file__))
    lib_path = os.path.join(test_dir, "..", "..", "build", "libbmbot.dylib")
    if not os.path.isfile(lib_path):
        lib_path = os.path.join(test_dir, "..", "..", "build", "libbmbot.so")
        if not os.path.isfile(lib_path):
            lib_path = os.path.join(test_dir, "..", "..", "build", "libbmbot.so")

if not lib_path or not os.path.isfile(lib_path):
    raise RuntimeError(f"bmbot shared library not found at {lib_path}")

lib = ctypes.CDLL(lib_path)

# Set up simple function signatures (the header defines them with C linkage)
# bmbot_abi_version -> int
lib.bmbot_abi_version.restype = ctypes.c_int
lib.bmbot_abi_version.argtypes = []

# bmbot_open -> int (config_json, **bot, **error)
lib.bmbot_open.restype = ctypes.c_int
lib.bmbot_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_char_p)]

# bmbot_close
lib.bmbot_close.argtypes = [ctypes.c_void_p]
lib.bmbot_close.restype = None

# bmbot_free
lib.bmbot_free.argtypes = [ctypes.c_void_p]
lib.bmbot_free.restype = None

# bmbot_status -> int (**status_json, **error)
lib.bmbot_status.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_char_p)]
lib.bmbot_status.restype = ctypes.c_int

# bmbot_state -> int (bot, **state, **error)
lib.bmbot_state.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p)]
lib.bmbot_state.restype = ctypes.c_int

# bmbot_send_chat -> int
lib.bmbot_send_chat.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                                ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_char_p)]
lib.bmbot_send_chat.restype = ctypes.c_int

# bmbot_send_letter -> int
lib.bmbot_send_letter.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                                  ctypes.c_char_p, ctypes.c_char_p,
                                  ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(ctypes.c_char_p)]
lib.bmbot_send_letter.restype = ctypes.c_int

# --- Exercise the ABI ---

# 1. ABI version
v = lib.bmbot_abi_version()
print(f"bmbot ABI version: {v}")
assert v == 1, f"expected version 1, got {v}"

# 2. Open with a minimal config
config = b'{"data_dir":"/tmp/bmbot-example","phrase_file":"/tmp/bmbot-example/phrase"}'
bot = ctypes.c_void_p()
error = ctypes.c_char_p()
ret = lib.bmbot_open(config, ctypes.byref(bot), ctypes.byref(error))
print(f"bmbot_open ret={ret}, bot={bot.value if bot else None}, error={error.value if error else None}")
assert ret == 0 or ret == 1, f"bmbot_open failed with ret={ret}"

if ret == 0 and bot.value:
    # 3. Status
    status = ctypes.c_char_p()
    err = ctypes.c_char_p()
    sret = lib.bmbot_status(bot.value, ctypes.byref(status), ctypes.byref(err))
    print(f"bmbot_status ret={sret}, status={status.value if status else None}, error={err.value if err else None}")
    assert sret == 0, f"bmbot_status failed"

    # 4. State
    state = ctypes.c_char_p()
    er = ctypes.c_char_p()
    stret = lib.bmbot_state(bot.value, b"chat-123", ctypes.byref(state), ctypes.byref(er))
    print(f"bmbot_state ret={stret}, state={state.value if state else None}, error={er.value if er else None}")
    assert strret == 0, f"bmbot_state failed"

    # 5. Close and free
    lib.bmbot_close(bot.value)
    lib.bmbot_free(ctypes.cast(bot, ctypes.c_void_p))
    print("closed and freed bot handle")

print("\necho example completed successfully")