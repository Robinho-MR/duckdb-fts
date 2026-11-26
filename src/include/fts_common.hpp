//===----------------------------------------------------------------------===//
//                         DuckDB
//
// fts_common.hpp
//
// Small helpers shared by the extension. The extension is written against
// DuckDB's stable C++ API only, so nothing here comes from the engine.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb_cpp.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace duckdb {

using std::string;
using std::vector;
using cxx::idx_t;

// Error codes of DUCKDB_V2_ERROR_*, which decide the error type of the query
// that fails.
static constexpr int FTS_ERROR_NOT_IMPLEMENTED = 5009;
static constexpr int FTS_ERROR_CATALOG = 6001;
static constexpr int FTS_ERROR_INTERNAL = 8001;

//! An exception of a given error type, with a printf-style message
template <int CODE> class FTSException : public cxx::Exception {
public:
  explicit FTSException(const string &message)
      : cxx::Exception(CODE, message, message) {}
};

using CatalogException = FTSException<FTS_ERROR_CATALOG>;
using NotImplementedException = FTSException<FTS_ERROR_NOT_IMPLEMENTED>;
using InternalException = FTSException<FTS_ERROR_INTERNAL>;
using cxx::InvalidInputException;

namespace fts_detail {
template <class T> inline const T &FormatArgument(const T &value) {
  return value;
}
inline const char *FormatArgument(const string &value) { return value.c_str(); }
} // namespace fts_detail

//! printf-style formatting that takes std::string arguments for %s
template <class... ARGS>
string Format(const char *format, const ARGS &...args) {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
  auto size = std::snprintf(nullptr, 0, format,
                            fts_detail::FormatArgument(args)...);
  assert(size >= 0);
  string result(static_cast<size_t>(size), '\0');
  std::snprintf(&result[0], result.size() + 1, format,
                fts_detail::FormatArgument(args)...);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
  return result;
}

//! Throws an exception of type EXCEPTION with a printf-style message
template <class EXCEPTION, class... ARGS>
[[noreturn]] void Throw(const char *format, const ARGS &...args) {
  throw EXCEPTION(Format(format, args...));
}

inline string Join(const vector<string> &input, const string &separator) {
  string result;
  for (idx_t i = 0; i < input.size(); i++) {
    if (i > 0) {
      result += separator;
    }
    result += input[i];
  }
  return result;
}

template <class T, class F>
string Join(const T &input, idx_t count, const string &separator, F &&fun) {
  string result;
  for (idx_t i = 0; i < count; i++) {
    if (i > 0) {
      result += separator;
    }
    result += fun(input[i]);
  }
  return result;
}

