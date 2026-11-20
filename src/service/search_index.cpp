#include "umbra/search_index.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace umbra {
namespace {

/// Common US street-name abbreviations, mapped to the full words used in OSM.
const std::unordered_map<std::string, std::string>& abbreviations() {
  static const std::unordered_map<std::string, std::string> kMap{
      {"st", "street"},    {"ave", "avenue"},     {"av", "avenue"},     {"dr", "drive"},
      {"rd", "road"},      {"blvd", "boulevard"}, {"pkwy", "parkway"},  {"ln", "lane"},
      {"ct", "court"},     {"pl", "place"},       {"hwy", "highway"},   {"fwy", "freeway"},
      {"cir", "circle"},   {"ter", "terrace"},    {"sq", "square"},     {"mt", "mount"},
      {"n", "north"},      {"s", "south"},        {"e", "east"},        {"w", "west"},
      {"ne", "northeast"}, {"nw", "northwest"},   {"se", "southeast"},  {"sw", "southwest"},
      {"univ", "university"}, {"ctr", "center"},  {"intl", "international"},
  };
  return kMap;
}

bool is_stop_word(const std::string& token) {
  return token == "and" || token == "at" || token == "the" || token == "of";
}

bool is_directional(const std::string& token) {
  return token == "north" || token == "south" || token == "east" || token == "west";
}

/// Levenshtein distance, stopping early once it exceeds `limit`.
std::size_t edit_distance(std::string_view a, std::string_view b, std::size_t limit) {
  if (a.size() > b.size() + limit || b.size() > a.size() + limit) return limit + 1;
  std::vector<std::size_t> prev(b.size() + 1);
  std::vector<std::size_t> cur(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
  for (std::size_t i = 1; i <= a.size(); ++i) {
    cur[0] = i;
    std::size_t row_min = cur[0];
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const std::size_t substitution = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, substitution});
      row_min = std::min(row_min, cur[j]);
    }
    if (row_min > limit) return limit + 1;
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

}  // namespace

// ----------------------------------------------------------------------------
// Tokenisation
// ----------------------------------------------------------------------------

std::vector<std::string> SearchIndex::tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  std::string current;
  auto flush = [&] {
    if (current.empty()) return;
    if (auto it = abbreviations().find(current); it != abbreviations().end()) current = it->second;
    if (!is_stop_word(current)) tokens.push_back(current);
    current.clear();
  };

  for (char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    if (std::isalnum(c)) {
      current.push_back(static_cast<char>(std::tolower(c)));
    } else if (c == '\'' || c == '.') {
      // "McDonald's" -> "mcdonalds", "St." -> "st"
    } else {
      flush();
    }
  }
  flush();
  return tokens;
}

// ----------------------------------------------------------------------------
// Building
// ----------------------------------------------------------------------------

void SearchIndex::add(SearchEntry entry) {
  if (built_) throw std::logic_error("SearchIndex::add after build()");
  std::vector<std::string> tokens = tokenize(entry.name);
  name_token_count_.push_back(static_cast<std::uint32_t>(tokens.size()));
  for (std::string& keyword : tokenize(entry.keywords)) tokens.push_back(std::move(keyword));
  pending_tokens_.push_back(std::move(tokens));
  entries_.push_back(std::move(entry));
}

void SearchIndex::build() {
  // Sorted, de-duplicated vocabulary of every token in every entry.
  for (const auto& tokens : pending_tokens_) {
    vocabulary_.insert(vocabulary_.end(), tokens.begin(), tokens.end());
  }
  std::sort(vocabulary_.begin(), vocabulary_.end());
  vocabulary_.erase(std::unique(vocabulary_.begin(), vocabulary_.end()), vocabulary_.end());

  auto token_id = [this](const std::string& token) {
    return static_cast<std::uint32_t>(std::lower_bound(vocabulary_.begin(), vocabulary_.end(), token) -
                                      vocabulary_.begin());
  };

  postings_.assign(vocabulary_.size(), {});
  entry_tokens_.resize(entries_.size());
  for (std::uint32_t e = 0; e < entries_.size(); ++e) {
    for (const std::string& token : pending_tokens_[e]) {
      const std::uint32_t id = token_id(token);
      entry_tokens_[e].push_back(id);
      // Entries are visited in increasing order, so each posting list stays sorted.
      if (postings_[id].empty() || postings_[id].back() != e) postings_[id].push_back(e);
    }
  }
  pending_tokens_.clear();
  pending_tokens_.shrink_to_fit();
  built_ = true;
}

