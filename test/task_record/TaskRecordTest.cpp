#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "TaskRecord.h"

namespace {
TaskRecord make(const char* id, const char* title, uint8_t priority, bool done = false) {
  TaskRecord r{};
  std::snprintf(r.id, sizeof(r.id), "%s", id);
  std::snprintf(r.title, sizeof(r.title), "%s", title);
  r.priority = priority;
  r.done = done;
  return r;
}
}  // namespace

TEST(TaskOrder, HighPriorityComesFirst) {
  EXPECT_TRUE(taskOrderBefore(make("w00000001", "haute", 0), make("w00000002", "basse", 2)));
  EXPECT_FALSE(taskOrderBefore(make("w00000002", "basse", 2), make("w00000001", "haute", 0)));
}

TEST(TaskOrder, DoneSinksBelowEveryOpenTask) {
  // Une tâche faite de priorité haute passe quand même après une ouverte de priorité basse.
  EXPECT_TRUE(taskOrderBefore(make("w2", "ouverte basse", 2), make("w1", "faite haute", 0, true)));
}

TEST(TaskOrder, EqualPriorityFallsBackToId) {
  EXPECT_TRUE(taskOrderBefore(make("w00000001", "a", 1), make("w00000002", "b", 1)));
}

TEST(TaskOrder, SortIsStableAndTotal) {
  std::vector<TaskRecord> v{make("w3", "c", 2), make("w1", "a", 0, true), make("w2", "b", 0), make("w4", "d", 1)};
  std::sort(v.begin(), v.end(), taskOrderBefore);
  EXPECT_STREQ(v[0].id, "w2");
  EXPECT_STREQ(v[1].id, "w4");
  EXPECT_STREQ(v[2].id, "w3");
  EXPECT_STREQ(v[3].id, "w1");
}

TEST(TaskRecord, TitleIsBoundedAndNullTerminated) {
  TaskRecord r{};
  const std::string longTitle(400, 'x');
  std::snprintf(r.title, sizeof(r.title), "%s", longTitle.c_str());
  EXPECT_EQ(std::strlen(r.title), 200u);
}
