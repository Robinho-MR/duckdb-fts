#include "fts_pattern.hpp"

#include "fts_vector.hpp"

#include "re2/re2.h"

namespace duckdb {

struct PatternToken {
  string literal;
  bool is_literal;
  bool is_quantifier;
};

struct PatternAnalysis {
  string verification_pattern;
  string lookup_kind;
  string lookup_literal;
  string error_message;
};

static idx_t UTF8CharacterSize(const string &input, idx_t position) {
  int char_size = 0;
  Utf8Proc::UTF8ToCodepoint(input.data() + position, char_size,
                            input.size() - position);
  return char_size > 0 ? UnsafeNumericCast<idx_t>(char_size) : 0;
}

static bool IsASCIIAlphanumeric(const string &input, idx_t position,
                                idx_t char_size) {
  if (char_size != 1) {
    return false;
  }
  auto c = input[position];
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
}

static string QuoteRegexLiteral(const string &literal) {
  return duckdb_re2::RE2::QuoteMeta(literal);
}

static bool ParseBoundedInteger(const string &input, idx_t begin, idx_t end,
                                idx_t &value) {
  if (begin == end) {
    return false;
  }
  value = 0;
  for (auto position = begin; position < end; position++) {
    auto c = input[position];
    if (c < '0' || c > '9') {
      return false;
    }
    if (value < 1001) {
      value =
          MinValue<idx_t>(1001, value * 10 + UnsafeNumericCast<idx_t>(c - '0'));
    }
  }
  return true;
}

static bool ParseWildcard(const string &pattern, vector<PatternToken> &tokens,
                          string &verification_pattern, string &error_message) {
  for (idx_t position = 0; position < pattern.size();) {
    auto char_size = UTF8CharacterSize(pattern, position);
    if (char_size == 0) {
      error_message = "wildcard pattern is invalid UTF-8";
      return false;
    }
    auto token = pattern.substr(position, char_size);
    if (char_size == 1 && pattern[position] == '\\') {
      position += char_size;
      if (position == pattern.size()) {
        error_message =
            "wildcard pattern must not end with an escape character";
        return false;
      }
      char_size = UTF8CharacterSize(pattern, position);
      if (char_size == 0) {
        error_message = "wildcard pattern is invalid UTF-8";
        return false;
      }
      token = pattern.substr(position, char_size);
      verification_pattern += QuoteRegexLiteral(token);
      tokens.push_back({token, true, false});
    } else if (char_size == 1 && pattern[position] == '*') {
      verification_pattern += ".*";
      tokens.push_back({string(), false, false});
    } else if (char_size == 1 && pattern[position] == '?') {
      verification_pattern += ".";
      tokens.push_back({string(), false, false});
    } else {
      verification_pattern += QuoteRegexLiteral(token);
      tokens.push_back({token, true, false});
    }
    position += char_size;
  }
  return true;
}

static string ParseRegex(const string &pattern, vector<PatternToken> &tokens,
                         string &error_message) {
  for (idx_t position = 0; position < pattern.size();) {
    auto char_size = UTF8CharacterSize(pattern, position);
    if (char_size == 0) {
      error_message = "regex pattern is invalid";
      return string();
    }
    auto c = pattern[position];
    if (char_size == 1 && c == '\\') {
      auto escaped_position = position + 1;
      if (escaped_position == pattern.size()) {
        error_message = "regex pattern must not end with an escape character";
        return string();
      }
      auto escaped_size = UTF8CharacterSize(pattern, escaped_position);
      if (escaped_size == 0) {
        error_message = "regex pattern is invalid";
        return string();
      }
      if (IsASCIIAlphanumeric(pattern, escaped_position, escaped_size)) {
        error_message = "regex pattern contains an unsupported escape sequence";
        return string();
      }
      tokens.push_back(
          {pattern.substr(escaped_position, escaped_size), true, false});
      position = escaped_position + escaped_size;
      continue;
    }
    if (char_size == 1 && c == '[') {
      auto class_position = position + 1;
      bool has_content = false;
      bool closed = false;
      bool class_has_unsupported_escape = false;
      bool class_has_nested_open = false;
      while (class_position < pattern.size()) {
        auto class_char_size = UTF8CharacterSize(pattern, class_position);
        if (class_char_size == 0) {
          error_message = "regex pattern is invalid";
          return string();
        }
        auto class_char = pattern[class_position];
        if (class_char_size == 1 && class_char == '\\') {
          auto escaped_position = class_position + 1;
          if (escaped_position == pattern.size()) {
            error_message = "regex pattern contains an invalid character class";
            return string();
          }
          auto escaped_size = UTF8CharacterSize(pattern, escaped_position);
          if (escaped_size == 0) {
            error_message = "regex pattern is invalid";
            return string();
          }
          if (IsASCIIAlphanumeric(pattern, escaped_position, escaped_size)) {
            class_has_unsupported_escape = true;
          }
          if (escaped_size == 1 && pattern[escaped_position] == '[') {
            class_has_nested_open = true;
          }
          has_content = true;
          class_position = escaped_position + escaped_size;
          continue;
        }
        if (class_char_size == 1 && class_char == '[') {
          class_has_nested_open = true;
        }
        if (class_char_size == 1 && class_char == ']') {
          closed = has_content;
          class_position++;
          break;
        }
        has_content = true;
        class_position += class_char_size;
      }
      if (!closed) {
        error_message = "regex pattern contains an invalid character class";
        return string();
      }
      if (class_has_unsupported_escape) {
        error_message = "regex character classes contain an unsupported "
                        "escape sequence";
        return string();
      }
      if (class_has_nested_open) {
        error_message =
            "regex pattern contains unsupported character class syntax";
        return string();
      }
      tokens.push_back({string(), false, false});
      position = class_position;
      continue;
    }
    if (char_size == 1 && (c == '*' || c == '+' || c == '?')) {
      if (tokens.empty() || tokens.back().is_quantifier) {
        error_message = "regex pattern contains a quantifier without an atom";
        return string();
      }
      tokens.push_back({string(), false, true});
      position++;
      continue;
    }
    if (char_size == 1 && c == '{') {
      if (tokens.empty() || tokens.back().is_quantifier) {
        error_message = "regex pattern contains a quantifier without an atom";
        return string();
      }
      auto close = pattern.find('}', position + 1);
      if (close == string::npos) {
        error_message = "regex pattern contains an invalid quantifier";
        return string();
      }
      auto comma = pattern.find(',', position + 1);
      if (comma != string::npos && comma > close) {
        comma = string::npos;
      }
      idx_t minimum = 0;
      auto minimum_end = comma == string::npos ? close : comma;
      if (!ParseBoundedInteger(pattern, position + 1, minimum_end, minimum)) {
        error_message = "regex pattern contains an invalid quantifier";
        return string();
      }
      if (minimum > 1000) {
        error_message =
            "regex quantifier exceeds the supported repetition limit";
        return string();
      }
      if (comma != string::npos && comma + 1 < close) {
        idx_t maximum = 0;
        if (!ParseBoundedInteger(pattern, comma + 1, close, maximum) ||
            maximum > 1000 || minimum > maximum) {
          error_message = "regex pattern contains an invalid quantifier range";
          return string();
        }
      }
      tokens.push_back({string(), false, true});
      position = close + 1;
      continue;
    }
    if (char_size == 1 &&
        (c == '(' || c == ')' || c == '|' || c == '^' || c == '$')) {
      error_message = "regex pattern contains unsupported syntax";
      return string();
    }
    if (char_size == 1 && (c == ']' || c == '}')) {
      error_message = "regex pattern contains a quantifier without an atom";
      return string();
    }
    if (char_size == 1 && c == '.') {
      tokens.push_back({string(), false, false});
    } else {
      tokens.push_back({pattern.substr(position, char_size), true, false});
    }
    position += char_size;
  }
  return pattern;
}

static void SelectLookupLiteral(const vector<PatternToken> &tokens,
                                PatternAnalysis &analysis) {
  string run;
  idx_t run_length = 0;
  idx_t run_start = 0;
  bool found_internal = false;
  idx_t selected_internal_length = 0;

  auto finish_run = [&]() {
    if (run_length == 0) {
      return false;
    }
    if (run_start == 0 && run_length >= 2) {
      analysis.lookup_kind = "prefix";
      analysis.lookup_literal = run;
      return true;
    }
    if (run_length >= 3 &&
        (!found_internal || run_length > selected_internal_length)) {
      analysis.lookup_kind = "gram";
      analysis.lookup_literal = run;
      selected_internal_length = run_length;
      found_internal = true;
    }
    run.clear();
    run_length = 0;
    return false;
  };

  for (idx_t token_index = 0; token_index < tokens.size(); token_index++) {
    auto mandatory = tokens[token_index].is_literal &&
                     (token_index + 1 == tokens.size() ||
                      !tokens[token_index + 1].is_quantifier);
    if (!mandatory) {
      if (finish_run()) {
        return;
      }
      continue;
    }
    if (run_length == 0) {
      run_start = token_index;
    }
    run += tokens[token_index].literal;
    run_length++;
  }
  finish_run();
}

static PatternAnalysis AnalyzePattern(const string &pattern,
                                      const string &pattern_mode) {
  PatternAnalysis analysis;
  vector<PatternToken> tokens;

  auto mode = LowerASCII(pattern_mode);
  if (pattern.empty()) {
    analysis.error_message = "pattern must not be empty";
    return analysis;
  }
  if (mode == "wildcard") {
    if (!ParseWildcard(pattern, tokens, analysis.verification_pattern,
                       analysis.error_message)) {
      return analysis;
    }
  } else if (mode == "regex") {
    analysis.verification_pattern =
        ParseRegex(pattern, tokens, analysis.error_message);
    if (!analysis.error_message.empty()) {
      return analysis;
    }
  } else {
    analysis.error_message = "pattern mode must be wildcard or regex";
    return analysis;
  }

  SelectLookupLiteral(tokens, analysis);
  if (analysis.lookup_literal.empty()) {
    analysis.error_message = "pattern must contain a leading literal prefix "
                             "of at least two characters or a mandatory "
                             "literal run of at least three characters";
    return analysis;
  }
  duckdb_re2::RE2::Options regex_options;
  regex_options.set_log_errors(false);
  duckdb_re2::RE2 regex(analysis.verification_pattern, regex_options);
  if (!regex.ok()) {
    analysis.lookup_kind.clear();
    analysis.lookup_literal.clear();
    analysis.error_message = "regex pattern is invalid";
  }
  return analysis;
}

static void AnalyzePatternFunction(cxx::ScalarFunction::ExecInput &input) {
  auto count = input.GetRowCount();
  FTSInput<cxx::varchar_t> patterns(input.GetArg(0));
  FTSInput<cxx::varchar_t> modes(input.GetArg(1));

  auto result = input.GetResult();
  FTSSetSizeAllValid(result, count);
  auto verification_column = result.GetChild(0);
  auto kind_column = result.GetChild(1);
  auto literal_column = result.GetChild(2);
  auto error_column = result.GetChild(3);
  FTSSetSizeAllValid(verification_column, count);
  FTSSetSizeAllValid(kind_column, count);
  FTSSetSizeAllValid(literal_column, count);
  FTSSetSizeAllValid(error_column, count);

  for (idx_t row = 0; row < count; row++) {
    PatternAnalysis analysis;
    if (!patterns.IsValid(row)) {
      analysis.error_message = "pattern must not be NULL";
    } else if (!modes.IsValid(row)) {
      analysis.error_message = "pattern mode must be wildcard or regex";
    } else {
      analysis = AnalyzePattern(string(patterns.Get(row).view()),
                                string(modes.Get(row).view()));
    }

    if (analysis.error_message.empty()) {
      verification_column.AssignString(row, analysis.verification_pattern);
      kind_column.AssignString(row, analysis.lookup_kind);
      literal_column.AssignString(row, analysis.lookup_literal);
      error_column.AssignString(row, "");
      error_column.GetValidityMutable().SetInvalid(row);
    } else {
      verification_column.AssignString(row, "");
      kind_column.AssignString(row, "");
      kind_column.GetValidityMutable().SetInvalid(row);
      literal_column.AssignString(row, "");
      literal_column.GetValidityMutable().SetInvalid(row);
      error_column.AssignString(row, analysis.error_message);
    }
  }
}

void RegisterFTSAnalyzePattern(cxx::Extension &extension,
                               cxx::Factory &factory) {
  auto function = cxx::ScalarFunction::Create(factory);
  function.SetName("fts_analyze_pattern")
      .SetExecCallback(AnalyzePatternFunction)
      .SetNullHandling(cxx::FunctionNullHandling::SPECIAL)
      .WithSignature([&](cxx::FunctionSignature &signature) {
        auto varchar = factory.ParseType("VARCHAR");
        auto positional = cxx::FunctionParameterKind::POSITIONAL_ONLY;
        signature.AddParameter("pattern", varchar, positional)
            .AddParameter("mode", varchar, positional)
            .SetReturnType(factory.ParseType(
                "STRUCT(verification_pattern VARCHAR, lookup_kind VARCHAR, "
                "lookup_literal VARCHAR, error_message VARCHAR)"));
      });
  extension.Register(function);
}

} // namespace duckdb
