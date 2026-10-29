// PicoPods - TLS client (mbedTLS) on top of pd::net::Tcp, and a small
// HTTP/1.1 client that speaks http:// and https://.
//
// The server certificate is verified against a built-in set of public root
// CAs (GitHub, Let's Encrypt, DigiCert, Google, Amazon, GlobalSign...) plus
// any added with tls_add_ca_pem() (a self-hosted server's CA, say). On the
// host the system CA store is trusted as well. The device has no clock, so
// certificate dates are not checked there.
//
//   pd::net::HttpClient http;
//   pd::net::HttpRequest rq;
//   rq.url = "https://example.com/";
//   if (http.open(rq, keep_going, &err)) { char b[512]; int n; while ((n = http.read(b, sizeof b)) > 0) ...; }
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pd/net.h"

namespace pd::net {

// Adds trusted root certificates (one or more PEM blocks). False if none parsed.
bool tls_add_ca_pem(const std::string& pem);

class TlsClient {
public:
    TlsClient();
    ~TlsClient();
    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    // Resolves host, connects and completes the handshake (certificate and
    // host name verified).
    bool connect(const std::string& host, uint16_t port, uint32_t timeout_ms, const KeepGoing& keep_going,
                 std::string* err);
    bool send(const void* data, uint32_t n, std::string* err);
    // Bytes read, 0 when the server closed the connection, -1 on error.
    int recv(void* buf, uint32_t n, std::string* err);
    void close();

    uint32_t timeout_ms = 20000;   // per send/recv wait
    KeepGoing keep_going;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

struct Url {
    bool https = true;
    std::string host;
    uint16_t port = 443;
    std::string path = "/";   // with the query string
};
// "https://host[:port]/path", "http://...". False if malformed.
bool parse_url(const std::string& url, Url& out);
std::string url_encode_component(const std::string& s);

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;   // extra headers
    std::string body;                                            // sent with Content-Length
    int max_redirects = 5;
};

class HttpClient {
public:
    HttpClient() = default;
    ~HttpClient() { close(); }
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Sends the request and reads the status line and headers, following
    // redirects (301/302/303/307/308). True when a response arrived (any
    // status); check status().
    bool open(const HttpRequest& rq, const KeepGoing& keep_going, std::string* err);
    int status() const { return status_; }
    std::string header(const std::string& name) const;   // case-insensitive, "" if absent
    const std::string& final_url() const { return url_; }
    int64_t content_length() const { return length_; }  // -1 unknown

    // Body bytes (chunked and fixed-length bodies decoded): n > 0 read, 0 end
    // of body, -1 error (see error()).
    int read(void* buf, uint32_t n);
    // Reads the whole body (up to max bytes) into a string.
    bool read_all(std::string& out, size_t max = 1 << 20);
    const std::string& error() const { return err_; }
    uint64_t bytes_received() const { return received_; }
    void close();

    uint32_t timeout_ms = 30000;

private:
    bool connect_to(const Url& u, const KeepGoing& keep_going, std::string* err);
    bool send_all(const std::string& s, std::string* err);
    int raw_recv(void* buf, uint32_t n);
    bool fill();
    bool read_line(std::string& line);
    bool read_headers(std::string* err);

    TlsClient* tls_ = nullptr;
    Tcp* tcp_ = nullptr;
    KeepGoing keep_going_;
    std::string url_, err_;
    int status_ = 0;
    std::vector<std::pair<std::string, std::string>> headers_;
    int64_t length_ = -1, remaining_ = -1;
    bool chunked_ = false, eof_ = false;
    int64_t chunk_left_ = 0;
    uint64_t received_ = 0;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0, end_ = 0;
};

}  // namespace pd::net
