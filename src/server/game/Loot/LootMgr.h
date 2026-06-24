/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ACORE_LOOTMGR_H
#define ACORE_LOOTMGR_H

#include "ByteBuffer.h"           // 用于 LootView 序列化到 SMSG_LOOT_RESPONSE
#include "ConditionMgr.h"         // ConditionList，用于掉落条件
#include "ObjectGuid.h"           // ObjectGuid，玩家/物品 GUID
#include "RefMgr.h"               // RefMgr，用于 LootValidatorRefMgr
#include "SharedDefines.h"        // LOOT_MODE_DEFAULT、ItemQuality 等
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

// ============================================================================
// 玩家在 ROLL 物品时可选的投票类型（Need / Greed / Disenchant / Pass）
// ============================================================================
enum RollType
{
    ROLL_PASS                           = 0,   // 放弃
    ROLL_NEED                           = 1,   // 需求（最高优先级）
    ROLL_GREED                          = 2,   // 贪婪
    ROLL_DISENCHANT                     = 3,   // 分解
    MAX_ROLL_TYPE                       = 4
};

// ROLL 选项的位掩码：用于构造"该玩家可用的投票选项"
// 客户端发的 SMSG_LOOT_START_ROLL_PERMISSIVE 里也用这个
enum RollMask
{
    ROLL_FLAG_TYPE_PASS                 = 0x01,
    ROLL_FLAG_TYPE_NEED                 = 0x02,
    ROLL_FLAG_TYPE_GREED                = 0x04,
    ROLL_FLAG_TYPE_DISENCHANT           = 0x08,

    ROLL_ALL_TYPE_NO_DISENCHANT         = 0x07, // PASS|NEED|GREED
    ROLL_ALL_TYPE_MASK                  = 0x0F  // 全部
};

// 客户端硬限制：单次掉落窗口最多显示 18 个物品（3.3.5a 客户端限制）
#define MAX_NR_LOOT_ITEMS 18
// note: the client cannot show more than 18 items in the loot window on 3.3.5a
// 任务物品预留上限 32（与显示无关，仅用于 vector reserve）
#define MAX_NR_QUEST_ITEMS 32
// unrelated to the number of quest items shown, just for reserve

// ============================================================================
// 队伍 loot 分配方式（由队长设置）
// ============================================================================
enum LootMethod
{
    FREE_FOR_ALL                        = 0,   // 所有人都能捡所有物品
    ROUND_ROBIN                         = 1,   // 轮流捡普通物品
    MASTER_LOOT                         = 2,   // 主分配者（ML）决定
    GROUP_LOOT                          = 3,   // 阈值之上 ROLL（greed/need/disenchant）
    NEED_BEFORE_GREED                   = 4    // 同上但 NEED 受职业/可用性限制
};

// ============================================================================
// 玩家打开 loot 窗口时被赋予的权限（在 Player::SendLoot 中按 LootMethod 决定）
// 决定该玩家在 LootView 中能看到什么、能拿什么
// ============================================================================
enum PermissionTypes
{
    ALL_PERMISSION                      = 0,   // 任意物品都能捡（FFA / 单人）
    GROUP_PERMISSION                    = 1,   // 队伍权限（参与 ROLL）
    MASTER_PERMISSION                   = 2,   // 主分配者本人
    RESTRICTED_PERMISSION               = 3,   // 受限（非 ML 看到的 ML 物品=锁定）
    ROUND_ROBIN_PERMISSION              = 4,   // 轮流捡的权限
    OWNER_PERMISSION                    = 5,   // 物品所有者（已 ROLL 到手）
    NONE_PERMISSION                     = 6    // 无权限
};

// ============================================================================
// SMSG_LOOT_RESPONSE 里携带的 loot 类型（影响客户端 UI 行为）
// ============================================================================
enum LootType
{
    LOOT_NONE                           = 0,   // 空 loot

    LOOT_CORPSE                         = 1,   // 尸体
    LOOT_PICKPOCKETING                  = 2,   // 偷窃
    LOOT_FISHING                        = 3,   // 钓鱼
    LOOT_DISENCHANTING                  = 4,   // 分解
    // ignored always by client
    LOOT_SKINNING                       = 6,   // 剥皮
    LOOT_PROSPECTING                    = 7,   // 选矿
    LOOT_MILLING                        = 8,   // 研磨

    LOOT_FISHINGHOLE                    = 20,   // unsupported by client, sending LOOT_FISHING instead
    LOOT_INSIGNIA                       = 21,   // unsupported by client, sending LOOT_CORPSE instead
    LOOT_FISHING_JUNK                   = 22    // unsupported by client, sending LOOT_FISHING instead
};