inline char ToLowerASCII(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline string LowerASCII(const string &input) {
  string result = input;
  for (auto &c : result) {
    c = ToLowerASCII(c);
  }
  return result;
}

//! Quotes an identifier: the name is kept as is, DuckDB matches quoted
//! identifiers case-insensitively
inline string QuoteIdentifier(const string &identifier) {
  string result = "\"";
  for (auto c : identifier) {
    if (c == '"') {
      result += '"';
    }
    result += c;
  }
  result += '"';
  return result;
}

//! A string literal
inline string QuoteLiteral(const string &value) {
  string result = "'";
  for (auto c : value) {
    if (c == '\'') {
      result += '\'';
    }
    result += c;
  }
  result += '\'';
  return result;
}

struct CaseInsensitiveHash {
  size_t operator()(const string &key) const {
    return std::hash<string>()(LowerASCII(key));
  }
};
struct CaseInsensitiveEquals {
  bool operator()(const string &a, const string &b) const {
    return LowerASCII(a) == LowerASCII(b);
  }
};
using case_insensitive_set_t =
    std::unordered_set<string, CaseInsensitiveHash, CaseInsensitiveEquals>;

template <class T> inline const T &MinValue(const T &a, const T &b) {
  return a < b ? a : b;
}
template <class T> inline const T &MaxValue(const T &a, const T &b) {
  return a < b ? b : a;
}
template <class T, class S> inline T UnsafeNumericCast(S value) {
  return static_cast<T>(value);
}
template <class T, class S> inline T NumericCast(S value) {
  return static_cast<T>(value);
}


//! UTF-8 helpers
struct Utf8Proc {
  //! Decodes the codepoint at the start of `input`; throws on invalid UTF-8
  static int32_t UTF8ToCodepoint(const char *input, int &size,
                                 const size_t length) {
    auto u = reinterpret_cast<const unsigned char *>(input);
    unsigned char u0 = u[0];
    if (u0 <= 127) {
      size = 1;
      return u0;
    }
    size_t needed;
    if (u0 >= 240 && u0 <= 247) {
      needed = 4;
    } else if (u0 >= 224 && u0 <= 239) {
      needed = 3;
    } else if (u0 >= 192 && u0 <= 223) {
      needed = 2;
    } else {
      throw InvalidInputException("invalid lead byte detected in "
                                  "Utf8Proc::UTF8ToCodepoint, likely due to "
                                  "invalid UTF-8");
    }
    if (length < needed) {
      throw InvalidInputException(
          "truncated UTF-8 sequence detected in Utf8Proc::UTF8ToCodepoint");
    }
    unsigned char u1 = u[1];
    if (needed == 2) {
      size = 2;
      return (u0 - 192) * 64 + (u1 - 128);
    }
    if (u0 == 0xed && (u1 & 0xa0) == 0xa0) {
      throw InvalidInputException(
          "invalid code point detected in Utf8Proc::UTF8ToCodepoint (0xd800 "
          "to 0xdfff), likely due to invalid UTF-8");
    }
    unsigned char u2 = u[2];
    if (needed == 3) {
      size = 3;
      return (u0 - 224) * 4096 + (u1 - 128) * 64 + (u2 - 128);
    }
    unsigned char u3 = u[3];
    size = 4;
    return (u0 - 240) * 262144 + (u1 - 128) * 4096 + (u2 - 128) * 64 +
           (u3 - 128);
  }

  //! Encodes a codepoint; false when it cannot be encoded
  static bool CodepointToUtf8(int cp, int &size, char *c) {
    if (cp <= 0x7F) {
      size = 1;
      c[0] = static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
      size = 2;
      c[0] = static_cast<char>((cp >> 6) + 192);
      c[1] = static_cast<char>((cp & 63) + 128);
    } else if (0xd800 <= cp && cp <= 0xdfff) {
      size = -1;
      return false;
    } else if (cp <= 0xFFFF) {
      size = 3;
      c[0] = static_cast<char>((cp >> 12) + 224);
      c[1] = static_cast<char>(((cp >> 6) & 63) + 128);
      c[2] = static_cast<char>((cp & 63) + 128);
    } else if (cp <= 0x10FFFF) {
      size = 4;
      c[0] = static_cast<char>((cp >> 18) + 240);
      c[1] = static_cast<char>(((cp >> 12) & 63) + 128);
      c[2] = static_cast<char>(((cp >> 6) & 63) + 128);
      c[3] = static_cast<char>((cp & 63) + 128);
    } else {
      size = -1;
      return false;
    }
    return true;
  }

  static int32_t CodepointToLower(int32_t codepoint);
};

//! The table an index is on: a catalog (empty when the name has none), a schema
//! and a name
class FTSTableName {
public:
  FTSTableName() = default;
  FTSTableName(string catalog_p, string schema_p, string name_p)
      : catalog(std::move(catalog_p)), schema(std::move(schema_p)),
        name(std::move(name_p)) {}

  const string &Catalog() const { return catalog; }
  const string &Schema() const { return schema; }
  const string &Name() const { return name; }

private:
  string catalog;
  string schema;
  string name;
};

} // namespace duckdb
