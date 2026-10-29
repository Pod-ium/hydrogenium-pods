// pd_bt internal: OBEX Object Push receiving side (see obex_receiver.h).
#include "obex_receiver.h"

#include <cstring>

#include "pd/fs_ops.h"
#include "pd/path.h"

extern "C" {
#include "classic/obex.h"
}

namespace pd::bt::detail {

namespace {

// UTF-16BE (OBEX Name header, NUL-terminated) -> UTF-8
std::string utf16be_to_utf8(const uint8_t* p, size_t n) {
    std::string out;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t c = (uint32_t(p[i]) << 8) | p[i + 1];
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {   // surrogate pair
            const uint32_t lo = (uint32_t(p[i + 2]) << 8) | p[i + 3];
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

}  // namespace

std::vector<uint8_t> obex_name_utf16(const std::string& in) {
    std::vector<uint8_t> out;
    auto unit = [&](uint32_t u) {
        out.push_back(uint8_t(u >> 8));
        out.push_back(uint8_t(u));
    };
    for (size_t i = 0; i < in.size();) {
        const uint8_t c = uint8_t(in[i]);
        uint32_t cp;
        int n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 6) cp = c & 0x1F, n = 2;
        else if ((c >> 4) == 14) cp = c & 0x0F, n = 3;
        else if ((c >> 3) == 30) cp = c & 0x07, n = 4;
        else cp = '?', n = 1;   // stray byte
        if (i + size_t(n) > in.size()) {
            cp = '?';
            n = 1;
        }
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (uint8_t(in[i + size_t(k)]) & 0x3F);
        i += size_t(n);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 + (cp >> 10));
            unit(0xDC00 + (cp & 0x3FF));
        } else {
            unit(cp);
        }
    }
    unit(0);
    return out;
}

std::string ObexReceiver::safe_name(const std::string& raw) {
    std::string n = raw;
    const size_t cut = n.find_last_of("/\\");   // never a path
    if (cut != std::string::npos) n = n.substr(cut + 1);
    for (char& c : n) {
        const unsigned char u = uint8_t(c);
        if (u < 0x20 || strchr("\"*/:<>?\\|", c)) c = '_';
    }
    while (!n.empty() && (n.back() == '.' || n.back() == ' ')) n.pop_back();
    while (!n.empty() && n[0] == ' ') n.erase(0, 1);
    if (n.size() > 200) n = n.substr(n.size() - 200);
    if (n.empty() || n == "." || n == "..") n = "received";
    return n;
}

void ObexReceiver::begin(const std::string& dir) {
    abort_file();
    dir_ = dir;
    parsing_ = false;
    name_.clear();
    done_ = total_ = 0;
    received.clear();
    error.clear();
}

void ObexReceiver::on_header(void* user, uint8_t id, uint16_t total_len, uint16_t offset, const uint8_t* data,
                             uint16_t len) {
    static_cast<ObexReceiver*>(user)->header(id, total_len, offset, data, len);
}

void ObexReceiver::header(uint8_t id, uint16_t total_len, uint16_t offset, const uint8_t* data, uint16_t len) {
    switch (id) {
        case OBEX_HEADER_NAME:
            if (obex_parser_header_store(name_buf_, sizeof name_buf_, total_len, offset, data, len) ==
                OBEX_PARSER_HEADER_COMPLETE) {
                name_len_ = total_len < sizeof name_buf_ ? total_len : uint16_t(sizeof name_buf_);
                name_ = safe_name(utf16be_to_utf8(name_buf_, name_len_));
            }
            break;
        case OBEX_HEADER_LENGTH:
            if (obex_parser_header_store(len_buf_, sizeof len_buf_, total_len, offset, data, len) ==
                OBEX_PARSER_HEADER_COMPLETE)
                got_length_ = true, total_ = (uint32_t(len_buf_[0]) << 24) | (uint32_t(len_buf_[1]) << 16) | (uint32_t(len_buf_[2]) << 8) |
                         len_buf_[3];
            break;
        case OBEX_HEADER_BODY:
        case OBEX_HEADER_END_OF_BODY:
            got_body_ = true;
            if (write_failed_) break;
            if (!file_.is_open() && !open_file()) {
                write_failed_ = true;
                break;
            }
            if (len && file_.write(data, len) != int(len)) {
                write_failed_ = true;
                error = "write failed (card full?)";
                break;
            }
            done_ += len;
            break;
        default:
            break;
    }
}