// ============================================================================
// SMSG_LOOT_RESPONSE 失败时返回的错误码（与客户端错误提示对应）
// ============================================================================
enum LootError
{
    LOOT_ERROR_DIDNT_KILL               = 0,    // You don't have permission to loot that corpse.
    LOOT_ERROR_TOO_FAR                  = 4,    // You are too far away to loot that corpse.
    LOOT_ERROR_BAD_FACING               = 5,    // You must be facing the corpse to loot it.
    LOOT_ERROR_LOCKED                   = 6,    // Someone is already looting that corpse.
    LOOT_ERROR_NOTSTANDING              = 8,    // You need to be standing up to loot something!
    LOOT_ERROR_STUNNED                  = 9,    // You can't loot anything while stunned!
    LOOT_ERROR_PLAYER_NOT_FOUND         = 10,   // Player not found
    LOOT_ERROR_PLAY_TIME_EXCEEDED       = 11,   // Maximum play time exceeded
    LOOT_ERROR_MASTER_INV_FULL          = 12,   // That player's inventory is full
    LOOT_ERROR_MASTER_UNIQUE_ITEM       = 13,   // Player has too many of that item already
    LOOT_ERROR_MASTER_OTHER             = 14,   // Can't assign item to that player
    LOOT_ERROR_ALREADY_PICKPOCKETED     = 15,   // Your target has already had its pockets picked
    LOOT_ERROR_NOT_WHILE_SHAPESHIFTED   = 16    // You can't do that while shapeshifted.
};

// ============================================================================
// 每个 loot 槽位的状态（决定该槽在客户端窗口里显示的颜色和可否点）
// ============================================================================
// type of Loot Item in Loot View
enum LootSlotType
{
    LOOT_SLOT_TYPE_ALLOW_LOOT           = 0,    // player can loot the item.    // 可捡
    LOOT_SLOT_TYPE_ROLL_ONGOING         = 1,    // roll is ongoing. player cannot loot.    // ROLL 中
    LOOT_SLOT_TYPE_MASTER               = 2,    // item can only be distributed by group loot master.    // 仅 ML 可见
    LOOT_SLOT_TYPE_LOCKED               = 3,    // item is shown in red. player cannot loot.    // 红色锁定
    LOOT_SLOT_TYPE_OWNER                = 4,    // ignore binding confirmation and etc, for single player looting    // ROLL 到手
};

class Player;
class LootStore;
class ConditionMgr;
class GameObject;
struct Loot;

// ============================================================================
// LootStoreItem：从数据库一行 *_loot_template 直接映射来的"模板定义"
// 描述"这个物品应该怎么 roll / 怎么掉"
//
// ★ 三种 roll 模式由 (reference, groupid, chance) 三个字段的组合决定：
//   ┌─────────┬──────────┬────────────┬───────────────────────────────────────┐
//   │reference│ groupid  │  chance    │   行为                                │
//   ├─────────┼──────────┼────────────┼───────────────────────────────────────┤
//   │  == 0   │   == 0   │  >0        │  模式一: 单物品独立 roll              │
//   │  == 0   │   >0     │  >0 或 ==0 │  模式二: 进组，组内互斥 roll          │
//   │  != 0   │   == 0   │  >0        │  引用: roll 自身概率, 命中后递归       │
//   │  != 0   │   >0     │  任意      │  组内引用 (跳进引用模板的指定 group)  │
//   └─────────┴──────────┴────────────┴───────────────────────────────────────┘
//   注: 组内 chance==0 表示等概率候选 (EqualChanced), 显式总和不中时才回退到它
// ============================================================================
struct LootStoreItem
{
    uint32  itemid;                             // id of the item    // 物品 id（或引用目标 id）
    int32   reference;                          // referenced TemplateleId    // 非 0 = 指向 reference_loot_template 的某 entry；abs() 后才是 entry（负值含义已废弃但仍兼容）
    float   chance;                             // chance to drop for both quest and non-quest items, chance to be used for refs    // 掉率；组内 0 = 等概率（只能与 groupid>0 搭配）
    bool    needs_quest : 1;                    // quest drop (quest is required for item to drop)    // 任务物品；对引用无意义（启动会警告）
    uint16  lootmode;                           // 位掩码（与 creature/go.GetLootMode() 做 &，0 永远掉不出，加载时强制改 1）
    uint8   groupid     : 7;                    // 组 id（0 = 未分组，>0 进入 Groups[gid-1]；位宽 7，上限 127）
    uint8   mincount;                           // mincount for drop items    // 最少掉几个（普通物品的堆叠下限）
    uint8   maxcount;                           // max drop count for the item mincount or Ref multiplicator    // 最多掉几个（普通=堆叠上限）；引用时是递归处理次数倍数
    ConditionList conditions;                   // additional loot condition    // 附加条件（来自 conditions 表；★注意：条件不参与 roll，只在 AllowedForPlayer 过滤可见性）

