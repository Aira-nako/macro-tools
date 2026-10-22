#pragma once

#include "../Module.hpp"
#include "macrostore.hpp"

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// In-game macro maker: /macro list | view | add | edit | append | delete | run | stop | export | import
class MacroModule : public Module {
public:
    MacroModule();
    ~MacroModule() override;

    void onInit() override;
    void onFrame() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the outgoing-packet hook. Returns true if the command was a /macro command
    // (it is then handled locally and never sent to the server).
    bool handleCommand(const std::string& raw);

private:
    void say(std::vector<std::string> lines);
    void queueMacro(const std::string& name);
    void drainQueue();
    void drawToast();
    void saveFromMenu();

    macro::Store m_store;
    std::mutex m_mutex;
    std::deque<std::string> m_queue;
    std::chrono::steady_clock::time_point m_nextSend{};

    std::vector<std::string> m_toast;
    std::chrono::steady_clock::time_point m_toastUntil{};
    bool m_toastDrawn = false;

    // Mod-menu editor fields
    std::string m_editName;
    std::string m_editContent;
    std::string m_pendingName;  // set by "/macro add <name>" or "/macro edit <name>"
};
