#include "umbra/user_store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

#include <sodium.h>
#include <sqlite3.h>

namespace umbra {
namespace {

constexpr int kSchemaVersion = 1;

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS users (
  id            INTEGER PRIMARY KEY,
  email         TEXT    NOT NULL UNIQUE COLLATE NOCASE,
  password_hash TEXT    NOT NULL,
  display_name  TEXT    NOT NULL,
  created_at    INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT    PRIMARY KEY,
  user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  expires_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);
CREATE TABLE IF NOT EXISTS places (
  id         INTEGER PRIMARY KEY,
  user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  name       TEXT    NOT NULL,
  lat        REAL    NOT NULL,
  lng        REAL    NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS places_user ON places(user_id);
CREATE TABLE IF NOT EXISTS trips (
  id         INTEGER PRIMARY KEY,
  user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  mode       TEXT    NOT NULL,
  from_name  TEXT    NOT NULL,
  from_lat   REAL    NOT NULL,
  from_lng   REAL    NOT NULL,
  to_name    TEXT    NOT NULL,
  to_lat     REAL    NOT NULL,
  to_lng     REAL    NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS trips_user ON trips(user_id, created_at);
)sql";

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/// A prepared statement that finalizes itself.
class Statement {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      throw StoreError(std::string("SQL prepare failed: ") + sqlite3_errmsg(db));
    }
  }
  ~Statement() { sqlite3_finalize(stmt_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  Statement& bind(int i, std::int64_t v) { return check(sqlite3_bind_int64(stmt_, i, v)); }
  Statement& bind(int i, double v) { return check(sqlite3_bind_double(stmt_, i, v)); }
  Statement& bind(int i, const std::string& v) {
    return check(sqlite3_bind_text(stmt_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
  }

  /// Advance; true while there is a row.
  bool step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw StoreError(std::string("SQL step failed: ") + sqlite3_errmsg(db_));
  }

  [[nodiscard]] std::int64_t int64(int col) const { return sqlite3_column_int64(stmt_, col); }
  [[nodiscard]] double real(int col) const { return sqlite3_column_double(stmt_, col); }
  [[nodiscard]] std::string text(int col) const {
    const auto* p = sqlite3_column_text(stmt_, col);
    return p ? std::string(reinterpret_cast<const char*>(p)) : std::string{};
  }

 private:
  Statement& check(int rc) {
    if (rc != SQLITE_OK) throw StoreError(std::string("SQL bind failed: ") + sqlite3_errmsg(db_));
    return *this;
  }
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

void exec(sqlite3* db, const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error ? error : "unknown error";
    sqlite3_free(error);
    throw StoreError("SQL failed: " + message);
  }
}

std::string trim(const std::string& s) {
  const auto begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}

/// Deliberately simple: one '@', something before it, a dot in the domain, no spaces.
bool plausible_email(const std::string& email) {
  if (email.size() < 3 || email.size() > 254) return false;
  const auto at = email.find('@');
  if (at == std::string::npos || at == 0 || email.find('@', at + 1) != std::string::npos) return false;
  const auto dot = email.find('.', at);
  if (dot == std::string::npos || dot == at + 1 || dot + 1 >= email.size()) return false;
  return std::none_of(email.begin(), email.end(), [](unsigned char c) { return std::isspace(c) || c < 32; });
}

std::string clean_name(const std::string& raw, const char* what) {
  std::string name = trim(raw);
  if (name.empty()) throw AccountError(std::string(what) + " is required");
  if (name.size() > UserStore::kMaxNameLength) throw AccountError(std::string(what) + " is too long");
  if (std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; })) {
    throw AccountError(std::string(what) + " contains control characters");
  }
  return name;
}

void check_position(const LatLng& p) {
  if (!std::isfinite(p.lat) || !std::isfinite(p.lng) || std::abs(p.lat) > 90 || std::abs(p.lng) > 180) {
    throw AccountError("Coordinates are out of range");
  }
}

std::string hash_password(const std::string& password) {
  char out[crypto_pwhash_STRBYTES];
  if (crypto_pwhash_str(out, password.data(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                        crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
    throw StoreError("Password hashing ran out of memory");
  }
  return out;
}

bool verify_password(const std::string& hash, const std::string& password) {
  return crypto_pwhash_str_verify(hash.c_str(), password.data(), password.size()) == 0;
}

/// A valid hash of a random password, verified when the email is unknown so
/// that the response takes as long as for a real account.
const std::string& dummy_hash() {
  static const std::string kHash = [] {
    unsigned char random[32];
    randombytes_buf(random, sizeof random);
    return hash_password(std::string(reinterpret_cast<const char*>(random), sizeof random));
  }();
  return kHash;
}

std::string hex(const unsigned char* data, std::size_t size) {
  std::string out(size * 2 + 1, '\0');
  sodium_bin2hex(out.data(), out.size(), data, size);
  out.pop_back();
  return out;
}

std::string token_hash(const std::string& token) {
  unsigned char digest[crypto_generichash_BYTES];
  crypto_generichash(digest, sizeof digest, reinterpret_cast<const unsigned char*>(token.data()), token.size(),
                     nullptr, 0);
  return hex(digest, sizeof digest);
}

}  // namespace

UserStore::UserStore(const std::string& path) {
  if (sodium_init() < 0) throw StoreError("libsodium failed to initialise");
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
    const std::string message = db_ ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    throw StoreError("Cannot open database " + path + ": " + message);
  }
  sqlite3_busy_timeout(db_, 5000);
  exec(db_, "PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;");

  Statement version(db_, "PRAGMA user_version");
  version.step();
  const auto current = version.int64(0);
  if (current > kSchemaVersion) throw StoreError("Database was created by a newer Umbra version");
  exec(db_, kSchema);
  exec(db_, ("PRAGMA user_version = " + std::to_string(kSchemaVersion)).c_str());
}

UserStore::~UserStore() { sqlite3_close(db_); }

User UserStore::create_user(const std::string& raw_email, const std::string& password,
                            const std::string& display_name) {
  const std::string email = trim(raw_email);
  if (!plausible_email(email)) throw AccountError("Enter a valid email address");
  if (password.size() < kMinPasswordLength) {
    throw AccountError("Password must be at least " + std::to_string(kMinPasswordLength) + " characters");
  }
  if (password.size() > kMaxPasswordLength) throw AccountError("Password is too long");
  const std::string name = clean_name(display_name, "Name");
  const std::string hash = hash_password(password);  // slow: outside the lock

  const std::lock_guard lock(mutex_);
  Statement exists(db_, "SELECT 1 FROM users WHERE email = ?");
  exists.bind(1, email);
  if (exists.step()) throw AccountError("An account with this email already exists");

  User user{0, email, name, now_seconds()};
  Statement insert(db_, "INSERT INTO users (email, password_hash, display_name, created_at) VALUES (?, ?, ?, ?)");
  insert.bind(1, email).bind(2, hash).bind(3, name).bind(4, user.created_at);
  insert.step();
  user.id = sqlite3_last_insert_rowid(db_);
  return user;
}

Session UserStore::login(const std::string& raw_email, const std::string& password) {
  const std::string email = trim(raw_email);
  std::optional<User> user;
  std::string hash = dummy_hash();
  {
    const std::lock_guard lock(mutex_);
    Statement find(db_, "SELECT id, email, display_name, created_at, password_hash FROM users WHERE email = ?");
    find.bind(1, email);
    if (find.step()) {
      user = User{find.int64(0), find.text(1), find.text(2), find.int64(3)};
      hash = find.text(4);
    }
  }
  const bool ok = verify_password(hash, password.substr(0, kMaxPasswordLength));  // always runs: constant timing
  if (!user || !ok) throw AccountError("Wrong email or password");

  unsigned char random[32];
  randombytes_buf(random, sizeof random);
  Session session{hex(random, sizeof random), *user, now_seconds() + kSessionSeconds};

  const std::lock_guard lock(mutex_);
  exec(db_, ("DELETE FROM sessions WHERE expires_at < " + std::to_string(now_seconds())).c_str());
  Statement insert(db_, "INSERT INTO sessions (token_hash, user_id, expires_at) VALUES (?, ?, ?)");
  insert.bind(1, token_hash(session.token)).bind(2, user->id).bind(3, session.expires_at);
  insert.step();
  return session;
}

std::optional<User> UserStore::user_for_token(const std::string& token) const {
  if (token.size() != 64) return std::nullopt;
  const std::lock_guard lock(mutex_);
  Statement find(db_,
                 "SELECT u.id, u.email, u.display_name, u.created_at FROM sessions s "
                 "JOIN users u ON u.id = s.user_id WHERE s.token_hash = ? AND s.expires_at > ?");
  find.bind(1, token_hash(token)).bind(2, now_seconds());
  if (!find.step()) return std::nullopt;
  return User{find.int64(0), find.text(1), find.text(2), find.int64(3)};
}

void UserStore::logout(const std::string& token) {
  const std::lock_guard lock(mutex_);
  Statement remove(db_, "DELETE FROM sessions WHERE token_hash = ?");
  remove.bind(1, token_hash(token));
  remove.step();
}

void UserStore::delete_user(std::int64_t user_id) {
  const std::lock_guard lock(mutex_);
  Statement remove(db_, "DELETE FROM users WHERE id = ?");
  remove.bind(1, user_id);
  remove.step();
}

std::vector<SavedPlace> UserStore::places(std::int64_t user_id) const {
  const std::lock_guard lock(mutex_);
  Statement find(db_, "SELECT id, name, lat, lng, created_at FROM places WHERE user_id = ? ORDER BY name");
  find.bind(1, user_id);
  std::vector<SavedPlace> out;
  while (find.step()) out.push_back({find.int64(0), find.text(1), {find.real(2), find.real(3)}, find.int64(4)});
  return out;
}

SavedPlace UserStore::add_place(std::int64_t user_id, const std::string& raw_name, const LatLng& position) {
  const std::string name = clean_name(raw_name, "Place name");
  check_position(position);
  const std::lock_guard lock(mutex_);
  Statement count(db_, "SELECT COUNT(*) FROM places WHERE user_id = ?");
  count.bind(1, user_id);
  count.step();
  if (static_cast<std::size_t>(count.int64(0)) >= kMaxPlaces) {
    throw AccountError("You can save up to " + std::to_string(kMaxPlaces) + " places");
  }
  SavedPlace place{0, name, position, now_seconds()};
  Statement insert(db_, "INSERT INTO places (user_id, name, lat, lng, created_at) VALUES (?, ?, ?, ?, ?)");
  insert.bind(1, user_id).bind(2, name).bind(3, position.lat).bind(4, position.lng).bind(5, place.created_at);
  insert.step();
  place.id = sqlite3_last_insert_rowid(db_);
  return place;
}

void UserStore::delete_place(std::int64_t user_id, std::int64_t place_id) {
  const std::lock_guard lock(mutex_);
  Statement remove(db_, "DELETE FROM places WHERE id = ? AND user_id = ?");  // only your own places
  remove.bind(1, place_id).bind(2, user_id);
  remove.step();
}

std::vector<TripRecord> UserStore::trips(std::int64_t user_id) const {
  const std::lock_guard lock(mutex_);
  Statement find(db_,
                 "SELECT id, mode, from_name, from_lat, from_lng, to_name, to_lat, to_lng, created_at FROM trips "
                 "WHERE user_id = ? ORDER BY created_at DESC, id DESC");
  find.bind(1, user_id);
  std::vector<TripRecord> out;
  while (find.step()) {
    out.push_back({find.int64(0), find.text(1), find.text(2), {find.real(3), find.real(4)}, find.text(5),
                   {find.real(6), find.real(7)}, find.int64(8)});
  }
  return out;
}

void UserStore::add_trip(std::int64_t user_id, const TripRecord& trip) {
  if (trip.mode != "drive" && trip.mode != "walk") throw AccountError("Trip mode must be 'drive' or 'walk'");
  check_position(trip.from);
  check_position(trip.to);
  const std::string from_name = trip.from_name.empty() ? "Map point" : clean_name(trip.from_name, "Start name");
  const std::string to_name = trip.to_name.empty() ? "Map point" : clean_name(trip.to_name, "Destination name");

  const std::lock_guard lock(mutex_);
  Statement insert(db_,
                   "INSERT INTO trips (user_id, mode, from_name, from_lat, from_lng, to_name, to_lat, to_lng, "
                   "created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
  insert.bind(1, user_id).bind(2, trip.mode).bind(3, from_name).bind(4, trip.from.lat).bind(5, trip.from.lng);
  insert.bind(6, to_name).bind(7, trip.to.lat).bind(8, trip.to.lng).bind(9, now_seconds());
  insert.step();
  // Keep only the most recent trips.
  Statement prune(db_,
                  "DELETE FROM trips WHERE user_id = ?1 AND id NOT IN "
                  "(SELECT id FROM trips WHERE user_id = ?1 ORDER BY created_at DESC, id DESC LIMIT ?2)");
  prune.bind(1, user_id).bind(2, static_cast<std::int64_t>(kMaxTrips));
  prune.step();
}

void UserStore::clear_trips(std::int64_t user_id) {
  const std::lock_guard lock(mutex_);
  Statement remove(db_, "DELETE FROM trips WHERE user_id = ?");
  remove.bind(1, user_id);
  remove.step();
}

}  // namespace umbra
