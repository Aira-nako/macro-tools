#include "macro.hpp"

#include "core/GameHooks.hpp"
#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <cctype>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace {

constexpr int kCommandRequestPacketId = 0x4D;
constexpr int kMaxQueuedLines = 500;
constexpr int kLineDelayMs = 120;

MacroModule* g_macro = nullptr;
thread_local bool g_sendingOwn = false;
void* g_commandVtable = nullptr;

using SendToServerFn = void* (*)(void*, void*, void*, void*);
using GetPacketSenderFn = void* (*)(void*);
using CreatePacketFn = std::shared_ptr<void> (*)(int);

SendToServerFn g_sendOriginal = nullptr;
GetPacketSenderFn g_getPacketSender = nullptr;
CreatePacketFn g_createPacket = nullptr;
bedrocktools::hooks::Handle g_sendHook = nullptr;

void resolveFunctions() {
    using bedrocktools::memory::SignatureId;
    if (!g_getPacketSender) {
        g_getPacketSender = reinterpret_cast<GetPacketSenderFn>(
            bedrocktools::memory::resolve(SignatureId::ClientInstanceGetPacketSender));
    }
    if (!g_createPacket) {
        g_createPacket = reinterpret_cast<CreatePacketFn>(
            bedrocktools::memory::resolve(SignatureId::MinecraftPacketsCreatePacket));
    }
}

// Every CommandRequestPacket shares one vtable, so we learn it once from a throw-away packet
// and use it to recognise command packets in the outgoing hook (no extra signature needed).
bool isCommandPacket(void* packet) {
    if (!packet) return false;
    if (!g_commandVtable) {
        resolveFunctions();
        if (!g_createPacket) return false;
        std::shared_ptr<void> probe = g_createPacket(kCommandRequestPacketId);
        if (!probe) return false;
        g_commandVtable = *reinterpret_cast<void**>(probe.get());
    }
    return *reinterpret_cast<void**>(packet) == g_commandVtable;
}

std::string readCommand(void* packet) {
    auto* payload = reinterpret_cast<std::byte*>(packet) + bedrocktools::sdk::offsets::Packet::Size;
    return *reinterpret_cast<std::string*>(payload + bedrocktools::sdk::offsets::CommandRequestPacketPayload::mCommand);
}

void* sendDetour(void* sender, void* packet, void* a2, void* a3) {
    if (!g_sendingOwn && g_macro && g_macro->enabled && isCommandPacket(packet)) {
        if (g_macro->handleCommand(readCommand(packet))) return nullptr;  // swallowed
    }
    return g_sendOriginal ? g_sendOriginal(sender, packet, a2, a3) : nullptr;
}

// Same approach BedrockTools' own modules use to send a command to the server.
void sendCommandToServer(const std::string& command) {
    using bedrocktools::memory::SignatureId;
    resolveFunctions();
    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!g_createPacket || !g_getPacketSender || !client || !g_sendOriginal) return;
    std::shared_ptr<void> packet = g_createPacket(kCommandRequestPacketId);
    if (!packet) return;
    auto* payload = reinterpret_cast<std::byte*>(packet.get()) + bedrocktools::sdk::offsets::Packet::Size;
    *reinterpret_cast<std::string*>(payload + bedrocktools::sdk::offsets::CommandRequestPacketPayload::mCommand) = command;
    *reinterpret_cast<std::uint8_t*>(payload + bedrocktools::sdk::offsets::CommandRequestPacketPayload::mOrigin +
                                     bedrocktools::sdk::offsets::CommandOriginData::mType) = 0;
    *reinterpret_cast<bool*>(payload + bedrocktools::sdk::offsets::CommandRequestPacketPayload::mInternalSource) = true;
    void* sender = g_getPacketSender(client);
    if (!sender) return;
    g_sendingOwn = true;
    g_sendOriginal(sender, packet.get(), nullptr, nullptr);
    g_sendingOwn = false;
}

std::string takeWord(std::string& rest) {
    std::size_t i = 0;
    while (i < rest.size() && !std::isspace(static_cast<unsigned char>(rest[i]))) ++i;
    std::string word = rest.substr(0, i);
    rest = macro::trim(rest.substr(i));
    return word;
}

bool isMacroCall(const std::string& line) {
    std::string l = macro::lower(macro::trim(line));
    if (!l.empty() && l[0] == '/') l.erase(0, 1);
    return l == "macro" || l.rfind("macro ", 0) == 0;
}

}  // namespace

MacroModule::MacroModule()
    : Module("Macro", "Create and run your own command lists in game with /macro."),
      m_store("/sdcard/games/BedrockTools/macros") {
    g_macro = this;
    // Needs to be on to catch /macro commands, so default to enabled (the menu can still turn it off).
    masterEnabled = true;
    enabled = true;
}