// ----------------------------------------------------------------------------
// Querying
// ----------------------------------------------------------------------------

std::vector<std::pair<std::uint32_t, SearchIndex::MatchQuality>> SearchIndex::match_token(
    const std::string& token) const {
  std::vector<std::pair<std::uint32_t, MatchQuality>> matches;

  // Prefix range [lower_bound(token), first word not starting with token).
  auto it = std::lower_bound(vocabulary_.begin(), vocabulary_.end(), token);
  for (; it != vocabulary_.end() && it->compare(0, token.size(), token) == 0; ++it) {
    const auto id = static_cast<std::uint32_t>(it - vocabulary_.begin());
    matches.emplace_back(id, it->size() == token.size() ? MatchQuality::kExact : MatchQuality::kPrefix);
  }
  if (!matches.empty() || token.size() < 4) return matches;

  // Typo tolerance: the token is within one edit of a word, or of a word's prefix
  // (the user may still be typing).
  for (std::uint32_t id = 0; id < vocabulary_.size(); ++id) {
    const std::string& word = vocabulary_[id];
    for (std::size_t len : {token.size() - 1, token.size(), token.size() + 1}) {
      if (len > word.size()) continue;
      if (edit_distance(token, std::string_view(word).substr(0, len), 1) <= 1) {
        matches.emplace_back(id, MatchQuality::kFuzzy);
        break;
      }
    }
  }
  return matches;
}

std::vector<SearchHit> SearchIndex::search(std::string_view query, std::size_t limit) const {
  if (!built_) throw std::logic_error("SearchIndex::search before build()");
  const std::vector<std::string> tokens = tokenize(query);
  if (tokens.empty() || limit == 0) return {};

  // For each query token: candidate entries and the best match quality per entry.
  std::vector<std::unordered_map<std::uint32_t, MatchQuality>> per_token;
  per_token.reserve(tokens.size());
  for (const std::string& token : tokens) {
    std::unordered_map<std::uint32_t, MatchQuality> best;
    for (const auto& [vocab_id, quality] : match_token(token)) {
      for (std::uint32_t e : postings_[vocab_id]) {
        auto& q = best[e];
        q = std::max(q, quality);
      }
    }
    if (best.empty()) return {};  // AND semantics: one unmatched token -> no results
    per_token.push_back(std::move(best));
  }

  // Intersect, starting from the smallest candidate set.
  std::size_t smallest = 0;
  for (std::size_t i = 1; i < per_token.size(); ++i) {
    if (per_token[i].size() < per_token[smallest].size()) smallest = i;
  }

  std::vector<SearchHit> hits;
  for (const auto& [e, unused] : per_token[smallest]) {
    double score = 0.0;
    bool all = true;
    for (const auto& candidates : per_token) {
      auto it = candidates.find(e);
      if (it == candidates.end()) {
        all = false;
        break;
      }
      score += static_cast<double>(it->second);  // exact 3, prefix 2, fuzzy 1
    }
    if (!all) continue;

    // Prefer names whose leading word (ignoring "East"/"West"...) matches the
    // first query word: "mill" ranks "South Mill Avenue" above "Mill Ave & 5th".
    const auto& entry_tokens = entry_tokens_[e];
    for (std::uint32_t id : entry_tokens) {
      if (is_directional(vocabulary_[id])) continue;
      if (vocabulary_[id].compare(0, tokens[0].size(), tokens[0]) == 0) score += 1.5;
      break;
    }

    // Specificity: penalise words in the name that the user did not ask for.
    const double extra = static_cast<double>(name_token_count_[e]) - static_cast<double>(tokens.size());
    score -= 0.25 * std::max(0.0, extra);
    score += entries_[e].importance;
    hits.push_back({e, score});
  }

  std::sort(hits.begin(), hits.end(), [this](const SearchHit& a, const SearchHit& b) {
    if (a.score != b.score) return a.score > b.score;
    const auto& ea = entries_[a.entry];
    const auto& eb = entries_[b.entry];
    if (ea.name.size() != eb.name.size()) return ea.name.size() < eb.name.size();
    return ea.name < eb.name;
  });

  // Drop exact duplicates (same name and subtitle), then truncate.
  std::vector<SearchHit> unique;
  std::unordered_set<std::string> seen;
  for (const SearchHit& hit : hits) {
    const auto& entry = entries_[hit.entry];
    if (!seen.insert(entry.name + '\x1f' + entry.subtitle).second) continue;
    unique.push_back(hit);
    if (unique.size() == limit) break;
  }
  return unique;
}

}  // namespace umbra
