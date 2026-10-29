// pd::net::HttpClient: HTTP/1.1 over TCP or TLS, one request per connection,
// redirects followed, chunked bodies decoded.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "pd/tls.h"

namespace pd::net {

namespace {
constexpr size_t kBuf = 4096;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
}  // namespace

bool parse_url(const std::string& url, Url& out) {
    std::string rest;
    if (url.compare(0, 8, "https://") == 0) {
        out.https = true;
        out.port = 443;
        rest = url.substr(8);
    } else if (url.compare(0, 7, "http://") == 0) {
        out.https = false;
        out.port = 80;
        rest = url.substr(7);
    } else {
        return false;
    }
    const size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t at = hostport.rfind('@');   // user@host: credentials are not taken from URLs
    if (at != std::string::npos) hostport = hostport.substr(at + 1);
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        const int p = atoi(hostport.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        out.port = uint16_t(p);
        hostport = hostport.substr(0, colon);
    }
    out.host = hostport;
    return !out.host.empty();
}

std::string url_encode_component(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || strchr("-._~", c)) {
            o += char(c);
        } else {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            o += b;
        }
    }
    return o;
}

void HttpClient::close() {
    if (tls_) {
        tls_->close();
        delete tls_;
        tls_ = nullptr;
    }
    if (tcp_) {
        tcp_->close();
        delete tcp_;
        tcp_ = nullptr;
    }
    pos_ = end_ = 0;
}

bool HttpClient::connect_to(const Url& u, const KeepGoing& kg, std::string* err) {
    close();
    if (u.https) {
        tls_ = new TlsClient();
        return tls_->connect(u.host, u.port, timeout_ms, kg, err);
    }
    uint32_t ip = 0;
    if (!resolve(u.host, &ip, timeout_ms, kg, err)) return false;
    tcp_ = new Tcp();
    return tcp_->connect(ip, u.port, timeout_ms, kg, err);
}

bool HttpClient::send_all(const std::string& s, std::string* err) {
    if (tls_) return tls_->send(s.data(), uint32_t(s.size()), err);
    return tcp_->send(s.data(), uint32_t(s.size()), timeout_ms, keep_going_, err);
}

int HttpClient::raw_recv(void* b, uint32_t n) {
    std::string e;
    const int r = tls_ ? tls_->recv(b, n, &e) : tcp_->recv(b, n, timeout_ms, keep_going_, &e);
    if (r < 0 && err_.empty()) err_ = e.empty() ? "receive failed" : e;
    if (r > 0) received_ += uint32_t(r);
    return r;
}

bool HttpClient::fill() {
    if (pos_ < end_) return true;
    const int r = raw_recv(buf_.data(), uint32_t(buf_.size()));
    if (r <= 0) return false;
    pos_ = 0;
    end_ = size_t(r);
    return true;
}

bool HttpClient::read_line(std::string& line) {
    line.clear();
    for (;;) {
        if (!fill()) return false;
        while (pos_ < end_) {
            const char c = char(buf_[pos_++]);
            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            if (line.size() > 8192) return false;
            line += c;
        }
    }
}

