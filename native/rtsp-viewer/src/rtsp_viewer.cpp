// SPDX-License-Identifier: GPL-3.0-or-later
// RTSP/RTP H.264 receiver for BMW MGU. Displays frames through the existing MGU renderer MGVF path.
#include "decoder_msdk.h"
#include "mgvf.h"

#include <arpa/inet.h>
#include <grp.h>
#include <netdb.h>
#include <poll.h>
#include <resolv.h>
#include <arpa/nameser.h>
#include <sys/types.h>
#include <sys/fsuid.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <fstream>
#include <iterator>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
const char *kVideoPath = "/dev/shm/mgu-video-rtsp";
const char *kSocketPath = "/run/rtsp-viewer/ctl.sock";
volatile sig_atomic_t stopping = 0;
int status_fd = -1;
uid_t network_uid = 0;
bool control_stop_requested = false;
std::string control_input;
void stop_signal(int) { stopping = 1; }

class ScopedFsuid {
public:
    explicit ScopedFsuid(uid_t uid) : old_(setfsuid(uid)), valid_(static_cast<uid_t>(setfsuid((uid_t)-1)) == uid) {}
    ~ScopedFsuid() { setfsuid(old_); }
    bool valid() const { return valid_; }
private:
    uid_t old_;
    bool valid_;
};

std::vector<std::string> uplink_dns_servers() {
    std::ifstream f("/run/mgu-uplink/lease.json");
    if (!f) return {};
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t key = json.find("\"dns\"");
    const size_t begin = key == std::string::npos ? key : json.find('[', key);
    const size_t end = begin == std::string::npos ? begin : json.find(']', begin);
    if (end == std::string::npos) return {};
    std::vector<std::string> out;
    for (size_t i = begin + 1; i < end;) {
        const size_t q = json.find('"', i);
        if (q == std::string::npos || q >= end) break;
        const size_t qe = json.find('"', q + 1);
        if (qe == std::string::npos || qe > end) break;
        const std::string ip = json.substr(q + 1, qe - q - 1);
        in_addr addr{};
        if (inet_pton(AF_INET, ip.c_str(), &addr) == 1) out.push_back(ip);
        i = qe + 1;
    }
    return out;
}

bool resolve_uplink_ipv4(const std::string &host, const std::string &port, std::vector<sockaddr_in> *out, std::string *err) {
    const std::vector<std::string> servers = uplink_dns_servers();
    if (servers.empty()) return false;
    struct __res_state state{};
    if (res_ninit(&state) != 0) { *err = "could not initialize uplink DNS resolver"; return false; }
    state.nscount = (int)std::min(servers.size(), (size_t)MAXNS);
    for (int i = 0; i < state.nscount; ++i) {
        state.nsaddr_list[i].sin_family = AF_INET;
        state.nsaddr_list[i].sin_port = htons(NS_DEFAULTPORT);
        inet_pton(AF_INET, servers[(size_t)i].c_str(), &state.nsaddr_list[i].sin_addr);
    }
    unsigned char answer[8192];
    const int n = res_nquery(&state, host.c_str(), ns_c_in, ns_t_a, answer, sizeof answer);
    res_nclose(&state);
    if (n < 0) { *err = "MGU Uplink DNS lookup failed for " + host; return false; }
    ns_msg msg;
    if (ns_initparse(answer, n, &msg) < 0) { *err = "invalid DNS response from MGU Uplink"; return false; }
    char *end = nullptr; const long p = strtol(port.c_str(), &end, 10);
    if (!end || *end || p < 1 || p > 65535) { *err = "invalid RTSP port"; return false; }
    for (int i = 0; i < ns_msg_count(msg, ns_s_an); ++i) {
        ns_rr rr;
        if (ns_parserr(&msg, ns_s_an, i, &rr) != 0 || ns_rr_type(rr) != ns_t_a || ns_rr_rdlen(rr) != 4) continue;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)p);
        memcpy(&a.sin_addr, ns_rr_rdata(rr), 4); out->push_back(a);
    }
    if (out->empty()) { *err = "MGU Uplink DNS returned no IPv4 address for " + host; return false; }
    return true;
}

int run_stream(const std::string &raw_url);

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
bool read_control_stop() {
    if (status_fd < 0) return false;
    char buf[128];
    for (;;) {
        ssize_t n = recv(status_fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
            control_input.append(buf, (size_t)n);
            size_t nl;
            while ((nl = control_input.find('\n')) != std::string::npos) {
                std::string line = trim(control_input.substr(0, nl));
                control_input.erase(0, nl + 1);
                if (line == "STOP") control_stop_requested = true;
            }
            if (control_input.size() > 1024) control_input.clear();
            continue;
        }
        if (n == 0) control_stop_requested = true;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) control_stop_requested = true;
        break;
    }
    return control_stop_requested;
}
std::string decode_url(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char h[3] = {s[i + 1], s[i + 2], 0}; char *e = nullptr; long v = strtol(h, &e, 16);
            if (e && !*e) { o.push_back((char)v); i += 2; continue; }
        }
        o.push_back(s[i]);
    }
    return o;
}
std::string base64(const std::string &in) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; unsigned val = 0; int bits = -6;
    for (unsigned char c : in) { val = (val << 8) | c; bits += 8; while (bits >= 0) { o.push_back(t[(val >> bits) & 63]); bits -= 6; } }
    if (bits > -6) o.push_back(t[((val << 8) >> (bits + 8)) & 63]);
    while (o.size() % 4) o.push_back('=');
    return o;
}
bool unbase64(const std::string &in, std::vector<uint8_t> *out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    unsigned acc = 0; int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = value(c); if (v < 0) return false;
        acc = (acc << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; out->push_back((uint8_t)(acc >> bits)); }
    }
    return !out->empty();
}

