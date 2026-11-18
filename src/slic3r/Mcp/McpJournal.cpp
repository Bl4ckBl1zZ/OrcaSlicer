#include "McpJournal.hpp"

#include <ctime>
#include <utility>

#include "McpError.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {

std::string now_hms()
{
    const std::time_t t = std::time(nullptr);
    std::tm           local {};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buffer[16] = "";
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

} // namespace

Journal& Journal::instance()
{
    static Journal journal;
    return journal;
}

size_t Journal::add(std::string tool, json scope, std::string key, json before, json after, std::function<void()> revert)
{
    JournalEntry entry;
    entry.seq = m_next_seq++;
    entry.time = now_hms();
    entry.tool = std::move(tool);
    entry.scope = std::move(scope);
    entry.key = std::move(key);
    entry.before = std::move(before);
    entry.after = std::move(after);
    entry.revert = std::move(revert);
    m_entries.push_back(std::move(entry));
    while (m_entries.size() > MAX_ENTRIES)
        m_entries.pop_front();
    return m_entries.back().seq;
}

void Journal::revert(size_t seq)
{
    for (JournalEntry& entry : m_entries) {
        if (entry.seq != seq)
            continue;
        if (entry.reverted)
            fail("already_reverted", "Journal entry " + std::to_string(seq) + " was already reverted.");
        if (!entry.revert)
            fail("not_revertible", "Journal entry " + std::to_string(seq) + " (" + entry.key + ") cannot be reverted automatically.",
                 "Set the old value again with the tool that changed it; `before` holds it.");
        entry.revert();
        entry.reverted = true;
        return;
    }
    fail("stale_id", "There is no journal entry " + std::to_string(seq) + ".", "history with action \"list\" shows the journal.");
}

void Journal::clear()
{
    m_entries.clear();
}

json Journal::to_json(const JournalEntry& entry)
{
    json out = json::object();
    out["seq"] = entry.seq;
    out["time"] = entry.time;
    out["tool"] = entry.tool;
    out["scope"] = entry.scope;
    out["key"] = entry.key;
    out["before"] = entry.before;
    out["after"] = entry.after;
    out["reverted"] = entry.reverted;
    out["revertible"] = static_cast<bool>(entry.revert);
    return out;
}

}}} // namespace Slic3r::GUI::Mcp
