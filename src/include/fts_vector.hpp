//===----------------------------------------------------------------------===//
//                         DuckDB
//
// fts_vector.hpp
//
// Reading the arguments and writing the results of the scalar functions
//
//===----------------------------------------------------------------------===//

#pragma once

#include "fts_common.hpp"

#include <string_view>

namespace duckdb {

//! Reads the rows of an argument vector of primitive type T, whatever its
//! layout
template <class T> class FTSInput {
public:
  explicit FTSInput(const cxx::Vector &vector) {
    if (vector.GetVectorType() == cxx::VectorType::OTHER) {
      vector.Flatten();
    }
    view = vector.GetView();
  }

  bool IsValid(idx_t row) const { return view.IsValid(row); }
  const T &Get(idx_t row) const { return view.Data<T>()[view.SelAt(row)]; }
  //! The element of the vector row `row` reads, for reading child vectors
  idx_t Element(idx_t row) const { return view.SelAt(row); }

private:
  cxx::VectorView view;
};

//! The elements of the lists of a LIST argument, which holds elements of
//! primitive type T
template <class T> class FTSListInput {
public:
  explicit FTSListInput(const cxx::Vector &vector)
      : entries(vector), elements(vector.GetChild(0)) {}

  bool IsValid(idx_t row) const { return entries.IsValid(row); }
  idx_t Length(idx_t row) const { return entries.Get(row).length; }
  bool ElementIsValid(idx_t row, idx_t i) const {
    return elements.IsValid(entries.Get(row).offset + i);
  }
  T Element(idx_t row, idx_t i) const {
    return elements.Get(entries.Get(row).offset + i);
  }

private:
  FTSInput<cxx::list_entry_t> entries;
  FTSInput<T> elements;
};

//! Marks the rows of a result vector that are NULL, in the order they come
class FTSNulls {
public:
  FTSNulls(cxx::Vector &vector_p, idx_t count_p)
      : vector(vector_p), count(count_p) {}

  void SetNull(idx_t row) {
    if (!fetched) {
      mask = vector.GetValidityMutable();
      mask.SetAllValid(count);
      fetched = true;
    }
    mask.SetInvalid(row);
  }

private:
  cxx::Vector &vector;
  idx_t count;
  bool fetched = false;
  cxx::ValidityMask mask {nullptr};
};

//! Resets a vector that is filled completely (every row a value) to the given
//! size
inline void FTSSetSizeAllValid(cxx::Vector &vector, idx_t count) {
  vector.SetSize(count);
  vector.GetValidityMutable().SetAllValid(count);
}

} // namespace duckdb
