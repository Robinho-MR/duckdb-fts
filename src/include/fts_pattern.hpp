#pragma once

#include "fts_common.hpp"

namespace duckdb {

void RegisterFTSAnalyzePattern(cxx::Extension &extension,
                               cxx::Factory &factory);

} // namespace duckdb