    // Constructor
    // displayid is filled in IsValid() which must be called after
    // 构造函数：参数顺序与数据库列一一对应
    LootStoreItem(uint32 _itemid, int32 _reference, float _chance, bool _needs_quest, uint16 _lootmode, uint8 _groupid, int32 _mincount, uint8 _maxcount)
        : itemid(_itemid), reference(_reference), chance(_chance), needs_quest(_needs_quest),
          lootmode(_lootmode), groupid(_groupid), mincount(_mincount), maxcount(_maxcount)
    {}

    // Checks if the entry takes it's chance (at loot generation)
    // 单物品 roll 判定（模式一）：按 chance × 品质修正系数 后摇骰
    // ★ 注意：rate 参数来自 LootStore::IsRatesAllowed()，Mail/Reference/Spell 恒为 false
    bool Roll(bool rate, Player const* player, Loot& loot, LootStore const& store) const;
    [[nodiscard]] bool IsValid(LootStore const& store, uint32 entry) const;
    // Checks correctness of values    // 启动时校验数据库行合法性（chance/groupid/mincount/maxcount/reference 的交叉校验）
};

// 一组 GUID（玩家），表示哪些玩家可拾取某个物品
typedef GuidSet AllowedLooterSet;

// ============================================================================
// LootItem：已经 roll 出来、放入 Loot 实例的运行时物品
// 由 LootStoreItem 在 Loot::AddItem 中实例化，并附加了运行时状态
//
// ★ 标志位组合的含义（决定物品在 LootView 中如何呈现）：
//   is_blocked + (rollWinnerGUID)            → 阈值之上/ROLL 中/ML 持有
//   !is_blocked + rollWinnerGUID==自己       → 已 ROLL 到手，等背包有空再捡
//   is_underthreshold                        → 走 round-robin，绕过 ROLL
//   freeforall                               → 每个玩家独立可拿一份（FFA）
//   follow_loot_rules                        → 即使是任务/条件物品也参与队伍规则
//   needs_quest                              → 必须玩家身上有对应任务才能看见
//
// ★ unlootedCount 三处计数点的协调（避免漏计/重计）：
//   - AddItem:           非任务 + 无条件 + 非FFA 的"普通个人物品"
//   - FillFFALoot:       FFA 物品（每个玩家独立计，每人 +1）
//   - FillQuestLoot:     非 FFA 任务物品（用 is_counted 防重计，只计一次）
//   - FillNonQuestNonFFAConditionalLoot: 带 conditions 的非FFA物品（同上，is_counted 防重）
// ============================================================================
struct LootItem
{
    uint32  itemid;                             // 物品 id
    uint32  itemIndex;                          // 在 items[] / quest_items[] 中的索引（发包用）
    uint32  randomSuffix;                       // 随机附魔后缀因子（由 GenerateEnchSuffixFactor 算出）
    int32   randomPropertyId;                   // 随机属性 id（item_template.RandomProperty，由 GenerateItemRandomPropertyId 算出）
    ConditionList conditions;                               // additional loot condition    // 附加条件（来自 conditions 表）
    AllowedLooterSet allowedGUIDs;              // 允许拾取的玩家集合（条件/任务过滤后填充；用于 ML 分配校验）
    ObjectGuid rollWinnerGUID;                              // Stores the guid of person who won loot, if his bags are full only he can see the item in loot list!    // ROLL 胜者 GUID（背包满后仅胜者可见，其他人隐藏整行）
    uint8   count             : 8;              // 当前堆叠数量（AddItem 时按 maxStackSize 拆分后赋值）
    bool    is_looted         : 1;              // 已被拾取（普通物品全局；FFA 按玩家计则看 QuestItem.is_looted）
    bool    is_blocked        : 1;              // 锁定（ROLL 中 / ML 持有 / 阈值之上）：决定 LootView 显示 ROLL_ONGOING/MASTER/LOCKED
    bool    freeforall        : 1;                          // free for all    // FFA（人人可拾取，源自物品模板 ITEM_FLAG_MULTI_DROP）
    bool    is_underthreshold : 1;              // 低于队伍 loot 阈值（quality < threshold）：走 round-robin，不参与 ROLL
    bool    is_counted        : 1;              // 是否已计入 unlootedCount（任务/条件物品防重计标志）
    bool    needs_quest       : 1;                          // quest drop    // 任务物品（决定进 quest_items[] 还是 items[]）
    bool    follow_loot_rules : 1;              // 是否遵循队伍 loot 规则（源自物品自定义标志 ITEM_FLAGS_CU_FOLLOW_LOOT_RULES）
    uint8   groupid           : 7;              // 所属组 id（用于 LootGroupInvalidSelector 判断重复掉落是否超额）

