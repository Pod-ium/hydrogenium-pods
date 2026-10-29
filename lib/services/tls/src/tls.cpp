// pd::net::TlsClient: mbedTLS 2.28 over pd::net::Tcp (blocking, with timeouts
// and a keep-going callback), certificates checked against the built-in roots.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <string>

#include "ca_bundle.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "pd/tls.h"

namespace pd::net {

namespace {

// One trust store for every connection, built on first use.
struct Trust {
    mbedtls_x509_crt chain;
    bool built = false;
    int count = 0;
    std::list<std::string> extra_pem;   // kept alive: parsed copies are owned by mbedTLS anyway
};
Trust g_trust;

void build_trust() {
    if (g_trust.built) return;
    mbedtls_x509_crt_init(&g_trust.chain);
    for (int i = 0; i < detail::kCaBundleCount; ++i) {
        // nocopy: the certificate stays in flash instead of a RAM copy
        if (mbedtls_x509_crt_parse_der_nocopy(&g_trust.chain, detail::kCaBundle[i].der, detail::kCaBundle[i].len) == 0)
            ++g_trust.count;
    }
#if PD_HOST
    // The host also trusts the system store (and PD_CA_FILE if set).
    const char* files[] = {getenv("PD_CA_FILE"), "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                           "/etc/ssl/cert.pem"};
    for (const char* f : files)
        if (f && mbedtls_x509_crt_parse_file(&g_trust.chain, f) >= 0) ++g_trust.count;
#endif
    g_trust.built = true;
}

std::string mbed_error(const char* what, int code) {
    char b[128];
    mbedtls_strerror(code, b, sizeof b);
    char out[200];
    snprintf(out, sizeof out, "%s: %s (-0x%04x)", what, b, unsigned(-code));
    return out;
}

}  // namespace

bool tls_add_ca_pem(const std::string& pem) {
    build_trust();
    std::string copy = pem;   // mbedTLS wants the terminating NUL counted
    const int r = mbedtls_x509_crt_parse(&g_trust.chain, reinterpret_cast<const unsigned char*>(copy.c_str()),
                                         copy.size() + 1);
    if (r < 0) return false;
    ++g_trust.count;
    return true;
}

struct TlsClient::Impl {
    Tcp tcp;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    TlsClient* owner = nullptr;
    std::string last_err;
    bool open = false;

    Impl() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
    }
    ~Impl() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
    }

    static int bio_send(void* ctx, const unsigned char* buf, size_t len) {
        Impl* self = static_cast<Impl*>(ctx);
        std::string err;
        if (!self->tcp.send(buf, uint32_t(len), self->owner->timeout_ms, self->owner->keep_going, &err)) {
            self->last_err = err;
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
        return int(len);
    }
    static int bio_recv(void* ctx, unsigned char* buf, size_t len) {
        Impl* self = static_cast<Impl*>(ctx);
        std::string err;
        const int n = self->tcp.recv(buf, uint32_t(len), self->owner->timeout_ms, self->owner->keep_going, &err);
        if (n < 0) {
            self->last_err = err;
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        return n;   // 0 = connection closed
    }
};

TlsClient::TlsClient() = default;
TlsClient::~TlsClient() { close(); }

bool TlsClient::connect(const std::string& host, uint16_t port, uint32_t tmo, const KeepGoing& kg, std::string* err) {
    close();
    keep_going = kg;
    timeout_ms = tmo;
    build_trust();
    uint32_t ip = 0;
    if (!resolve(host, &ip, tmo, kg, err)) return false;

    impl_ = new Impl();
    impl_->owner = this;
    Impl& m = *impl_;
    if (!m.tcp.connect(ip, port, tmo, kg, err)) {
        close();
        return false;
    }
    static const char pers[] = "picopods-tls";
    int r = mbedtls_ctr_drbg_seed(&m.drbg, mbedtls_entropy_func, &m.entropy,
                                  reinterpret_cast<const unsigned char*>(pers), sizeof pers - 1);
    if (r == 0) r = mbedtls_ssl_config_defaults(&m.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) {
        if (err) *err = mbed_error("TLS setup", r);
        close();
        return false;
    }
    mbedtls_ssl_conf_authmode(&m.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&m.conf, &g_trust.chain, nullptr);
    mbedtls_ssl_conf_rng(&m.conf, mbedtls_ctr_drbg_random, &m.drbg);
    if ((r = mbedtls_ssl_setup(&m.ssl, &m.conf)) != 0 || (r = mbedtls_ssl_set_hostname(&m.ssl, host.c_str())) != 0) {
        if (err) *err = mbed_error("TLS setup", r);
        close();
        return false;
    }
    mbedtls_ssl_set_bio(&m.ssl, &m, Impl::bio_send, Impl::bio_recv, nullptr);
    while ((r = mbedtls_ssl_handshake(&m.ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (err) {
            const uint32_t flags = mbedtls_ssl_get_verify_result(&m.ssl);
            if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags) {
                char vb[256];
                mbedtls_x509_crt_verify_info(vb, sizeof vb, "", flags);
                std::string v = vb;
                while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
                *err = "certificate not trusted for " + host + ": " + v;
            } else {
                *err = m.last_err.empty() ? mbed_error("TLS handshake", r) : m.last_err;
            }
        }
        close();
        return false;
    }
    m.open = true;
    return true;
}

bool TlsClient::send(const void* data, uint32_t n, std::string* err) {
    if (!impl_ || !impl_->open) {
        if (err) *err = "not connected";
        return false;
    }
    const unsigned char* p = static_cast<const unsigned char*>(data);
    while (n) {
        const int r = mbedtls_ssl_write(&impl_->ssl, p, n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r < 0) {
            if (err) *err = impl_->last_err.empty() ? mbed_error("TLS send", r) : impl_->last_err;
            return false;
        }
        p += r;
        n -= uint32_t(r);
    }
    return true;
}

int TlsClient::recv(void* buf, uint32_t n, std::string* err) {
    if (!impl_ || !impl_->open) return 0;
    for (;;) {
        const int r = mbedtls_ssl_read(&impl_->ssl, static_cast<unsigned char*>(buf), n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == 0) return 0;
        if (r < 0) {
            if (err) *err = impl_->last_err.empty() ? mbed_error("TLS receive", r) : impl_->last_err;
            return -1;
        }
        return r;
    }
}

void TlsClient::close() {
    if (!impl_) return;
    if (impl_->open) mbedtls_ssl_close_notify(&impl_->ssl);
    impl_->tcp.close();
    delete impl_;
    impl_ = nullptr;
}

}  // namespace pd::net
