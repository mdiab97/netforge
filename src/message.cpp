#include "netforge/message.hpp"

// Message implementation is fully inline in the header.
// This translation unit exists to ensure the library always has
// at least one compiled symbol from this module.

namespace netforge {
namespace detail {
    // Force a symbol to exist in the compiled library
    void message_module_anchor() {}
}
} // namespace netforge