    // Constructor, copies most fields from LootStoreItem, generates random count and random suffixes/properties
    // Should be called for non-reference LootStoreItem entries only (reference = 0)
    // 构造函数：从 LootStoreItem 拷贝模板信息，生成随机属性，初始化标志位
    // ★ 注意：count 在这里设为 0，真正的数量在 Loot::AddItem 中按 urand(min,max) 赋值
    explicit LootItem(LootStoreItem const& li);

    LootItem() = default;

    // Basic checks for player/item compatibility - if false no chance to see the item in the loot
    // 玩家能否看到/拾取此物品的总闸（顺序很重要，前置失败直接 return）：
    //   1) 物品模板存在
    //   2) 未被 DisableMgr 屏蔽（DISABLE_TYPE_LOOT）
    //   3) conditions 表条件全部满足（★ 关键：条件只过滤可见性，不影响 roll）
    //   4) 阵营匹配（部落/联盟限定物品）
    //   5) 专业/配方：有专业且未学会该配方
    //   6) 任务物品：玩家身上有对应任务且需要这个 item（除非物品 IGNORE_QUEST_STATUS）
    //   7) 任务起始物品：未接/未完成/未达上限/前置已完成
    //   8) 脚本钩子 OnAllowedForPlayerLootCheck 通过
    // 被 FillFFALoot/FillQuestLoot/FillNonQuestNonFFAConditionalLoot/LootView/CanRollOnItem/HandleLootMasterGiveOpcode 共用
    bool AllowedForPlayer(Player const* player, ObjectGuid source) const;
    void AddAllowedLooter(Player const* player);
    [[nodiscard]] const AllowedLooterSet& GetAllowedLooters() const { return allowedGUIDs; }
};

// ============================================================================
// QuestItem：一个"槽位引用"，指向 Loot::quest_items / items 中的某条
// 配合 PlayerQuestItems / PlayerFFAItems / PlayerNonQuestNonFFAConditionalItems 三个映射使用
// ============================================================================
struct QuestItem
{
    uint8   index{0};                                          // position in quest_items;    // 指向 Loot 中的下标
    bool    is_looted{false};                                  // 该玩家是否已捡此槽

    QuestItem()
         = default;

    QuestItem(uint8 _index, bool _islooted = false)
        : index(_index), is_looted(_islooted) {}
};

class LootTemplate;

// 单玩家的"任务/FFA/条件可见槽位列表"
typedef std::vector<QuestItem> QuestItemList;
// 一具尸体里的全部普通物品
typedef std::vector<LootItem> LootItemList;
// 玩家 GUID -> 该玩家可见的任务/FFA/条件槽列表
typedef std::map<ObjectGuid, QuestItemList*> QuestItemMap;
// 单个模板的条目列表（list 因为要 remove_if）
typedef std::list<LootStoreItem*> LootStoreItemList;
// lootid -> LootTemplate
typedef std::unordered_map<uint32, LootTemplate*> LootTemplateMap;

// 启动时收集到的所有 lootid 集合（用于 ReportUnusedIds）
typedef std::set<uint32> LootIdSet;

// ============================================================================
// LootStore：一张 *_loot_template 表的内存抽象
// 启动时 LoadAndCollectLootIds() 把整张表加载到 m_LootTemplates
// 运行时 GetLootFor() 提供按 lootid 查询
//
// ★ 生命周期：
//   启动 → LoadAndCollectLootIds (SQL→内存) → Verify (组概率校验)
//        → ConditionMgr 调 ResetConditions + addConditionItem (挂条件)
//        → CheckLootRefs (引用存在性校验) → ReportUnusedIds (孤立 lootid 报告)
//   运行时 → GetLootFor 响应 Loot::FillLoot 查询
//   reload → Clear + 重复上述流程
// ============================================================================
class LootStore
{
public:
    // name: 表名（"creature_loot_template"等），用于 SQL 查询和日志
    // entryName: 描述 entry 含义的字符串（如"creature entry"），用于错误报告
    // ratesAllowed: 是否在 Roll 时应用品质掉率修正（Mail/Reference/Spell = false）
    explicit LootStore(char const* name, char const* entryName, bool ratesAllowed)
        : m_name(name), m_entryName(entryName), m_ratesAllowed(ratesAllowed) {}

    virtual ~LootStore() { Clear(); }

    // 启动时调用：加载 SQL 表 + 收集所有 lootid 到 ids_set
    uint32 LoadAndCollectLootIds(LootIdSet& ids_set);
    // 重置所有条目的 conditions（reload conditions 时调用）
    void ResetConditions();

