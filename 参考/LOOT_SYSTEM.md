# AzerothCore 掉落系统详解

> **相关文档**: [架构总览](./ARCHITECTURE.md)
> **分析日期**: 2026-06-22

---

## 快速索引

- **[1. 总体架构](#1-总体架构)**
- **[2. 数据层：模板与表结构](#2-数据层模板与表结构)**
  - [2.1 13 个 LootStore 与数据库表对应](#21-13-个-lootstore-与数据库表对应)
  - [2.2 数据库行结构](#22-数据库行结构)
  - [2.3 核心运行时结构](#23-核心运行时结构)
  - [2.4 LootTemplate 内部组织](#24-loottemplate-内部组织)
- **[3. 启动加载流程](#3-启动加载流程)**
- **[4. 运行时核心：从死亡到生成掉落](#4-运行时核心从死亡到生成掉落)**
  - [4.1 触发点：Unit::Kill](#41-触发点unitkill)
  - [4.2 Loot::FillLoot —— 入口](#42-lootfillloot--入口)
  - [4.3 LootTemplate::Process —— roll 引擎](#43-loottemplateprocess--roll-引擎)
  - [4.4 Loot::AddItem —— 落袋](#44-lootadditem--落袋)
- **[5. 三种 roll 模式详解](#5-三种-roll-模式详解)**
  - [5.1 模式一：单物品概率 roll](#51-模式一单物品概率-roll--lootstoreitemrolllootmgrcpp311)
  - [5.2 模式二：组内显式概率 roll](#52-模式二组内显式概率-roll--lootgrouprolllootmgrcpp1262)
  - [5.3 模式三：组内多次选取](#53-模式三组内多次选取--lootgroupprocesslootmgrcpp1409)
  - [5.4 引用（reference）机制](#54-引用reference机制)
- **[6. 金钱掉落](#6-金钱掉落--lootgeneratemoneylootlootmgrcpp808)**
- **[7. 任务物品 vs 普通物品](#7-任务物品-vs-普通物品)**
- **[8. 组队规则、权限、阈值与 ROLL](#8-组队规则权限阈值与-roll)**
- **[9. LootView 发包](#9-lootview-发包--每个玩家看到的窗口不同)**
- **[10. 掉落条件（Conditions）](#10-掉落条件conditions)**
- **[11. 容器物品的持久化](#11-容器物品的持久化--lootitemstoragelootitemstoragecpp)**
- **[12. 速率修正与脚本钩子](#12-速率修正与脚本钩子)**
- **[13. 完整调用链](#13-完整调用链生物死亡--完成)**
- **[14. 核心文件索引](#14-核心文件索引)**
- **[15. 常见配置 / 调试要点](#15-常见配置--调试要点)**

---

## 1. 总体架构

掉落系统全部代码位于 `src/server/game/Loot/`（共 4 个文件），核心在 `LootMgr.h/.cpp`。整个机制分为**三层**：

```
模板层 (静态, 启动时加载一次)
   │  LootStore × 13  →  LootTemplateMap  →  LootTemplate
   │                                       ├─ Entries (未分组行)
   │                                       └─ Groups[] (分组行, 每组单 roll)
   ▼
运行时层 (每具尸体/物体/邮件... 一次)
   │  Loot 实例  →  items[] / quest_items[] / gold / per-player 可见性映射
   │   FillLoot() → Process() → Roll() / LootGroup::Roll() → AddItem()
   ▼
展示层 (按玩家过滤 + 发包)
      LootView → SMSG_LOOT_RESPONSE
      各玩家看到的槽位类型 (LOOT_SLOT_TYPE_*) 不同
```

关键设计要点：
- **掉落是在击杀/打开瞬间生成的**，不是每次开窗口都重摇（生物尸体）
- **任务/条件物品照常参与 roll，只是被过滤可见性**，不会真的消失
- **每个玩家看到的窗口可以不同**（FFA、任务、round-robin、ML 锁定等）

---

## 2. 数据层：模板与表结构

### 2.1 13 个 LootStore 与数据库表对应

启动时为每个 `*_loot_template` 表创建一个全局 `LootStore` 实例（`LootMgr.cpp:44-56`）：

| 全局 Store | 数据库表 | entry 含义 | ratesAllowed |
|---|---|---|---|
| `LootTemplates_Creature` | `creature_loot_template` | `creature_template.lootid` | ✓ |
| `LootTemplates_Pickpocketing` | `pickpocketing_loot_template` | `creature_template.pickpocketLootId` | ✓ |
| `LootTemplates_Skinning` | `skinning_loot_template` | `creature_template.SkinLootId` | ✓ |
| `LootTemplates_Gameobject` | `gameobject_loot_template` | GO 的 `GetLootId()` | ✓ |
| `LootTemplates_Item` | `item_loot_template` | 可开物品 entry | ✓ |
| `LootTemplates_Disenchant` | `disenchant_loot_template` | `item_template.DisenchantID` | ✓ |
| `LootTemplates_Prospecting` | `prospecting_loot_template` | 矿石 item entry | ✓ |
| `LootTemplates_Milling` | `milling_loot_template` | 草药 item entry | ✓ |
| `LootTemplates_Fishing` | `fishing_loot_template` | 区域 id | ✓ |
| `LootTemplates_Mail` | `mail_loot_template` | 邮件模板 id | ✗ |
| `LootTemplates_Spell` | `spell_loot_template` | 法术 id | ✗ |
| `LootTemplates_Player` | `player_loot_template` | 阵营 id | ✓ |
| `LootTemplates_Reference` | `reference_loot_template` | 共享引用 id | ✗ |

`ratesAllowed` 决定是否在 roll 时应用品质掉率修正（`Rate.Drop.Item.*`），邮件/引用/法术不掉率修正。

### 2.2 数据库行结构

所有表结构完全一致（`creature_loot_template.sql:23-35`）：

```sql
CREATE TABLE `creature_loot_template` (
  `Entry`          int unsigned NOT NULL,        -- lootid
  `Item`           int unsigned NOT NULL,        -- 物品 id 或引用目标 id
  `Reference`      int          NOT NULL DEFAULT '0',  -- 非0 = 引用
  `Chance`         float        NOT NULL DEFAULT '100',-- 掉率 (组内 0 = 等概率)
  `QuestRequired`  tinyint      NOT NULL DEFAULT '0',  -- 任务物品
  `LootMode`       smallint unsigned NOT NULL DEFAULT '1', -- 位掩码
  `GroupId`        tinyint unsigned NOT NULL DEFAULT '0', -- 0=未分组, >0=组号
  `MinCount`       tinyint unsigned NOT NULL DEFAULT '1',
  `MaxCount`       tinyint unsigned NOT NULL DEFAULT '1',
  `Comment`        varchar(255),
  PRIMARY KEY (`Entry`,`Item`,`Reference`,`GroupId`)
);
```

### 2.3 核心运行时结构

#### `LootStoreItem`（模板行，`LootMgr.h:127`）

直接映射数据库行：
```cpp
struct LootStoreItem {
    uint32  itemid;          // 物品 id (或引用目标)
    int32   reference;       // 非 0 = 引用 reference_loot_template
    float   chance;          // 掉率 (组内 0 = 等概率)
    bool    needs_quest : 1; // 任务物品
    uint16  lootmode;        // 位掩码 (LOOT_MODE_*)
    uint8   groupid    : 7;  // 组 id (0 = 未分组)
    uint8   mincount, maxcount;
    ConditionList conditions;
    bool Roll(bool rate, Player const*, Loot&, LootStore const&) const;
    bool IsValid(LootStore const&, uint32 entry) const;
};
```

#### `LootItem`（运行时实物，`LootMgr.h:153`）

已 roll 出来的、装进 `Loot` 的物品，附加了大量运行时状态：
```cpp
struct LootItem {
    uint32 itemid, itemIndex;
    uint32 randomSuffix;
    int32  randomPropertyId;
    ConditionList conditions;
    AllowedLooterSet allowedGUIDs;      // 谁能看见/拿取
    ObjectGuid rollWinnerGUID;          // ROLL 胜者
    uint8 count : 8;
    bool is_looted         : 1;
    bool is_blocked        : 1;          // 阈值之上/正在 ROLL/ML 限定
    bool freeforall        : 1;          // ITEM_FLAG_MULTI_DROP
    bool is_underthreshold : 1;          // 低于阈值，走 round-robin
    bool is_counted        : 1;
    bool needs_quest       : 1;
    bool follow_loot_rules : 1;          // ITEM_FLAGS_CU_FOLLOW_LOOT_RULES
    uint8 groupid          : 7;
    bool AllowedForPlayer(Player const*, ObjectGuid source) const;
};
```

#### `Loot`（每具尸体/物体实例，`LootMgr.h:312`）

```cpp
struct Loot {
    std::vector<LootItem> items;       // 普通掉落 (上限 18)
    std::vector<LootItem> quest_items; // 任务掉落 (上限 32)
    uint32 gold;
    uint8  unlootedCount;
    ObjectGuid roundRobinPlayer;
    ObjectGuid lootOwnerGUID;
    LootType loot_type;
    // ...
    // 每玩家可见性映射 (任务 / FFA / 条件)
    QuestItemMap PlayerQuestItems;
    QuestItemMap PlayerFFAItems;
    QuestItemMap PlayerNonQuestNonFFAConditionalItems;

    bool FillLoot(...);
    void AddItem(LootStoreItem const&);
    void generateMoneyLoot(uint32 min, uint32 max);
    LootItem* LootItemInSlot(...);
    void FillNotNormalLootFor(Player*);
};
```

`MAX_NR_LOOT_ITEMS = 18`（`LootMgr.h:51`），`MAX_NR_QUEST_ITEMS = 32`（`LootMgr.h:53`），都是客户端硬限制。

### 2.4 LootTemplate 内部组织

每个 `Entry`（lootid）对应一个 `LootTemplate`（`LootMgr.h:244`），它把行按 `GroupId` 分流：

```
LootTemplate (Entry=lootid)
 ├─ Entries[]      ← groupid == 0 的行, 每个独立 roll
 └─ Groups[]       ← groupid > 0 的行
      ├─ Groups[0] (对应 GroupId=1)
      │    ├─ ExplicitlyChanced[]  ← Chance > 0 的行
      │    └─ EqualChanced[]       ← Chance == 0 的行
      ├─ Groups[1] (对应 GroupId=2)
      └─ ...
```

**`LootGroup`**（内部类，`LootMgr.cpp:94-123`）：每组只产出一个物品（除非 `RATE_DROP_ITEM_GROUP_AMOUNT` 让它多摇几次）。

---

## 3. 启动加载流程

```
LoadLootTables()  [LootMgr.h:443]      (worldserver 启动时调一次)
  └─ 对每张表: LoadLootTemplates_X()  [LootMgr.cpp:1910-2319]
       └─ LootStore::LoadAndCollectLootIds()  [LootMgr.cpp:267]
            └─ LootStore::LoadLootTable()     [LootMgr.cpp:143]
                 │  SQL:
                 │  SELECT Entry, Item, Reference, Chance, QuestRequired,
                 │         LootMode, GroupId, MinCount, MaxCount FROM <表>
                 │
                 ├─ 每行 → new LootStoreItem
                 │       └─ IsValid() 校验  [LootMgr.cpp:333]
                 │          (拒绝坏的 groupid/mincount/chance/引用)
                 │
                 └─ LootTemplate::AddEntry(storeitem)  [LootMgr.cpp:1532]
                      ├─ groupid==0 → Entries.push_back
                      └─ groupid>0  → Groups[gid-1]->AddEntry()
                                       └─ 按 chance 分到 Explicitly / Equal

       └─ Verify()  [LootMgr.cpp:135]
            └─ 每个模板遍历: 组显式概率总和 > 101% 报错

       └─ 交叉校验 owner 表
            (如 creature_template.lootid 是否存在；item_template 是否带 ITEM_FLAG_HAS_LOOT 等)

       └─ ReportUnusedIds  (没人引用的 lootid 报告)
       └─ LootTemplates_Reference.CheckLootRefs()  (标记哪些引用 id 在用)
```

`lootmode == 0` 会被强制改为 `1`（`LootMgr.cpp:178-182`），避免永远掉不出。

---

## 4. 运行时核心：从死亡到生成掉落

### 4.1 触发点：`Unit::Kill`

生物死亡时，**掉落在击杀瞬间就生成好了**（不是开尸体时），位于 `Unit.cpp:14060-14083`：

```cpp
if (creature)
{
    Loot* loot = &creature->loot;          // Creature.loot 成员 (Creature.h:231)
    loot->clear();

    if (uint32 lootid = creature->GetCreatureTemplate()->lootid)
        loot->FillLoot(lootid, LootTemplates_Creature, looter, false,
                       false, creature->GetLootMode(), creature);

    if (creature->GetLootMode())
        loot->generateMoneyLoot(creature->GetCreatureTemplate()->mingold,
                                creature->GetCreatureTemplate()->maxgold);
}
```

随后（`Unit.cpp:14199-14210`）给尸体打上 `UNIT_DYNFLAG_LOOTABLE` 动态标记（客户端金色闪光）。

**其他触发点**（均调 `FillLoot` + 视情况调 `generateMoneyLoot`）：
- `Player.cpp:7893` GO 掉落
- `Player.cpp:7982` 分解、`7985` 选矿、`7988` 研磨、`7991` 物品容器
- `Player.cpp:8019` 玩家尸体（按阵营）、`8060` 偷窃、`8123` 剥皮、`13555` 钓鱼法术
- `GameObject.cpp:1010` 钓鱼
- `Mail.cpp:116` 邮件附件
- `Group.cpp:1619` 队伍分解

### 4.2 `Loot::FillLoot` —— 入口

**`LootMgr.cpp:540`**

```cpp
bool Loot::FillLoot(uint32 lootId, LootStore const& store, Player* lootOwner,
                    bool personal, bool noEmptyError, uint16 lootMode,
                    WorldObject* lootSource)
```

1. 保存 `lootOwnerGUID`
2. `tab = store.GetLootFor(lootId)` —— 查模板
3. **核心**：`tab->Process(*this, store, lootMode, lootOwner, 0, true)` —— 在这里完成所有物品 roll
4. 触发脚本钩子 `OnAfterLootTemplateProcess`
5. 队伍场景：遍历附近队员调 `FillNotNormalLootFor`，按 `group->GetLootThreshold()` 标记 `is_underthreshold`（低于阈值的物品走 round-robin，不参与 roll）

### 4.3 `LootTemplate::Process` —— roll 引擎

**`LootMgr.cpp:1667`**

掉落决策的中枢，分三大块：

```cpp
void LootTemplate::Process(Loot& loot, LootStore const& store, uint16 lootMode,
                           Player const* player, uint8 groupId, bool isTopLevel) const
```

**(A) 引用组**：若 `groupId != 0`，只处理 `Groups[groupId-1]`。`isTopLevel` 时传入 `RATE_DROP_ITEM_GROUP_AMOUNT` 作为额外 roll 次数。

**(B) 未分组条目**（`Entries`），**每个独立 roll**：

```cpp
for (i = Entries.begin(); ...) {
    if (!(item->lootmode & lootMode)) continue;          // 模式掩码
    if (!item->Roll(rate, player, loot, store)) continue; // 单物品 roll

    if (item->reference) {            // 引用模式: 递归到 reference_loot_template
        Referenced = LootTemplates_Reference.GetLootFor(abs(reference));
        maxcount *= sWorld->getRate(RATE_DROP_ITEM_REFERENCED_AMOUNT);
        for (loop = 0; loop < maxcount; ++loop)
            Referenced->Process(loot, store, lootMode, player, item->groupid, false);
    } else {                          // 普通物品
        sScriptMgr->OnBeforeDropAddItem(...);
        loot.AddItem(*item);
    }
}
```

**(C) 分组条目**（`Groups`），**每组只出一个**（组内单次 roll）：

```cpp
for (i = Groups.begin(); ...) {
    if (isTopLevel) group->Process(loot, player, store, lootMode,
                                   sWorld->getRate(RATE_DROP_ITEM_GROUP_AMOUNT));
    else            group->Process(loot, player, store, lootMode, 0);
}
```

### 4.4 `Loot::AddItem` —— 落袋

**`LootMgr.cpp:481`**

```cpp
count  = urand(mincount, maxcount);
stacks = count / proto->GetMaxStackSize() + (余数 ? 1 : 0);
// 任务物品 → quest_items (上限 32), 普通 → items (上限 18)
for (i = 0; i < stacks && lootItems.size() < limit; ++i) {
    LootItem generated(item);
    generated.count = std::min(count, proto->GetMaxStackSize());
    generated.itemIndex = lootItems.size();
    lootItems.push_back(generated);
    if (!needs_quest && conditions.empty() && !ITEM_FLAG_MULTI_DROP)
        ++unlootedCount;     // 用于判断尸体是否被捡光
}
```

---

## 5. 三种 roll 模式详解

### 5.1 模式一：单物品概率 roll —— `LootStoreItem::Roll`（`LootMgr.cpp:311`）

```cpp
if (_chance >= 100.0f) return true;                 // 必掉
if (reference)                                      // 引用走单独 rate
    return roll_chance_f(_chance * (rate ? RATE_DROP_ITEM_REFERENCED : 1.0f));
// 按品质应用 worldserver.conf 的 Rate.Drop.Item.* 修正
qualityModifier = sWorld->getRate(qualityToRate[pProto->Quality]);
return roll_chance_f(_chance * qualityModifier);
```

其中（`Random.h:51-57`）：
```cpp
inline bool roll_chance_f(float chance) { return chance > rand_chance(); }
// rand_chance() 返回 [0, 100) 的 double
```

`qualityToRate[]` 把 Poor..Artifact 映射到 `RATE_DROP_ITEM_POOR..ARTIFACT`（`LootMgr.cpp:33-42`）。

### 5.2 模式二：组内显式概率 roll —— `LootGroup::Roll`（`LootMgr.cpp:1262`）

```cpp
LootStoreItemList possibleLoot = ExplicitlyChanced;
possibleLoot.remove_if(LootGroupInvalidSelector(...));  // 过滤 lootmode 不符 + 重复上限

if (!possibleLoot.empty()) {
    float roll = rand_chance();              // 整组只摇一次骰子
    for (itr = ...; itr != end; ++itr) {
        float chance = item->chance;
        if (chance >= 100.0f) return item;   // 必中
        roll -= chance;
        if (roll < 0) return item;          // 中奖
    }
}
// 显式全部未中 → 等概率池随机挑
possibleLoot = EqualChanced; ...
return SelectRandomContainerElement(possibleLoot);
```

**关键**：组内显式概率应**总和 ≤ 100%**，否则无法触发等概率回退。

**`LootGroupInvalidSelector`**（`LootMgr.cpp:59-92`）：装备类重复上限 1、非装备类 3（战利品 47242 例外），并屏蔽 lootmode 不符的项。

### 5.3 模式三：组内多次选取 —— `LootGroup::Process`（`LootMgr.cpp:1409`）

当 `nonRefIterationsLeft > 1`（由 `RATE_DROP_ITEM_GROUP_AMOUNT` 决定）且获胜项非任务、非引用物品时，**递归再 roll**：

```cpp
loot.AddItem(*item);
if (nonRefIterationsLeft > 1 && !item->needs_quest)
    this->Process(loot, player, store, lootMode, nonRefIterationsLeft - 1);
```

**任务物品不能多次出**。

### 5.4 引用（reference）机制

`Reference` 列非 0 时，行本身仍按模式一/二 roll；一旦命中，就用 `maxcount * RATE_DROP_ITEM_REFERENCED_AMOUNT` 作为次数递归处理 `reference_loot_template` 中对应 entry。

**组内禁止放引用**（`LootMgr.h:246` 注释），引用只能在未分组条目中使用。

---

## 6. 金钱掉落 —— `Loot::generateMoneyLoot`（`LootMgr.cpp:808`）

```cpp
void Loot::generateMoneyLoot(uint32 minAmount, uint32 maxAmount)
{
    if (maxAmount > 0) {
        if (maxAmount <= minAmount)
            gold = maxAmount * RATE_DROP_MONEY;
        else if ((maxAmount - minAmount) < 32700)
            gold = urand(minAmount, maxAmount) * RATE_DROP_MONEY;
        else                                            // 范围过大
            gold = (urand(minAmount >> 8, maxAmount >> 8)
                    * RATE_DROP_MONEY) << 8;            // 右移 8 位粗粒度摇
    }
}
```

调用点：
- 生物尸体：`creature_template.mingold/maxgold`
- GO：`gameobject_template_addon.mingold/maxgold`
- 物品容器：`item_template.MinMoneyLoot/MaxMoneyLoot`
- 偷窃：按等级计算
- 玩家尸体取徽章

**队伍分钱**发生在玩家点"拿钱"时 —— `WorldSession::HandleLootMoneyOpcode`（`LootHandler.cpp:114`）把 `loot->gold` 在所有 `IsAtLootRewardDistance` 的队员间均分，并广播 `SMSG_LOOT_MONEY_NOTIFY`。

---

## 7. 任务物品 vs 普通物品

完全由 `needs_quest` 标志（列 `QuestRequired`）区分：

| 阶段 | 任务物品 | 普通物品 |
|---|---|---|
| 生成（`AddItem`） | 进入 `quest_items`（上限 32） | 进入 `items`（上限 18） |
| 组内多次 roll | **禁止**（`LootMgr.cpp:1436`） | 允许 |
| 玩家可见（`AllowedForPlayer`） | 必须 `HasQuestForItem(itemid)` | 按条件 / FFA 判断 |
| 发包位置 | `items` 之后，slot = `items.size()+i` | slot = `i` |

**`LootItem::AllowedForPlayer`**（`LootMgr.cpp:417`）是任务 / 职业 / 阵营 / 专业 / 配方 / 唯一性等所有过滤的总闸：

```cpp
if (!pProto->HasFlagCu(ITEM_FLAGS_CU_IGNORE_QUEST_STATUS))
    if (needs_quest && !player->HasQuestForItem(itemid)) return false;
if (!sConditionMgr->IsObjectMeetToConditions(player, conditions)) return false;
if (pProto->HasFlag2(ITEM_FLAG2_FACTION_HORDE) && 玩家不是部落) return false;
// ...专业、配方、唯一装备、脚本钩子
```

调用点：
- `FillFFALoot` / `FillQuestLoot` / `FillNonQuestNonFFAConditionalLoot`
- `Loot::AddItem` 自身（决定是否 bump `unlootedCount`）
- `LootView` operator（每 viewer 槽位可见性）
- `CanRollOnItem`（`Group.cpp:977`）—— 玩家能否 roll 某组物品
- `HandleLootMasterGiveOpcode`（`LootHandler.cpp:488`）—— ML 目标能否收到

---

## 8. 组队规则、权限、阈值与 ROLL

### 8.1 队伍分配方式（`LootMethod`，`LootMgr.h:56`）

| 枚举 | 行为 |
|---|---|
| `FREE_FOR_ALL` | 所有人都能捡所有物品 |
| `ROUND_ROBIN` | 轮流捡普通物品，任务/条件物品仍各自可见 |
| `MASTER_LOOT` | 主分配者（ML）决定物品归属 |
| `GROUP_LOOT` | 阈值之上物品 ROLL（greed/need/disenchant） |
| `NEED_BEFORE_GREED` | 同上但 NEED 受职业/能否使用限制 |

### 8.2 权限分配 —— `Player::SendLoot`（`Player.cpp:8138-8165`）

玩家打开尸体时，按队伍方式和是否 ML 决定 `PermissionTypes`（`LootMgr.h:65`）：

```
MASTER_LOOT     → MASTER_PERMISSION (自己是 ML) / RESTRICTED_PERMISSION (其他人)
FREE_FOR_ALL    → ALL_PERMISSION
ROUND_ROBIN     → ROUND_ROBIN_PERMISSION
GROUP_LOOT/NBG  → GROUP_PERMISSION
单人            → OWNER_PERMISSION / NONE_PERMISSION
```

### 8.3 ROLL 触发（开尸体瞬间，非击杀时，`Player.cpp:8094-8111`）

```cpp
if (loot->loot_type == LOOT_NONE) {     // 首次开
    switch (recipientGroup->GetLootMethod()) {
        case GROUP_LOOT:        recipientGroup->GroupLoot(loot, creature);       break;
        case NEED_BEFORE_GREED: recipientGroup->NeedBeforeGreed(loot, creature); break;
        case MASTER_LOOT:       recipientGroup->MasterLoot(loot, creature);      break;
    }
}
```

### 8.4 三种队伍规则实现（均在 `Group.cpp`）

- **`GroupLoot`**（`983` 行）：对每个 `quality ≥ m_lootThreshold` 且非 FFA 的物品建 `Roll` 对象，设 `is_blocked = true`（窗口显示 `ROLL_ONGOING`），60 秒倒计时（`creature->m_groupLootTimer = 60000`），广播 `SendLootStartRoll`。
- **`NeedBeforeGreed`**（`1139` 行）：类似但更细——`ITEM_FLAG2_CAN_ONLY_ROLL_GREED` 去掉 NEED 选项；有 `DisenchantID` 且附魔等级够时加 `ROLL_FLAG_TYPE_DISENCHANT`。
- **`MasterLoot`**（`1290` 行）：不 roll，直接把所有非阈值物品设 `is_blocked`，ML 看到 `LOOT_SLOT_TYPE_MASTER`，其他人看到 `LOCKED`，并发 `SMSG_LOOT_MASTER_LIST`。

### 8.5 实际骰子 —— `Group::CountTheRoll`（`Group.cpp:1435`）

所有人投完票（`CountRollVote`，`Group.cpp:1339`）或超时（`EndRoll`，1390 行）后：

1. **NEED 池**优先：每个 NEED 投票者 `urand(1, 100)`（`Group.cpp:1466`），最高者赢
2. 没 NEED，转 **GREED/DISENCHANT 池**：同样 `urand(1, 100)`（`1545` 行）
3. 赢家：`StoreNewItem`；分解赢家调 `player->AutoStoreLoot(DisenchantID, LootTemplates_Disenchant)`（`Group.cpp:1614`）
4. 赢家背包满：记 `item->rollWinnerGUID`、清 `is_blocked`，只让赢家以后能取（`1512-1515, 1595-1597`）

客户端投票经 `CMSG_LOOT_ROLL` → `HandleLootRoll`（`GroupHandler.cpp:494`）→ `CountRollVote`。

### 8.6 主分配者给物 —— `HandleLootMasterGiveOpcode`（`LootHandler.cpp:421`）

校验发送者确为 ML 且队伍用 `MASTER_LOOT`、目标在同一 raid、`item.AllowedForPlayer(target)` 通过后，`target->StoreNewItem(...)`，标记 `item.is_looted = true`，`--loot->unlootedCount`。

### 8.7 `LootMode` 位掩码

每行 `LootStoreItem.lootmode` 与运行时 `creature->GetLootMode()` 做 `&`，不匹配即跳过（`LootMgr.cpp:1695`、`LootGroupInvalidSelector`）。

脚本可通过 `Creature::AddLootMode/RemoveLootMode/SetLootMode`（`Creature.h:246`）动态切换，常用于"英雄 / 普通难度掉不同物品"。

---

## 9. LootView 发包 —— 每个玩家看到的窗口不同

`operator<<(ByteBuffer, LootView)`（`LootMgr.cpp:974`）按 `permission` 给每个槽设置 `LootSlotType`（`LootMgr.h:112`）：

```cpp
if (l.items[i].is_blocked) {
    switch (lv.permission) {
        case GROUP_PERMISSION:      slot_type = LOOT_SLOT_TYPE_ROLL_ONGOING; break;
        case MASTER_PERMISSION:     slot_type = (自己是 ML) ? MASTER : LOCKED; break;
        case RESTRICTED_PERMISSION: slot_type = LOOT_SLOT_TYPE_LOCKED;        break;
    }
}
else if (l.items[i].rollWinnerGUID) {
    if (winner == 自己) slot_type = LOOT_SLOT_TYPE_OWNER;
    else continue;                     // 对其他人隐藏
}
else if (!roundRobinPlayer || 自己 == roundRobinPlayer || !is_underthreshold)
    slot_type = LOOT_SLOT_TYPE_ALLOW_LOOT;
else continue;                         // 等 round-robin 轮到自己
```

| `LootSlotType` | 含义 |
|---|---|
| `LOOT_SLOT_TYPE_ALLOW_LOOT` (0) | 可捡 |
| `LOOT_SLOT_TYPE_ROLL_ONGOING` (1) | ROLL 中 |
| `LOOT_SLOT_TYPE_MASTER` (2) | 仅 ML 可见 |
| `LOOT_SLOT_TYPE_LOCKED` (3) | 红色锁定 |
| `LOOT_SLOT_TYPE_OWNER` (4) | 已 ROLL 到手 |

任务 / FFA / 条件物品则分别从 `PlayerQuestItems` / `PlayerFFAItems` / `PlayerNonQuestNonFFAConditionalItems` 这些**按玩家**的映射中取出再发（由 `FillNotNormalLootFor` → `FillQuestLoot/FillFFALoot/FillNonQuestNonFFAConditionalLoot` 在 `FillLoot` 末尾构建）。

---

## 10. 掉落条件（Conditions）

`conditions` 表中 `SourceType` = `CONDITION_SOURCE_TYPE_*_LOOT_TEMPLATE` 的行，在启动时由 `ConditionMgr::LoadConditions` 分发（`ConditionMgr.cpp:1273-1366`）到对应 Store。规则：

- `SourceGroup` = 模板 entry（如 `creature_loot_template.Entry`）
- `SourceEntry` = 该模板里的 item id

```
addToLootTemplate (ConditionMgr.cpp:1408)
   → LootTemplate::addConditionItem (LootMgr.cpp:1839)
      遍历 Entries、各组 ExplicitlyChanced/EqualChanced,
      把条件 push 到匹配 itemid 的 LootStoreItem.conditions
```

**关键：条件不参与 roll，只过滤可见性**。在 `LootItem::AllowedForPlayer`（`LootMgr.cpp:426`）里调 `sConditionMgr->IsObjectMeetToConditions(player, conditions)`。

也就是说条件物品仍按 chance 生成，但只有满足条件的玩家能看到 / 能 roll / 能被 ML 分配。对于非 FFA、非任务的条件物品，通过 `PlayerNonQuestNonFFAConditionalItems` 映射呈现（`FillNonQuestNonFFAConditionalLoot`，`LootMgr.cpp:708`）。

---

## 11. 容器物品的持久化 —— `LootItemStorage`（`LootItemStorage.cpp`）

普通尸体 / GO 的 `Loot` 只存活于内存，但**物品容器（可开的包 / 礼物）**的掉落要跨重启保存，存到 `item_instance_container_items`（CharacterDatabase）。

| 方法 | 用途 |
|---|---|
| `AddNewStoredLoot` | 物品首次开包时写入 |
| `LoadStoredLoot` | 重启后从 DB 还原到 `Loot` |
| `RemoveStoredLootItem` | 物品被取出后删除 |

---

## 12. 速率修正与脚本钩子

### 12.1 worldserver.conf 中的关键 rate

| Rate | 影响 |
|---|---|
| `Rate.Drop.Item.<Quality>` | 单物品按品质乘 chance（Poor..Artifact） |
| `Rate.Drop.Item.Referenced` | 引用物品是否参与品质修正 |
| `Rate.Drop.Item.ReferencedAmount` | 引用物品的递归处理次数倍数 |
| `Rate.Drop.Item.GroupAmount` | 每组 roll 多少次（产生多个非任务物品） |
| `Rate.Drop.Money` | 所有金钱统一倍率 |

### 12.2 主要脚本钩子

| 钩子 | 注入点 |
|---|---|
| `OnItemRoll` | `LootStoreItem::Roll` / `LootGroup::Roll` 中，可改 chance 或否决 |
| `OnBeforeLootEqualChanced` | 等概率回退前，可否决 |
| `OnBeforeDropAddItem` | `AddItem` 之前，可改 / 否决 |
| `OnAfterRefCount` | 引用次数计算之后 |
| `OnAfterLootTemplateProcess` | 整个模板处理完之后 |
| `OnAllowedForPlayerLootCheck` | `AllowedForPlayer` 末尾的脚本可改判断 |

---

## 13. 完整调用链（生物死亡 → 完成）

```
Unit::Kill                                  [Unit.cpp:13975]
 ├─ group->UpdateLooterGuid / SetLootRecipient          [14034-14044]
 ├─ creature->loot.clear()
 ├─ Loot::FillLoot(lootid, LootTemplates_Creature, ...) [Unit.cpp:14067]
 │   │                                                  [LootMgr.cpp:540]
 │   ├─ store.GetLootFor(lootId)
 │   └─ LootTemplate::Process(groupId=0, isTopLevel=true) [LootMgr.cpp:1667]
 │       ├─ Entries (未分组): 每个 LootStoreItem::Roll()   [311]
 │       │     ├─ reference → 递归 Referenced->Process
 │       │     └─ 普通物品  → Loot::AddItem               [481]
 │       │                    ├─ needs_quest → quest_items
 │       │                    └─ 否则      → items
 │       └─ Groups (分组): LootGroup::Process(iterations) [1409]
 │             └─ LootGroup::Roll()                       [1262]
 │                  ├─ ExplicitlyChanced: 单次 rand_chance(), 顺序减
 │                  └─ EqualChanced:   SelectRandomContainerElement
 ├─ Loot::generateMoneyLoot(mingold, maxgold)            [LootMgr.cpp:808]
 └─ creature->SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE)      [Unit.cpp:14204]

玩家打开尸体:
 HandleLootOpcode → Player::SendLoot                      [Player.cpp:7822]
 ├─ 算 permission
 ├─ 首次开 + 队伍: GroupLoot / NeedBeforeGreed / MasterLoot [Group.cpp:983/1139/1290]
 └─ SMSG_LOOT_RESPONSE (LootView)                          [LootMgr.cpp:974]

ROLL 阶段:
 CMSG_LOOT_ROLL → HandleLootRoll → CountRollVote          [Group.cpp:1339]
                  └─ CountTheRoll → urand(1,100)           [Group.cpp:1435]
                       └─ 胜者 StoreNewItem / 分解 / 标 rollWinnerGUID

ML 给物:
 CMSG_LOOT_MASTER_GIVE → HandleLootMasterGiveOpcode        [LootHandler.cpp:421]
                         └─ target->StoreNewItem(...)

玩家自己捡物:
 CMSG_AUTOSTORE_LOOT_ITEM → HandleAutostoreLootItemOpcode  [LootHandler.cpp:33]
                            └─ player->StoreLootItem(slot, loot)
```

---

## 14. 核心文件索引

| 文件 | 行数 | 内容 |
|---|---|---|
| `src/server/game/Loot/LootMgr.h` | 462 | 所有枚举、`Loot`/`LootTemplate`/`LootStore` 定义 |
| `src/server/game/Loot/LootMgr.cpp` | 2319 | FillLoot、Process、Roll、generateMoneyLoot、AddItem、AllowedForPlayer、LootView operator、加载函数 |
| `src/server/game/Loot/LootItemStorage.cpp/.h` | — | 物品容器持久化 |
| `src/server/game/Handlers/LootHandler.cpp` | 516 | HandleAutostoreLootItem、HandleLootMoney、HandleLoot、HandleLootRelease、HandleLootMasterGive |
| `src/server/game/Handlers/GroupHandler.cpp:494` | — | HandleLootRoll |
| `src/server/game/Groups/Group.cpp:830-1700` | — | SendLootStartRoll、GroupLoot、NeedBeforeGreed、MasterLoot、CountRollVote、CountTheRoll、EndRoll |
| `src/server/game/Groups/Group.h:48,149` | — | RollVote 枚举、Roll 类 |
| `src/server/game/Entities/Unit/Unit.cpp:13975-14210` | — | Unit::Kill（掉落触发点） |
| `src/server/game/Entities/Player/Player.cpp:7815-8213` | — | SendLoot / 权限逻辑 / 各种 loot 类型分派 |
| `src/server/game/Entities/Creature/Creature.h:231,246-249` | — | `Loot loot` 成员 + loot mode API |
| `src/server/game/Entities/GameObject/GameObject.h:229-232,247` | — | GO 同上 |
| `src/server/game/Conditions/ConditionMgr.cpp:1273-1421,1567-1911` | — | loot 条件分派、addToLootTemplate、校验 |
| `src/common/Utilities/Random.h:51-60` | — | rand_chance / roll_chance_f |

### 关键行号速查

| 含义 | 位置 |
|---|---|
| `LootStoreItem` 定义 | `LootMgr.h:127` |
| `LootItem` 定义 | `LootMgr.h:153` |
| `Loot` 定义 | `LootMgr.h:312` |
| `LootTemplate` + `LootGroup` 定义 | `LootMgr.h:244` / `LootMgr.cpp:94` |
| 13 个全局 Store 实例化 | `LootMgr.cpp:44-56` |
| 数据库 SQL 查询 | `LootMgr.cpp:151` |
| `LootStoreItem::Roll` | `LootMgr.cpp:311` |
| `LootItem::AllowedForPlayer` | `LootMgr.cpp:417` |
| `Loot::AddItem` | `LootMgr.cpp:481` |
| `Loot::FillLoot` | `LootMgr.cpp:540` |
| `Loot::generateMoneyLoot` | `LootMgr.cpp:808` |
| `LootView` 序列化 | `LootMgr.cpp:974` |
| `LootGroup::Roll` | `LootMgr.cpp:1262` |
| `LootGroup::Process` | `LootMgr.cpp:1409` |
| `LootTemplate::Process` | `LootMgr.cpp:1667` |
| `LootTemplate::AddEntry` | `LootMgr.cpp:1532` |
| `Unit::Kill` 掉落生成 | `Unit.cpp:14060-14083` |
| `Player::SendLoot` 权限分派 | `Player.cpp:8138-8165` |
| `Group::GroupLoot` | `Group.cpp:983` |
| `Group::NeedBeforeGreed` | `Group.cpp:1139` |
| `Group::MasterLoot` | `Group.cpp:1290` |
| `Group::CountTheRoll` | `Group.cpp:1435` |

---

## 15. 常见配置 / 调试要点

### 15.1 配置一个简单掉落

```sql
-- 怪物 lootid=10001 永远掉 1 个 [亚麻布]
INSERT INTO creature_loot_template (Entry, Item, Reference, Chance, QuestRequired, LootMode, GroupId, MinCount, MaxCount)
VALUES (10001, 2589, 0, 100, 0, 1, 0, 1, 1);

-- 同一怪物有 5% 概率掉 [毛料], 1~3 个
INSERT INTO creature_loot_template VALUES (10001, 2592, 0, 5, 0, 1, 0, 1, 3, '');
```

### 15.2 配置一个分组掉落（三选一）

```sql
-- GroupId=1: 三件装备等概率选一件
INSERT INTO creature_loot_template VALUES
 (10002, 100001, 0, 0, 0, 1, 1, 1, 1, ''),   -- Chance=0 = 等概率
 (10002, 100002, 0, 0, 0, 1, 1, 1, 1, ''),
 (10002, 100003, 0, 0, 0, 1, 1, 1, 1, '');

-- 同怪物 GroupId=2: 50% 出金币包, 50% 出宝石 (显式概率)
INSERT INTO creature_loot_template VALUES
 (10002, 200001, 0, 50, 0, 1, 2, 1, 1, ''),
 (10002, 200002, 0, 50, 0, 1, 2, 1, 1, '');
```

> ⚠️ 显式概率总和应 ≤ 100%，否则永远走不到等概率回退。

### 15.3 用 reference 复用模板

```sql
-- 公共"垃圾物品池" entry=50000
INSERT INTO reference_loot_template VALUES
 (50000, 2589, 0, 30, 0, 1, 0, 1, 2, ''),   -- 亚麻布 30%
 (50000, 2592, 0, 10, 0, 1, 0, 1, 1, '');   -- 毛料 10%

-- 多个怪物引用同一个池
INSERT INTO creature_loot_template VALUES
 (10001, 0, 50000, 80, 0, 1, 0, 1, 1, ''),  -- 怪物A: 80% 概率走引用池, 处理 1 次
 (10002, 0, 50000, 80, 0, 1, 0, 1, 1, '');  -- 怪物B: 同上
```

### 15.4 调试技巧

- **掉率不对**：检查 `worldserver.conf` 的 `Rate.Drop.Item.*` 和 `Rate.Drop.Item.GroupAmount`
- **物品掉不出**：
  1. 检查 `creature_template.lootid` 是否正确指向 `creature_loot_template.Entry`
  2. 检查 `LootMode` 是否匹配 `creature->GetLootMode()`（默认 1）
  3. 启动日志里搜 `ERROR: ... loot template`，验证 `Verify()` 没报组概率和 > 101%
  4. 启动日志搜 `non-existing`、`unused` —— `LoadLootTemplates_*` 会报告孤立的 lootid
- **任务物品看不到**：检查玩家是否真的接了对应任务（`HasQuestForItem`）
- **ML 给不了**：`HandleLootMasterGiveOpcode` 会调 `AllowedForPlayer`，阵营 / 职业 / 唯一限制都会拦
- **超 18 / 32 上限**：客户端硬限制，超出部分被丢弃；想多掉只能拆栈（依赖物品 `GetMaxStackSize`）

### 15.5 常用 GM 命令

| 命令 | 用途 |
|---|---|
| `.lookup item <name>` | 查物品 id |
| `.additem <itemid> <count>` | 直接给自己加物品（绕过掉落） |
| `.npc info` | 查看当前目标 `lootid`、`mingold/maxgold` |
| `.reload creature_loot_template` | 热重载掉落表（部分模块支持） |

---

## 附录：流程图

```
┌─────────────────────────────────────────────────────────────────┐
│                       启动阶段                                   │
│  LoadLootTables() → 13 张表 → 13 个 LootStore                   │
│  每行 → LootStoreItem → LootTemplate.AddEntry                    │
│  分组(groupid>0) → Groups[gid-1] → Explicitly/EqualChanced       │
│  Verify() 校验组概率和 / 交叉校验 owner 表 / ReportUnusedIds     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    运行时：生成阶段 (击杀瞬间)                    │
│  Unit::Kill                                                      │
│   └─ Loot::FillLoot(lootid, store, looter, ...)                  │
│        └─ LootTemplate::Process                                  │
│             ├─ Entries[] 每个 LootStoreItem::Roll()               │
│             │     ├─ 必掉 (Chance>=100)                           │
│             │     ├─ reference → 递归                             │
│             │     └─ roll_chance_f(chance * qualityRate)         │
│             └─ Groups[] 每组 LootGroup::Process                  │
│                  └─ LootGroup::Roll                              │
│                       ├─ ExplicitlyChanced: 累减 rand_chance()   │
│                       └─ EqualChanced: 随机选                    │
│   └─ Loot::generateMoneyLoot                                     │
│   └─ SetDynamicFlag(UNIT_DYNFLAG_LOOTABLE)                       │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                运行时：打开阶段 (玩家点尸体)                       │
│  Player::SendLoot                                                │
│   ├─ 算 PermissionTypes (按 LootMethod + 是否 ML)                │
│   ├─ 首次开 + 队伍 → GroupLoot / NeedBeforeGreed / MasterLoot    │
│   │     ├─ GroupLoot: 设 is_blocked, 广播 SendLootStartRoll (60s)│
│   │     ├─ NBG: 同上 + NEED 限制 + 分解选项                      │
│   │     └─ MasterLoot: 设 is_blocked, 发 SMSG_LOOT_MASTER_LIST   │
│   └─ SMSG_LOOT_RESPONSE (LootView)                              │
│        每 item → LootSlotType 由 permission + 状态决定           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                运行时：交互阶段                                    │
│  CMSG_LOOT_ROLL → CountRollVote → 全员投完 → CountTheRoll        │
│       └─ urand(1,100) NEED 优先 > GREED/DISENCHANT              │
│       └─ 胜者 StoreNewItem / AutoStoreLoot(分解)                 │
│  CMSG_LOOT_MASTER_GIVE → 校验 → target->StoreNewItem            │
│  CMSG_AUTOSTORE_LOOT_ITEM → player->StoreLootItem                │
│  CMSG_LOOT_MONEY → 队伍均分 gold                                 │
└─────────────────────────────────────────────────────────────────┘
```
