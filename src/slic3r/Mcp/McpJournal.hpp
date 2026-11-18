#ifndef slic3r_Mcp_Journal_hpp_
#define slic3r_Mcp_Journal_hpp_

// Record of changes the application's undo stack does not cover: preset edits and selection,
// the filament list and colours, other project-level values. A tool that makes such a change
// records it with ToolContext::journal(); the `history` tool lists the entries and can revert
// them by running the entry's revert function, which re-applies the old value through the same
// code path that set the new one.
//
// Main thread only. Entries live for the session (the newest MAX_ENTRIES are kept).

#include <cstddef>
#include <deque>
#include <functional>
#include <string>

#include "McpJson.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

struct JournalEntry
{
    size_t      seq { 0 };
    std::string time;   // HH:MM:SS local
    std::string tool;   // tool that made the change
    json        scope;  // "process", {"filament": 2}, "filaments" ... as the tool describes it
    std::string key;    // setting key or a short name of what changed ("preset", "color")
    json        before;
    json        after;
    bool        reverted { false };
    // Re-applies `before`. May throw ToolError. Empty: the change cannot be reverted.
    std::function<void()> revert;
};

class Journal
{
public:
    static Journal& instance();

    static const size_t MAX_ENTRIES = 200;

    // Returns the sequence number of the new entry.
    size_t add(std::string tool, json scope, std::string key, json before, json after, std::function<void()> revert);
    const std::deque<JournalEntry>& entries() const { return m_entries; }
    // Reverts one entry. Fails with stale_id (unknown seq), not_revertible or already_reverted.
    void   revert(size_t seq);
    void   clear();

    // {"seq", "time", "tool", "scope", "key", "before", "after", "reverted", "revertible"}
    static json to_json(const JournalEntry& entry);

private:
    std::deque<JournalEntry> m_entries;
    size_t                   m_next_seq { 1 };
};

}}} // namespace Slic3r::GUI::Mcp

#endif