uint32_t rol32(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

std::string md5_hex(const std::string &input) {
    static const uint32_t k[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const uint32_t s[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    std::vector<uint8_t> msg(input.begin(), input.end());
    const uint64_t bits = (uint64_t)msg.size() * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 0; i < 8; ++i) msg.push_back((uint8_t)(bits >> (8 * i)));
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) m[i] = (uint32_t)msg[off + i*4] | ((uint32_t)msg[off + i*4+1] << 8) |
            ((uint32_t)msg[off + i*4+2] << 16) | ((uint32_t)msg[off + i*4+3] << 24);
        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t f, g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3*i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7*i) % 16; }
            const uint32_t old_d = d;
            d = c; c = b; b += rol32(a + f + k[i] + m[g], s[i]); a = old_d;
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    }
    std::ostringstream out; out << std::hex << std::setfill('0');
    const uint32_t words[4] = {a0,b0,c0,d0};
    for (uint32_t w : words) for (int i = 0; i < 4; ++i) out << std::setw(2) << ((w >> (8*i)) & 0xff);
    return out.str();
}

struct Url {
    std::string host, port = "554", path, user, pass;
    bool parse(std::string s, std::string *err) {
        const size_t scheme = s.find("://");
        if (scheme == std::string::npos || (lower(s.substr(0, scheme)) != "rtsp" && lower(s.substr(0, scheme)) != "rtsps")) {
            *err = "URL must start with rtsp:// (rtsps is not supported by this build)"; return false;
        }
        if (lower(s.substr(0, scheme)) == "rtsps") { *err = "RTSPS/TLS is not supported yet; use RTSP over a trusted LAN"; return false; }
        size_t auth = scheme + 3, end = s.find_first_of("/?#", auth);
        if (end == std::string::npos) end = s.size();
        std::string authority = s.substr(auth, end - auth);
        size_t at = authority.rfind('@');
        if (at != std::string::npos) {
            std::string up = authority.substr(0, at); authority.erase(0, at + 1);
            size_t colon = up.find(':'); user = decode_url(up.substr(0, colon));
            if (colon != std::string::npos) pass = decode_url(up.substr(colon + 1));
        }
        if (!authority.empty() && authority[0] == '[') {
            size_t close = authority.find(']'); if (close == std::string::npos) { *err = "Invalid IPv6 host"; return false; }
            host = authority.substr(1, close - 1);
            if (close + 1 < authority.size() && authority[close + 1] == ':') port = authority.substr(close + 2);
        } else {
            size_t colon = authority.rfind(':');
            if (colon != std::string::npos && authority.find(':') == colon) { host = authority.substr(0, colon); port = authority.substr(colon + 1); }
            else host = authority;
        }
        path = end < s.size() ? s.substr(end) : "/";
        size_t hash = path.find('#'); if (hash != std::string::npos) path.resize(hash);
        if (path.empty()) path = "/";
        if (host.empty() || port.empty()) { *err = "Camera URL needs a host and port"; return false; }
        return true;
    }
    std::string request_url() const {
        const std::string h = host.find(':') == std::string::npos ? host : "[" + host + "]";
        return "rtsp://" + h + ":" + port + path;
    }
};

struct Response { int status = 0; std::map<std::string, std::string> headers; std::string body; };

ssize_t recv_interruptible(int fd, void *buf, size_t length, int flags) {
    for (;;) {
        pollfd fds[2] = {{fd, POLLIN | POLLERR | POLLHUP, 0},
                         {status_fd, POLLIN | POLLRDHUP | POLLERR | POLLHUP, 0}};
        const nfds_t count = status_fd >= 0 ? 2 : 1;
        int ready = poll(fds, count, 10000);
        if (ready < 0) { if (errno == EINTR) continue; return -1; }
        if (ready == 0) { errno = ETIMEDOUT; return -1; }
        if (status_fd >= 0 && (fds[1].revents & (POLLRDHUP | POLLERR | POLLHUP | POLLNVAL))) {
            control_stop_requested = true;
            errno = ECANCELED; return -1;
        }
        if (status_fd >= 0 && (fds[1].revents & POLLIN) && read_control_stop()) {
            errno = ECANCELED; return -1;
        }
        if (fds[0].revents & POLLNVAL) { errno = EBADF; return -1; }
        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) return recv(fd, buf, length, flags);
    }
}

bool read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        ssize_t r = recv_interruptible(fd, p, n, 0);
        if (r <= 0) { if (r == 0) errno = ECONNRESET; return false; }
        p += r; n -= (size_t)r;
    }
    return true;
}

std::string read_failure(const char *what) {
    if (errno == ECANCELED) return "Stream cancelled by app";
    if (errno == ETIMEDOUT) return std::string("Timed out while reading ") + what;
    return std::string("Camera disconnected while reading ") + what;
}

bool read_rtsp_header_byte(int fd, char *out, std::string *err) {
    for (;;) {
        ssize_t n = recv_interruptible(fd, out, 1, 0);
        if (n <= 0) {
            *err = errno == ECANCELED ? "Stream cancelled by app" :
                   errno == ETIMEDOUT ? "RTSP response timed out waiting for headers" :
                   "RTSP server disconnected while reading response headers";
            return false;
        }
        if ((uint8_t)*out != '$') return true;

        // RTP/RTCP interleaved over the RTSP TCP connection may arrive while a
        // response is pending. Skip the complete $ channel/length/payload frame
        // so binary bytes cannot be mistaken for an RTSP status line or header.
        // The frame length is uint16-bounded and payloads are drained in chunks;
        // do not cap their cumulative size because high-bitrate streams can
        // exceed 16 MiB while a camera is still completing its RTSP response.
        uint8_t frame[3];
        if (!read_exact(fd, frame, sizeof frame)) { *err = "Truncated interleaved frame in RTSP response headers"; return false; }
        size_t length = ((size_t)frame[1] << 8) | frame[2];
        uint8_t discard[4096];
        while (length) {
            size_t chunk = std::min(length, sizeof discard);
            if (!read_exact(fd, discard, chunk)) { *err = "Truncated interleaved payload in RTSP response headers"; return false; }
            length -= chunk;
        }
    }
}
bool send_exact(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) return false; p += w; n -= (size_t)w; }
    return true;
}

