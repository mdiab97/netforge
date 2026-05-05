#include "netforge/crypto/tls_context.hpp"

#ifdef NETFORGE_TLS_ENABLED

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace netforge::crypto {

TlsContext::TlsContext(TlsRole role) {
    const SSL_METHOD* method = nullptr;

    if (role == TlsRole::Server)
        method = TLS_server_method();
    else
        method = TLS_client_method();

    ctx_ = SSL_CTX_new(method);
    if (!ctx_)
        return;

    SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
    SSL_CTX_set_options(ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
}

TlsContext::~TlsContext() {
    if (ctx_)
        SSL_CTX_free(ctx_);
}

bool TlsContext::load_certificate(const std::string& cert_path, const std::string& key_path) {
    if (!ctx_) return false;

    if (SSL_CTX_use_certificate_file(ctx_, cert_path.c_str(), SSL_FILETYPE_PEM) != 1)
        return false;

    if (SSL_CTX_use_PrivateKey_file(ctx_, key_path.c_str(), SSL_FILETYPE_PEM) != 1)
        return false;

    return SSL_CTX_check_private_key(ctx_) == 1;
}

bool TlsContext::set_ca_file(const std::string& ca_path) {
    if (!ctx_) return false;
    return SSL_CTX_load_verify_locations(ctx_, ca_path.c_str(), nullptr) == 1;
}

// --- TlsSocket ---

TlsSocket::TlsSocket(TlsContext& ctx, int fd) {
    ssl_ = SSL_new(ctx.native_handle());
    if (!ssl_) return;
    SSL_set_fd(ssl_, fd);
}

TlsSocket::~TlsSocket() {
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
    }
}

bool TlsSocket::handshake() {
    if (!ssl_) return false;
    return SSL_do_handshake(ssl_) == 1;
}

int TlsSocket::send(const uint8_t* data, size_t len) {
    if (!ssl_) return -1;
    return SSL_write(ssl_, data, static_cast<int>(len));
}

int TlsSocket::recv(uint8_t* buf, size_t len) {
    if (!ssl_) return -1;
    return SSL_read(ssl_, buf, static_cast<int>(len));
}

void TlsSocket::shutdown() {
    if (ssl_)
        SSL_shutdown(ssl_);
}

} // namespace netforge::crypto

#endif // NETFORGE_TLS_ENABLED
