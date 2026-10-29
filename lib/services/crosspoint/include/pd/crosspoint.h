// PicoPods - upload to an e-reader running CrossPoint (Xteink X3 / X4) over HTTP.
//
// CrossPoint's file-transfer web server (open "File Transfer" on the reader,
// joined to the same Wi-Fi) accepts:
//   POST /upload?path=<folder>   multipart/form-data, field "file"
//                                (400 "File already exists" if present)
//   POST /delete                 form field path=<full path>
//   GET  /api/status             JSON with version, ip, device
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "pd/net.h"

namespace pd::crosspoint {

struct Target {
    std::string host;
    uint16_t port = 80;
    std::string prefix;  // path prefix from the URL, without trailing '/'
};

constexpr const char* kDefaultUrl = "http://crosspoint.local/";

// Accepts "http://host[:port][/prefix]", "host", "host:port" or an IP.
bool parse_url(const std::string& url, Target* t, std::string* err);

struct Reply {
    int status = 0;     // HTTP status, 0 if none
    std::string body;   // first 512 bytes of the response body
};

// Byte source for the upload body: returns bytes read at offset, <0 on error.
using Source = std::function<int(uint32_t off, uint8_t* buf, uint32_t n)>;
using Progress = std::function<bool(uint32_t done, uint32_t total)>;  // false = cancel

class Client {
public:
    // ip: already resolved address of t.host (network byte order).
    Client(const Target& t, uint32_t ip) : t_(t), ip_(ip) {}

    bool status(Reply* r, const net::KeepGoing& kg, std::string* err);
    bool upload(const std::string& folder, const std::string& name, uint32_t size, const Source& src,
                Reply* r, const Progress& progress, std::string* err);
    bool remove(const std::string& path, Reply* r, const net::KeepGoing& kg, std::string* err);

private:
    bool request(const std::string& head, const std::string& bodyStart, uint32_t srcSize,
                 const Source* src, const std::string& bodyEnd, Reply* r, const Progress& progress,
                 std::string* err);
    Target t_;
    uint32_t ip_;
};

std::string url_encode(const std::string& s);
// True if a failed upload reply means the file is already on the reader.
bool is_already_exists(const Reply& r);

}  // namespace pd::crosspoint
