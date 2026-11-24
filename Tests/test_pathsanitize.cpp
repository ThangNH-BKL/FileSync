#include "framework.h"
#include "MiniTest.h"
#include "PathSanitizer.h"

using namespace fsyn;

TEST(path_rejects_traversal) {
    std::wstring root = L"C:\\TestRoot";
    std::wstring full;
    const char* bad[] = {
        "", "..\\..\\etc\\passwd", "a\\..\\b", "/absolute/path",
        "C:\\Windows\\system32", "\\\\server\\share", "file.txt:hidden",
        "CON", "NUL", "COM1", "LPT1", "aux.txt",
        "file|bad", "file>bad", "file<bad", "file\"bad", "file?bad",
    };
    for (const char* b : bad) {
        EXPECT_FALSE(PathSanitizer::Validate(root, b, full));
    }
}

TEST(path_accepts_valid) {
    std::wstring root = L"C:\\TestRoot";
    std::wstring full;
    EXPECT_TRUE(PathSanitizer::Validate(root, "a.txt", full));
    EXPECT_TRUE(PathSanitizer::Validate(root, "sub\\b.txt", full));
    EXPECT_TRUE(PathSanitizer::Validate(root, "a/b/c.txt", full));
    EXPECT_TRUE(PathSanitizer::Validate(root, "folder\\file.log", full));
}

TEST(path_containment_check) {
    std::wstring root = L"C:\\TestRoot";
    std::wstring full;
    // Nếu "root" = C:\TestRoot, thì C:\TestRoot2\x KHÔNG nằm trong root
    EXPECT_FALSE(PathSanitizer::Validate(root, "..\\TestRoot2\\x", full));
    EXPECT_TRUE(PathSanitizer::Validate(root, "ok.txt", full));
    EXPECT_TRUE(full.find(L"C:\\TestRoot") == 0);
}

TEST(path_normalize_sep) {
    EXPECT_EQ(PathSanitizer::NormalizeSeparators("a/b\\c//d"), std::string("a\\b\\c\\d"));
}
