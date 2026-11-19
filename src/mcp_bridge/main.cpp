// orca-mcp-bridge: lets stdio-only MCP clients (Claude Desktop) use OrcaSlicer's MCP server,
// which listens on Streamable HTTP on 127.0.0.1. See docs/mcp/orca-mcp-bridge.md.
#include "BridgeCommon.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace orca_mcp_bridge {
namespace {

const char *USAGE =
    "Usage: orca-mcp-bridge [options]\n"
    "\n"
    "Forwards MCP messages between stdio (newline-delimited JSON-RPC, as launched by Claude\n"
    "Desktop) and OrcaSlicer's MCP server (Streamable HTTP on 127.0.0.1). With no options it\n"
    "reads the server's URL and token from the endpoint file OrcaSlicer writes when its MCP\n"
    "server starts: <data dir>/mcp/endpoint.json.\n"
    "\n"
    "Server selection (command line wins over environment):\n"
    "  --url URL              server URL, e.g. http://127.0.0.1:13720/mcp      [ORCA_MCP_URL]\n"
    "                         (or ORCA_MCP_PORT=<port> for http://127.0.0.1:<port>/mcp)\n"
    "  --token-file PATH      file holding the bearer token                    [ORCA_MCP_TOKEN_FILE]\n"
    "  --token TOKEN          bearer token (visible to other processes; prefer\n"
    "                         --token-file or the environment)                [ORCA_MCP_TOKEN]\n"
    "  --endpoint-file PATH   endpoint file to read                            [ORCA_MCP_ENDPOINT_FILE]\n"
    "  --datadir DIR          OrcaSlicer data directory (uses DIR/mcp/endpoint.json)\n"
    "                                                                          [ORCA_MCP_DATADIR]\n"
    "  --allow-remote         allow a non-loopback server (token sent in clear text)\n"
    "\n"
    "Behaviour:\n"
    "  --request-timeout SEC  fail a request after SEC seconds (default 0 = never; long tool\n"
    "                         calls are expected, the MCP client applies its own limits)\n"
    "  --connect-wait SEC     keep retrying for SEC seconds while the app is not reachable\n"
    "                         (default 0 = answer with an error at once)\n"
    "  --busy-wait SEC        keep retrying HTTP 429 for SEC seconds (default 60)\n"
    "  --eof-grace SEC        after stdin closes, let running requests finish for up to SEC\n"
    "                         seconds (default 0)\n"
    "  --no-listen-stream     legacy era: do not open the optional GET event stream\n"
    "\n"
    "Diagnostics (stdout carries MCP messages only; everything else goes to stderr):\n"
    "  --log-file PATH        also append the log to PATH                      [ORCA_MCP_BRIDGE_LOG]\n"
    "  -v, --verbose          debug logging (never includes the token or message bodies)\n"
    "  -q, --quiet            errors only\n"
    "  --check                probe the server (server/discover, then initialize) and report on\n"
    "                         stderr; exit 0 when it answers, 4 when it does not\n"
    "  --version, --help\n"
    "\n"
    "Exit codes: 0 stdin closed / signal, 1 internal error, 2 bad usage, 3 stdout closed,\n"
    "4 --check failed.\n";

bool parse_seconds(const std::string &s, double &out)
{
    try {
        std::size_t pos = 0;
        out             = std::stod(s, &pos);
        return pos == s.size() && out >= 0;
    } catch (const std::exception &) {
        return false;
    }
}

std::vector<std::string> utf8_args(int argc, char **argv)
{
    std::vector<std::string> args;
#ifdef _WIN32
    (void) argc;
    (void) argv;
    int     n    = 0;
    LPWSTR *wargv = ::CommandLineToArgvW(::GetCommandLineW(), &n);
    for (int i = 1; wargv && i < n; ++i) {
        const std::wstring w(wargv[i]);
        const int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(std::size_t(len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), len, nullptr, nullptr);
        args.push_back(std::move(s));
    }
    if (wargv)
        ::LocalFree(wargv);
#else
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
#endif
    return args;
}

int usage_error(const std::string &msg)
{
    std::fprintf(stderr, "%s: %s\nRun with --help for usage.\n", BRIDGE_NAME, msg.c_str());
    return EXIT_USAGE;
}

} // namespace
} // namespace orca_mcp_bridge