    // 校验整个 Store 的模板完整性（组概率总和、引用存在等）
    void Verify() const;
    // 检查引用目标是否存在；存在的从 ref_set 中移除
    void CheckLootRefs(LootIdSet* ref_set = nullptr) const; // check existence reference and remove it from ref_set
    // 报告 ids_set 中剩余的（即没被任何东西引用的）lootid
    void ReportUnusedIds(LootIdSet const& ids_set) const;
    void ReportNonExistingId(uint32 lootId) const;
    void ReportNonExistingId(uint32 lootId, const char* ownerType, uint32 ownerId) const;
    void ReportInvalidCount(uint32 lootId, const char* ownerType, uint32 ownerId, uint32 itemId, uint8 minCount, uint8 maxCount) const;

    // 是否存在指定 lootid
    [[nodiscard]] bool HaveLootFor(uint32 loot_id) const { return m_LootTemplates.find(loot_id) != m_LootTemplates.end(); }
    // 该模板里是否含至少一个任务掉落
    [[nodiscard]] bool HaveQuestLootFor(uint32 loot_id) const;
    // 该模板里是否含至少一个该玩家当前任务需要的掉落
    bool HaveQuestLootForPlayer(uint32 loot_id, Player const* player) const;

    // 按 lootid 拿模板（const 版本，运行时生成 loot 用）
    [[nodiscard]] LootTemplate const* GetLootFor(uint32 loot_id) const;
    // 非 const 版本：ConditionMgr 加载条件时用（需要修改模板）
    [[nodiscard]] LootTemplate* GetLootForConditionFill(uint32 loot_id) const;

    [[nodiscard]] char const* GetName() const { return m_name; }
    [[nodiscard]] char const* GetEntryName() const { return m_entryName; }
    [[nodiscard]] bool IsRatesAllowed() const { return m_ratesAllowed; }
protected:
    // 真正执行 SQL 加载
    uint32 LoadLootTable();
    // 清空（析构 / reload 时）
    void Clear();
private:
    LootTemplateMap m_LootTemplates;    // 核心：lootid -> LootTemplate
    char const* m_name;                 // 表名
    char const* m_entryName;            // entry 含义
    bool m_ratesAllowed;                // 是否应用品质掉率修正
};

// ============================================================================
// LootTemplate：一个 lootid 对应的模板，内含未分组条目 + 多个分组
// 启动时由 AddEntry 装配，运行时由 Process() 完成所有 roll
//
// ★ 数据结构：
//   Entries[]      ← groupid==0 的行（每个独立 roll，模式一）
//   Groups[]       ← groupid>0 的行（按 gid-1 索引）
//     └─ LootGroup 内部再分 ExplicitlyChanced / EqualChanced
//
// ★ Process() 的递归调用关系（可能很深）：
//   Process(isTopLevel=true)
//     ├─ Entries[].Roll → 引用命中 → Referenced->Process(isTopLevel=false) [递归]
//     │                                       ├─ 引用内部 Entries [再递归]
//     │                                       └─ 引用内部 Groups
//     └─ Groups[i].Process(groupAmount)
//         └─ LootGroup::Roll → 命中 → loot.AddItem
//                            → 非任务物品 + iterationsLeft>1 → Process(iterationsLeft-1) [递归]
//
// ★ 引用支持嵌套：reference_loot_template 可以再引用另一个 reference_loot_template
//   但通过 isTopLevel=false 防止 GroupAmount 被重复放大（避免指数爆炸）
// ============================================================================
class LootTemplate
{
    class LootGroup;                                       // A set of loot definitions for items (refs are not allowed inside)    // 内部类：一组条目（组内禁止引用）
    typedef std::vector<LootGroup*> LootGroups;

public:
    LootTemplate() = default;
    ~LootTemplate();

    // Adds an entry to the group (at loading stage)
    // 加载时调用：按 groupid 把行分到 Entries 或 Groups[gid-1]
    void AddEntry(LootStoreItem* item);
    // Rolls for every item in the template and adds the rolled items the the loot
    // 核心：遍历所有条目和组执行 roll，命中则调 Loot::AddItem 落袋
    // groupId != 0 时只处理指定组（用于引用指向具体组）
    // isTopLevel=true 时应用 RATE_DROP_ITEM_GROUP_AMOUNT
    void Process(Loot& loot, LootStore const& store, uint16 lootMode, Player const* player, uint8 groupId = 0, bool isTopLevel = true) const;
    // 用 conditions 列表重置所有条目条件（reload 用）
    void CopyConditions(ConditionList conditions);
    // 把条件复制到指定 LootItem（LootItemStorage 还原时用，按 conditionLootId 精确匹配）
    bool CopyConditions(LootItem* li, uint32 conditionLootId = 0) const;

