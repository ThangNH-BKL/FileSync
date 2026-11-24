// Portable verification for the server dialog numeric settings (no MFC required).
#include "../Common/ServerLimitSettings.h"
#include <cassert>
#include <string>
#include <iostream>

int main() {
    using namespace fsyn::ui_server_limits;
    unsigned value = 99999;
    auto ok = [&](std::wstring s, unsigned low, unsigned high, unsigned expected) {
        value = 99999;
        assert(Parse(s, low, high, value));
        assert(value == expected);
    };
    auto bad = [&](std::wstring s, unsigned low, unsigned high) {
        value = 7777;
        assert(!Parse(s, low, high, value));
        assert(value == 7777);  // No partial output on failure.
    };
    ok(L"512", kMinClients, kMaxClients, 512);
    ok(L"1", kMinClients, kMaxClients, 1);
    ok(L"4096", kMinClients, kMaxClients, 4096);
    ok(L"  128  ", kMinClients, kMaxClients, 128);
    ok(L"0001", kMinClients, kMaxClients, 1);
    ok(L"12", kMinTransfers, kMaxTransfers, 12);
    ok(L"64", kMinTransfers, kMaxTransfers, 64);
    bad(L"", kMinClients, kMaxClients);
    bad(L" ", kMinClients, kMaxClients);
    bad(L"0", kMinClients, kMaxClients);
    bad(L"4097", kMinClients, kMaxClients);
    bad(L"65", kMinTransfers, kMaxTransfers);
    bad(L"-1", kMinClients, kMaxClients);
    bad(L"+12", kMinTransfers, kMaxTransfers);
    bad(L"2.5", kMinClients, kMaxClients);
    bad(L"512abc", kMinClients, kMaxClients);
    bad(L"9999999999999999999999", kMinClients, kMaxClients);
    bad(L"1 2", kMinTransfers, kMaxTransfers);
    bad(L"１２", kMinTransfers, kMaxTransfers);
    for (unsigned n = 1; n <= 4096; ++n)
        ok(std::to_wstring(n), kMinClients, kMaxClients, n);
    for (unsigned n = 1; n <= 64; ++n)
        ok(std::to_wstring(n), kMinTransfers, kMaxTransfers, n);
    std::cout << "PASS server limits parser (bounds + invalid input + full ranges)\n";
}
