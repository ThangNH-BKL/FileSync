#include "pch.h"
#include "Callbacks.h"

namespace fsyn {

void LoggerSinkToUi(void* ctx, LogLevel lv, const std::string& line) {
    if (!ctx) return;
    static_cast<IUiCallbacks*>(ctx)->OnLog(lv, line);
}

} // namespace fsyn
