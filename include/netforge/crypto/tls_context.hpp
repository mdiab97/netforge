#pragma once

#ifdef NETFORGE_TLS_ENABLED

#include <memory>
#include <string>

// forward declare to avoid leaking openssl headers
struct ssl_ctx_st;
struct ssl_st;

namespace netforge::crypto {

enum class TlsRole { Client, Server };

class TlsContext {
public:
    explicit TlsContext(TlsRole role);
    ~TlsContext();

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    bool load_certificate(const std::string& cert_path, const std::string& key_path);
    bool set_ca_file(const std::string& ca_path);

    ssl_ctx_st* native_handle() const { return ctx_; }

private:
    ssl_ctx_st* ctx_{nullptr};
};

class TlsSocket {
public:
    TlsSocket(TlsContext& ctx, int fd);
    ~TlsSocket();

    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    bool handshake();
    int send(const uint8_t* data, size_t len);
    int recv(uint8_t* buf, size_t len);
    void shutdown();

    [[nodiscard]] bool is_valid() const { return ssl_ != nullptr; }

private:
    ssl_st* ssl_{nullptr};
};

} // namespace netforge::crypto

#endif // NETFORGE_TLS_ENABLED
