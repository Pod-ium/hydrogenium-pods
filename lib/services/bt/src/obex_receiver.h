// pd_bt internal: the receiving side of OBEX Object Push, independent of the
// radio (fed with the bytes GOEP delivers), so it can be tested on the host.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "pd/fs.h"

extern "C" {
#include "classic/obex_parser.h"
}

namespace pd::bt::detail {

// OBEX Name header value: UTF-8 -> UTF-16BE with the terminating NUL.
// (BTstack's own helper widens bytes, garbling any non-ASCII name.)
std::vector<uint8_t> obex_name_utf16(const std::string& utf8);

class ObexReceiver {
public:
    enum class Reply { None, Connect, General };

    void begin(const std::string& dir);   // folder for received files
    // Feeds request bytes (any chunking). When a whole request has arrived,
    // returns what to answer: Connect (a CONNECT response) or General with
    // response_code().
    Reply feed(const uint8_t* data, uint16_t len);
    uint8_t response_code() const { return code_; }
    uint8_t last_opcode() const { return opcode_; }   // of the last complete request
    void closed();                        // connection gone: drop a partial file

    // For the UI
    bool active() const { return file_.is_open(); }
    const std::string& name() const { return name_; }
    uint64_t done() const { return done_; }
    uint64_t total() const { return total_; }
    std::vector<std::string> received;   // complete files
    std::string error;

    // "a/b:c?.txt" (UTF-8) -> "b_c_.txt"; never empty.
    static std::string safe_name(const std::string& raw);

private:
    static void on_header(void* user, uint8_t id, uint16_t total_len, uint16_t offset, const uint8_t* data, uint16_t len);
    void header(uint8_t id, uint16_t total_len, uint16_t offset, const uint8_t* data, uint16_t len);
    bool open_file();
    void finish_file(bool ok);
    void abort_file();

    obex_parser_t parser_;
    bool parsing_ = false;
    uint8_t code_ = 0;
    uint8_t opcode_ = 0;
    std::string dir_, name_, part_path_;
    uint8_t name_buf_[512];
    uint16_t name_len_ = 0;
    uint8_t len_buf_[4];
    bool got_body_ = false;
    bool got_length_ = false;
    bool write_failed_ = false;
    fs::File file_;
    uint64_t done_ = 0, total_ = 0;
};

}  // namespace pd::bt::detail
