#include "common.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace qex {

uint64_t fnv1a64(const void *data, size_t len, uint64_t seed) {
    auto *p = static_cast<const unsigned char *>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t fnv1a64_str(const std::string &s, uint64_t seed) {
    return fnv1a64(s.data(), s.size(), seed);
}

uint64_t fnv1a64_u64(uint64_t x, uint64_t seed) {
    return fnv1a64(&x, sizeof(x), seed);
}

std::string to_hex(uint64_t x) {
    static const char *d = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[i] = d[x & 15];
        x >>= 4;
    }
    return s;
}

std::string human_bytes(size_t n) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)n;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f%s", v, units[u]);
    return buf;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool starts_with(const std::string &s, const std::string &pfx) {
    return s.size() >= pfx.size() && s.compare(0, pfx.size(), pfx) == 0;
}
bool ends_with(const std::string &s, const std::string &sfx) {
    return s.size() >= sfx.size() &&
           s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool file_exists(const std::string &path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file(const std::string &path, const std::string &content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + path);
    f.write(content.data(), (std::streamsize)content.size());
}

void ensure_dir(const std::string &path) {
    if (path.empty() || file_exists(path)) return;
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos && pos > 0)
        ensure_dir(path.substr(0, pos));
    ::mkdir(path.c_str(), 0755);
}

std::string join_path(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

} // namespace qex