MacroModule::~MacroModule() {
    if (g_macro == this) g_macro = nullptr;
}

void MacroModule::onInit() {
    if (g_sendHook) return;
    const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LoopbackPacketSenderSendToServer);
    if (!address) return;
    g_sendHook = bedrocktools::hooks::install(reinterpret_cast<void*>(address), reinterpret_cast<void*>(sendDetour),
                                              reinterpret_cast<void**>(&g_sendOriginal));
}

void MacroModule::say(std::vector<std::string> lines) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (lines.size() > 14) {
        const auto extra = lines.size() - 13;
        lines.resize(13);
        lines.push_back("... +" + std::to_string(extra) + " more");
    }
    m_toast = std::move(lines);
    m_toastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(7);
}

void MacroModule::queueMacro(const std::string& name) {
    const auto text = m_store.read(name);
    if (!text) {
        say({"Macro '" + name + "' not found."});
        return;
    }
    const auto lines = macro::commandLines(*text);
    int queued = 0, skipped = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& line : lines) {
            if (isMacroCall(line)) { ++skipped; continue; }  // macros can't start macros
            if (static_cast<int>(m_queue.size()) >= kMaxQueuedLines) break;
            m_queue.push_back(line);
            ++queued;
        }
    }
    std::string msg = "Running '" + name + "' (" + std::to_string(queued) + " lines)";
    if (skipped) msg += ", skipped " + std::to_string(skipped) + " nested /macro";
    say({msg});
}

bool MacroModule::handleCommand(const std::string& raw) {
    std::string cmd = macro::trim(raw);
    if (cmd.size() < 6 || cmd[0] != '/' || macro::lower(cmd.substr(0, 6)) != "/macro") return false;
    if (cmd.size() > 6 && !std::isspace(static_cast<unsigned char>(cmd[6]))) return false;

    std::string rest = macro::trim(cmd.substr(6));
    const std::string sub = macro::lower(takeWord(rest));
    std::string name = takeWord(rest);  // `rest` is now whatever follows the name

    auto needName = [&]() {
        if (macro::validName(name)) return true;
        say({"Give a name: letters, numbers, _ or - (max 32 characters)."});
        return false;
    };

    if (sub.empty() || sub == "help") {
        say({"/macro list", "/macro view <name>", "/macro add <name> [cmd1\\ncmd2]", "/macro edit <name> [cmd1\\ncmd2]",
             "/macro append <name> <command>", "/macro delete <name>", "/macro run <name>", "/macro stop",
             "/macro export [file]", "/macro import [file]", "Editor: Mod Menu > Macro"});
    } else if (sub == "list") {
        const auto names = m_store.list();
        std::vector<std::string> out{"Macros (" + std::to_string(names.size()) + "):"};
        for (const auto& n : names) out.push_back("- " + n);
        if (names.empty()) out.push_back("none yet, try /macro add <name>");
        say(out);
    } else if (sub == "view") {
        if (!needName()) return true;
        const auto text = m_store.read(name);
        if (!text) { say({"Macro '" + name + "' not found."}); return true; }
        std::vector<std::string> out{name + ":"};
        int i = 1;
        for (const auto& line : macro::commandLines(*text)) out.push_back(std::to_string(i++) + ". " + line);
        if (i == 1) out.push_back("(empty)");
        say(out);
    } else if (sub == "add") {
        if (!needName()) return true;
        if (m_store.exists(name)) { say({"'" + name + "' already exists. Use /macro edit " + name}); return true; }
        if (!m_store.write(name, rest)) { say({"Could not save '" + name + "'."}); return true; }
        m_pendingName = name;
        say({"Created '" + name + "'.", "Add lines: /macro append " + name + " <command>", "or Mod Menu > Macro > Edit Content > Save."});
    } else if (sub == "edit") {
        if (!needName()) return true;
        const auto text = m_store.read(name);
        if (!text) { say({"Macro '" + name + "' not found."}); return true; }
        if (!rest.empty()) {
            say({m_store.write(name, rest) ? "Replaced '" + name + "'." : "Could not save '" + name + "'."});
            return true;
        }
        m_pendingName = name;
        std::vector<std::string> out{"Editing '" + name + "'. Current lines:"};
        for (const auto& line : macro::commandLines(*text)) out.push_back(line);
        out.push_back("Mod Menu > Macro > Edit Content > Save replaces it.");
        say(out);
    } else if (sub == "append") {
        if (!needName()) return true;
        if (rest.empty()) { say({"Usage: /macro append <name> <command>"}); return true; }
        say({m_store.append(name, rest) ? "Added a line to '" + name + "'." : "Could not save '" + name + "'."});
    } else if (sub == "delete" || sub == "remove") {
        if (!needName()) return true;
        say({m_store.remove(name) ? "Deleted '" + name + "'." : "Macro '" + name + "' not found."});
    } else if (sub == "run") {
        if (!needName()) return true;
        queueMacro(name);
    } else if (sub == "stop") {
        std::size_t dropped;
        { std::lock_guard<std::mutex> lock(m_mutex); dropped = m_queue.size(); m_queue.clear(); }
        say({"Stopped (" + std::to_string(dropped) + " lines dropped)."});
    } else if (sub == "export" || sub == "import") {
        std::string file = name.empty() ? "macros_export.txt" : name;
        if (file.find('/') != std::string::npos || file.find('\\') != std::string::npos || file.find("..") != std::string::npos) {
            say({"Use a plain file name (no folders)."});
            return true;
        }
        const auto path = m_store.dir().parent_path() / file;
        if (sub == "export") {
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << m_store.exportBundle();
            say({out ? "Exported to " + path.string() : "Export failed."});
        } else {
            std::ifstream in(path, std::ios::binary);
            if (!in) { say({"File not found: " + path.string()}); return true; }
            std::ostringstream ss;
            ss << in.rdbuf();
            say({"Imported " + std::to_string(m_store.importBundle(ss.str())) + " macros."});
        }
    } else {
        say({"Unknown: /macro " + sub, "Try /macro help"});
    }
    return true;
}