bool HttpClient::read_headers(std::string* err) {
    std::string line;
    if (!read_line(line)) {
        if (err) *err = err_.empty() ? "no response from the server" : err_;
        return false;
    }
    // "HTTP/1.1 200 OK"
    const size_t sp = line.find(' ');
    status_ = sp == std::string::npos ? 0 : atoi(line.c_str() + sp + 1);
    if (line.compare(0, 5, "HTTP/") != 0 || status_ < 100) {
        if (err) *err = "bad response: " + line.substr(0, 60);
        return false;
    }
    headers_.clear();
    for (;;) {
        if (!read_line(line)) {
            if (err) *err = "response headers cut short";
            return false;
        }
        if (line.empty()) break;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        headers_.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    const std::string te = header("Transfer-Encoding");
    std::string te_lower = te;
    for (char& ch : te_lower) ch = char(tolower(uint8_t(ch)));
    chunked_ = te_lower.find("chunked") != std::string::npos;
    const std::string cl = header("Content-Length");
    length_ = cl.empty() ? -1 : strtoll(cl.c_str(), nullptr, 10);
    remaining_ = chunked_ ? -1 : length_;
    chunk_left_ = 0;
    eof_ = (!chunked_ && length_ == 0) || status_ == 204 || status_ == 304;
    return true;
}

std::string HttpClient::header(const std::string& name) const {
    for (const auto& h : headers_)
        if (strcasecmp(h.first.c_str(), name.c_str()) == 0) return h.second;
    return "";
}

bool HttpClient::open(const HttpRequest& rq0, const KeepGoing& kg, std::string* err) {
    keep_going_ = kg;
    buf_.resize(kBuf);
    received_ = 0;
    err_.clear();
    HttpRequest rq = rq0;
    for (int hop = 0; hop <= rq0.max_redirects; ++hop) {
        Url u;
        if (!parse_url(rq.url, u)) {
            if (err) *err = "not an http(s) URL: " + rq.url;
            return false;
        }
        url_ = rq.url;
        if (!connect_to(u, kg, err)) return false;
        std::string head = rq.method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host;
        if ((u.https && u.port != 443) || (!u.https && u.port != 80)) head += ":" + std::to_string(u.port);
        head += "\r\nConnection: close\r\n";
        bool has_ua = false;
        for (const auto& h : rq.headers) {
            head += h.first + ": " + h.second + "\r\n";
            if (strcasecmp(h.first.c_str(), "User-Agent") == 0) has_ua = true;
        }
        if (!has_ua) head += "User-Agent: picopods\r\n";
        if (!rq.body.empty() || rq.method == "POST" || rq.method == "PUT")
            head += "Content-Length: " + std::to_string(rq.body.size()) + "\r\n";
        head += "\r\n";
        if (!send_all(head, err) || (!rq.body.empty() && !send_all(rq.body, err))) {
            close();
            return false;
        }
        if (!read_headers(err)) {
            close();
            return false;
        }
        const bool redirect = status_ == 301 || status_ == 302 || status_ == 303 || status_ == 307 || status_ == 308;
        const std::string loc = header("Location");
        if (!redirect || loc.empty()) return true;
        // Follow: absolute or host-relative location.
        if (loc.compare(0, 4, "http") == 0) {
            rq.url = loc;
        } else {
            rq.url = std::string(u.https ? "https://" : "http://") + u.host +
                     ((u.https && u.port != 443) || (!u.https && u.port != 80) ? ":" + std::to_string(u.port) : "") +
                     (loc[0] == '/' ? loc : "/" + loc);
        }
        if (status_ == 303 || ((status_ == 301 || status_ == 302) && rq.method == "POST")) {
            rq.method = "GET";
            rq.body.clear();
        }
        close();
    }
    if (err) *err = "too many redirects";
    return false;
}

int HttpClient::read(void* out, uint32_t n) {
    if (eof_ || n == 0) return 0;
    uint8_t* o = static_cast<uint8_t*>(out);
    if (chunked_) {
        if (chunk_left_ == 0) {
            std::string line;
            if (!read_line(line)) {   // may be the CRLF after the previous chunk
                if (err_.empty()) err_ = "chunked body cut short";
                return -1;
            }
            if (line.empty() && !read_line(line)) {
                if (err_.empty()) err_ = "chunked body cut short";
                return -1;
            }
            chunk_left_ = strtoll(line.c_str(), nullptr, 16);
            if (chunk_left_ <= 0) {
                eof_ = true;   // trailers are ignored (Connection: close)
                return 0;
            }
        }
        if (!fill()) {
            if (err_.empty()) err_ = "chunked body cut short";
            return -1;
        }
        size_t k = std::min<size_t>({size_t(n), end_ - pos_, size_t(chunk_left_)});
        memcpy(o, &buf_[pos_], k);
        pos_ += k;
        chunk_left_ -= int64_t(k);
        return int(k);
    }
    if (remaining_ == 0) {
        eof_ = true;
        return 0;
    }
    if (!fill()) {
        if (remaining_ > 0) {
            if (err_.empty()) err_ = "response cut short";
            return -1;
        }
        eof_ = true;   // length unknown: the body ends with the connection
        return 0;
    }
    size_t k = std::min<size_t>(size_t(n), end_ - pos_);
    if (remaining_ > 0) k = std::min<size_t>(k, size_t(remaining_));
    memcpy(o, &buf_[pos_], k);
    pos_ += k;
    if (remaining_ > 0) remaining_ -= int64_t(k);
    return int(k);
}

bool HttpClient::read_all(std::string& out, size_t max) {
    out.clear();
    char b[1024];
    for (;;) {
        const int r = read(b, sizeof b);
        if (r < 0) return false;
        if (r == 0) return true;
        if (out.size() + size_t(r) > max) {
            err_ = "response too large";
            return false;
        }
        out.append(b, size_t(r));
    }
}

}  // namespace pd::net
