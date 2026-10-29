#include "pd/crosspoint.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pd::crosspoint {

bool parse_url(const std::string& urlIn, Target* t, std::string* err) {
    std::string u = urlIn;
    while (!u.empty() && u.front() == ' ') u.erase(0, 1);
    while (!u.empty() && u.back() == ' ') u.pop_back();
    if (u.compare(0, 7, "http://") == 0) u = u.substr(7);
    else if (u.compare(0, 8, "https://") == 0) {
        if (err) *err = "https is not supported, use http://";
        return false;
    }
    const size_t slash = u.find('/');
    std::string hostPort = u.substr(0, slash);
    std::string prefix = slash == std::string::npos ? "" : u.substr(slash);
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    Target r;
    const size_t colon = hostPort.find(':');
    if (colon != std::string::npos) {
        const long p = strtol(hostPort.c_str() + colon + 1, nullptr, 10);
        if (p <= 0 || p > 65535) {
            if (err) *err = "Bad port in address";
            return false;
        }
        r.port = uint16_t(p);
        hostPort.resize(colon);
    }
    if (hostPort.empty()) {
        if (err) *err = "Missing host name";
        return false;
    }
    r.host = hostPort;
    r.prefix = prefix;
    *t = r;
    return true;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            strchr("-_.~/", c)) {
            o += char(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

bool is_already_exists(const Reply& r) {
    return r.status == 400 && r.body.find("already exists") != std::string::npos;
}

bool Client::request(const std::string& head, const std::string& bodyStart, uint32_t srcSize,
                     const Source* src, const std::string& bodyEnd, Reply* r,
                     const Progress& progress, std::string* err) {
    *r = Reply();
    uint32_t done = 0;
    const uint32_t total = uint32_t(bodyStart.size()) + srcSize + uint32_t(bodyEnd.size());
    bool cancelled = false;
    net::KeepGoing keep = [&] {
        if (progress && !progress(done, total)) {
            cancelled = true;
            return false;
        }
        return true;
    };
    net::Tcp tcp;
    if (!tcp.connect(ip_, t_.port, 8000, keep, err)) return false;
    if (!tcp.send(head.data(), uint32_t(head.size()), 15000, keep, err)) return false;
    if (!bodyStart.empty() && !tcp.send(bodyStart.data(), uint32_t(bodyStart.size()), 15000, keep, err))
        return false;
    done = uint32_t(bodyStart.size());
    if (src) {
        static uint8_t buf[2048];
        for (uint32_t off = 0; off < srcSize;) {
            const uint32_t want = std::min<uint32_t>(sizeof buf, srcSize - off);
            const int n = (*src)(off, buf, want);
            if (n <= 0) {
                if (err) *err = "Cannot read the file to send";
                return false;
            }
            if (!tcp.send(buf, uint32_t(n), 15000, keep, err)) return false;
            off += uint32_t(n);
            done += uint32_t(n);
            if (progress && !progress(done, total)) {
                if (err) *err = "Cancelled";
                return false;
            }
        }
    }
    if (!bodyEnd.empty() && !tcp.send(bodyEnd.data(), uint32_t(bodyEnd.size()), 15000, keep, err))
        return false;
    done = total;
    // Response: status line, headers, then the body until the peer closes
    // (we asked for Connection: close) or Content-Length is reached.
    std::string resp;
    long contentLength = -1;
    size_t bodyAt = std::string::npos;
    char buf[512];
    for (;;) {
        const int n = tcp.recv(buf, sizeof buf, 30000, keep, err);
        if (n < 0) {
            if (bodyAt != std::string::npos) break;  // got headers: use what we have
            return false;
        }
        if (n == 0) break;
        if (resp.size() < 4096) resp.append(buf, size_t(n));
        if (bodyAt == std::string::npos) {
            bodyAt = resp.find("\r\n\r\n");
            if (bodyAt != std::string::npos) {
                bodyAt += 4;
                std::string h = resp.substr(0, bodyAt);
                for (char& c : h) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
                const size_t cl = h.find("\ncontent-length:");
                if (cl != std::string::npos) contentLength = strtol(h.c_str() + cl + 16, nullptr, 10);
            }
        }
        if (bodyAt != std::string::npos && contentLength >= 0 &&
            resp.size() >= bodyAt + size_t(contentLength))
            break;
    }
    tcp.close();
    if (resp.compare(0, 5, "HTTP/") != 0) {
        if (err) *err = "Not an HTTP reply";
        return false;
    }
    const size_t sp = resp.find(' ');
    r->status = sp == std::string::npos ? 0 : atoi(resp.c_str() + sp + 1);
    if (bodyAt != std::string::npos) r->body = resp.substr(bodyAt, 512);
    (void)cancelled;
    return true;
}

bool Client::status(Reply* r, const net::KeepGoing& kg, std::string* err) {
    const std::string head = "GET " + t_.prefix + "/api/status HTTP/1.1\r\nHost: " + t_.host +
                             "\r\nConnection: close\r\n\r\n";
    return request(head, "", 0, nullptr, "", r, [&](uint32_t, uint32_t) { return !kg || kg(); }, err);
}

bool Client::upload(const std::string& folder, const std::string& name, uint32_t size,
                    const Source& src, Reply* r, const Progress& progress, std::string* err) {
    static const char kBoundary[] = "----PicoPodsBoundary7MA4YWxkTrZu0gW";
    std::string dir = folder.empty() ? "/" : folder;
    if (dir[0] != '/') dir = "/" + dir;
    const std::string start = std::string("--") + kBoundary +
                              "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + name +
                              "\"\r\nContent-Type: application/epub+zip\r\n\r\n";
    const std::string end = std::string("\r\n--") + kBoundary + "--\r\n";
    const uint32_t len = uint32_t(start.size()) + size + uint32_t(end.size());
    char cl[16];
    snprintf(cl, sizeof cl, "%lu", (unsigned long)len);
    const std::string head = "POST " + t_.prefix + "/upload?path=" + url_encode(dir) +
                             " HTTP/1.1\r\nHost: " + t_.host +
                             "\r\nContent-Type: multipart/form-data; boundary=" + kBoundary +
                             "\r\nContent-Length: " + cl + "\r\nConnection: close\r\n\r\n";
    return request(head, start, size, &src, end, r, progress, err);
}

bool Client::remove(const std::string& path, Reply* r, const net::KeepGoing& kg, std::string* err) {
    const std::string body = "path=" + url_encode(path);
    char cl[16];
    snprintf(cl, sizeof cl, "%lu", (unsigned long)body.size());
    const std::string head = "POST " + t_.prefix + "/delete HTTP/1.1\r\nHost: " + t_.host +
                             "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
                             cl + "\r\nConnection: close\r\n\r\n";
    return request(head, body, 0, nullptr, "", r, [&](uint32_t, uint32_t) { return !kg || kg(); }, err);
}

}  // namespace pd::crosspoint
