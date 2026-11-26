#pragma once

#include "fts_common.hpp"

namespace duckdb {

void RegisterFTSNearTf(cxx::Extension &extension, cxx::Factory &factory);

} // namespace duckdb
