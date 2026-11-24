#include "framework.h"
#include "MiniTest.h"

int main() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup failed\n");
        return 2;
    }
    int rc = mt::RunAll();
    WSACleanup();
    return rc;
}
