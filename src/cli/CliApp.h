// SPDX-License-Identifier: AGPL-3.0-or-later

// `freezerctl` argument parsing and command dispatch. Thin layer over CLI11 that
// wires the `sample list` / `sample export` subcommands to the command
// functions. Returns a process exit code; never throws to the caller.
#ifndef FMGR_CLI_CLIAPP_H
#define FMGR_CLI_CLIAPP_H

#include <ostream>

namespace fmgr::cli {

  // Parse argv and run the selected command. Output goes to `out`, diagnostics to
  // `err`. Returns 0 on success, non-zero on usage or runtime error.
  [[nodiscard]] int run_cli(int argc, const char* const* argv, std::ostream& out,
                            std::ostream& err);

} // namespace fmgr::cli

#endif // FMGR_CLI_CLIAPP_H
