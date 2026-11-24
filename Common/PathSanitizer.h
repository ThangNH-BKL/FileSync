#pragma once
// PathSanitizer.h — Chuẩn hóa & kiểm tra relPath nhận từ mạng.
// Từ chối: rỗng, "..", đường dẫn tuyệt đối, ổ đĩa, UNC, ADS ':', ký tự cấm,
// tên thiết bị (CON/NUL/COM1..), byte điều khiển, quá dài.
// Kết quả luôn nằm dưới root (containment check).

#include <string>

namespace fsyn {

class PathSanitizer {
public:
    // Kiểm tra relPath (UTF-8). Nếu hợp lệ, trả fullPath (UTF-16) = root + relPath.
    // Trả false nếu không hợp lệ; reasonOut (nếu khác null) nhận lý do.
    static bool Validate(const std::wstring& rootUtf16,
                         const std::string& relPathUtf8,
                         std::wstring& fullPathOut,
                         std::string* reasonOut = nullptr);

    // Chuẩn hóa dấu phân cách: '/' -> '\', gộp nhiều '\' liên tiếp.
    // KHÔNG xử lý ".." — dùng Validate để xử lý đầy đủ.
    static std::string NormalizeSeparators(const std::string& relPathUtf8);

    // Chuẩn hóa root (bỏ '\' cuối, chuyển sang dạng tuyệt đối).
    static bool NormalizeRoot(const std::wstring& rootIn, std::wstring& rootOut);
};

} // namespace fsyn
