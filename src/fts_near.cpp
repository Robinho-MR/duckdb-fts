#include "fts_near.hpp"

#include "fts_vector.hpp"

namespace duckdb {

// Positions arrive unordered with the term id of each posting; the result is
// the smallest per-term count of positions within width of a match end.
static int64_t NearTf(const vector<int64_t> &positions,
                      const vector<int64_t> &term_ids, int64_t slot_count,
                      int64_t width) {
  if (slot_count <= 0 || width < 0) {
    return 0;
  }
  vector<std::pair<int64_t, int64_t>> postings;
  postings.reserve(positions.size());
  for (idx_t i = 0; i < positions.size(); i++) {
    postings.emplace_back(positions[i], term_ids[i]);
  }
  std::sort(postings.begin(), postings.end());
  vector<int64_t> unique_ids;
  vector<idx_t> slots;
  slots.reserve(postings.size());
  for (const auto &posting : postings) {
    auto term_id = posting.second;
    idx_t slot = unique_ids.size();
    for (idx_t known = 0; known < unique_ids.size(); known++) {
      if (unique_ids[known] == term_id) {
        slot = known;
        break;
      }
    }
    if (slot == unique_ids.size()) {
      unique_ids.push_back(term_id);
    }
    slots.push_back(slot);
  }
  if (NumericCast<int64_t>(unique_ids.size()) < slot_count) {
    return 0;
  }
  auto found_count = unique_ids.size();
  vector<int64_t> last(found_count, std::numeric_limits<int64_t>::min());
  vector<int64_t> ends;
  idx_t filled = 0;
  for (idx_t i = 0; i < postings.size(); i++) {
    if (last[slots[i]] == std::numeric_limits<int64_t>::min()) {
      filled++;
    }
    last[slots[i]] = postings[i].first;
    if (filled < found_count) {
      continue;
    }
    auto min_last = last[0];
    for (idx_t s = 1; s < found_count; s++) {
      min_last = MinValue(min_last, last[s]);
    }
    if (min_last >= postings[i].first - width) {
      ends.push_back(postings[i].first);
    }
  }
  if (ends.empty()) {
    return 0;
  }
  vector<int64_t> matched(found_count, 0);
  auto next_end = std::numeric_limits<int64_t>::max();
  idx_t end_index = ends.size();
  for (idx_t i = postings.size(); i > 0; i--) {
    auto position = postings[i - 1].first;
    while (end_index > 0 && ends[end_index - 1] >= position) {
      next_end = ends[end_index - 1];
      end_index--;
    }
    if (next_end != std::numeric_limits<int64_t>::max() &&
        next_end - position <= width) {
      matched[slots[i - 1]]++;
    }
  }
  auto tf = matched[0];
  for (idx_t s = 1; s < found_count; s++) {
    tf = MinValue(tf, matched[s]);
  }
  return tf;
}

static void NearTfFunction(cxx::ScalarFunction::ExecInput &input) {
  auto count = input.GetRowCount();
  FTSListInput<int64_t> position_lists(input.GetArg(0));
  FTSListInput<int64_t> term_id_lists(input.GetArg(1));
  FTSInput<int64_t> slot_counts(input.GetArg(2));
  FTSInput<int64_t> widths(input.GetArg(3));

  auto result = input.GetResult();
  result.SetSize(count);
  auto result_data = result.GetDataMutable<int64_t>();
  FTSNulls nulls(result, count);
  for (idx_t i = 0; i < count; i++) {
    if (!position_lists.IsValid(i) || !term_id_lists.IsValid(i) ||
        !slot_counts.IsValid(i) || !widths.IsValid(i)) {
      result_data[i] = 0;
      nulls.SetNull(i);
      continue;
    }
    vector<int64_t> positions;
    vector<int64_t> term_ids;
    positions.reserve(position_lists.Length(i));
    term_ids.reserve(term_id_lists.Length(i));
    bool entries_valid = true;
    for (idx_t j = 0; j < position_lists.Length(i); j++) {
      if (!position_lists.ElementIsValid(i, j)) {
        entries_valid = false;
        break;
      }
      positions.push_back(position_lists.Element(i, j));
    }
    for (idx_t j = 0; j < term_id_lists.Length(i); j++) {
      if (!term_id_lists.ElementIsValid(i, j)) {
        entries_valid = false;
        break;
      }
      term_ids.push_back(term_id_lists.Element(i, j));
    }
    if (!entries_valid || positions.size() != term_ids.size()) {
      result_data[i] = 0;
      nulls.SetNull(i);
      continue;
    }
    result_data[i] =
        NearTf(positions, term_ids, slot_counts.Get(i), widths.Get(i));
  }
}

void RegisterFTSNearTf(cxx::Extension &extension, cxx::Factory &factory) {
  auto function = cxx::ScalarFunction::Create(factory);
  function.SetName("fts_near_tf")
      .SetExecCallback(NearTfFunction)
      .SetFallibility(cxx::FunctionFallibility::INFALLIBLE)
      .WithSignature([&](cxx::FunctionSignature &signature) {
        using cxx::FunctionParameterKind;
        auto bigint = factory.ParseType("BIGINT");
        auto bigint_list = factory.ParseType("BIGINT[]");
        auto positional = FunctionParameterKind::POSITIONAL_ONLY;
        signature.AddParameter("positions", bigint_list, positional)
            .AddParameter("term_ids", bigint_list, positional)
            .AddParameter("slot_count", bigint, positional)
            .AddParameter("width", bigint, positional)
            .SetReturnType(bigint);
      });
  extension.Register(function);
}

} // namespace duckdb
