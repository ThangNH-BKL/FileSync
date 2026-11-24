#pragma once
// Portable, bounded dashboard layout for the MFC server dialog.
// Inputs are pixel dimensions of the client area, and the number of pixels
// corresponding to 100 horizontal and vertical dialog units respectively.
// All rectangles remain inside the visible client area at supported sizes.
#include <algorithm>

namespace fsyn::server_dialog_layout {
struct Rect { int x=0, y=0, w=0, h=0; };
struct Layout {
    Rect clientsHeader, treeHeader, clients, tree, headline, progress, detail,
         logHeader, log, status;
    bool fits = false;
};
inline Layout Compute(int width, int height, int pxPer100X, int pxPer100Y) {
    const auto x = [pxPer100X](int du) { return (du * pxPer100X + 50) / 100; };
    const auto y = [pxPer100Y](int du) { return (du * pxPer100Y + 50) / 100; };
    Layout out{};
    if (width <= 0 || height <= 0 || pxPer100X <= 0 || pxPer100Y <= 0) return out;
    const int margin = x(8);
    const int gap = x(7);
    const int bodyWidth = width - 2 * margin - gap;
    if (bodyWidth <= 0) return out;
    const int leftWidth = bodyWidth / 2;
    const int rightWidth = bodyWidth - leftWidth;
    const int rightX = margin + leftWidth + gap;
    out.clientsHeader = {margin, y(89), leftWidth, y(12)};
    out.treeHeader = {rightX, y(89), rightWidth, y(12)};

    const int top = y(103);
    const int statusTop = height - y(17);
    const int logBottom = statusTop - y(4);
    // Between the data panels and the log are the progress gauges and labels.
    const int fixedGaps = y(5)+y(11)+y(2)+y(14)+y(3)+y(12)+y(4)+y(11)+y(2);
    const int panelAndLog = logBottom - top - fixedGaps;
    const int minPanel = y(36);
    const int minLog = y(18);
    if (panelAndLog < minPanel + minLog || bodyWidth < x(410)) return out;
    int panelHeight = panelAndLog * 63 / 100;
    panelHeight = (std::max)(minPanel, (std::min)(panelHeight, panelAndLog - minLog));
    out.clients = {margin, top, leftWidth, panelHeight};
    out.tree = {rightX, top, rightWidth, panelHeight};
    int cursor = top + panelHeight + y(5);
    out.headline = {margin, cursor, width - 2*margin, y(11)};
    cursor += y(11) + y(2);
    out.progress = {margin, cursor, width - 2*margin, y(14)};
    cursor += y(14) + y(3);
    out.detail = {margin, cursor, width - 2*margin, y(12)};
    cursor += y(12) + y(4);
    out.logHeader = {margin, cursor, width - 2*margin, y(11)};
    cursor += y(11) + y(2);
    out.log = {margin, cursor, width - 2*margin, logBottom - cursor};
    out.status = {margin, statusTop, width - 2*margin, y(12)};
    out.fits = out.log.h >= minLog && out.status.y >= out.log.y + out.log.h;
    return out;
}
}