bool is_private_ipv4_literal(const std::string &host) {
    in_addr addr{};
    if (inet_pton(AF_INET, host.c_str(), &addr) != 1) return false;
    const uint32_t ip = ntohl(addr.s_addr);
    return (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8;
}

bool connect_tailscale_socks5(const Url &u, int *out_fd, std::string *err) {
    std::ifstream port_file("/var/opt/iteriodev-tailscale/socks5-port");
    std::string configured_port;
    if (!std::getline(port_file, configured_port)) {
        *err = "Tailscale SOCKS5 proxy is not enabled; update/restart the Tailscale tool"; return false;
    }
    char *port_end = nullptr;
    const unsigned long proxy_port = strtoul(configured_port.c_str(), &port_end, 10);
    while (port_end && (*port_end == ' ' || *port_end == '\t' || *port_end == '\r')) ++port_end;
    if (!proxy_port || proxy_port > 65535 || !port_end || *port_end) {
        *err = "Tailscale SOCKS5 port configuration is invalid"; return false;
    }
    const unsigned long port = strtoul(u.port.c_str(), nullptr, 10);
    if (!port || port > 65535 || u.port.find_first_not_of("0123456789") != std::string::npos) {
        *err = "Invalid RTSP camera port"; return false;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { *err = std::string("Cannot create Tailscale proxy socket: ") + strerror(errno); return false; }
    sockaddr_in proxy{}; proxy.sin_family = AF_INET; proxy.sin_port = htons((uint16_t)proxy_port); inet_pton(AF_INET, "127.0.0.2", &proxy.sin_addr);
    if (::connect(fd, (const sockaddr *)&proxy, sizeof proxy) != 0) {
        *err = std::string("Tailscale SOCKS5 proxy is unavailable at 127.0.0.2:") + std::to_string(proxy_port) + "; update/restart the Tailscale tool: " + strerror(errno);
        ::close(fd); return false;
    }
    const uint8_t greeting[] = {5, 1, 0}; uint8_t chosen[2];
    if (!send_exact(fd, greeting, sizeof greeting) || !read_exact(fd, chosen, sizeof chosen) || chosen[0] != 5 || chosen[1] != 0) {
        *err = "Tailscale SOCKS5 proxy rejected unauthenticated access"; ::close(fd); return false;
    }
    in_addr target{}; inet_pton(AF_INET, u.host.c_str(), &target);
    uint8_t request[10] = {5, 1, 0, 1};
    memcpy(request + 4, &target.s_addr, 4);
    request[8] = (uint8_t)(port >> 8); request[9] = (uint8_t)port;
    uint8_t response[4];
    if (!send_exact(fd, request, sizeof request) || !read_exact(fd, response, sizeof response)) {
        *err = "Tailscale SOCKS5 proxy closed during camera connection"; ::close(fd); return false;
    }
    if (response[0] != 5 || response[1] != 0) {
        *err = "Tailscale cannot reach the private camera (SOCKS5 error " + std::to_string(response[1]) + ")";
        ::close(fd); return false;
    }
    size_t address_length = response[3] == 1 ? 4 : response[3] == 4 ? 16 : 0;
    uint8_t domain_length = 0;
    if (response[3] == 3) { if (!read_exact(fd, &domain_length, 1)) { *err = "Truncated Tailscale SOCKS5 reply"; ::close(fd); return false; } address_length = domain_length; }
    else if (response[3] != 1 && response[3] != 4) { *err = "Invalid Tailscale SOCKS5 reply"; ::close(fd); return false; }
    uint8_t discard[258];
    if (!read_exact(fd, discard, address_length + 2)) { *err = "Truncated Tailscale SOCKS5 reply"; ::close(fd); return false; }
    *out_fd = fd;
    return true;
}

bool read_response(int fd, Response *out, std::string *err) {
    // Read through the response body, skipping interleaved frames that arrive
    // before/during the RTSP headers to keep the control stream aligned.
    *out = Response{};
    std::string data; char c; size_t split = std::string::npos;
    while ((split = data.find("\r\n\r\n")) == std::string::npos) {
        if (!read_rtsp_header_byte(fd, &c, err)) return false;
        // Some camera firmware adds large vendor-specific metadata to DESCRIBE
        // and authentication responses. Keep a generous but finite bound so
        // these headers are accepted without allowing unbounded allocation.
        data.push_back(c); if (data.size() > 1024 * 1024) { *err = "RTSP response headers exceed 1 MiB"; return false; }
    }
    std::istringstream hs(data.substr(0, split)); std::string line;
    while (std::getline(hs, line) && trim(line).empty()) {}
    if (trim(line).empty()) { *err = "Empty RTSP response status line"; return false; }
    std::istringstream status(line); std::string proto, code; status >> proto >> code;
    char *end = nullptr; const long code_value = strtol(code.c_str(), &end, 10);
    if (lower(proto) != "rtsp/1.0" || code.empty() || !end || *end || code_value < 100 || code_value > 999) {
        *err = "Malformed RTSP response status line: " + trim(line.substr(0, 120)); return false;
    }
    out->status = (int)code_value;
    while (std::getline(hs, line)) {
        size_t c = line.find(':'); if (c != std::string::npos) {
            std::string key = lower(trim(line.substr(0, c))), value = trim(line.substr(c + 1));
            if (key == "www-authenticate" && out->headers.count(key)) out->headers[key] += ", " + value;
            else out->headers[key] = value;
        }
    }
    size_t length = 0;
    if (out->headers.count("content-length")) length = (size_t)strtoul(out->headers["content-length"].c_str(), nullptr, 10);
    if (length > 2 * 1024 * 1024) { *err = "RTSP SDP is too large"; return false; }
    out->body.resize(length);
    if (length && !read_exact(fd, &out->body[0], length)) { *err = "Truncated RTSP response"; return false; }
    return true;
}

std::map<std::string, std::string> digest_params(const std::string &header) {
    std::map<std::string, std::string> p;
    size_t pos = lower(header).find("digest ");
    if (pos == std::string::npos) return p;
    pos += 7;
    while (pos < header.size()) {
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == ',')) ++pos;
        size_t eq = header.find('=', pos);
        if (eq == std::string::npos) break;
        std::string key = lower(trim(header.substr(pos, eq - pos)));
        pos = eq + 1;
        std::string value;
        if (pos < header.size() && header[pos] == '"') {
            ++pos;
            while (pos < header.size()) {
                char c = header[pos++];
                if (c == '"') break;
                if (c == '\\' && pos < header.size()) c = header[pos++];
                value.push_back(c);
            }
            while (pos < header.size() && header[pos] != ',') ++pos;
        } else {
            size_t end = header.find(',', pos);
            value = trim(header.substr(pos, end == std::string::npos ? end : end - pos));
            pos = end == std::string::npos ? header.size() : end;
        }
        p[key] = value;
    }
    return p;
}

