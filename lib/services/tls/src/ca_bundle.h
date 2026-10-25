// pd_tls internal: the built-in root certificates.
#pragma once
#include <cstddef>

namespace pd::net::detail {
struct CaCert {
    const unsigned char* der;
    size_t len;
};
extern const CaCert kCaBundle[];
extern const int kCaBundleCount;
}  // namespace pd::net::detail