void MacroModule::drainQueue() {
    std::string line;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty() || std::chrono::steady_clock::now() < m_nextSend) return;
        line = std::move(m_queue.front());
        m_queue.pop_front();
    }
    const auto now = std::chrono::steady_clock::now();
    const std::string lowered = macro::lower(line);
    if (lowered.rfind("wait ", 0) == 0) {  // "wait 500" pauses the macro for 500 ms (max 10 s)
        int ms = std::atoi(line.c_str() + 5);
        ms = ms < 0 ? 0 : (ms > 10000 ? 10000 : ms);
        m_nextSend = now + std::chrono::milliseconds(ms);
        return;
    }
    if (line[0] != '/') line.insert(0, "/");
    sendCommandToServer(line);
    m_nextSend = now + std::chrono::milliseconds(kLineDelayMs);
}

void MacroModule::drawToast() {
    std::vector<std::string> lines;
    bool active;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        active = !m_toast.empty() && std::chrono::steady_clock::now() < m_toastUntil;
        if (active) lines = m_toast;
    }
    if (!active) {
        if (m_toastDrawn) { ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{}); m_toastDrawn = false; }
        return;
    }
    std::vector<PLModMenu_DrawCommand> cmds;
    float y = 220.0f;
    for (const auto& line : lines) {
        PLModMenu_DrawCommand c = {};
        c.type = PL_DRAW_TEXT;
        c.x = 20.0f;
        c.y = y;
        c.w = 900.0f;
        c.h = 30.0f;
        c.color = 0xFFFFFFFF;
        c.size = 28.0f;
        c.text = line.c_str();
        cmds.push_back(c);
        y += 32.0f;
    }
    ::submitDrawCommands(moduleId, cmds);
    m_toastDrawn = true;
}

void MacroModule::onFrame() {
    drainQueue();
    drawToast();
}

void MacroModule::saveFromMenu() {
    const std::string name = macro::validName(m_editName) ? m_editName : m_pendingName;
    if (!macro::validName(name)) {
        say({"Type a valid name in Edit Name first (letters, numbers, _ or -)."});
        return;
    }
    say({m_store.write(name, m_editContent) ? "Saved '" + name + "' (" +
             std::to_string(macro::commandLines(m_editContent).size()) + " lines)."
                                            : "Could not save '" + name + "'."});
}

bool MacroModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    const bool pressed = value == "true" || value == "1";
    if (key == "editName") m_editName = macro::trim(std::string(value));
    else if (key == "editContent") m_editContent = std::string(value);
    else if (key == "saveButton" && pressed) saveFromMenu();
    else if (key == "runButton" && pressed) {
        const std::string name = macro::validName(m_editName) ? m_editName : m_pendingName;
        if (macro::validName(name)) queueMacro(name);
        else say({"Type the macro's name in Edit Name first."});
    } else if (key == "deleteButton" && pressed) {
        const std::string name = macro::validName(m_editName) ? m_editName : m_pendingName;
        say({macro::validName(name) && m_store.remove(name) ? "Deleted '" + name + "'." : "Macro not found."});
    } else {
        return false;  // base options (enable, keybind...) use the default handling
    }
    return true;
}

void MacroModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);  // editor fields are intentionally not persisted
}

void MacroModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    // Empty defaults: the menu builds a text box for each string and a button for each "...Button" bool.
    j["editName"] = "";
    j["editContent"] = "";
    j["saveButton"] = false;
    j["runButton"] = false;
    j["deleteButton"] = false;
}