std::string auth_quote(const std::string &value) {
    std::string out = "\"";
    for (char c : value) { if (c == '\\' || c == '"') out.push_back('\\'); out.push_back(c); }
    return out + "\"";
}

std::string make_cnonce(const std::string &fallback) {
    char bytes[12];
    std::ifstream random("/dev/urandom", std::ios::in | std::ios::binary);
    if (random.read(bytes, sizeof bytes)) {
        std::ostringstream out; out << std::hex << std::setfill('0');
        for (unsigned char c : bytes) out << std::setw(2) << (unsigned)c;
        return out.str();
    }
    return md5_hex(fallback).substr(0, 24);
}

std::string digest_authorization(const std::string &user, const std::string &password,
                                 const std::string &method, const std::string &uri,
                                 const std::map<std::string, std::string> &p,
                                 const std::string &cnonce, unsigned count, std::string *err) {
    auto get = [&](const char *key) -> std::string { auto it = p.find(key); return it == p.end() ? "" : it->second; };
    const std::string realm = get("realm"), nonce = get("nonce"), algorithm = get("algorithm");
    if (nonce.empty()) { *err = "Camera sent an incomplete Digest authentication challenge"; return ""; }
    if (!algorithm.empty() && lower(algorithm) != "md5" && lower(algorithm) != "md5-sess") {
        *err = "Camera Digest authentication uses unsupported algorithm " + algorithm + " (MD5 is supported)"; return "";
    }
    std::string qop;
    const std::string offered = get("qop");
    if (!offered.empty()) {
        std::istringstream choices(offered); std::string choice;
        while (std::getline(choices, choice, ',')) if (lower(trim(choice)) == "auth") { qop = "auth"; break; }
        if (qop.empty()) { *err = "Camera Digest challenge does not offer the supported qop=auth mode"; return ""; }
    }
    std::string ha1 = md5_hex(user + ":" + realm + ":" + password);
    if (lower(algorithm) == "md5-sess") ha1 = md5_hex(ha1 + ":" + nonce + ":" + cnonce);
    const std::string ha2 = md5_hex(method + ":" + uri);
    char nc[9]; snprintf(nc, sizeof nc, "%08x", count);
    const std::string response = qop.empty() ? md5_hex(ha1 + ":" + nonce + ":" + ha2)
        : md5_hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
    std::ostringstream out;
    out << "Digest username=" << auth_quote(user) << ", realm=" << auth_quote(realm)
        << ", nonce=" << auth_quote(nonce) << ", uri=" << auth_quote(uri)
        << ", response=" << auth_quote(response);
    if (!algorithm.empty()) out << ", algorithm=" << algorithm;
    if (!qop.empty()) out << ", qop=" << qop << ", nc=" << nc << ", cnonce=" << auth_quote(cnonce);
    else if (lower(algorithm) == "md5-sess") out << ", cnonce=" << auth_quote(cnonce);
    if (!get("opaque").empty()) out << ", opaque=" << auth_quote(get("opaque"));
    return out.str();
}