bool ObexReceiver::open_file() {
    if (name_.empty()) name_ = "received";
    part_path_ = pd::path::join(dir_, ".bt-incoming.part");
    if (!file_.open(part_path_, fs::Mode::Write)) {
        error = "cannot create files in " + dir_;
        return false;
    }
    return true;
}

void ObexReceiver::finish_file(bool ok) {
    if (!file_.is_open()) return;
    ok = file_.sync() && ok;
    file_.close();
    if (!ok) {
        fs::remove(part_path_.c_str());
        return;
    }
    const std::string final_name = fs::unique_name(dir_, name_);
    const std::string final_path = pd::path::join(dir_, final_name);
    if (!fs::rename(part_path_.c_str(), final_path.c_str())) {
        error = "cannot rename the received file";
        fs::remove(part_path_.c_str());
        return;
    }
    name_ = final_name;
    received.push_back(final_path);
}

void ObexReceiver::abort_file() {
    if (file_.is_open()) {
        file_.close();
        fs::remove(part_path_.c_str());
    }
}

void ObexReceiver::closed() {
    abort_file();
    parsing_ = false;
}

ObexReceiver::Reply ObexReceiver::feed(const uint8_t* data, uint16_t len) {
    if (!parsing_) {
        obex_parser_init_for_request(&parser_, &ObexReceiver::on_header, this);
        parsing_ = true;
    }
    const obex_parser_object_state_t st = obex_parser_process_data(&parser_, data, len);
    if (st == OBEX_PARSER_OBJECT_STATE_INCOMPLETE) return Reply::None;
    parsing_ = false;
    if (st != OBEX_PARSER_OBJECT_STATE_COMPLETE) {
        abort_file();
        code_ = OBEX_RESP_BAD_REQUEST;
        return Reply::General;
    }
    obex_parser_operation_info_t op;
    obex_parser_get_operation_info(&parser_, &op);
    const uint8_t opcode = op.opcode;
    opcode_ = opcode;
    const bool final = (opcode & OBEX_OPCODE_FINAL_BIT_MASK) != 0;
    switch (opcode & 0x7F) {
        case OBEX_OPCODE_CONNECT & 0x7F:
            if (opcode == OBEX_OPCODE_CONNECT) return Reply::Connect;
            break;
        case OBEX_OPCODE_DISCONNECT & 0x7F:
            if (opcode == OBEX_OPCODE_DISCONNECT) {
                abort_file();
                code_ = OBEX_RESP_SUCCESS;
                return Reply::General;
            }
            break;
        case OBEX_OPCODE_PUT: {
            if (write_failed_) {
                abort_file();
                write_failed_ = false;
                got_body_ = false;
                code_ = OBEX_RESP_FORBIDDEN;
                return Reply::General;
            }
            if (!final) {
                code_ = OBEX_RESP_CONTINUE;
                return Reply::General;
            }
            // Final PUT: the object is complete. BTstack's parser skips empty
            // headers, so an empty End-of-Body is invisible: senders give a
            // Length header with files (Length 0 for an empty one), while a
            // "delete" request has neither body nor length.
            if (!got_body_ && !got_length_) {
                code_ = OBEX_RESP_FORBIDDEN;
            } else {
                const size_t before = received.size();
                if (!file_.is_open()) open_file();   // empty file (End-of-Body of length 0)
                finish_file(true);
                code_ = received.size() > before ? OBEX_RESP_SUCCESS : OBEX_RESP_FORBIDDEN;
            }
            name_.clear();   // the next object brings its own name
            got_length_ = false;
            got_body_ = false;
            name_len_ = 0;
            return Reply::General;
        }
        case OBEX_OPCODE_ABORT & 0x7F:
            if (opcode == OBEX_OPCODE_ABORT) {
                abort_file();
                got_body_ = false;
                got_length_ = false;
                write_failed_ = false;
                code_ = OBEX_RESP_SUCCESS;
                return Reply::General;
            }
            break;
        default:
            break;
    }
    // GET (business card pull), SETPATH and anything else: not offered.
    code_ = ((opcode & 0x7F) == (OBEX_OPCODE_GET & 0x7F) || opcode == OBEX_OPCODE_SETPATH) ? OBEX_RESP_FORBIDDEN
                                                                                         : OBEX_RESP_BAD_REQUEST;
    return Reply::General;
}

}  // namespace pd::bt::detail
