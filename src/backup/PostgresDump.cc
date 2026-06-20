// SPDX-License-Identifier: AGPL-3.0-or-later

#include "backup/PostgresDump.h"

#include "backup/SqliteBackup.h" // BackupError

// We link libpq (PostgreSQL::PostgreSQL) for connection-string parsing but declare
// the three stable public symbols we use here, rather than #include <libpq-fe.h>:
// the Conan libpq package ships the library but does not export its include
// directory. PQconninfoOption is a long-stable public ABI struct.
extern "C" {
struct fmgr_PQconninfoOption {
  char* keyword;
  char* envvar;
  char* compiled;
  char* val;
  char* label;
  char* dispchar;
  int dispsize;
};
// NOLINTBEGIN(readability-identifier-naming)
fmgr_PQconninfoOption* PQconninfoParse(const char* conninfo, char** errmsg);
void PQconninfoFree(fmgr_PQconninfoOption* connOptions);
void PQfreemem(void* ptr);
// NOLINTEND(readability-identifier-naming)
}

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fmgr::backup {

  namespace {

    // libpq value escaping for a keyword/value conninfo: wrap in single quotes and
    // backslash-escape embedded quotes and backslashes (per libpq's documented rules).
    std::string quote_conninfo_value(const std::string& value) {
      std::string out = "'";
      for (const char character : value) {
        if (character == '\\' || character == '\'') {
          out.push_back('\\');
        }
        out.push_back(character);
      }
      out.push_back('\'');
      return out;
    }

    struct SanitizedConn {
      std::string conninfo;                // password-free keyword/value conninfo
      std::optional<std::string> password; // pulled out, passed via PGPASSWORD
    };

    // Parse the caller's connection string with libpq and rebuild a keyword/value
    // conninfo with the password removed. Handles both URI and keyword/value input
    // because PQconninfoParse accepts both. Throws BackupError on a malformed string.
    SanitizedConn sanitize_conninfo(const std::string& conninfo) {
      char* errmsg = nullptr;
      fmgr_PQconninfoOption* options = PQconninfoParse(conninfo.c_str(), &errmsg);
      if (options == nullptr) {
        const std::string detail = (errmsg != nullptr) ? errmsg : "malformed connection string";
        PQfreemem(errmsg);
        throw BackupError("invalid PostgreSQL connection string: " + detail);
      }

      SanitizedConn result;
      std::ostringstream rebuilt;
      bool first = true;
      for (fmgr_PQconninfoOption* opt = options; opt->keyword != nullptr; ++opt) {
        if (opt->val == nullptr || *opt->val == '\0') {
          continue; // only emit options the caller actually set
        }
        if (std::string(opt->keyword) == "password") {
          result.password = std::string(opt->val);
          continue;
        }
        if (!first) {
          rebuilt << ' ';
        }
        rebuilt << opt->keyword << '=' << quote_conninfo_value(opt->val);
        first = false;
      }
      PQconninfoFree(options);
      result.conninfo = rebuilt.str();
      return result;
    }

    // Run a client tool with explicit argv (no shell). stdout is discarded; stderr
    // is captured to `captured_stderr`. If `password` is set it is exported as
    // PGPASSWORD in the child only — never placed in argv. Returns the exit status,
    // or -1 if the process could not be spawned/awaited.
    int run_pg_tool(const std::vector<std::string>& argv,
                    const std::optional<std::string>& password, std::string& captured_stderr) {
      // Temp file to collect the child's stderr (avoids pipe-deadlock bookkeeping).
      std::string err_path = "/tmp/fmgr-pgtool-XXXXXX"; // NOLINT(concurrency-mt-unsafe)
      const int err_fd = ::mkstemp(err_path.data());
      if (err_fd < 0) {
        captured_stderr = "cannot create temp file for tool stderr";
        return -1;
      }

      const pid_t pid = ::fork();
      if (pid < 0) {
        ::close(err_fd);
        ::unlink(err_path.c_str());
        captured_stderr = "fork failed";
        return -1;
      }

      if (pid == 0) {
        // ---- child ----
        if (password.has_value()) {
          ::setenv("PGPASSWORD", password->c_str(), 1); // NOLINT(concurrency-mt-unsafe)
        }
        const int devnull = ::open("/dev/null", O_WRONLY); // NOLINT
        if (devnull >= 0) {
          ::dup2(devnull, STDOUT_FILENO);
        }
        ::dup2(err_fd, STDERR_FILENO);
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (const auto& arg : argv) {
          raw.push_back(
              const_cast<char*>(arg.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast)
        }
        raw.push_back(nullptr);
        ::execvp(raw[0], raw.data());
        ::_exit(127); // exec failed
      }

      // ---- parent ----
      ::close(err_fd);
      int status = 0;
      const pid_t waited = ::waitpid(pid, &status, 0);

      std::ifstream err_in(err_path);
      std::ostringstream err_buf;
      err_buf << err_in.rdbuf();
      captured_stderr = err_buf.str();
      err_in.close();
      ::unlink(err_path.c_str());

      if (waited < 0) {
        return -1;
      }
      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      }
      return -1;
    }

    std::string trim_trailing_newlines(std::string text) {
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
      }
      return text;
    }

  } // namespace

  void pg_dump_to_file(const std::string& conninfo, const std::string& out_path) {
    const SanitizedConn conn = sanitize_conninfo(conninfo);
    std::string tool_stderr;
    const int rc = run_pg_tool(
        {"pg_dump", "-Fc", "--no-owner", "--no-acl", "-f", out_path, "-d", conn.conninfo},
        conn.password, tool_stderr);
    if (rc != 0) {
      throw BackupError("pg_dump failed (exit " + std::to_string(rc) +
                        "): " + trim_trailing_newlines(tool_stderr));
    }
  }

  void pg_restore_from_file(const std::string& conninfo, const std::string& dump_path) {
    const SanitizedConn conn = sanitize_conninfo(conninfo);
    std::string tool_stderr;
    const int rc = run_pg_tool(
        {"pg_restore", "--clean", "--if-exists", "--no-owner", "-d", conn.conninfo, dump_path},
        conn.password, tool_stderr);
    if (rc != 0) {
      throw BackupError("pg_restore failed (exit " + std::to_string(rc) +
                        "): " + trim_trailing_newlines(tool_stderr));
    }
  }

  bool pg_restore_list_ok(const std::string& dump_path, std::string& detail) {
    std::string tool_stderr;
    const int rc = run_pg_tool({"pg_restore", "--list", dump_path}, std::nullopt, tool_stderr);
    if (rc != 0) {
      detail = "pg_restore --list failed (exit " + std::to_string(rc) +
               "): " + trim_trailing_newlines(tool_stderr);
      return false;
    }
    return true;
  }

} // namespace fmgr::backup
