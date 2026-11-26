#pragma once

#include "fts_common.hpp"

namespace duckdb {

//! fts_tokenize and fts_tokenize_spans
void RegisterFTSTokenize(cxx::Extension &extension, cxx::Factory &factory);

} // namespace duckdb
