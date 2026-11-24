#pragma once
// Portable model for Server's extension checkboxes. Keeps the existing
// ExtensionFilter grammar (comma-separated *.ext tokens; '*' means all).
#include <string>
#include <vector>
#include <algorithm>

namespace fsyn {
namespace ui_filter {
struct Selection {
    bool all = false;
    bool xml = false;
    bool json = false;
    bool raw = false;
    bool ini = false;
    bool rtf = false;
    bool other = false;
    std::string custom;
};
// UI policy: ALL is the only active filter while selected; other options stay
// visible but cannot be edited.  Keep their check state for later reuse.
struct ControlState {
    bool extensionOptionsEnabled = false;
    bool customEditorEnabled = false;
};
inline ControlState GetControlState(bool configEditable, bool allChecked, bool otherChecked) {
    const bool optionsEnabled = configEditable && !allChecked;
    return { optionsEnabled, optionsEnabled && otherChecked };
}
inline Selection DisplaySelection(const Selection& active, const Selection& prior) {
    if (active.all && !prior.all) return prior;
    return active;
}
inline std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}
inline std::string Lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a'-'A'));
    return value;
}
inline bool CanonicalToken(std::string raw, std::string& out) {
    raw = Lower(Trim(raw));
    if (raw.size() > 2 && raw[0] == '*' && raw[1] == '.') raw.erase(0, 1);
    else if (!raw.empty() && raw[0] != '.') raw.insert(raw.begin(), '.');
    if (raw.size() < 2 || raw[0] != '.') return false;
    for (size_t i = 1; i < raw.size(); ++i) {
        const char c = raw[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    out = "*" + raw;
    return true;
}
inline Selection Parse(const std::string& saved) {
    Selection s;
    if (Trim(saved).empty() || Trim(saved) == "*") { s.all = true; return s; }
    size_t pos = 0;
    while (pos <= saved.size()) {
        const size_t comma = saved.find(',', pos);
        const std::string tok = Trim(saved.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (tok == "*") { s = Selection{}; s.all = true; return s; }
        if (!tok.empty()) {
            std::string canonical;
            const std::string norm = CanonicalToken(tok, canonical) ? canonical : tok;
            if (norm == "*.xml") s.xml = true;
            else if (norm == "*.json") s.json = true;
            else if (norm == "*.raw") s.raw = true;
            else if (norm == "*.ini") s.ini = true;
            else if (norm == "*.rtf") s.rtf = true;
            else {
                if (!s.custom.empty()) s.custom += ',';
                s.custom += tok;
                s.other = true;
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return s;
}
inline bool Compose(const Selection& s, std::string& result, std::string& error) {
    result.clear(); error.clear();
    if (s.all) { result = "*"; return true; }
    std::vector<std::string> tokens;
    if (s.xml) tokens.push_back("*.xml");
    if (s.json) tokens.push_back("*.json");
    if (s.raw) tokens.push_back("*.raw");
    if (s.ini) tokens.push_back("*.ini");
    if (s.rtf) tokens.push_back("*.rtf");
    if (s.other) {
        if (Trim(s.custom).empty()) { error = "Other extensions are enabled but empty"; return false; }
        size_t pos = 0;
        while (pos <= s.custom.size()) {
            const size_t comma = s.custom.find(',', pos);
            std::string canonical;
            if (!CanonicalToken(s.custom.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos), canonical)) {
                error = "Invalid custom extension. Use *.csv,*.log";
                return false;
            }
            if (std::find(tokens.begin(), tokens.end(), canonical) == tokens.end()) tokens.push_back(canonical);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    if (tokens.empty()) { error = "Choose at least one file extension or All"; return false; }
    for (const auto& token : tokens) {
        if (!result.empty()) result += ',';
        result += token;
    }
    return true;
}
} // namespace ui_filter
} // namespace fsyn
