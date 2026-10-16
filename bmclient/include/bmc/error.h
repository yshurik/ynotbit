// The error of the core: a std::runtime_error whose what() is the English text the translation
// catalogue lists, and a code for callers that cannot read text (the C API of bmbot).
#pragma once
#include <stdexcept>
#include <string>

namespace bmc {
enum class Code { Invalid = 1, Argument, State, Auth, Io, Network, Internal };

class Error : public std::runtime_error {
  public:
    Error(Code code, const std::string &what) : std::runtime_error(what), code_(code) {}
    Code code() const {
        return code_;
    }

  private:
    Code code_;
};

// scripts/update-error-catalog.py lists the text of every require(..., "text") for translation.
inline void require(bool ok, const char *what, Code code = Code::Invalid) {
    if (!ok)
        throw Error(code, what);
}
} // namespace bmc
