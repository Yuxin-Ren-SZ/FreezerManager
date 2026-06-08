// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/CliApp.h"

#include <iostream>

int main(int argc, char** argv) {
  return fmgr::cli::run_cli(argc, argv, std::cout, std::cerr);
}