class Rtsp {
public:
    ~Rtsp() { close(); }
    bool connect_to(const Url &u, std::string *err) {
        if (!network_uid) { *err = "MGU Uplink network uid is unavailable; open this from the RTSP Camera app"; return false; }
        host_ = u;
        if (is_private_ipv4_literal(u.host)) {
            // The Tailscale tool exposes its userspace netstack as a loopback-only SOCKS5 proxy.
            // This sends private camera traffic into Tailscale without using the HU's IP route table.
            if (!connect_tailscale_socks5(u, &fd_, err)) return false;
        } else {
            // Public camera sockets use the calling app's MGU Uplink permission.
            ScopedFsuid socket_owner(network_uid);
            if (!socket_owner.valid()) { *err = "could not apply the app's MGU Uplink socket uid"; return false; }
            addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
            addrinfo *list = nullptr; int e = getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &list);
            if (!e) {
                for (addrinfo *a = list; a; a = a->ai_next) {
                    fd_ = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                    if (fd_ >= 0 && ::connect(fd_, a->ai_addr, a->ai_addrlen) == 0) break;
                    if (fd_ >= 0) ::close(fd_);
                    fd_ = -1;
                }
                freeaddrinfo(list);
            }
            bool uplink_resolved = false;
            if (fd_ < 0 && !uplink_dns_servers().empty()) {
                std::vector<sockaddr_in> addresses;
                std::string dns_error;
                if (resolve_uplink_ipv4(u.host, u.port, &addresses, &dns_error)) {
                    uplink_resolved = true;
                    for (const sockaddr_in &a : addresses) {
                        fd_ = socket(AF_INET, SOCK_STREAM, 0);
                        if (fd_ >= 0 && ::connect(fd_, (const sockaddr *)&a, sizeof a) == 0) break;
                        if (fd_ >= 0) ::close(fd_);
                        fd_ = -1;
                    }
                }
                if (fd_ < 0 && !dns_error.empty() && e) { *err = dns_error; return false; }
            }
            if (fd_ < 0) {
                if (uplink_resolved) *err = std::string("Cannot connect to camera through MGU Uplink: ") + strerror(errno);
                else if (!uplink_dns_servers().empty() && e) *err = std::string("MGU Uplink could not resolve the camera host: ") + gai_strerror(e);
                else *err = e ? std::string("DNS lookup failed: ") + gai_strerror(e)
                               : std::string("Cannot connect to camera: ") + strerror(errno);
                return false;
            }
        }
        timeval tv{10, 0}; setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        return true;
    }
    bool request(const std::string &method, const std::string &uri, Response *r, std::string *err,
                 const std::string &extra = "") {
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::ostringstream q; q << method << ' ' << uri << " RTSP/1.0\r\nCSeq: " << ++cseq_ << "\r\nUser-Agent: MGU-RTSP-Viewer/0.1\r\n";
            if (!session_.empty() && method != "SETUP") q << "Session: " << session_ << "\r\n";
            if (!host_.user.empty()) {
                if (!digest_.empty()) {
                    ++digest_count_;
                    std::string auth = digest_authorization(host_.user, host_.pass, method, uri, digest_, cnonce_, digest_count_, err);
                    if (auth.empty()) return false;
                    q << "Authorization: " << auth << "\r\n";
                } else q << "Authorization: Basic " << base64(host_.user + ":" + host_.pass) << "\r\n";
            }
            q << extra << "\r\n";
            std::string s = q.str(); size_t sent = 0;
            while (sent < s.size()) { ssize_t n = send(fd_, s.data() + sent, s.size() - sent, MSG_NOSIGNAL); if (n <= 0) { *err = "RTSP write failed"; return false; } sent += (size_t)n; }
            if (!read_response(fd_, r, err)) return false;
            if (r->status != 401) break;
            if (attempt != 0 || host_.user.empty()) { *err = "Camera rejected the supplied RTSP credentials (401 Unauthorized)"; return false; }
            auto wh = r->headers.find("www-authenticate");
            std::map<std::string, std::string> challenge = digest_params(wh == r->headers.end() ? "" : wh->second);
            if (challenge.empty()) { *err = "Camera requires authentication but did not provide a supported Digest challenge"; return false; }
            const auto nonce = challenge.find("nonce");
            if (nonce == challenge.end()) { *err = "Camera sent an incomplete Digest authentication challenge"; return false; }
            if (digest_.find("nonce") == digest_.end() || digest_["nonce"] != nonce->second ||
                (challenge.count("stale") && lower(challenge["stale"]) == "true")) {
                digest_count_ = 0;
                cnonce_ = make_cnonce(host_.user + nonce->second + std::to_string(++cnonce_seed_));
            }
            digest_ = challenge;
        }
        if (r->status < 200 || r->status >= 300) { *err = method + " failed: RTSP " + std::to_string(r->status); return false; }
        auto it = r->headers.find("session");
        if (it != r->headers.end()) { session_ = it->second.substr(0, it->second.find(';')); }
        return true;
    }
    bool play(const std::string &uri, std::string *err) {
        Response r;
        if (!request("PLAY", uri, &r, err, "Range: npt=0.000-\r\n")) return false;
        return true;
    }
    bool setup(const std::string &track, std::string *err) {
        Response r;
        if (!request("SETUP", track, &r, err, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")) return false;
        auto ti = r.headers.find("transport");
        const std::string tr = ti == r.headers.end() ? "" : ti->second;
        const size_t at = lower(tr).find("interleaved=");
        if (at != std::string::npos) {
            const char *p = tr.c_str() + at + 12; char *end = nullptr; long c = strtol(p, &end, 10);
            if (end != p && c >= 0 && c <= 255) rtp_channel_ = (int)c;
        }
        return true;
    }
    bool teardown(std::string *err) {
        if (fd_ < 0 || session_.empty()) return true;
        Response r;
        const bool ok = request("TEARDOWN", host_.request_url(), &r, err);
        session_.clear();
        return ok;
    }
    int fd() const { return fd_; }
    int rtp_channel() const { return rtp_channel_; }
    void close() { if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; } }
    const Url &url() const { return host_; }
private:
    int fd_ = -1, cseq_ = 0, rtp_channel_ = 0, cnonce_seed_ = 0; unsigned digest_count_ = 0;
    Url host_; std::string session_, cnonce_; std::map<std::string, std::string> digest_;
};

