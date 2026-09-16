#include "camera_agent/backoff.h"
#include "test_harness.h"

TEST(backoff_sequence_and_cap) {
    ca::BackoffScheduler b({1, 2, 5, 10});
    ASSERT_EQ(b.next(), 1);
    ASSERT_EQ(b.next(), 2);
    ASSERT_EQ(b.next(), 5);
    ASSERT_EQ(b.next(), 10);
    ASSERT_EQ(b.next(), 10); // capped at last entry
    ASSERT_EQ(b.next(), 10);
    b.reset();
    ASSERT_EQ(b.next(), 1); // restarts
    return true;
}
