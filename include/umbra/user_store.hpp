#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "umbra/geo.hpp"

struct sqlite3;

namespace umbra {

/// The database failed (I/O, corruption, ...).
class StoreError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// A request the user can fix: bad credentials, invalid input, a limit reached.
class AccountError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct User {
  std::int64_t id = 0;
  std::string email;
  std::string display_name;
  std::int64_t created_at = 0;  ///< Unix seconds
};

struct Session {
  std::string token;  ///< secret; only its hash is stored
  User user;
  std::int64_t expires_at = 0;
};

struct SavedPlace {
  std::int64_t id = 0;
  std::string name;  ///< e.g. "Home", "Calc class"
  LatLng position;
  std::int64_t created_at = 0;
};

struct TripRecord {
  std::int64_t id = 0;
  std::string mode;  ///< "drive" or "walk"
  std::string from_name;
  LatLng from;
  std::string to_name;
  LatLng to;
  std::int64_t created_at = 0;
};

/**
 * Accounts, sessions, saved places and trip history in SQLite.
 *
 * - Passwords are hashed with Argon2id (libsodium crypto_pwhash, interactive
 *   limits); plaintext is never stored or logged.
 * - Session tokens are 256 random bits; the database keeps only their BLAKE2b
 *   hash, so a leaked database cannot be used to log in. Sessions last 30 days.
 * - Logging in with an unknown email costs the same time as a wrong password,
 *   so responses do not reveal which emails have accounts.
 * - Deleting a user deletes their sessions, places and trips (ON DELETE CASCADE).
 *
 * Thread-safe: one connection guarded by a mutex; password hashing, the slow
 * part, runs outside the lock.
 */
class UserStore {
 public:
  static constexpr std::size_t kMinPasswordLength = 10;
  static constexpr std::size_t kMaxPasswordLength = 128;
  static constexpr std::size_t kMaxPlaces = 100;
  static constexpr std::size_t kMaxTrips = 50;  ///< older trips are dropped
  static constexpr std::size_t kMaxNameLength = 80;
  static constexpr std::int64_t kSessionSeconds = 30LL * 24 * 3600;

  /// Open (or create) the database at `path`; ":memory:" for tests.
  explicit UserStore(const std::string& path);
  ~UserStore();
  UserStore(const UserStore&) = delete;
  UserStore& operator=(const UserStore&) = delete;

  /// Throws AccountError for an invalid email, a weak password or a taken email.
  User create_user(const std::string& email, const std::string& password, const std::string& display_name);

  /// Throws AccountError("Wrong email or password") on bad credentials.
  Session login(const std::string& email, const std::string& password);

  /// The signed-in user for a session token, or nullopt if unknown or expired.
  [[nodiscard]] std::optional<User> user_for_token(const std::string& token) const;

  void logout(const std::string& token);
  void delete_user(std::int64_t user_id);

  [[nodiscard]] std::vector<SavedPlace> places(std::int64_t user_id) const;
  SavedPlace add_place(std::int64_t user_id, const std::string& name, const LatLng& position);
  void delete_place(std::int64_t user_id, std::int64_t place_id);

  [[nodiscard]] std::vector<TripRecord> trips(std::int64_t user_id) const;
  void add_trip(std::int64_t user_id, const TripRecord& trip);
  void clear_trips(std::int64_t user_id);

 private:
  sqlite3* db_ = nullptr;
  mutable std::mutex mutex_;
};

}  // namespace umbra
