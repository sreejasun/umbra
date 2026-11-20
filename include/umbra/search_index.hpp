#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "umbra/geo.hpp"

namespace umbra {

/// What a search result refers to.
enum class EntryKind : std::uint8_t { kPlace, kBuilding, kStreet, kIntersection };

/// A searchable item: a named place, a campus building, a street or an intersection.
struct SearchEntry {
  SearchEntry() = default;
  SearchEntry(EntryKind kind_, std::string name_, std::string subtitle_, std::string keywords_, LatLng position_,
              double importance_)
      : kind(kind_),
        name(std::move(name_)),
        subtitle(std::move(subtitle_)),
        keywords(std::move(keywords_)),
        position(position_),
        importance(importance_) {}

  EntryKind kind = EntryKind::kPlace;
  std::string name;      ///< display name, e.g. "Mill Avenue & 5th Street"
  std::string subtitle;  ///< e.g. "University · near Mill Avenue & 5th Street"
  std::string keywords;  ///< extra searchable text that is not displayed (e.g. "ASU")
  LatLng position;
  double importance = 0.0;  ///< ranking boost (bigger landmarks rank higher)
  std::string code;         ///< building code, e.g. "COOR" (buildings only)
  std::vector<LatLng> access_points;  ///< entrances or outline points to route to (buildings only)
  bool entrances_mapped = false;
};

struct SearchHit {
  std::uint32_t entry = 0;  ///< index into SearchIndex::entry()
  double score = 0.0;
};

/**
 * Autocomplete search over place, street and intersection names.
 *
 * Indexing: every name is normalised into tokens (lower-case, punctuation
 * removed, "St" -> "street", "N" -> "north", ...). A sorted vocabulary of all
 * distinct tokens maps to posting lists of entry ids (an inverted index).
 *
 * Querying: each query token is matched against the vocabulary
 *   - as a prefix, via binary search for the first token >= query token
 *     (so "univ" finds "university" while the user is still typing), and
 *   - if nothing matches, with typo tolerance (edit distance <= 1).
 * Posting lists of all query tokens are intersected (AND semantics) and the
 * survivors are ranked by match quality, name specificity and importance.
 *
 *   build: O(T log T) for T tokens   query: O(q log V + matches) typical
 */
class SearchIndex {
 public:
  /// Add an entry. Must be called before build().
  void add(SearchEntry entry);

  /// Freeze the index. Must be called before search().
  void build();

  [[nodiscard]] std::vector<SearchHit> search(std::string_view query, std::size_t limit = 8) const;

  [[nodiscard]] const SearchEntry& entry(std::uint32_t id) const { return entries_.at(id); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t vocabulary_size() const noexcept { return vocabulary_.size(); }

  /// Normalise text into search tokens (exposed for testing).
  [[nodiscard]] static std::vector<std::string> tokenize(std::string_view text);

 private:
  enum class MatchQuality : std::uint8_t { kNone = 0, kFuzzy = 1, kPrefix = 2, kExact = 3 };

  /// Vocabulary ids matching one query token, with their match quality.
  [[nodiscard]] std::vector<std::pair<std::uint32_t, MatchQuality>> match_token(
      const std::string& token) const;

  std::vector<SearchEntry> entries_;
  std::vector<std::vector<std::uint32_t>> entry_tokens_;  // entry -> vocabulary ids
  std::vector<std::uint32_t> name_token_count_;           // tokens from the display name
  std::vector<std::string> vocabulary_;                    // sorted, unique
  std::vector<std::vector<std::uint32_t>> postings_;       // vocabulary id -> sorted entry ids
  std::vector<std::vector<std::string>> pending_tokens_;   // tokens collected before build()
  bool built_ = false;
};

}  // namespace umbra
