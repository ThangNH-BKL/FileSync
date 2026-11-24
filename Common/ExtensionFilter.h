#pragma once
// ExtensionFilter.h — Bộ lọc đuôi file.
// Cấu hình dạng "*.txt,*.log,*" — không phân biệt hoa/thường; "*" = tất cả.

#include <string>
#include <vector>
#include <unordered_set>

namespace fsyn {

class ExtensionFilter {
public:
    // Parse chuỗi cấu hình; "*" hoặc rỗng -> acceptAll.
    void Parse(const std::string& spec);

    // Trả true nếu đường dẫn có đuôi được chấp nhận.
    // Path có thể chứa dấu \ hoặc /; chỉ phần đuôi sau dấu chấm cuối được so.
    bool Accept(const std::string& relPathUtf8) const;

    bool AcceptAll() const { return acceptAll_; }

private:
    bool acceptAll_ = true;
    std::unordered_set<std::string> exts_;  // lưu dạng ".txt", ".log" (đã lowercase)
};

} // namespace fsyn
