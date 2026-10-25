// Host tests for pd_bt's OBEX Object Push receiver: real OBEX requests built
// with BTstack's message builder, fed in awkward chunk sizes.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../lib/services/bt/src/obex_receiver.h"
#include "pd/fs.h"

extern "C" {
#include "btstack_util.h"
#include "classic/obex.h"
#include "classic/obex_message_builder.h"
}

using pd::bt::detail::ObexReceiver;
static int g_checks = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(c)) {                                                       \
            fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

using Pkt = std::vector<uint8_t>;
static Pkt finish(Pkt p) {
    p.resize(big_endian_read_16(p.data(), 1));
    return p;
}
static Pkt connect_req() {
    Pkt p(64);
    obex_message_builder_request_create_connect(p.data(), 64, OBEX_VERSION, 0, 1024);
    return finish(p);
}
// PUT packets for one object (name "" = no Name header; max = packet size)
static std::vector<Pkt> put_object(const std::string& name, const std::string& data, uint16_t max = 256) {
    std::vector<Pkt> out;
    size_t off = 0;
    bool first = true;
    do {
        Pkt p(max);
        obex_message_builder_request_create_put(p.data(), max, OBEX_CONNECTION_ID_INVALID);
        if (first) {
            if (!name.empty()) {   // proper UTF-16, as phones and PCs send it
                const std::vector<uint8_t> nm = pd::bt::detail::obex_name_utf16(name);
                obex_message_builder_header_add_variable(p.data(), max, OBEX_HEADER_NAME, nm.data(), uint16_t(nm.size()));
            }
            obex_message_builder_header_add_length(p.data(), max, uint32_t(data.size()));
            first = false;
        }
        const uint16_t room = uint16_t(max - big_endian_read_16(p.data(), 1) - 3);
        const size_t k = std::min<size_t>(room, data.size() - off);
        const bool last = off + k == data.size();
        obex_message_builder_header_add_variable(p.data(), max, last ? OBEX_HEADER_END_OF_BODY : OBEX_HEADER_BODY,
                                                 reinterpret_cast<const uint8_t*>(data.data()) + off, uint16_t(k));
        obex_message_builder_set_final_bit(p.data(), max, last);
        out.push_back(finish(p));
        off += k;
    } while (off < data.size());
    return out;
}
// Feeds one packet in chunks of `chunk` bytes; returns the reply at its end.
static ObexReceiver::Reply feed(ObexReceiver& rx, const Pkt& p, size_t chunk) {
    ObexReceiver::Reply r = ObexReceiver::Reply::None;
    for (size_t i = 0; i < p.size(); i += chunk) {
        const uint16_t k = uint16_t(std::min(chunk, p.size() - i));
        const ObexReceiver::Reply x = rx.feed(p.data() + i, k);
        if (i + k < p.size()) CHECK(x == ObexReceiver::Reply::None);   // nothing before the packet ends
        r = x;
    }
    return r;
}
static std::string read(const char* path) {
    std::string s;
    CHECK(pd::fs::read_file(path, s));
    return s;
}

int main(int argc, char** argv) {
    pd::fs::set_root(argc > 1 ? argv[1] : "obex_sd");
    CHECK(pd::fs::mount());
    pd::fs::mkdirs("/in");
    std::vector<pd::fs::DirEntry> old;
    pd::fs::list_dir("/in", old, 1000, true);
    for (auto& e : old) pd::fs::remove(("/in/" + e.name).c_str());

    std::string big;
    for (int i = 0; i < 5000; ++i) big += char('A' + (i * 7) % 26);

    for (size_t chunk : {size_t(1), size_t(7), size_t(4096)}) {
        fprintf(stderr, "[obex] chunk size %zu\n", chunk);
        ObexReceiver rx;
        rx.begin("/in");
        CHECK(feed(rx, connect_req(), chunk) == ObexReceiver::Reply::Connect);

        // Multi-packet file: CONTINUE for each part, SUCCESS at the end.
        const std::vector<Pkt> pk = put_object("città.txt", big);
        CHECK(pk.size() > 3);
        for (size_t i = 0; i < pk.size(); ++i) {
            CHECK(feed(rx, pk[i], chunk) == ObexReceiver::Reply::General);
            CHECK(rx.response_code() == (i + 1 < pk.size() ? OBEX_RESP_CONTINUE : OBEX_RESP_SUCCESS));
            if (i + 1 < pk.size()) CHECK(rx.active() && rx.total() == big.size());
        }
        const std::string first = chunk == 1 ? "/in/città.txt" : chunk == 7 ? "/in/città (2).txt" : "/in/città (3).txt";
        CHECK(!rx.active() && rx.received.size() == 1 && rx.received[0] == first);
        CHECK(read(first.c_str()) == big);

        // Hostile name: no paths, no FAT-illegal characters.
        for (const Pkt& p : put_object("../../evil/a:b?.txt", "x")) feed(rx, p, chunk);
        CHECK(rx.response_code() == OBEX_RESP_SUCCESS && rx.received.back() == "/in/a_b_.txt" + std::string(chunk == 1 ? "" : chunk == 7 ? "" : ""));

        // Empty file.
        for (const Pkt& p : put_object("empty.bin", "")) feed(rx, p, chunk);
        CHECK(rx.response_code() == OBEX_RESP_SUCCESS);
        CHECK(read(rx.received.back().c_str()).empty());

        // Abort in the middle: nothing left behind.
        const std::vector<Pkt> pa = put_object("aborted.txt", big);
        feed(rx, pa[0], chunk);
        CHECK(rx.response_code() == OBEX_RESP_CONTINUE && rx.active());
        Pkt ab(16);
        obex_message_builder_request_create_abort(ab.data(), 16, OBEX_CONNECTION_ID_INVALID);
        CHECK(feed(rx, finish(ab), chunk) == ObexReceiver::Reply::General && rx.response_code() == OBEX_RESP_SUCCESS);
        CHECK(!rx.active() && !pd::fs::exists("/in/aborted.txt") && !pd::fs::exists("/in/.bt-incoming.part"));

        // Delete request (final PUT without body) and GET are refused.
        Pkt del(64);
        obex_message_builder_request_create_put(del.data(), 64, OBEX_CONNECTION_ID_INVALID);
        obex_message_builder_header_add_name(del.data(), 64, "x.txt");
        CHECK(feed(rx, finish(del), chunk) == ObexReceiver::Reply::General && rx.response_code() == OBEX_RESP_FORBIDDEN);
        Pkt get(16);
        obex_message_builder_request_create_get(get.data(), 16, OBEX_CONNECTION_ID_INVALID);
        CHECK(feed(rx, finish(get), chunk) == ObexReceiver::Reply::General && rx.response_code() == OBEX_RESP_FORBIDDEN);

        // Connection lost mid-file: partial file removed.
        feed(rx, pa[0], chunk);
        CHECK(rx.active());
        rx.closed();
        CHECK(!rx.active() && !pd::fs::exists("/in/.bt-incoming.part"));

        Pkt dc(16);
        obex_message_builder_request_create_disconnect(dc.data(), 16, OBEX_CONNECTION_ID_INVALID);
        CHECK(feed(rx, finish(dc), chunk) == ObexReceiver::Reply::General && rx.response_code() == OBEX_RESP_SUCCESS);
        CHECK(rx.received.size() == 3);
        // the next rounds reuse names: unique names are made
        pd::fs::remove("/in/a_b_.txt");
        pd::fs::remove("/in/empty.bin");
    }

    // Unwritable destination: refused, no crash.
    ObexReceiver rx;
    rx.begin("/no/such/folder");
    for (const Pkt& p : put_object("f.txt", big)) feed(rx, p, 4096);
    CHECK(rx.response_code() == OBEX_RESP_FORBIDDEN && rx.received.empty() && !rx.error.empty());

    // Round trip of the name encoding, including a character outside the BMP.
    for (const char* n : {"città.txt", "日本語.md", "emoji-\xF0\x9F\x98\x80.png", "plain.txt"}) {
        pd::fs::remove((std::string("/in/") + n).c_str());
        ObexReceiver r2;
        r2.begin("/in");
        for (const Pkt& p : put_object(n, "z")) feed(r2, p, 5);
        CHECK(r2.response_code() == OBEX_RESP_SUCCESS && r2.received.back() == std::string("/in/") + n);
    }
    CHECK(ObexReceiver::safe_name("") == "received" && ObexReceiver::safe_name("..") == "received");
    CHECK(ObexReceiver::safe_name("C:\\Users\\me\\photo.jpg") == "photo.jpg");
    CHECK(ObexReceiver::safe_name("notes. ") == "notes");
    fprintf(stderr, "[obex] %d checks passed\n", g_checks);
    return 0;
}
