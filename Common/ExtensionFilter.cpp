#include "pch.h"
#include "ExtensionFilter.h"

namespace fsyn {
namespace {

std::string TrimAscii(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string ToLowerAscii(const std::string& s) {
    std::string r = s;
    for (auto& c : r) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return r;
}

} // namespace

void ExtensionFilter::Parse(const std::string& spec) {
    exts_.clear();
    acceptAll_ = false;

    std::string trimmed = TrimAscii(spec);
    if (trimmed.empty() || trimmed == "*") {
        acceptAll_ = true;
        return;
    }

    size_t start = 0;
    while (start <= trimmed.size()) {
        size_t pos = trimmed.find(',', start);
        std::string tok = (pos == std::string::npos)
            ? trimmed.substr(start)
            : trimmed.substr(start, pos - start);
        tok = TrimAscii(tok);

        if (tok == "*") {
            acceptAll_ = true;
            exts_.clear();
            return;
        }

        // Chấp nhận cả ".txt" lẫn "*.txt" — chuẩn hóa thành ".txt"
        if (tok.size() >= 2 && tok[0] == '*' && tok[1] == '.') tok = tok.substr(1);
        if (!tok.empty() && tok[0] != '.') tok = "." + tok;
        if (tok.size() > 1) {
            exts_.insert(ToLowerAscii(tok));
        }

        if (pos == std::string::npos) break;
        start = pos + 1;
    }

    if (exts_.empty()) acceptAll_ = true;
}

bool ExtensionFilter::Accept(const std::string& relPathUtf8) const {
    if (acceptAll_) return true;
    if (relPathUtf8.empty()) return false;

    // Bỏ phần thư mục — chỉ xét tên file
    size_t slash = relPathUtf8.find_last_of("\\/");
    std::string name = (slash == std::string::npos) ? relPathUtf8 : relPathUtf8.substr(slash + 1);

    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;

    std::string ext = ToLowerAscii(name.substr(dot));
    return exts_.find(ext) != exts_.end();
}

} // namespace fsyn