std::string header(const Response &r, const std::string &name) {
    auto it = r.headers.find(lower(name)); return it == r.headers.end() ? "" : it->second;
}
bool video_track(const Response &r, const std::string &request_base, std::string *track, int *payload,
                 std::vector<uint8_t> *sps_pps, std::string *err) {
    std::string base = header(r, "content-base"); if (base.empty()) base = header(r, "content-location");
    if (base.empty()) base = request_base;
    std::istringstream sdp(r.body); std::string line; bool video = false, track_h264 = false, h264 = false;
    std::string control, h264_control, video_codec;
    while (std::getline(sdp, line)) {
        line = trim(line);
        if (line.rfind("m=", 0) == 0) {
            if (video && track_h264 && !control.empty() && h264_control.empty()) h264_control = control;
            video = line.rfind("m=video ", 0) == 0; track_h264 = false; control.clear();
        } else if (video && line.rfind("a=rtpmap:", 0) == 0) {
            size_t sp = line.find(' '); if (sp != std::string::npos) {
                int pt = atoi(line.substr(9, sp - 9).c_str());
                std::string codec = trim(line.substr(sp + 1));
                size_t slash = codec.find('/');
                std::string name = slash == std::string::npos ? codec : codec.substr(0, slash);
                if (!video_codec.empty()) video_codec += ", ";
                video_codec += name;
                if (lower(codec).find("h264/90000") == 0) { *payload = pt; track_h264 = true; h264 = true; }
            }
        } else if (video && track_h264 && line.rfind("a=fmtp:", 0) == 0) {
            const size_t sets = line.find("sprop-parameter-sets=");
            if (sets != std::string::npos) {
                size_t a = sets + 21, b = line.find(';', a);
                std::string list = line.substr(a, b == std::string::npos ? b : b - a);
                size_t comma = list.find(',');
                if (comma != std::string::npos) {
                    std::vector<uint8_t> nal; static const uint8_t sc[] = {0, 0, 0, 1};
                    if (unbase64(list.substr(0, comma), &nal)) { sps_pps->insert(sps_pps->end(), sc, sc + 4); sps_pps->insert(sps_pps->end(), nal.begin(), nal.end()); }
                    nal.clear();
                    if (unbase64(list.substr(comma + 1), &nal)) { sps_pps->insert(sps_pps->end(), sc, sc + 4); sps_pps->insert(sps_pps->end(), nal.begin(), nal.end()); }
                }
            }
        } else if (video && line.rfind("a=control:", 0) == 0) control = trim(line.substr(10));
    }
    if (video && track_h264 && !control.empty() && h264_control.empty()) h264_control = control;
    if (!h264) {
        *err = video_codec.empty()
            ? "Camera SDP has no advertised video codec; verify the RTSP stream and camera firmware"
            : "Camera SDP advertises " + video_codec + " video; this receiver needs H.264. Try the camera's H.264 sub stream or change its encoding to H.264";
        return false;
    }
    control = h264_control;
    if (control.empty()) { *err = "Camera SDP advertises H.264 but has no video track control URL"; return false; }
    if (control.find("rtsp://") == 0) *track = control;
    else if (control[0] == '/') {
        size_t scheme = base.find("://"), slash = scheme == std::string::npos ? std::string::npos : base.find('/', scheme + 3);
        *track = (slash == std::string::npos ? base : base.substr(0, slash)) + control;
    }
    else {
        if (base.empty()) base = "rtsp://"; // replaced below with request base by caller when absent
        if (base.back() != '/') base.push_back('/');
        *track = base + control;
    }
    return true;
}

void append_nal(std::vector<uint8_t> &au, const uint8_t *p, size_t n) {
    static const uint8_t sc[] = {0, 0, 0, 1};
    au.insert(au.end(), sc, sc + sizeof sc); au.insert(au.end(), p, p + n);
}

void inspect_access_unit(const std::vector<uint8_t> &au, bool *has_idr, bool *has_sps, bool *has_pps,
                         std::vector<uint8_t> *params) {
    *has_idr = false;
    *has_sps = *has_pps = false;
    for (size_t i = 0; i + 3 < au.size();) {
        size_t start = std::string::npos, prefix = 0;
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) start = i, prefix = 3;
        else if (i + 4 < au.size() && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) start = i, prefix = 4;
        if (start == std::string::npos) { ++i; continue; }
        const size_t header = start + prefix;
        size_t end = au.size();
        for (size_t j = header + 1; j + 3 < au.size(); ++j) {
            if (au[j] == 0 && au[j + 1] == 0 &&
                (au[j + 2] == 1 || (au[j + 2] == 0 && au[j + 3] == 1))) { end = j; break; }
        }
        if (header < au.size()) {
            const uint8_t type = au[header] & 0x1f;
            if (type == 5) *has_idr = true;
            if (type == 7 || type == 8) {
                if (type == 7) *has_sps = true;
                if (type == 8) *has_pps = true;
                params->insert(params->end(), au.begin() + start, au.begin() + end);
            }
        }
        i = end;
    }
}

bool recv_interleaved_packet(int fd, int rtp_channel, std::vector<uint8_t> *packet, bool *rtp, std::string *err) {
    uint8_t first;
    if (!read_exact(fd, &first, 1)) { *err = read_failure("interleaved RTP packet"); return false; }
    if (first == '$') {
        uint8_t rest[3]; if (!read_exact(fd, rest, 3)) { *err = read_failure("interleaved packet header"); return false; }
        uint16_t n = (uint16_t)((rest[1] << 8) | rest[2]); packet->resize(n);
        if (!read_exact(fd, packet->data(), n)) { *err = read_failure("RTP payload"); return false; }
        *rtp = rest[0] == rtp_channel; return true;
    }
    // An RTSP keepalive/notification arrived between media packets. Drain one response line block.
    char buf[2048]; std::string junk(1, (char)first);
    while (junk.find("\r\n\r\n") == std::string::npos && junk.size() < 8192) {
        ssize_t n = recv(fd, buf, sizeof buf, 0); if (n <= 0) break; junk.append(buf, (size_t)n);
    }
    *rtp = false; return true;
}

