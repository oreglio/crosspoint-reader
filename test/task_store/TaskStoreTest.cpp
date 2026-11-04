#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "TaskStore.h"

namespace {

TaskRecord makeTask(const char* id, const char* group = "") {
  TaskRecord r{};
  std::snprintf(r.id, sizeof(r.id), "%s", id);
  std::snprintf(r.title, sizeof(r.title), "titre %s", id);
  r.priority = TASK_PRIORITY_NORMAL;
  r.order = TASK_ORDER_UNSET;
  std::snprintf(r.group, sizeof(r.group), "%s", group);
  return r;
}

TaskGroup makeGroup(const char* id, const char* name, uint16_t position) {
  TaskGroup g{};
  std::snprintf(g.id, sizeof(g.id), "%s", id);
  std::snprintf(g.name, sizeof(g.name), "%s", name);
  g.position = position;
  return g;
}

// Repart d'une carte contenant `json`, store decharge puis recharge.
void loadIndex(const char* json) {
  Storage.reset();
  Storage.put(TaskStore::getFilePath(), json);
  TASK_STORE.unload();
  TASK_STORE.ensureLoaded();
}

void freshEmptyStore() {
  Storage.reset();
  TASK_STORE.unload();
  TASK_STORE.ensureLoaded();
}

void fillGroups() {
  for (size_t i = 0; i < MAX_GROUPS; i++) {
    char id[TASK_ID_LEN + 1];
    std::snprintf(id, sizeof(id), "g%07zx", i);
    ASSERT_TRUE(TASK_STORE.upsertGroup(makeGroup(id, "x", static_cast<uint16_t>(i)))) << i;
  }
}

}  // namespace

TEST(TaskStoreGroups, RecordAndGroupSizesStayWithinBudget) {
  EXPECT_EQ(sizeof(TaskRecord), 228u);  // 218 + 10
  EXPECT_EQ(sizeof(TaskGroup), 46u);
}

TEST(TaskStoreGroups, JsonRoundTripKeepsGroupsFlagAndTaskMembership) {
  freshEmptyStore();
  ASSERT_TRUE(TASK_STORE.upsertGroup(makeGroup("g0000001", "Maison", 0)));
  ASSERT_TRUE(TASK_STORE.upsertGroup(makeGroup("g0000002", "Travail", 40)));  // > MAX_GROUPS : cle de tri seulement
  TASK_STORE.setGroupsSynced(true);
  ASSERT_TRUE(TASK_STORE.upsert(makeTask("w00000001", "g0000002")));
  ASSERT_TRUE(TASK_STORE.upsert(makeTask("w00000002")));

  TASK_STORE.unload();
  TASK_STORE.ensureLoaded();

  ASSERT_EQ(TASK_STORE.groups().size(), 2u);
  EXPECT_STREQ(TASK_STORE.groups()[0].name, "Maison");
  EXPECT_STREQ(TASK_STORE.groups()[1].id, "g0000002");
  EXPECT_EQ(TASK_STORE.groups()[1].position, 40u);
  EXPECT_TRUE(TASK_STORE.groupsSynced());
  ASSERT_EQ(TASK_STORE.all().size(), 2u);
  EXPECT_STREQ(TASK_STORE.find("w00000001")->group, "g0000002");
  EXPECT_STREQ(TASK_STORE.find("w00000002")->group, "");
}

TEST(TaskStoreGroups, UngroupedTaskWritesNoGKey) {
  freshEmptyStore();
  ASSERT_TRUE(TASK_STORE.upsert(makeTask("w00000001")));
  const std::string saved = Storage.readFile(TaskStore::getFilePath()).c_str();
  EXPECT_EQ(saved.find("\"g\""), std::string::npos);
  EXPECT_NE(saved.find("\"groups\""), std::string::npos);
}

TEST(TaskStoreGroups, OldIndexWithoutGroupKeysStillLoads) {
  loadIndex(R"({"schema":1,"tasks":[{"id":"w00000001","t":"vieux","p":1,"d":false,"n":0,"o":3}]})");
  ASSERT_EQ(TASK_STORE.all().size(), 1u);
  EXPECT_STREQ(TASK_STORE.all()[0].title, "vieux");
  EXPECT_STREQ(TASK_STORE.all()[0].group, "");
  EXPECT_TRUE(TASK_STORE.groups().empty());
  EXPECT_FALSE(TASK_STORE.groupsSynced());
}

TEST(TaskStoreGroups, RemoveGroupClearsItsTasksOnly) {
  freshEmptyStore();
  ASSERT_TRUE(TASK_STORE.upsertGroup(makeGroup("g0000001", "A", 0)));
  ASSERT_TRUE(TASK_STORE.upsertGroup(makeGroup("g0000002", "B", 1)));
  ASSERT_TRUE(TASK_STORE.upsert(makeTask("w00000001", "g0000001")));
  ASSERT_TRUE(TASK_STORE.upsert(makeTask("w00000002", "g0000002")));

  TASK_STORE.removeGroup("g0000001");

  EXPECT_EQ(TASK_STORE.findGroup("g0000001"), nullptr);
  ASSERT_NE(TASK_STORE.findGroup("g0000002"), nullptr);
  EXPECT_STREQ(TASK_STORE.find("w00000001")->group, "");
  EXPECT_STREQ(TASK_STORE.find("w00000002")->group, "g0000002");
}

TEST(TaskStoreGroups, SeventeenthNewGroupIsRefused) {
  freshEmptyStore();
  fillGroups();
  EXPECT_FALSE(TASK_STORE.upsertGroup(makeGroup("g00000ff", "de trop", 99)));
  EXPECT_EQ(TASK_STORE.groups().size(), MAX_GROUPS);
  EXPECT_EQ(TASK_STORE.findGroup("g00000ff"), nullptr);
}

TEST(TaskStoreGroups, UpsertOfExistingGroupUpdatesWithoutCountingTowardsTheCap) {
  freshEmptyStore();
  fillGroups();
  ASSERT_TRUE(TASK_STORE.upsertGroup(makeGroup("g0000003", "Renomme", 77)));
  EXPECT_EQ(TASK_STORE.groups().size(), MAX_GROUPS);
  const TaskGroup* g = TASK_STORE.findGroup("g0000003");
  ASSERT_NE(g, nullptr);
  EXPECT_STREQ(g->name, "Renomme");
  EXPECT_EQ(g->position, 77u);
}

TEST(TaskStoreGroups, OversizedGroupNameIsCutOnAUtf8Boundary) {
  // 31 octets ASCII puis « é » (2 octets) : le 32e octet couperait le caractere.
  const std::string name = std::string(31, 'a') + "\xC3\xA9" + "zzz";
  loadIndex(("{\"groups\":[{\"id\":\"g0000001\",\"name\":\"" + name + "\",\"pos\":2}],\"tasks\":[]}").c_str());
  ASSERT_EQ(TASK_STORE.groups().size(), 1u);
  EXPECT_EQ(std::strlen(TASK_STORE.groups()[0].name), 31u);
}

TEST(TaskStoreGroups, UnloadReleasesGroupCapacity) {
  loadIndex(R"({"gs":true,"groups":[{"id":"g0000001","name":"A","pos":0}],"tasks":[]})");
  ASSERT_EQ(TASK_STORE.groups().size(), 1u);
  TASK_STORE.unload();
  EXPECT_TRUE(TASK_STORE.groups().empty());
  EXPECT_EQ(TASK_STORE.groups().capacity(), 0u);
  EXPECT_FALSE(TASK_STORE.groupsSynced());
}
