//===----------------------------------------------------------------------===//
//                         DuckDB
//
// fts_indexing.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "fts_common.hpp"

namespace duckdb {

//! The PRAGMAs of the extension, which expand to the SQL that does the work
struct FTSIndexing {
  static string DropFTSIndexQuery(cxx::PragmaFunction::QueryInput &input);
  static string CreateFTSIndexQuery(cxx::PragmaFunction::QueryInput &input);
  static string
  CreateFTSBooleanQueryMacrosQuery(cxx::PragmaFunction::QueryInput &input);
  static void Register(cxx::Extension &extension, cxx::Factory &factory);
};

} // namespace duckdb
