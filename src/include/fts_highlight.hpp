#pragma once

#include "fts_common.hpp"

namespace duckdb {

void RegisterFTSRenderHighlight(cxx::Extension &extension,
                                cxx::Factory &factory);

} // namespace duckdb
