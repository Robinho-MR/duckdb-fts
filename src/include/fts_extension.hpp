//===----------------------------------------------------------------------===//
//                         DuckDB
//
// fts_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "fts_common.hpp"

namespace duckdb {

//! Registers the functions of the extension that compute on vectors
void RegisterFTSScalarFunctions(cxx::Extension &extension,
                                cxx::Factory &factory);

} // namespace duckdb