    // True if template includes at least 1 quest drop entry
    // 含至少一个任务掉落（递归查引用）
    [[nodiscard]] bool HasQuestDrop(LootTemplateMap const& store) const;
    // True if template includes at least 1 quest drop for an active quest of the player
    bool HasQuestDropForPlayer(LootTemplateMap const& store, Player const* player) const;

    // Checks integrity of the template
    // 启动时校验（组概率和等）
    void Verify(LootStore const& store, uint32 Id) const;
    // 检查所有引用目标是否存在
    void CheckLootRefs(LootStore const& lootstore, uint32 Id, LootIdSet* ref_set) const;
    // ConditionMgr 调用：把 cond 挂到所有 itemid==SourceEntry 的条目上
    bool addConditionItem(Condition* cond);
    // 该 itemid 是否是引用
    [[nodiscard]] bool isReference(uint32 id) const;

private:
    LootStoreItemList Entries;                          // not grouped only    // 未分组条目（每个独立 roll）
    LootGroups        Groups;                           // groups have own (optimised) processing, grouped entries go there    // 分组条目（每组只产出一项）

    // Objects of this class must never be copied, we are storing pointers in container
    LootTemplate(LootTemplate const&);
    LootTemplate& operator=(LootTemplate const&);
};

//=====================================================

// ============================================================================
// LootValidatorRef / LootValidatorRefMgr：引用计数式监听器
// Group 在创建 Roll 时会通过这个机制注册自己对某 Loot 的引用，
// 当 Loot 失效（被 clear / 析构）时所有引用会被通知，避免悬空指针
// ============================================================================

class LootValidatorRef :  public Reference<Loot, LootValidatorRef>
{
public:
    LootValidatorRef() = default;
    void targetObjectDestroyLink() override {}
    void sourceObjectDestroyLink() override {}
};

//=====================================================

class LootValidatorRefMgr : public RefMgr<Loot, LootValidatorRef>
{
public:
    typedef LinkedListHead::Iterator< LootValidatorRef > iterator;

    LootValidatorRef* getFirst() { return (LootValidatorRef*)RefMgr<Loot, LootValidatorRef>::getFirst(); }
    LootValidatorRef* getLast() { return (LootValidatorRef*)RefMgr<Loot, LootValidatorRef>::getLast(); }

    iterator begin() { return iterator(getFirst()); }
    iterator end() { return iterator(nullptr); }
    iterator rbegin() { return iterator(getLast()); }
    iterator rend() { return iterator(nullptr); }
};

//=====================================================
struct LootView;

// 序列化函数：LootItem / LootView 写入 ByteBuffer，用于发 SMSG_LOOT_RESPONSE
ByteBuffer& operator<<(ByteBuffer& b, LootItem const& li);
ByteBuffer& operator<<(ByteBuffer& b, LootView const& lv);

// ============================================================================
// Loot：每具尸体 / 每个 GO / 每个物品容器 / 每封邮件的运行时掉落实例
// 在 Unit::Kill / Player::SendLoot 等处由 FillLoot() 填充
//
// ★ 数据流：FillLoot → LootTemplate::Process → LootGroup::Roll/AddItem → items/quest_items
//          → FillNotNormalLootFor → PlayerQuestItems/PlayerFFAItems/PlayerNonQuestNonFFAConditionalItems
//          → LootView operator<< → SMSG_LOOT_RESPONSE
//
// ★ items vs quest_items 分流：
//   needs_quest=true → quest_items (上限 32, 发包时排在 items 之后, slot = items.size()+i)
//   needs_quest=false → items     (上限 18, 客户端窗口硬限制)
//
// ★ 三个按玩家的可见性映射（每个玩家打开 loot 窗口前由 FillNotNormalLootFor 构造一次）：
//   PlayerQuestItems:                       任务槽列表（玩家有对应任务才出现）
//   PlayerFFAItems:                         FFA 槽列表（每玩家独立可见，freeforall=true）
//   PlayerNonQuestNonFFAConditionalItems:   条件槽列表（带 conditions 但非任务非FFA）
//   普通物品（非任务/非FFA/无条件）不进映射，直接在 LootView 中按 permission 显示
// ============================================================================
struct Loot
{
    // 让 LootView operator<< 能访问私有成员
    friend ByteBuffer& operator<<(ByteBuffer& b, LootView const& lv);