bool decode_rtp(const std::vector<uint8_t> &p, int payload, std::vector<uint8_t> *au, bool *marker, uint32_t *timestamp) {
    if (p.size() < 12 || (p[0] >> 6) != 2) return false;
    const size_t cc = p[0] & 15; size_t h = 12 + cc * 4;
    if (p[0] & 0x10) { if (p.size() < h + 4) return false; size_t ext = ((p[h + 2] << 8) | p[h + 3]) * 4; h += 4 + ext; }
    if (h > p.size()) return false;
    size_t end = p.size(); if (p[0] & 0x20) { uint8_t pad = p.back(); if (!pad || pad > end - h) return false; end -= pad; }
    if (end <= h || (p[1] & 0x7f) != payload) return false;
    *marker = (p[1] & 0x80) != 0;
    *timestamp = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | p[7];
    const uint8_t *d = p.data() + h; size_t n = end - h; uint8_t type = d[0] & 0x1f;
    if (type >= 1 && type <= 23) append_nal(*au, d, n);
    else if (type == 24) { // STAP-A
        size_t i = 1; while (i + 2 <= n) { size_t z = (d[i] << 8) | d[i + 1]; i += 2; if (!z || i + z > n) return false; append_nal(*au, d + i, z); i += z; }
    } else if (type == 28) { // FU-A: reassemble a fragmented NAL
        if (n < 2) return false;
        uint8_t fh = d[1];
        if (fh & 0x80) { au->push_back(0); au->push_back(0); au->push_back(0); au->push_back(1); au->push_back((d[0] & 0xe0) | (fh & 0x1f)); }
        au->insert(au->end(), d + 2, d + n);
    }
    return true;
}

void event(const std::string &s) {
    std::ostringstream o; o << "{\"ev\":\"status\",\"msg\":\"";
    for (char c : s) { if (c == '"' || c == '\\') o << '\\'; if ((unsigned char)c >= 32) o << c; }
    o << "\"}\n"; const std::string line = o.str();
    if (status_fd < 0) { std::cout << line << std::flush; return; }
    size_t sent = 0; while (sent < line.size()) { ssize_t n = send(status_fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL); if (n <= 0) break; sent += (size_t)n; }
}

