#ifndef slic3r_Mcp_Files_hpp_
#define slic3r_Mcp_Files_hpp_

// Which files a tool may read or write. Every path an agent passes goes through check_read() or
// check_write() before anything touches the disk.
//
//   read roots   app_config "mcp_read_roots" (';'-separated). Default: the user's Documents,
//                Downloads and Desktop folders and the workspace.
//   write roots  app_config "mcp_write_roots". Default: the workspace only.
//   workspace    <data_dir>/mcp/workspace, always allowed for both.
//   ORCA_MCP_ROOTS (environment, ';'-separated) adds folders to both lists; for test launches.
//
// A path must be absolute. Before anything touches the disk its text is checked (no UNC or device
// paths such as \\server\share or \\?\C:\..., no reserved device names like CON or NUL.3mf, no ':'
// after the drive letter, no names ending in a dot or space) and its lexically normalised form must be
// inside an allowed folder. Then it is resolved (symbolic links, junctions) and tested again, so a
// link inside an allowed folder cannot lead out of it; a file to write must not itself be a link, nor
// a file with other hard links (writing it would change the other names too).
// All refusals are path_denied (details.reason names the text check that failed, if one did).
//
// "Inside a folder" is decided on the resolved path by identity (the folder itself, or one of the
// path's parents, is the same directory as the root: fs::equivalent), so it holds whatever the case
// of the names on a case-insensitive volume (NTFS, APFS and HFS+ by default, FAT, SMB) and never
// matches a different folder on a case-sensitive one. The lexical test before anything touches the
// disk folds ASCII case on every platform: it is only a pre-filter.
//
// Time of check / time of use: the loaders and writers open files by name after these checks.
// check_read() therefore opens the file, verifies where the open handle really is (its final path,
// from the operating system) and, given a Pin, keeps it open while the tool loads it; on Windows the
// pin also stops the file and its folders from being renamed or replaced meanwhile. verify_written()
// does the same after a write. What remains open is listed in docs/mcp/README.md (File paths).

#include <cstdint>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Files {

// An open handle on a file check_read() approved, closed when the pin is destroyed. On Windows it
// is opened without FILE_SHARE_DELETE: while it lives the file cannot be deleted, renamed or replaced
// and no folder above it can be renamed, so the loader that opens the path next gets the file that
// was checked. POSIX cannot lock a name; there the pin only keeps the descriptor.
class Pin
{
public:
    Pin() = default;
    ~Pin();
    Pin(Pin&& other) noexcept;
    Pin& operator=(Pin&& other) noexcept;
    Pin(const Pin&)            = delete;
    Pin& operator=(const Pin&) = delete;

    void reset();
    // Implementation detail: a HANDLE on Windows, a file descriptor elsewhere; -1 when empty.
    std::intptr_t handle = -1;
};

std::vector<std::string> read_roots();
std::vector<std::string> write_roots();
// Created on first use.
std::string              workspace_dir();

// Extensions (lower case, with the dot) of model files the application can import.
const std::vector<std::string>& model_extensions();

// The canonical path of an existing file inside a read root whose extension is one of
// `extensions` (empty = any). Fails with path_denied or not_found; `what` names the argument.
// The file is opened and the open handle's real location checked too; with `pin`, the handle stays
// open in it (keep the pin alive until the file has been loaded).
boost::filesystem::path check_read(const std::string& path, const std::vector<std::string>& extensions, const char* what = "path", Pin* pin = nullptr);

// The canonical path of a file to create inside a write root. Fails with path_denied, or with
// `exists` when the file is there and `overwrite` is false. Never allows a path inside the
// application's data or installation folders other than the workspace.
boost::filesystem::path check_write(const std::string& path, const std::vector<std::string>& extensions, bool overwrite, const char* what = "path");

// After writing `path` (a check_write() result): opens what is now at that name without following a
// link and checks that it is a regular file with no other hard links whose real location is still
// inside a write root (and outside the application's folders). Fails with path_denied when the write
// was redirected (a folder or the file swapped for a link after the check); the file is left alone.
void verify_written(const boost::filesystem::path& path, const char* what = "path");

} // namespace Files

}}} // namespace Slic3r::GUI::Mcp

#endif