    // 三个按玩家的可见性映射（外部的 Group/Player 用这些读"我能看到什么"）
    [[nodiscard]] QuestItemMap const& GetPlayerQuestItems() const { return PlayerQuestItems; }
    [[nodiscard]] QuestItemMap const& GetPlayerFFAItems() const { return PlayerFFAItems; }
    [[nodiscard]] QuestItemMap const& GetPlayerNonQuestNonFFAConditionalItems() const { return PlayerNonQuestNonFFAConditionalItems; }

    std::vector<LootItem> items;         // 普通掉落（含 FFA、条件物品），上限 18
    std::vector<LootItem> quest_items;   // 任务掉落，上限 32
    uint32 gold;                         // 金钱（generateMoneyLoot 填充；拾取时队伍均分）
    uint8 unlootedCount{0};              // 尚未拾取的物品数（含每位玩家的 FFA/任务/条件，详见 LootItem 注释）
    ObjectGuid roundRobinPlayer;        // GUID of the player having the Round-Robin ownership for the loot. If 0, round robin owner has released.    // 当前 round-robin 持有者；Clear() 后为 0 表示已释放
    ObjectGuid lootOwnerGUID;            // loot 所有者（被认证的击杀者/捡取者；AddItem 中据此找 group/player 决定可见性计数）
    LootType loot_type{LOOT_NONE};      // required for achievement system    // loot 类型（成就系统要用）

    // GUID of container that holds this loot (item_instance.entry), set for items that can be looted
    ObjectGuid containerGUID;            // 容器 GUID（物品容器的 item_instance.guid，用于 LootItemStorage 持久化；也是 FFA AllowedForPlayer 的 source 参数）
    ObjectGuid sourceWorldObjectGUID;    // loot 源（尸体/GO/物品）的 GUID；AllowedForPlayer 的 source 参数
    GameObject* sourceGameObject{nullptr};

    Loot(uint32 _gold = 0) : gold(_gold) { }
    ~Loot() { clear(); }

    // if loot becomes invalid this reference is used to inform the listener
    // 注册监听器（Group 的 Roll 用）：当 Loot 被 clear/析构时通知所有注册的 Roll 失效
    void addLootValidatorRef(LootValidatorRef* pLootValidatorRef)
    {
        i_LootValidatorRefMgr.insertFirst(pLootValidatorRef);
    }

    // void clear();
    // 清空所有数据并释放每玩家的 QuestItemList 内存
    void clear()
    {
        for (QuestItemMap::const_iterator itr = PlayerQuestItems.begin(); itr != PlayerQuestItems.end(); ++itr)
            delete itr->second;
        PlayerQuestItems.clear();

        for (QuestItemMap::const_iterator itr = PlayerFFAItems.begin(); itr != PlayerFFAItems.end(); ++itr)
            delete itr->second;
        PlayerFFAItems.clear();

        for (QuestItemMap::const_iterator itr = PlayerNonQuestNonFFAConditionalItems.begin(); itr != PlayerNonQuestNonFFAConditionalItems.end(); ++itr)
            delete itr->second;
        PlayerNonQuestNonFFAConditionalItems.clear();

        PlayersLooting.clear();
        items.clear();
        quest_items.clear();
        gold = 0;
        unlootedCount = 0;
        roundRobinPlayer.Clear();
        i_LootValidatorRefMgr.clearReferences();
        loot_type = LOOT_NONE;
    }

    // 是否完全空（无物品无金钱）
    [[nodiscard]] bool empty() const { return items.empty() && gold == 0; }
    // 是否被捡光（无金钱 + unlootedCount=0）
    [[nodiscard]] bool isLooted() const { return gold == 0 && unlootedCount == 0; }

    // 通知所有正在 loot 的玩家：某槽位物品被取走（更新客户端窗口）
    void NotifyItemRemoved(uint8 lootIndex);
    void NotifyQuestItemRemoved(uint8 questIndex);
    void NotifyMoneyRemoved();
    // 注册/注销"正在打开 loot 窗口的玩家"
    void AddLooter(ObjectGuid GUID) { PlayersLooting.insert(GUID); }
    void RemoveLooter(ObjectGuid GUID) { PlayersLooting.erase(GUID); }

    // 按区间生成金钱并应用 RATE_DROP_MONEY
    void generateMoneyLoot(uint32 minAmount, uint32 maxAmount);
    // 入口：查模板 + 调 Process + 设置每玩家可见性
    // personal=true 表示个人 loot（不走队伍规则）
    bool FillLoot(uint32 lootId, LootStore const& store, Player* lootOwner, bool personal, bool noEmptyError = false, uint16 lootMode = LOOT_MODE_DEFAULT, WorldObject* lootSource = nullptr);

    // Inserts the item into the loot (called by LootTemplate processors)
    // 把 LootStoreItem 转成 LootItem 并 push 到 items/quest_items（由 Process 回调）
    void AddItem(LootStoreItem const& item);