bool read_start_line(int fd, std::string *line) {
    char c;
    while (line->size() < 4096) {
        ssize_t n = recv(fd, &c, 1, 0); if (n <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') line->push_back(c);
    }
    return false;
}

int run_stream(const std::string &raw_url);

int serve() {
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) { event(std::string("socket: ") + strerror(errno)); return 1; }
    unlink(kSocketPath);
    sockaddr_un addr{}; addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, kSocketPath, sizeof(addr.sun_path) - 1);
    if (bind(server, (sockaddr *)&addr, sizeof addr) != 0 || chmod(kSocketPath, 0660) != 0) {
        event(std::string("bind control socket: ") + strerror(errno)); close(server); return 1;
    }
    struct group *g = getgrnam("tm_oap_app");
    if (!g || chown(kSocketPath, 0, g->gr_gid) != 0) {
        event(std::string("set control socket group: ") + (g ? strerror(errno) : "tm_oap_app group not found"));
        close(server); unlink(kSocketPath); return 1;
    }
    if (listen(server, 2) != 0) { event(std::string("listen: ") + strerror(errno)); close(server); return 1; }
    while (!stopping) {
        pollfd p{server, POLLIN, 0}; int ready = poll(&p, 1, 500);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (!ready) continue;
        int client = accept(server, nullptr, nullptr); if (client < 0) continue;
        status_fd = client;
        control_stop_requested = false;
        control_input.clear();
        struct ucred peer{}; socklen_t peer_len = sizeof peer;
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0 || peer.uid == 0) {
            event("Cannot identify the camera app's non-root network uid");
            status_fd = -1; close(client); continue;
        }
        std::string url;
        if (!read_start_line(client, &url) || url.empty()) { status_fd = -1; close(client); continue; }
        // Clear an old frame file before starting the next stream.
        unlink(kVideoPath);
        network_uid = peer.uid;
        run_stream(url);
        network_uid = 0;
        status_fd = -1; close(client);
    }
    close(server); unlink(kSocketPath); return 0;
}
int run_stream(const std::string &raw_url) {
    const std::string &url_arg = raw_url;
    std::string err; Url u;
    if (!u.parse(url_arg, &err)) { event(err); return 2; }
    Rtsp rtsp;
    if (!rtsp.connect_to(u, &err)) { event(err); return 1; }
    const std::string url = u.request_url();
    Response describe;
    if (!rtsp.request("DESCRIBE", url, &describe, &err, "Accept: application/sdp\r\n")) { event(err); return 1; }
    std::string track; int payload = -1; std::vector<uint8_t> sps_pps;
    if (!video_track(describe, url, &track, &payload, &sps_pps, &err)) { event(err); return 1; }
    if (!rtsp.setup(track, &err) || !rtsp.play(url, &err)) { event(err); return 1; }

    ytdec::Mgvf out; std::string warn;
    const char *vpath = getenv("MGU_RTSP_VIDEO");
    if (!out.open(vpath && *vpath ? vpath : kVideoPath, 1280, 480, 3, &err, &warn)) { event(err); return 1; }
    woc::Decoder decoder;
    // Fit each camera frame inside the 8:3 MGU video layer. This matches the
    // full-height app canvas and keeps narrower camera video undistorted.
    if (!decoder.init("/dev/dri/renderD128", 1280, 480, false, &err)) { event(err); out.close(); unlink(vpath && *vpath ? vpath : kVideoPath); return 1; }
    if (!warn.empty()) event(warn);
    event("Connected; waiting for H.264 frames");

    std::vector<uint8_t> au, packet; uint32_t au_ts = 0; int64_t first_pts = -1;
    bool decoder_started = false;
    std::vector<uint8_t> inband_params;
    auto last_frame = std::chrono::steady_clock::now();
    unsigned stall_restarts = 0;
    auto finish_access_unit = [&]() {
        if (au.empty()) return;
        bool has_idr = false, has_sps = false, has_pps = false;
        std::vector<uint8_t> found_params;
        inspect_access_unit(au, &has_idr, &has_sps, &has_pps, &found_params);
        if (!found_params.empty() && sps_pps.empty() && inband_params.size() + found_params.size() <= 256 * 1024)
            inband_params.insert(inband_params.end(), found_params.begin(), found_params.end());
        if (has_idr && sps_pps.empty() && !inband_params.empty()) sps_pps.swap(inband_params);
        if (!decoder_started) {
            if (!has_idr) { au.clear(); return; }
            // A new RTSP PLAY can begin in the middle of a GOP. Start the
            // decoder only on an IDR, with fresh parameter sets, so stale
            // reference pictures from before Stop cannot appear garbled.
            // Prefer SDP parameter sets when present; cameras may repeat them
            // in the IDR access unit, and feeding both copies can confuse MSDK.
            decoder_started = true;
        }
        // Repeat cached parameter sets at later IDRs if this GOP omits them.
        // That lets MSDK rebuild its sequence after a decoder restart.
        if (has_idr && !(has_sps && has_pps) && !sps_pps.empty())
            au.insert(au.begin(), sps_pps.begin(), sps_pps.end());
        int64_t pts = first_pts < 0 ? 0 : (int64_t)(uint32_t)(au_ts - (uint32_t)first_pts) / 90;
        if (first_pts < 0) first_pts = (int64_t)au_ts;
        std::string de;
        decoder.feed(au.data(), au.size(), [&](const woc::VideoFrame &f) {
            if (f.y && f.uv && f.w == out.w() && f.h == out.h()) {
                ytdec::nv12_to_i420(f.y, f.uv, f.pitch, f.w, f.h, out.begin()); out.commit();
                last_frame = std::chrono::steady_clock::now();
                stall_restarts = 0;
            }
        }, &de, true, true, pts);
        if (!de.empty()) event(de);
        au.clear();
    };
    while (!stopping) {
        pollfd fds[2] = {{rtsp.fd(), POLLIN, 0}, {status_fd, POLLIN | POLLRDHUP | POLLHUP | POLLERR, 0}};
        int ready = poll(fds, status_fd >= 0 ? 2 : 1, 250);
        if (ready < 0) { if (errno == EINTR) continue; err = std::string("RTSP poll failed: ") + strerror(errno); event(err); break; }
        if (decoder_started && std::chrono::steady_clock::now() - last_frame > std::chrono::seconds(15)) {
            if (stall_restarts == 0) {
                event("H.264 decoder stalled; restarting it and waiting for the next keyframe");
                decoder.shutdown();
                if (!decoder.init("/dev/dri/renderD128", 1280, 480, false, &err)) {
                    event("Could not restart H.264 decoder: " + err);
                    break;
                }
                decoder_started = false;
                inband_params.clear();
                au.clear();
                ++stall_restarts;
                last_frame = std::chrono::steady_clock::now();
            } else {
                event("H.264 decoder stopped producing video frames after a restart");
                break;
            }
        }
        if (!ready) continue;
        if (status_fd >= 0 && (fds[1].revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL))) {
            control_stop_requested = true;
            break;
        }
        if (status_fd >= 0 && (fds[1].revents & POLLIN) && read_control_stop()) break;
        if (!(fds[0].revents & POLLIN)) {
            err = (fds[0].revents & POLLNVAL) ? "RTSP socket became invalid" : "RTSP connection closed by camera";
            event(err); break;
        }
        bool is_rtp = false;
        if (!recv_interleaved_packet(rtsp.fd(), rtsp.rtp_channel(), &packet, &is_rtp, &err)) {
            if (!stopping && err != "Stream cancelled by app") event(err);
            break;
        }
        if (!is_rtp) continue;
        bool marker = false; uint32_t ts = 0;
        std::vector<uint8_t> packet_au;
        if (!decode_rtp(packet, payload, &packet_au, &marker, &ts) || packet_au.empty()) continue;
        // Some cameras omit RTP's marker bit. A timestamp change still marks
        // the end of the preceding H.264 access unit; use both signals so those
        // streams keep feeding frames instead of accumulating forever.
        if (!au.empty() && ts != au_ts) finish_access_unit();
        au.insert(au.end(), packet_au.begin(), packet_au.end());
        au_ts = ts;
        if (marker) finish_access_unit();
    }
    std::string teardown_error;
    if (!rtsp.teardown(&teardown_error) && !control_stop_requested && !teardown_error.empty())
        std::cerr << "RTSP TEARDOWN: " << teardown_error << '\n';
    decoder.shutdown(); out.close(); unlink(vpath && *vpath ? vpath : kVideoPath);
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal);
    if (argc == 2 && std::string(argv[1]) == "--server") return serve();
    std::cerr << "usage: rtsp-viewer --server\n"; return 2;
}
