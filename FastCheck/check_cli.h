#pragma once

// FastCheck CLI parsing and preconditions (fastcheck, M6/FR-04~12). Parsing style copied from route_probe_main:
// ArgAt/ParseLongStrict/ParseHostPort, errors throw std::runtime_error, usage to stderr.
// Key constraints: --checkers (not --streams); --streams/--chunk-kb are treated as unknown args and error out directly;
// usage does not mention --streams. Parameter errors fail before any TCP (the caller maps this to exit code 2, not 0/1).

#include "check_options.h"

#include <string>
#include <vector>

namespace fc::check {

void PrintUsage();

// Parse arguments. Throws std::runtime_error on missing required / illegal value / unknown argument (including --streams).
CheckOptions ParseCheckArgs(const std::vector<std::string>& args);

// Precondition checks on local/output paths (before any TCP, FR-12/AC-15/16). On failure, prints an error to stderr and returns false
// (the caller returns exit code 3); does not create the --output parent directory and does not connect to the server.
bool CheckLocalPreconditions(const CheckOptions& o);

}  // namespace fc::check
