#include "../FileServer/ServerDialogLayout.h"
#include <cassert>
#include <iostream>
using namespace fsyn::server_dialog_layout;

static void Check(const char* description, int w, int h, int x100, int y100) {
    const Layout l = Compute(w, h, x100, y100);
    assert(l.fits && "dashboard panels must fit the client area");
    const Rect r[] = {l.clientsHeader, l.treeHeader, l.clients, l.tree,
        l.headline, l.progress, l.detail, l.logHeader, l.log, l.status};
    for (const auto& p : r) {
        assert(p.w > 0 && p.h > 0);
        assert(p.x >= 0 && p.y >= 0);
        assert(p.x + p.w <= w);
        assert(p.y + p.h <= h);
    }
    assert(l.clients.y == l.tree.y && l.clients.h == l.tree.h);
    assert(l.clients.x + l.clients.w < l.tree.x);
    assert(l.clients.y + l.clients.h < l.headline.y);
    assert(l.progress.y >= l.headline.y + l.headline.h);
    assert(l.detail.y >= l.progress.y + l.progress.h);
    assert(l.log.y >= l.logHeader.y + l.logHeader.h);
    assert(l.status.y >= l.log.y + l.log.h);
    std::cout << "PASS " << description << " panel=" << l.clients.h
              << " log=" << l.log.h << " width=" << w << " height=" << h << "\n";
}
int main() {
    Check("1366x768 scale100", 860, 570, 154, 185);
    Check("1366x768 scale125", 1080, 668, 192, 235);
    Check("1366x768 scale150 narrow", 1260, 650, 230, 265);
    Check("1600x900 scale125", 1140, 720, 192, 235);
    Check("1920x1080 scale100 maximized", 1860, 970, 154, 185);
    Check("1920x1080 scale125 maximized", 1860, 970, 192, 235);
    Check("3840x2160 large", 3560, 1920, 230, 265);
    const Layout tooSmall=Compute(400,200,192,235);
    assert(!tooSmall.fits);
    std::cout << "PASS impossible-too-small rejected\n";
}