    // 按槽位查物品（处理任务/FFA/条件三种映射）；返回 nullptr 表示已捡或不属于该玩家
    LootItem* LootItemInSlot(uint32 lootslot, Player* player, QuestItem** qitem = nullptr, QuestItem** ffaitem = nullptr, QuestItem** conditem = nullptr);
    // 该玩家能看到的最大槽位数（items + 自己的任务槽）
    uint32 GetMaxSlotInLootFor(Player* player) const;
    // 是否还有任何玩家都能看到的物品（用于判断尸体是否还有公共 loot）
    [[nodiscard]] bool hasItemForAll() const;
    // 是否还有该玩家专属可见物品（任务/FFA/条件）
    bool hasItemFor(Player* player) const;
    // 是否还有任何高于阈值的物品（用于判断要不要触发队伍 ROLL）
    [[nodiscard]] bool hasOverThresholdItem() const;
    // 给某玩家构造三个可见性映射（任务/FFA/条件），并自动收取货币代币
    void FillNotNormalLootFor(Player* player);

private:
    // 三选一：分别为玩家构造 FFA / 任务 / 条件物品的可见槽列表
    QuestItemList* FillFFALoot(Player* player);
    QuestItemList* FillQuestLoot(Player* player);
    QuestItemList* FillNonQuestNonFFAConditionalLoot(Player* player);

    typedef GuidSet PlayersLootingSet;
    PlayersLootingSet PlayersLooting;                    // 当前打开 loot 窗口的玩家集合
    QuestItemMap PlayerQuestItems;                       // 玩家 -> 可见任务槽列表
    QuestItemMap PlayerFFAItems;                         // 玩家 -> 可见 FFA 槽列表
    QuestItemMap PlayerNonQuestNonFFAConditionalItems;   // 玩家 -> 可见条件槽列表

    // All rolls are registered here. They need to know, when the loot is not valid anymore
    LootValidatorRefMgr i_LootValidatorRefMgr;           // 引用管理器（Group 的 Roll 在此注册）
};

// ============================================================================
// LootView：把 Loot + 当前 viewer + 权限打包，用于序列化到 SMSG_LOOT_RESPONSE
// 同一个 Loot 不同玩家看到的 LootView 不同
// ============================================================================
struct LootView
{
    Loot& loot;
    Player* viewer;
    PermissionTypes permission;
    LootView(Loot& _loot, Player* _viewer, PermissionTypes _permission = ALL_PERMISSION)
        : loot(_loot), viewer(_viewer), permission(_permission) {}
};

// ============================================================================
// 13 个全局 LootStore 实例（声明；定义在 LootMgr.cpp:44-56）
// ============================================================================
extern LootStore LootTemplates_Creature;
extern LootStore LootTemplates_Fishing;
extern LootStore LootTemplates_Gameobject;
extern LootStore LootTemplates_Item;
extern LootStore LootTemplates_Mail;
extern LootStore LootTemplates_Milling;
extern LootStore LootTemplates_Pickpocketing;
extern LootStore LootTemplates_Reference;
extern LootStore LootTemplates_Skinning;
extern LootStore LootTemplates_Disenchant;
extern LootStore LootTemplates_Prospecting;
extern LootStore LootTemplates_Spell;
extern LootStore LootTemplates_Player;

// 各 Store 的加载函数（启动时由 LoadLootTables 统一调用）
// 每个 LoadLootTemplates_X 都会做：1) SQL 加载 2) 与 owner 表交叉校验 3) ReportUnusedIds
void LoadLootTemplates_Creature();
void LoadLootTemplates_Fishing();
void LoadLootTemplates_Gameobject();
void LoadLootTemplates_Item();
void LoadLootTemplates_Mail();
void LoadLootTemplates_Milling();
void LoadLootTemplates_Pickpocketing();
void LoadLootTemplates_Skinning();
void LoadLootTemplates_Disenchant();
void LoadLootTemplates_Prospecting();

void LoadLootTemplates_Spell();
void LoadLootTemplates_Reference();

void LoadLootTemplates_Player();

// 启动入口：依次加载所有表（worldserver 启动时调一次）
inline void LoadLootTables()
{
    LoadLootTemplates_Creature();
    LoadLootTemplates_Fishing();
    LoadLootTemplates_Gameobject();
    LoadLootTemplates_Item();
    LoadLootTemplates_Mail();
    LoadLootTemplates_Milling();
    LoadLootTemplates_Pickpocketing();
    LoadLootTemplates_Skinning();
    LoadLootTemplates_Disenchant();
    LoadLootTemplates_Prospecting();
    LoadLootTemplates_Spell();

    LoadLootTemplates_Reference();

    LoadLootTemplates_Player();
}

#endif