int main(int argc, char **argv)
{
    using namespace orca_mcp_bridge;
    stdio_init_binary();

    Options opts;
    const auto args = utf8_args(argc, argv);
    bool token_on_cmdline = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string a = args[i];
        std::string value;
        bool        has_inline = false;
        if (a.rfind("--", 0) == 0) {
            const auto eq = a.find('=');
            if (eq != std::string::npos) {
                value      = a.substr(eq + 1);
                a          = a.substr(0, eq);
                has_inline = true;
            }
        }
        auto need = [&](std::string &dst) -> bool {
            if (has_inline) {
                dst = value;
                return true;
            }
            if (i + 1 >= args.size())
                return false;
            dst = args[++i];
            return true;
        };
        auto need_seconds = [&](double &dst) -> bool {
            std::string s;
            return need(s) && parse_seconds(s, dst);
        };
        if (a == "-h" || a == "--help") {
            std::fputs(USAGE, stderr);
            return EXIT_OK;
        } else if (a == "--version") {
            std::fprintf(stderr, "%s %s\n", BRIDGE_NAME, BRIDGE_VERSION);
            return EXIT_OK;
        } else if (a == "--url") {
            if (!need(opts.url))
                return usage_error("--url needs a value");
            opts.url_source = "--url";
        } else if (a == "--token") {
            if (!need(opts.token))
                return usage_error("--token needs a value");
            opts.token_source = "--token";
            token_on_cmdline  = true;
        } else if (a == "--token-file") {
            if (!need(opts.token_file))
                return usage_error("--token-file needs a value");
        } else if (a == "--endpoint-file") {
            if (!need(opts.endpoint_file))
                return usage_error("--endpoint-file needs a value");
        } else if (a == "--datadir") {
            if (!need(opts.datadir))
                return usage_error("--datadir needs a value");
        } else if (a == "--log-file") {
            if (!need(opts.log_file))
                return usage_error("--log-file needs a value");
        } else if (a == "--allow-remote") {
            opts.allow_remote = true;
        } else if (a == "--request-timeout") {
            if (!need_seconds(opts.request_timeout_s))
                return usage_error("--request-timeout needs a number of seconds");
        } else if (a == "--connect-wait") {
            if (!need_seconds(opts.connect_wait_s))
                return usage_error("--connect-wait needs a number of seconds");
        } else if (a == "--busy-wait") {
            if (!need_seconds(opts.busy_wait_s))
                return usage_error("--busy-wait needs a number of seconds");
        } else if (a == "--eof-grace") {
            if (!need_seconds(opts.eof_grace_s))
                return usage_error("--eof-grace needs a number of seconds");
        } else if (a == "--no-listen-stream") {
            opts.listen_stream = false;
        } else if (a == "-v" || a == "--verbose") {
            opts.verbosity = 2;
        } else if (a == "-q" || a == "--quiet") {
            opts.verbosity = 0;
        } else if (a == "--check") {
            opts.check_only = true;
        } else {
            return usage_error("unknown argument: " + a);
        }
    }

    // Environment fills what the command line left open.
    if (opts.url.empty()) {
        if (auto u = get_env("ORCA_MCP_URL"); u && !u->empty()) {
            opts.url        = *u;
            opts.url_source = "ORCA_MCP_URL";
        } else if (auto p = get_env("ORCA_MCP_PORT"); p && !p->empty()) {
            opts.url        = "http://127.0.0.1:" + *p + "/mcp";
            opts.url_source = "ORCA_MCP_PORT";
        }
    }
    if (opts.token.empty() && opts.token_file.empty()) {
        if (auto t = get_env("ORCA_MCP_TOKEN"); t && !t->empty()) {
            opts.token        = *t;
            opts.token_source = "ORCA_MCP_TOKEN";
        } else if (auto f = get_env("ORCA_MCP_TOKEN_FILE"); f && !f->empty()) {
            opts.token_file = *f;
        }
    }
    if (opts.endpoint_file.empty())
        if (auto f = get_env("ORCA_MCP_ENDPOINT_FILE"); f && !f->empty())
            opts.endpoint_file = *f;
    if (opts.datadir.empty())
        if (auto d = get_env("ORCA_MCP_DATADIR"); d && !d->empty())
            opts.datadir = *d;
    if (opts.log_file.empty())
        if (auto l = get_env("ORCA_MCP_BRIDGE_LOG"); l && !l->empty())
            opts.log_file = *l;

    log_init(opts.verbosity, opts.log_file);
    if (token_on_cmdline)
        log_info("note: a token given with --token is visible to other processes; prefer --token-file or ORCA_MCP_TOKEN");

    {
        EndpointResolver resolver(opts);
        std::string      err;
        if (!resolver.validate_static(err))
            return usage_error(err);
    }

    try {
        return opts.check_only ? run_check(opts) : run_bridge(opts);
    } catch (const std::exception &e) {
        log_error(std::string("fatal: ") + e.what());
        return EXIT_INTERNAL;
    }
}
