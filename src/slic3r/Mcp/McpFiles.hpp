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
// link inside an allowed folder cannot lead out of it; a file to write must not itself be a link.
// All refusals are path_denied (details.reason names the text check that failed, if one did).

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Files {

std::vector<std::string> read_roots();
std::vector<std::string> write_roots();
// Created on first use.
std::string              workspace_dir();

// Extensions (lower case, with the dot) of model files the application can import.
const std::vector<std::string>& model_extensions();

// The canonical path of an existing file inside a read root whose extension is one of
// `extensions` (empty = any). Fails with path_denied or not_found; `what` names the argument.
boost::filesystem::path check_read(const std::string& path, const std::vector<std::string>& extensions, const char* what = "path");

// The canonical path of a file to create inside a write root. Fails with path_denied, or with
// `exists` when the file is there and `overwrite` is false. Never allows a path inside the
// application's data or installation folders other than the workspace.
boost::filesystem::path check_write(const std::string& path, const std::vector<std::string>& extensions, bool overwrite, const char* what = "path");

} // namespace Files

}}} // namespace Slic3r::GUI::Mcp

#endif
