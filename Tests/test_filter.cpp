#include "framework.h"
#include "MiniTest.h"
#include "ExtensionFilter.h"

using namespace fsyn;

TEST(filter_accept_all) {
    ExtensionFilter f;
    f.Parse("*");
    EXPECT_TRUE(f.AcceptAll());
    EXPECT_TRUE(f.Accept("a.txt"));
    EXPECT_TRUE(f.Accept("no_ext"));
    EXPECT_TRUE(f.Accept("sub\\b.jpg"));
}

TEST(filter_by_ext) {
    ExtensionFilter f;
    f.Parse("*.txt,*.log");
    EXPECT_FALSE(f.AcceptAll());
    EXPECT_TRUE(f.Accept("a.txt"));
    EXPECT_TRUE(f.Accept("dir\\b.TXT"));   // case-insensitive
    EXPECT_TRUE(f.Accept("x.log"));
    EXPECT_FALSE(f.Accept("a.bin"));
    EXPECT_FALSE(f.Accept("no_ext"));
}

TEST(filter_empty_means_all) {
    ExtensionFilter f;
    f.Parse("");
    EXPECT_TRUE(f.AcceptAll());
}

TEST(filter_dot_prefix) {
    ExtensionFilter f;
    f.Parse(".txt,.log");
    EXPECT_TRUE(f.Accept("a.txt"));
    EXPECT_TRUE(f.Accept("a.log"));
    EXPECT_FALSE(f.Accept("a.bin"));
}
