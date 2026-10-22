#pragma once

// Plain C++ storage for macros. No Android / game dependencies, so it can be unit tested on a PC.
// A macro is a text file "<name>.mcfunction" inside one folder, so macros are global (not per world)
// and can be copied between devices by copying the folder.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace macro {

inline std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline bool validName(const std::string& n) {
    if (n.empty() || n.size() > 32) return false;
    return std::all_of(n.begin(), n.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
}

// Turns "\r\n" into "\n" and the two characters backslash+n into a real newline
// (lets a single-line chat command or text field carry several lines).
inline std::string normalize(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            if (i + 1 < s.size() && s[i + 1] == '\n') continue;
            out += '\n';
        } else if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out += '\n';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Non-empty, non-comment lines (comments start with # or //), trimmed.
inline std::vector<std::string> commandLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(normalize(text));
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0) continue;
        lines.push_back(line);
    }
    return lines;
}

class Store {
public:
    explicit Store(std::filesystem::path dir) : m_dir(std::move(dir)) {}

    const std::filesystem::path& dir() const { return m_dir; }

    std::vector<std::string> list() const {
        std::vector<std::string> names;
        std::error_code ec;
        if (!std::filesystem::is_directory(m_dir, ec)) return names;
        for (const auto& entry : std::filesystem::directory_iterator(m_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto& p = entry.path();
            if (p.extension() != ".mcfunction") continue;
            const std::string stem = p.stem().string();
            if (validName(stem)) names.push_back(stem);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    bool exists(const std::string& name) const {
        std::error_code ec;
        return validName(name) && std::filesystem::is_regular_file(pathFor(name), ec);
    }

    std::optional<std::string> read(const std::string& name) const {
        if (!validName(name)) return std::nullopt;
        std::ifstream in(pathFor(name), std::ios::binary);
        if (!in) return std::nullopt;
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool write(const std::string& name, const std::string& text) const {
        if (!validName(name)) return false;
        std::error_code ec;
        std::filesystem::create_directories(m_dir, ec);
        std::ofstream out(pathFor(name), std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << normalize(text);
        return static_cast<bool>(out);
    }

    bool append(const std::string& name, const std::string& line) const {
        std::string current = read(name).value_or("");
        if (!current.empty() && current.back() != '\n') current += '\n';
        return write(name, current + normalize(line) + "\n");
    }

    bool remove(const std::string& name) const {
        if (!validName(name)) return false;
        std::error_code ec;
        return std::filesystem::remove(pathFor(name), ec);
    }

    // One text file holding every macro:  "=== name ===" line, then the macro's lines.
    std::string exportBundle() const {
        std::string bundle;
        for (const auto& name : list()) {
            std::string text = read(name).value_or("");
            if (!text.empty() && text.back() != '\n') text += '\n';
            bundle += "=== " + name + " ===\n" + text;
        }
        return bundle;
    }

    // Writes every macro found in a bundle (overwrites same-named ones). Returns how many were imported.
    int importBundle(const std::string& bundle) const {
        int count = 0;
        std::istringstream in(bundle);
        std::string line, name, body;
        bool inMacro = false;
        auto flush = [&]() {
            if (inMacro && validName(name) && write(name, body)) ++count;
            body.clear();
        };
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > 8 && line.rfind("=== ", 0) == 0 && line.compare(line.size() - 4, 4, " ===") == 0) {
                flush();
                name = trim(line.substr(4, line.size() - 8));
                inMacro = true;
            } else if (inMacro) {
                body += line + "\n";
            }
        }
        flush();
        return count;
    }

private:
    std::filesystem::path pathFor(const std::string& name) const { return m_dir / (name + ".mcfunction"); }
    std::filesystem::path m_dir;
};

}  // namespace macro
