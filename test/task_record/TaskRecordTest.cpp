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
  r.order = TASK_ORDER_UNSET;
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

TEST(TaskOrder, ManualOrderRanksWithinAPriorityButNeverAcrossOne) {
  TaskRecord first = make("w00000009", "rang 1", 1);
  first.order = 1;
  TaskRecord second = make("w00000001", "rang 2", 1);
  second.order = 2;
  EXPECT_TRUE(taskOrderBefore(first, second));  // le rang passe avant l'id
  TaskRecord unset = make("w00000000", "pas classee", 1);
  EXPECT_TRUE(taskOrderBefore(second, unset));  // non classee : au bout du groupe
  TaskRecord high = make("w00000005", "haute", 0);
  high.order = 50;
  EXPECT_TRUE(taskOrderBefore(high, first));  // la priorite reste le premier critere
}

TEST(TaskOrder, EqualPriorityFallsBackToId) {
  EXPECT_TRUE(taskOrderBefore(make("w00000001", "a", 1), make("w00000002", "b", 1)));
}

TEST(TaskOrder, SortIsStableAndTotal) {
  std::vector<TaskRecord> v{make("w3", "c", 2), make("w1", "a", 0, true), make("w2", "b", 0), make("w4", "d", 1)};
  std::sort(v.begin(), v.end(), [](const TaskRecord& a, const TaskRecord& b) { return taskOrderBefore(a, b); });
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
