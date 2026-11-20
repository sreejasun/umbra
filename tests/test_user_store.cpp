#include <gtest/gtest.h>

#include "umbra/user_store.hpp"

namespace umbra {
namespace {

constexpr const char* kPassword = "correct horse battery";

class UserStoreTest : public ::testing::Test {
 protected:
  UserStore store{":memory:"};
};

TEST_F(UserStoreTest, RegistersAndLogsIn) {
  const User user = store.create_user("  Sparky@Example.com ", kPassword, "Sparky");
  EXPECT_GT(user.id, 0);
  EXPECT_EQ(user.email, "Sparky@Example.com");

  const Session session = store.login("sparky@example.com", kPassword);  // emails are case-insensitive
  EXPECT_EQ(session.user.id, user.id);
  EXPECT_EQ(session.token.size(), 64U);  // 256 random bits, hex
  ASSERT_TRUE(store.user_for_token(session.token));
  EXPECT_EQ(store.user_for_token(session.token)->display_name, "Sparky");
}

TEST_F(UserStoreTest, RejectsBadSignUps) {
  EXPECT_THROW((void)store.create_user("not-an-email", kPassword, "A"), AccountError);
  EXPECT_THROW((void)store.create_user("a@b.co", "short", "A"), AccountError);
  EXPECT_THROW((void)store.create_user("a@b.co", kPassword, "   "), AccountError);
  (void)store.create_user("a@b.co", kPassword, "A");
  EXPECT_THROW((void)store.create_user("A@B.CO", kPassword, "A"), AccountError) << "duplicate email";
}

TEST_F(UserStoreTest, WrongPasswordAndUnknownEmailLookTheSame) {
  (void)store.create_user("a@b.co", kPassword, "A");
  std::string wrong_password;
  std::string unknown_email;
  try {
    (void)store.login("a@b.co", "wrong password!");
  } catch (const AccountError& e) {
    wrong_password = e.what();
  }
  try {
    (void)store.login("nobody@b.co", kPassword);
  } catch (const AccountError& e) {
    unknown_email = e.what();
  }
  EXPECT_FALSE(wrong_password.empty());
  EXPECT_EQ(wrong_password, unknown_email);
}

TEST_F(UserStoreTest, LogoutAndForgedTokens) {
  (void)store.create_user("a@b.co", kPassword, "A");
  const Session s = store.login("a@b.co", kPassword);
  EXPECT_FALSE(store.user_for_token(std::string(64, '0')));
  EXPECT_FALSE(store.user_for_token("short"));
  store.logout(s.token);
  EXPECT_FALSE(store.user_for_token(s.token));
}

TEST_F(UserStoreTest, PlacesBelongToTheirOwner) {
  const User a = store.create_user("a@b.co", kPassword, "A");
  const User b = store.create_user("b@b.co", kPassword, "B");
  const SavedPlace home = store.add_place(a.id, "Home", {33.42, -111.93});
  store.delete_place(b.id, home.id);  // someone else's id: no effect
  ASSERT_EQ(store.places(a.id).size(), 1U);
  EXPECT_TRUE(store.places(b.id).empty());
  EXPECT_THROW((void)store.add_place(a.id, "Bad", {123.0, 0.0}), AccountError);
  store.delete_place(a.id, home.id);
  EXPECT_TRUE(store.places(a.id).empty());
}

TEST_F(UserStoreTest, KeepsOnlyRecentTrips) {
  const User a = store.create_user("a@b.co", kPassword, "A");
  for (std::size_t i = 0; i < UserStore::kMaxTrips + 5; ++i) {
    store.add_trip(a.id, {0, "walk", "MU", {33.4176, -111.9346}, "Library " + std::to_string(i), {33.419, -111.934}, 0});
  }
  const auto trips = store.trips(a.id);
  ASSERT_EQ(trips.size(), UserStore::kMaxTrips);
  EXPECT_EQ(trips.front().to_name, "Library " + std::to_string(UserStore::kMaxTrips + 4)) << "newest first";
  EXPECT_THROW(store.add_trip(a.id, {0, "fly", "", {0, 0}, "", {0, 0}, 0}), AccountError);
  store.clear_trips(a.id);
  EXPECT_TRUE(store.trips(a.id).empty());
}

TEST_F(UserStoreTest, DeletingAUserDeletesTheirData) {
  const User a = store.create_user("a@b.co", kPassword, "A");
  const Session s = store.login("a@b.co", kPassword);
  (void)store.add_place(a.id, "Home", {33.42, -111.93});
  store.add_trip(a.id, {0, "drive", "", {33.42, -111.93}, "", {33.43, -111.90}, 0});
  store.delete_user(a.id);
  EXPECT_FALSE(store.user_for_token(s.token));
  EXPECT_TRUE(store.places(a.id).empty());
  EXPECT_TRUE(store.trips(a.id).empty());
  EXPECT_THROW((void)store.login("a@b.co", kPassword), AccountError);
}

}  // namespace
}  // namespace umbra
