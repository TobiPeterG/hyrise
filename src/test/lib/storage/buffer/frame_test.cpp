#include "base_test.hpp"

#include <thread>
#include <vector>

#include "storage/buffer/frame.hpp"
#include "storage/buffer/helper.hpp"

namespace hyrise {

class FrameTest : public BaseTest {};

TEST_F(FrameTest, TestInitialStateIsEvicted) {
  Frame frame;

  const auto sv = frame.state_and_version();

  EXPECT_EQ(Frame::state(sv), Frame::EVICTED) << "Frame should start as EVICTED";
  EXPECT_EQ(Frame::version(sv), 0u) << "Fresh frame should start with version 0";

  // Dirty must be false initially
  EXPECT_FALSE(frame.is_dirty());

  EXPECT_EQ(frame.node_id(), Frame::node_id(sv));
}

TEST_F(FrameTest, TestExclusiveLockUnlockIncrementsVersion) {
  Frame frame;

  const auto before = frame.state_and_version();
  ASSERT_EQ(Frame::state(before), Frame::EVICTED);

  // Lock exclusive from EVICTED should succeed.
  auto expected = before;
  ASSERT_TRUE(frame.try_lock_exclusive(expected));

  const auto locked = frame.state_and_version();
  EXPECT_EQ(Frame::state(locked), Frame::LOCKED);

  // Unlock exclusive should set UNLOCKED and increment version by 1.
  frame.unlock_exclusive();

  const auto after = frame.state_and_version();
  EXPECT_EQ(Frame::state(after), Frame::UNLOCKED);
  EXPECT_EQ(Frame::version(after), Frame::version(locked) + 1);
}

TEST_F(FrameTest, TestUnlockExclusiveAndSetEvictedIncrementsVersion) {
  Frame frame;

  const auto before = frame.state_and_version();
  auto expected = before;
  ASSERT_TRUE(frame.try_lock_exclusive(expected));

  const auto locked = frame.state_and_version();
  ASSERT_EQ(Frame::state(locked), Frame::LOCKED);

  frame.unlock_exclusive_and_set_evicted();

  const auto after = frame.state_and_version();
  EXPECT_EQ(Frame::state(after), Frame::EVICTED);
  EXPECT_EQ(Frame::version(after), Frame::version(locked) + 1);
}

TEST_F(FrameTest, TestSharedLockCountingAndFinalUnlockSignal) {
  Frame frame;

  // Move to UNLOCKED first
  auto expected = frame.state_and_version();
  ASSERT_TRUE(frame.try_lock_exclusive(expected));
  frame.unlock_exclusive();

  ASSERT_TRUE(frame.is_unlocked());

  // Acquire 3 shared locks
  for (int i = 0; i < 3; ++i) {
    auto sv = frame.state_and_version();
    ASSERT_TRUE(frame.try_lock_shared(sv));
  }

  const auto sv_locked = frame.state_and_version();
  EXPECT_EQ(Frame::state(sv_locked), 3u) << "Shared lock count should equal number of holders";

  // Release 2 locks -> should return false (still locked)
  EXPECT_FALSE(frame.unlock_shared());
  EXPECT_FALSE(frame.unlock_shared());

  // Release final lock -> should return true (now unlocked)
  EXPECT_TRUE(frame.unlock_shared());
  EXPECT_TRUE(frame.is_unlocked());
}

TEST_F(FrameTest, TestMarkThenSharedLockFromMarked) {
  Frame frame;

  // Move to UNLOCKED
  auto expected = frame.state_and_version();
  ASSERT_TRUE(frame.try_lock_exclusive(expected));
  frame.unlock_exclusive();
  ASSERT_TRUE(frame.is_unlocked());

  // Mark
  auto sv = frame.state_and_version();
  ASSERT_TRUE(frame.try_mark(sv));

  const auto marked = frame.state_and_version();
  EXPECT_EQ(Frame::state(marked), Frame::MARKED);

  auto sv2 = frame.state_and_version();
  ASSERT_TRUE(frame.try_lock_shared(sv2));

  const auto shared1 = frame.state_and_version();
  EXPECT_EQ(Frame::state(shared1), 1u);

  // Releasing should bring it back to UNLOCKED
  EXPECT_TRUE(frame.unlock_shared());
  EXPECT_TRUE(frame.is_unlocked());
}

TEST_F(FrameTest, TestDirtyFlagSetAndReset) {
  Frame frame;
  EXPECT_FALSE(frame.is_dirty());

  frame.set_dirty(true);
  EXPECT_TRUE(frame.is_dirty());

  frame.reset_dirty();
  EXPECT_FALSE(frame.is_dirty());

  // Setting false should keep it false
  frame.set_dirty(false);
  EXPECT_FALSE(frame.is_dirty());
}

TEST_F(FrameTest, TestSetNodeIdEncodesIntoStateWord) {
  Frame frame;

  // Lock exclusive first
  auto expected = frame.state_and_version();
  ASSERT_TRUE(frame.try_lock_exclusive(expected));

  const NodeID node{3};
  frame.set_node_id(node);

  EXPECT_EQ(frame.node_id(), node);
  EXPECT_EQ(Frame::node_id(frame.state_and_version()), node);

  // Unlocking should not change node id bits.
  frame.unlock_exclusive();
  EXPECT_EQ(frame.node_id(), node);
}

TEST_F(FrameTest, TestConcurrentSharedLockingDoesNotCorruptState) {
  Frame frame;

  // Move to UNLOCKED
  auto expected = frame.state_and_version();
  ASSERT_TRUE(frame.try_lock_exclusive(expected));
  frame.unlock_exclusive();

  constexpr int threads = 8;
  constexpr int iters = 2000;

  auto worker = [&]() {
    for (int i = 0; i < iters; ++i) {
      while (true) {
        auto sv = frame.state_and_version();
        if (frame.try_lock_shared(sv)) break;
        // If it fails, retry
        std::this_thread::yield();
      }
      const auto now_unlocked = frame.unlock_shared();
      (void)now_unlocked;
    }
  };

  std::vector<std::thread> ts;
  ts.reserve(threads);
  for (int i = 0; i < threads; ++i) ts.emplace_back(worker);
  for (auto& t : ts) t.join();

  // After all threads finished, the frame must be unlocked.
  EXPECT_TRUE(frame.is_unlocked());

  // And it must not accidentally end up in a special terminal state.
  const auto sv_end = frame.state_and_version();
  EXPECT_NE(Frame::state(sv_end), Frame::LOCKED);
  EXPECT_NE(Frame::state(sv_end), Frame::EVICTED);
}

}  // namespace hyrise
