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

#include "LootMgr.h"
#include "Containers.h"               // SelectRandomContainerElement
#include "DisableMgr.h"               // IsDisabledFor (DISABLE_TYPE_LOOT)
#include "Group.h"                    // Group / GroupReference
#include "ItemEnchantmentMgr.h"       // GenerateEnchSuffixFactor
#include "Log.h"
#include "ObjectMgr.h"                // GetItemTemplate / GetCreatureTemplates 等
#include "Player.h"
#include "ScriptMgr.h"                // 所有 On* 脚本钩子
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Util.h"                     // urand
#include "World.h"                    // getRate / sWorld

// ============================================================================
// 物品品质 -> worldserver.conf 中 Rate.Drop.Item.* 的映射表
// 索引值就是 ItemQuality 枚举（0=Grey 1=White 2=Green 3=Blue 4=Purple 5=Orange 6=Artifact）
// Heirloom（传家宝，品质=7）不参与修正，单独判断
// ============================================================================
ServerConfigs const qualityToRate[] =
{
    RATE_DROP_ITEM_POOR,                                    // ITEM_QUALITY_POOR    // 灰色（垃圾）
    RATE_DROP_ITEM_NORMAL,                                  // ITEM_QUALITY_NORMAL  // 白色（普通）
    RATE_DROP_ITEM_UNCOMMON,                                // ITEM_QUALITY_UNCOMMON// 绿色（优秀）
    RATE_DROP_ITEM_RARE,                                    // ITEM_QUALITY_RARE    // 蓝色（精良）
    RATE_DROP_ITEM_EPIC,                                    // ITEM_QUALITY_EPIC    // 紫色（史诗）
    RATE_DROP_ITEM_LEGENDARY,                               // ITEM_QUALITY_LEGENDARY// 橙色（传说）
    RATE_DROP_ITEM_ARTIFACT,                                // ITEM_QUALITY_ARTIFACT// 金色（神器）
};

// ============================================================================
// 13 个全局 LootStore 实例化（构造参数：表名, entry 描述, 是否应用品质 rate）
// Mail / Reference / Spell 的 ratesAllowed = false：这三个不应用 Rate.Drop.Item.* 修正
// ============================================================================
LootStore LootTemplates_Creature("creature_loot_template",           "creature entry",                  true);
LootStore LootTemplates_Disenchant("disenchant_loot_template",       "item disenchant id",              true);
LootStore LootTemplates_Fishing("fishing_loot_template",             "area id",                         true);
LootStore LootTemplates_Gameobject("gameobject_loot_template",       "gameobject entry",                true);
LootStore LootTemplates_Item("item_loot_template",                   "item entry",                      true);
LootStore LootTemplates_Mail("mail_loot_template",                   "mail template id",                false);
LootStore LootTemplates_Milling("milling_loot_template",             "item entry (herb)",               true);
LootStore LootTemplates_Pickpocketing("pickpocketing_loot_template", "creature pickpocket lootid",      true);
LootStore LootTemplates_Prospecting("prospecting_loot_template",     "item entry (ore)",                true);
LootStore LootTemplates_Reference("reference_loot_template",         "reference id",                    false);
LootStore LootTemplates_Skinning("skinning_loot_template",           "creature skinning id",            true);
LootStore LootTemplates_Spell("spell_loot_template",                 "spell id (random item creating)", false);
LootStore LootTemplates_Player("player_loot_template",               "team id",                         true);

// ============================================================================
// LootGroupInvalidSelector：用于在组 roll 之前过滤掉无效候选条目
// 用法：LootStoreItemList.remove_if(LootGroupInvalidSelector(loot, lootMode))
//
// ★ 设计目的：当 RATE_DROP_ITEM_GROUP_AMOUNT > 1 时一个组会被 roll 多次，
//   若不限制可能出现"一个怪掉 5 把相同装备"的异常情况。这里在每次 roll 前
//   动态扫描当前 Loot 已有的同 itemid 数量来决定是否继续允许该候选。
//
// ★ 重复上限规则（ Blizzard 设计，只对未分组前的候选生效）：
//   - InventoryType == 0 (非装备：消耗品/材料/垃圾等) → 最多掉 3 个
//   - InventoryType != 0 (装备：武器/护甲等)          → 最多掉 1 个
//   - 特例：Trophy of the Crusade (47242) 装备类但允许掉多个（副本设计需要）
// ============================================================================
// Selects invalid loot items to be removed from group possible entries (before rolling)
struct LootGroupInvalidSelector : public Acore::unary_function<LootStoreItem*, bool>
{
    explicit LootGroupInvalidSelector(Loot const& loot, uint16 lootMode) : _loot(loot), _lootMode(lootMode) { }

    bool operator()(LootStoreItem* item) const
    {
        // (1) lootmode 位掩码不匹配：剔除（常用作英雄/普通难度的差异化掉落）
        if (!(item->lootmode & _lootMode))
            return true;

        // (2) 普通条目（非引用）还要检查"已掉落重复次数"
        //     引用条目由其内部条目各自检查，这里跳过
        if (!item->reference)
        {
            ItemTemplate const* _proto = sObjectMgr->GetItemTemplate(item->itemid);
            if (!_proto)
                return true;

            // 统计当前 Loot 中已存在多少个同 itemid + 同 groupid 的物品
            // 注意：只统计同一 groupid 的，不同组的同名物品互不影响
            uint8 foundDuplicates = 0;
            for (std::vector<LootItem>::const_iterator itr = _loot.items.begin(); itr != _loot.items.end(); ++itr)
                if (itr->itemid == item->itemid && itr->groupid == item->groupid)
                {
                    ++foundDuplicates;
                    if (_proto->InventoryType == 0 && foundDuplicates == 3 && _proto->ItemId != 47242 /*Trophy of the Crusade*/) // Non-equippable items are limited to 3 drops
                        return true;                       // 非装备类物品最多掉 3 个（十字军战利品 47242 例外）
                    else if (_proto->InventoryType != 0 && foundDuplicates == 1) // Equippable item are limited to 1 drop
                        return true;                       // 装备类物品最多掉 1 个（防止一个怪掉两把相同武器）
                }
        }

        return false;
    }

private:
    Loot const& _loot;
    uint16 _lootMode;
};

// ============================================================================
// LootTemplate::LootGroup：内部类，表示"一组互斥的掉落定义"
// 组内分 ExplicitlyChanced（显式概率）和 EqualChanced（等概率）两堆
// 组内禁止放引用（reference），引用只能在未分组条目里
// ============================================================================
class LootTemplate::LootGroup                               // A set of loot definitions for items (refs are not allowed)
{
public:
    LootGroup() { }
    ~LootGroup();

    void AddEntry(LootStoreItem* item);                     // Adds an entry to the group (at loading stage)    // 加载时按 chance 分流
    bool HasQuestDrop(LootTemplateMap const& store) const;  // True if group includes at least 1 quest drop entry    // 含任务掉落（递归查引用）
    bool HasQuestDropForPlayer(Player const* player, LootTemplateMap const& store) const;
    // The same for active quests of the player
    void Process(Loot& loot, Player const* player, LootStore const& lootstore, uint16 lootMode, uint16 nonRefIterationsLeft) const;    // Rolls an item from the group (if any) and adds the item to the loot    // 选一个条目 roll，命中则调 loot.AddItem
    float RawTotalChance() const;                       // Overall chance for the group (without equal chanced items)    // 显式条目概率总和（不含等概率）
    float TotalChance() const;                          // Overall chance for the group    // 含等概率回退时的总概率（有等概率项时返回 100）

    void Verify(LootStore const& lootstore, uint32 id, uint8 group_id) const;    // 校验组概率和
    void CollectLootIds(LootIdSet& set) const;
    void CheckLootRefs(LootStore const& lootstore, uint32 Id, LootIdSet* ref_set) const;
    LootStoreItemList* GetExplicitlyChancedItemList() { return &ExplicitlyChanced; }
    LootStoreItemList* GetEqualChancedItemList() { return &EqualChanced; }
    void CopyConditions(ConditionList conditions);
private:
    LootStoreItemList ExplicitlyChanced;                // Entries with chances defined in DB    // DB 里写了 Chance > 0 的
    LootStoreItemList EqualChanced;                     // Zero chances - every entry takes the same chance    // DB 里 Chance == 0 的

    LootStoreItem const* Roll(Loot& loot, Player const* player, LootStore const& store, uint16 lootMode) const;   // Rolls an item from the group, returns nullptr if all miss their chances    // 单次摇骰选一个

    // This class must never be copied - storing pointers
    LootGroup(LootGroup const&);
    LootGroup& operator=(LootGroup const&);
};

//Remove all data and free all memory
// 清空 Store：释放所有 LootTemplate（它们自己负责释放 Entries/Groups）
void LootStore::Clear()
{
    for (LootTemplateMap::const_iterator itr = m_LootTemplates.begin(); itr != m_LootTemplates.end(); ++itr)
        delete itr->second;
    m_LootTemplates.clear();
}

// Checks validity of the loot store
// Actual checks are done within LootTemplate::Verify() which is called for every template
// 调用每个模板的 Verify()（组概率和、引用存在等）
void LootStore::Verify() const
{
    for (LootTemplateMap::const_iterator i = m_LootTemplates.begin(); i != m_LootTemplates.end(); ++i)
        i->second->Verify(*this, i->first);
}

// Loads a *_loot_template DB table into loot store
// All checks of the loaded template are called from here, no error reports at loot generation required
// 启动核心：执行 SQL 把整张表加载到 m_LootTemplates
uint32 LootStore::LoadLootTable()
{
    LootTemplateMap::const_iterator tab;

    // Clearing store (for reloading case)
    Clear();                                            // 重载时先清空

    //                                                  0     1            2               3         4         5             6
    // 一次 SELECT 把整张表读出来
    QueryResult result = WorldDatabase.Query("SELECT Entry, Item, Reference, Chance, QuestRequired, LootMode, GroupId, MinCount, MaxCount FROM {}", GetName());

    if (!result)
        return 0;

    uint32 count = 0;

    do
    {
        Field* fields = result->Fetch();

        uint32 entry               = fields[0].Get<uint32>();   // lootid
        uint32 item                = fields[1].Get<uint32>();   // 物品 id / 引用目标
        int32  reference           = fields[2].Get<int32>();    // 引用（0=非引用）
        float  chance              = fields[3].Get<float>();    // 掉率
        bool   needsquest          = fields[4].Get<bool>();     // 任务物品
        uint16 lootmode            = fields[5].Get<uint16>();   // 模式位掩码
        uint8  groupid             = fields[6].Get<uint8>();    // 组 id
        int32  mincount            = fields[7].Get<uint8>();    // 最少掉几个
        int32  maxcount            = fields[8].Get<uint8>();    // 最多掉几个

        // maxcount 不能超过 uint8 上限（255）
        if (maxcount > std::numeric_limits<uint8>::max())
        {
            LOG_ERROR("sql.sql", "Table '{}' Entry {} Item {}: MaxCount value ({}) to large. must be less {} - skipped", GetName(), entry, item, maxcount, std::numeric_limits<uint8>::max());
            continue;                                   // error already printed to log/console.
        }

        // lootmode == 0 表示永远不会掉（位运算恒为 0），强制改为 1 并报错
        if (lootmode == 0)
        {
            LOG_ERROR("sql.sql", "Table '{}' Entry {} Item {}: LootMode is equal to 0, item will never drop - setting mode 1", GetName(), entry, item);
            lootmode = 1;
        }

        LootStoreItem* storeitem = new LootStoreItem(item, reference, chance, needsquest, lootmode, groupid, mincount, maxcount);

        if (!storeitem->IsValid(*this, entry))            // Validity checks
        {
            delete storeitem;                            // 校验失败（坏组/chance/不存在物品等）
            continue;
        }

        // Looking for the template of the entry
        // often entries are put together
        // ★ 性能优化：DBA 通常把同一 entry 的行连续写在一起（GROUP BY Entry 导出）
        //   这里缓存上次用的 iterator tab，如果当前行 entry 与上次相同就跳过 find
        //   在大表（creature_loot_template 通常几万行）上能省掉绝大多数 map 查找
        // cppcheck-suppress eraseDereference
        if (m_LootTemplates.empty() || tab->first != entry)
        {
            // Searching the template (in case template Id changed)
            tab = m_LootTemplates.find(entry);
            if (tab == m_LootTemplates.end())
            {
                // 该 entry 还没模板，创建新 LootTemplate 并插入 map
                std::pair< LootTemplateMap::iterator, bool > pr = m_LootTemplates.insert(LootTemplateMap::value_type(entry, new LootTemplate()));
                tab = pr.first;
            }
        }
        // else is empty - template Id and iter are the same
        // finally iter refers to already existed or just created <entry, LootTemplate>

        // Adds current row to the template
        // 把这一行分到 Entries（groupid==0）或 Groups[gid-1]（groupid>0）
        tab->second->AddEntry(storeitem);
        ++count;
    } while (result->NextRow());

    Verify();                                           // Checks validity of the loot store

    return count;
}

// 该 lootid 模板里是否含至少一个任务掉落条目（递归查引用）
bool LootStore::HaveQuestLootFor(uint32 loot_id) const
{
    LootTemplateMap::const_iterator itr = m_LootTemplates.find(loot_id);
    if (itr == m_LootTemplates.end())
        return false;

    // scan loot for quest items
    return itr->second->HasQuestDrop(m_LootTemplates);
}

// 是否含玩家当前任务需要的掉落（用于判断玩家走近尸体时是否要提示）
bool LootStore::HaveQuestLootForPlayer(uint32 loot_id, Player const* player) const
{
    LootTemplateMap::const_iterator tab = m_LootTemplates.find(loot_id);
    if (tab != m_LootTemplates.end())
        if (tab->second->HasQuestDropForPlayer(m_LootTemplates, player))
            return true;

    return false;
}

// 重置所有模板的所有条目 conditions（ConditionMgr reload 时先清旧再装新）
void LootStore::ResetConditions()
{
    for (LootTemplateMap::iterator itr = m_LootTemplates.begin(); itr != m_LootTemplates.end(); ++itr)
    {
        ConditionList empty;
        itr->second->CopyConditions(empty);
    }
}

// 按 lootid 取模板（const，运行时 roll 用）
LootTemplate const* LootStore::GetLootFor(uint32 loot_id) const
{
    LootTemplateMap::const_iterator tab = m_LootTemplates.find(loot_id);

    if (tab == m_LootTemplates.end())
        return nullptr;

    return tab->second;
}

// 非 const 版本：ConditionMgr 装载条件时要修改模板（addConditionItem）
LootTemplate* LootStore::GetLootForConditionFill(uint32 loot_id) const
{
    LootTemplateMap::const_iterator tab = m_LootTemplates.find(loot_id);

    if (tab == m_LootTemplates.end())
        return nullptr;

    return tab->second;
}

// 启动加载入口：调 LoadLootTable() 把表载入内存，并把所有 lootid 收集到 lootIdSet
// （lootIdSet 之后会被 LoadLootTemplates_X 拿去做交叉校验）
uint32 LootStore::LoadAndCollectLootIds(LootIdSet& lootIdSet)
{
    uint32 count = LoadLootTable();

    for (LootTemplateMap::const_iterator tab = m_LootTemplates.begin(); tab != m_LootTemplates.end(); ++tab)
        lootIdSet.insert(tab->first);

    return count;
}

// 检查所有引用目标是否存在；存在的从 ref_set 里移除
// 用于 LoadLootTemplates_Reference：最终 ref_set 中剩下的就是没人引用的孤立 id
void LootStore::CheckLootRefs(LootIdSet* ref_set) const
{
    for (LootTemplateMap::const_iterator ltItr = m_LootTemplates.begin(); ltItr != m_LootTemplates.end(); ++ltItr)
        ltItr->second->CheckLootRefs(*this, ltItr->first, ref_set);
}

// 报告 lootIdSet 中剩余的（没人引用的）lootid
void LootStore::ReportUnusedIds(LootIdSet const& lootIdSet) const
{
    // all still listed ids isn't referenced
    for (LootIdSet::const_iterator itr = lootIdSet.begin(); itr != lootIdSet.end(); ++itr)
        LOG_ERROR("sql.sql", "Table '{}' Entry {} isn't {} and not referenced from loot, and thus useless.", GetName(), *itr, GetEntryName());
}

void LootStore::ReportNonExistingId(uint32 lootId) const
{
    LOG_ERROR("sql.sql", "Table '{}' Entry {} does not exist", GetName(), lootId);
}

// ownerType + ownerId：告诉管理员"是哪个 creature/go/item 用了这个不存在的 lootid"
void LootStore::ReportNonExistingId(uint32 lootId, const char* ownerType, uint32 ownerId) const
{
    LOG_ERROR("sql.sql", "Table '{}' Entry {} does not exist but it is used by {} {}", GetName(), lootId, ownerType, ownerId);
}

void LootStore::ReportInvalidCount(uint32 lootId, const char* ownerType, uint32 ownerId, uint32 itemId, uint8 minCount, uint8 maxCount) const
{
    LOG_ERROR("sql.sql", "Table '{}' Entry {} used by {} entry {} item {} has minCount ( {} ) != maxCount ( {} ) which is not supported for this loot type.", GetName(), lootId, ownerType, ownerId, itemId, minCount, maxCount);
}

//
// --------- LootStoreItem ---------
//

// ============================================================================
// 模式一：单物品掉率 roll（在 LootTemplate::Process 中对每个未分组条目调用）
// 返回 true 表示该条目本次命中，应该掉落
//
// ★ 三分支决策树（按优先级从高到低）：
//   1) 脚本钩子 OnItemRoll 可改 chance 或直接否决（return false）
//   2) chance >= 100.0f：必掉（DB 写 100 或脚本调高到 100+）
//   3) reference != 0（引用条目）：只乘 RATE_DROP_ITEM_REFERENCED，不按品质
//   4) 普通物品：chance × 品质修正系数（Rate.Drop.Item.<Quality>）
//
// ★ 最终判定：roll_chance_f(c) == (c > rand_chance())
//   rand_chance() 返回 [0.0, 100.0) 的 double（见 Random.h:51）
//   所以 chance=30 表示约 30% 命中概率
// ============================================================================
// Checks if the entry (quest, non-quest, reference) takes it's chance (at loot generation)
// RATE_DROP_ITEMS is no longer used for all types of entries
bool LootStoreItem::Roll(bool rate, Player const* player, Loot& loot, LootStore const& store) const
{
    float _chance = chance;

    // 脚本钩子：可改 _chance 或直接否决（return false 让整组不 roll）
    // ★ 注意这里修改的是局部变量 _chance，原始 chance 字段不变（线程安全）
    if (!sScriptMgr->OnItemRoll(player, this, _chance, loot, store))
        return false;

    // chance >= 100：必掉（DB 写 100 或更大；脚本提升到 100+ 也触发）
    if (_chance >= 100.0f)
        return true;

    if (reference)                                   // reference case
        // 引用条目：只应用 RATE_DROP_ITEM_REFERENCED（整体缩放，不按品质）
        // 因为引用指向的是模板而非具体物品，品质要等递归进去后才确定
        return roll_chance_f(_chance * (rate ? sWorld->getRate(RATE_DROP_ITEM_REFERENCED) : 1.0f));

    // 普通物品：按品质应用 Rate.Drop.Item.* 修正
    // ★ qualityToRate[] 把 Poor..Artifact 映射到 RATE_DROP_ITEM_POOR..ARTIFACT
    //   Heirloom（品质=7）不修正（条件 Quality < ITEM_QUALITY_HEIRLOOM 拦住）
    ItemTemplate const* pProto = sObjectMgr->GetItemTemplate(itemid);
    float qualityModifier = 1.0f;
    if (pProto && pProto->Quality < ITEM_QUALITY_HEIRLOOM && rate)
        qualityModifier = sWorld->getRate(qualityToRate[pProto->Quality]);

    // ★ rate=false（Mail/Reference/Spell）时 qualityModifier 恒为 1.0f
    //   这三个 Store 不应用品质掉率修正
    return roll_chance_f(_chance * qualityModifier);
}

// ============================================================================
// 启动校验：每行 *_loot_template 的字段合法性
// 返回 false 表示这一行应被丢弃
// ============================================================================
// Checks correctness of values
bool LootStoreItem::IsValid(LootStore const& store, uint32 entry) const
{
    // groupid 占 7 位，上限 127
    if (groupid >= 1 << 7)                                     // it stored in 7 bit field
    {
        LOG_ERROR("sql.sql", "Table '{}' Entry {} Item {}: GroupId ({}) must be less {} - skipped", store.GetName(), entry, itemid, groupid, 1 << 7);
        return false;
    }

    // mincount 不能为 0
    if (mincount == 0)
    {
        LOG_ERROR("sql.sql", "Table '{}' Entry {} Item {}: wrong MinCount ({}) - skipped", store.GetName(), entry, itemid, mincount);
        return false;
    }

    if (!reference)                                  // item (quest or non-quest) entry, maybe grouped
    {
        // 普通物品：必须能在 item_template 找到
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemid);
        if (!proto)
        {
            LOG_ERROR("sql.sql", "Table '{}' Entry {} Item {}: item entry not listed in `item_template` - skipped", store.GetName(), entry, itemid);
            return false;
        }

        // 等概率（chance=0）只允许在组内
        if (chance == 0 && groupid == 0)                     // Zero chance is allowed for grouped entries only
        {
            LOG_ERROR("sql.sql", "Table '{}' Entry {} Item {}: equal-chanced grouped entry, but group not defined - skipped", store.GetName(), entry, itemid);
            return false;
        }

        // 极低概率（小于百万分之一）：拒绝，避免精度问题
        if (chance != 0 && chance < 0.000001f)             // loot with low chance
        {
            LOG_ERROR("sql.sql", "Table '{}' Entry {} Item {}: low chance ({}) - skipped",
                             store.GetName(), entry, itemid, chance);
            return false;
        }

        if (maxcount < mincount)                       // wrong max count
        {
            LOG_ERROR("sql.sql", "Table '{}' Entry {} Item {}: MaxCount ({}) less that MinCount ({}) - skipped", store.GetName(), entry, itemid, int32(maxcount), mincount);
            return false;
        }
    }
    else                                                    // if reference loot
    {
        // 引用条目：needs_quest 没意义（引用是模板不是物品），警告但不拒绝
        if (needs_quest)
            LOG_ERROR("sql.sql", "Table '{}' Entry {} Item {}: quest required will be ignored", store.GetName(), entry, itemid);
        // 未分组的引用 chance 不能为 0
        else if (chance == 0 && groupid == 0)
        {
            LOG_ERROR("sql.sql", "Table '{}' Entry {} Item {}: zero chance is specified for an ungrouped reference, skipped", store.GetName(), entry, itemid);
            return false;
        }
    }
    return true;                                            // Referenced template existence is checked at whole store level
}

//
// --------- LootItem ---------
//

// ============================================================================
// LootItem 构造：从 LootStoreItem 拷贝模板信息 + 初始化运行时状态
// 注意：count 还没确定，会在 Loot::AddItem 中赋值
// ============================================================================
// Constructor, copies most fields from LootStoreItem and generates random count
LootItem::LootItem(LootStoreItem const& li)
{
    itemid       = li.itemid;
    itemIndex    = 0;                                   // 后面 AddItem 会修正
    conditions   = li.conditions;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemid);
    // FFA：物品模板带 ITEM_FLAG_MULTI_DROP（人人可拾取）
    freeforall  = proto && proto->HasFlag(ITEM_FLAG_MULTI_DROP);
    // 遵循队伍 loot 规则：物品自定义标志 ITEM_FLAGS_CU_FOLLOW_LOOT_RULES
    follow_loot_rules = proto && proto->HasFlagCu(ITEM_FLAGS_CU_FOLLOW_LOOT_RULES);

    needs_quest = li.needs_quest;

    // 随机属性 / 随机附魔（很多装备才有，例如"强壮之XXX"）
    randomSuffix = GenerateEnchSuffixFactor(itemid);
    randomPropertyId = Item::GenerateItemRandomPropertyId(itemid);
    count = 0;                                          // 由 AddItem 设
    is_looted = false;
    is_blocked = false;
    is_underthreshold = false;
    is_counted = false;
    rollWinnerGUID = ObjectGuid::Empty;
    groupid = li.groupid;
}

// ============================================================================
// 玩家能否拾取/看到该物品的总闸（阵营、职业、专业、任务、条件、脚本全检查）
// 被 FillFFALoot / FillQuestLoot / FillNonQuestNonFFAConditionalLoot /
//   LootView / CanRollOnItem / HandleLootMasterGiveOpcode 共同调用
//
// ★★★ 检查顺序（前置失败立即 return false，短路优化）：
//   1) 物品模板必须存在
//   2) 未被 DisableMgr 屏蔽（GM 用 .disable loot 命令屏蔽）
//   3) conditions 表条件全部满足（★ 关键：条件不参与 roll，只过滤可见性）
//   4) 阵营匹配（ITEM_FLAG2_FACTION_HORDE/ALLIANCE）
//   5) 配方可学性（没专业或已学会 -> 看不到）
//   6) 任务物品：玩家身上有对应任务且需要这个 item
//   7) 任务起始物品：未接/未完成/未达上限/前置已完成
//   8) 脚本钩子通过
//
// ★ 为什么 conditions 不参与 roll（重要设计）：
//   场景：A 任务物品在 B 区域掉落，玩家没接 A 任务时不应看到
//   如果在 roll 时判断条件：玩家没接任务时该条目不 roll，roll 结果存进 loot
//   → 玩家接任务后再开 loot 窗口，物品已经不存在了（永远捡不到）
//   正确做法：照常 roll 命中存进 loot，可见性在每次打开窗口时按玩家实时判断
//   这样玩家接任务后开 loot 就能看到（只要尸体还没消失）
// ============================================================================
// Basic checks for player/item compatibility - if false no chance to see the item in the loot
bool LootItem::AllowedForPlayer(Player const* player, ObjectGuid source) const
{
    ItemTemplate const* pProto = sObjectMgr->GetItemTemplate(itemid);
    if (!pProto)
        return false;

    // 该物品被 disable（DisableMgr 屏蔽了 loot 类型）
    // ★ GM 命令 .disable loot <itemid> 可以屏蔽某物品的所有掉落
    if (sDisableMgr->IsDisabledFor(DISABLE_TYPE_LOOT, itemid, nullptr))
        return false;

    // conditions 表里挂的条件：不满足则看不到（关键：条件不参与 roll，只过滤可见性）
    // ★ 这是条件系统的核心入口，支持各种条件类型（等级/职业/成就/区域/任务等）
    if (!sConditionMgr->IsObjectMeetToConditions(const_cast<Player*>(player), conditions))
        return false;

    // not show loot for not own team
    // 阵营限定物品：部落专属 / 联盟专属
    if (pProto->HasFlag2(ITEM_FLAG2_FACTION_HORDE) && player->GetTeamId(true) != TEAM_HORDE)
        return false;

    if (pProto->HasFlag2(ITEM_FLAG2_FACTION_ALLIANCE) && player->GetTeamId(true) != TEAM_ALLIANCE)
        return false;

    // profession / recipe checks
    // 配方类物品：
    //   - 没有对应专业（RequiredSkill）-> 看不到
    //   - 已经学会这个配方（Spells[1].SpellId）-> 看不到（防止重复捡取）
    if (pProto->HasFlag(ITEM_FLAG_HIDE_UNUSABLE_RECIPE) && (!player->HasSkill(pProto->RequiredSkill) || player->HasSpell(pProto->Spells[1].SpellId)))
        return false;

    // 拾取绑定配方：已经学会 -> 看不到（防止占用 loot 槽）
    if (pProto->Class == ITEM_CLASS_RECIPE && pProto->Bonding == BIND_WHEN_PICKED_UP && pProto->Spells[1].SpellId != 0 && player->HasSpell(pProto->Spells[1].SpellId))
        return false;

    // check quest requirements
    // ★ ITEM_FLAGS_CU_IGNORE_QUEST_STATUS 标志：跳过任务相关检查（用于特殊物品）
    if (!pProto->HasFlagCu(ITEM_FLAGS_CU_IGNORE_QUEST_STATUS))
    {
        // 任务物品：必须玩家身上有对应任务且需要这个 item
        // ★ HasQuestForItem 检查玩家任务列表中是否有任务的目标物品包含 itemid
        if (needs_quest && !player->HasQuestForItem(itemid))
            return false;

        // Hide quest starter items when quest is already started/rewarded,
        // when unique count is already reached, or when prerequisite is missing.
        // 任务起始物品（右键使用能接任务的物品）：四种情况看不到
        //   1) 已接该任务（QUEST_STATUS != NONE）
        //   2) 已完成该任务（GetQuestRewardStatus）
        //   3) 已达物品上限（MaxCount 限制，防止重复接任务）
        //   4) 前置任务未完成（prevQuestId）
        if (pProto->StartQuest)
        {
            uint32 prevQuestId = 0;
            if (Quest const* startQuest = sObjectMgr->GetQuestTemplate(pProto->StartQuest))
                prevQuestId = startQuest->GetPrevQuestId();

            if (player->GetQuestStatus(pProto->StartQuest) != QUEST_STATUS_NONE ||
                player->GetQuestRewardStatus(pProto->StartQuest) ||
                (pProto->MaxCount && player->HasItemCount(itemid, pProto->MaxCount, true)) ||
                (prevQuestId && !player->GetQuestRewardStatus(prevQuestId)))
                return false;
        }
    }

    // 脚本钩子：自定义可见性判断（模块可在此扩展）
    if (!sScriptMgr->OnAllowedForPlayerLootCheck(player, source))
        return false;

    return true;
}

// 把玩家加入"允许拾取者"集合（FFA/任务物品每玩家单独建一份）
void LootItem::AddAllowedLooter(Player const* player)
{
    allowedGUIDs.insert(player->GetGUID());
}

//
// --------- Loot ---------
//

// ============================================================================
// Loot::AddItem：把已 roll 中的 LootStoreItem 转成 LootItem 落袋
// 由 LootTemplate::Process / LootGroup::Process 在 roll 命中后调用
//
// ★ 三大职责（按顺序）：
//   1) 数量确定 + 堆叠拆分：urand(min,max) 得到总数量，按 maxStackSize 拆成多 stack
//   2) 容量上限：items ≤ 18 / quest_items ≤ 32（客户端硬限制）
//   3) 可见性计数：只有"至少一个队伍成员能看到"才计 unlootedCount
//
// ★ 为什么 unlootedCount 要在这里计而不是统一在 Fill* 中：
//   - 普通个人物品（非任务/非FFA/无条件）：在这里计最简单
//   - FFA / 任务 / 条件物品：每玩家可见性不同，必须在各自的 Fill* 中按玩家独立计
//   - canSeeItemInLootWindow 检查避免对"团队本中没人能看到的物品"误计
// ============================================================================
// Inserts the item into the loot (called by LootTemplate processors)
void Loot::AddItem(LootStoreItem const& item)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemid);
    if (!proto)
        return;

    // 在 [mincount, maxcount] 之间随机一个总数量
    // ★ 即使是 maxcount=1 也走 urand(1,1)（保证随机源一致性，便于重现/调试）
    uint32 count = urand(item.mincount, item.maxcount);
    // 按物品最大堆叠数拆成几个 stack（例如 200 个箭头 / maxstack=100 → 2 stack）
    uint32 stacks = count / proto->GetMaxStackSize() + (count % proto->GetMaxStackSize() ? 1 : 0);

    // 任务物品进 quest_items（上限 32），普通进 items（上限 18）
    // ★ 选 vector 的依据：客户端按数组下标访问槽位，必须连续
    std::vector<LootItem>& lootItems = item.needs_quest ? quest_items : items;
    uint32 limit = item.needs_quest ? MAX_NR_QUEST_ITEMS : MAX_NR_LOOT_ITEMS;

    for (uint32 i = 0; i < stacks && lootItems.size() < limit; ++i)
    {
        // 每次循环新建一个 LootItem（构造时拷贝模板字段 + 生成随机属性）
        LootItem generatedLoot(item);
        // 当前 stack 的数量 = min(剩余总数, 单 stack 上限)
        generatedLoot.count = std::min(count, proto->GetMaxStackSize());
        // itemIndex 用于发包时客户端定位该槽
        generatedLoot.itemIndex = lootItems.size();
        lootItems.push_back(generatedLoot);
        // 扣掉已分配的数量，进入下一个 stack
        count -= proto->GetMaxStackSize();

        // 检查"至少有一个队伍成员能看到这个物品"，看不到就不计 unlootedCount
        // ★ 业务场景：25 人团本某 BOSS 掉落 SM 限定配方，但团里没 SM，那这物品
        //   实际上没人能捡，不计 unlootedCount 避免尸体永远"未捡光"卡住
        // In some cases, a dropped item should be visible/lootable only for some players in group
        bool canSeeItemInLootWindow = false;
        if (auto player = ObjectAccessor::FindPlayer(lootOwnerGUID))
        {
            if (auto group = player->GetGroup())
            {
                // 队伍场景：只要有一个队员能看到，就计
                // ★ 注意遍历整个队伍（O(n)），对大团本可能有性能影响，但 AddItem 调用频率不高
                for (auto itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    if (auto member = itr->GetSource())
                    {
                        if (generatedLoot.AllowedForPlayer(member, sourceWorldObjectGUID))
                        {
                            canSeeItemInLootWindow = true;
                            break;                      // 只要一个能看到就够
                        }
                    }
                }
            }
            else if (generatedLoot.AllowedForPlayer(player, sourceWorldObjectGUID))
            {
                // 个人场景：自己能看到就计
                canSeeItemInLootWindow = true;
            }
        }

        if (!canSeeItemInLootWindow)
        {
            // 无人可见：物品仍存进 items（万一之后组员变化），但不计 unlootedCount
            LOG_DEBUG("loot", "Skipping ++unlootedCount for unlootable item: {}", item.itemid);
            continue;
        }

        // unlootedCount 计数规则（与各 Fill* 协调，详见 LootItem 注释）：
        // - 非任务、无条件、非 FFA 的"普通个人可见"物品：在这里计
        // - FFA 物品：在 FillFFALoot() 中按玩家独立计（每人 +1）
        // - 非 FFA 条件物品：在 FillNonQuestNonFFAConditionalLoot() 中计（is_counted 防重）
        // non-conditional one-player only items are counted here,
        // free for all items are counted in FillFFALoot(),
        // non-ffa conditionals are counted in FillNonQuestNonFFAConditionalLoot()
        if (!item.needs_quest && item.conditions.empty() && !proto->HasFlag(ITEM_FLAG_MULTI_DROP))
            ++unlootedCount;
    }
}

// ============================================================================
// Loot::FillLoot：填充 Loot 实例的入口（在 Unit::Kill / Player::SendLoot 等处调用）
//
// ★ 完整流程：
//   1) 校验 lootOwner 非空
//   2) 保存 lootOwnerGUID（后续 AddItem / AllowedForPlayer 都要用）
//   3) store.GetLootFor(lootId) 查模板；不存在则报错返回
//   4) reserve 容量（避免 vector 扩容搬移导致 itemIndex 失效）
//   5) ★ 核心：调 LootTemplate::Process 完成所有 roll（通过回调 AddItem 落袋）
//   6) 脚本钩子 OnAfterLootTemplateProcess
//   7) 分队伍/个人两条路设置可见性映射：
//      - 队伍：遍历附近队员，每人 FillNotNormalLootFor；并标记 is_underthreshold
//      - 个人：只为自己 FillNotNormalLootFor
//
// ★ personal 参数的作用：
//   - true：个人 loot（如偷窃、分解、钓鱼），不走队伍规则
//   - false：队伍场景，要构造每位队员的可见性映射
//
// ★ noEmptyError 参数的作用：
//   - true：模板不存在时不报错（调用方知道这是可选的，如某些 spell loot）
//   - false：模板不存在时记 ERROR 日志（默认行为）
// ============================================================================
// Calls processor of corresponding LootTemplate (which handles everything including references)
bool Loot::FillLoot(uint32 lootId, LootStore const& store, Player* lootOwner, bool personal, bool noEmptyError, uint16 lootMode /*= LOOT_MODE_DEFAULT*/, WorldObject* lootSource /*= nullptr*/)
{
    // Must be provided
    // 校验 lootOwner 非空（防御性，理论上调用方保证）
    if (!lootOwner)
        return false;

    lootOwnerGUID = lootOwner->GetGUID();

    LootTemplate const* tab = store.GetLootFor(lootId);

    if (!tab)
    {
        // 模板不存在：报错（除非调用方明确说不要报，比如"可选掉落"）
        if (!noEmptyError)
            LOG_ERROR("sql.sql", "Table '{}' loot id #{} used but it doesn't have records.", store.GetName(), lootId);
        return false;
    }

    // ★ reserve 关键：避免后续 push_back 触发 vector 扩容，导致 LootItem* 失效
    //   （LootItemInSlot / LootView 等会持有 items[i] 的指针/引用）
    items.reserve(MAX_NR_LOOT_ITEMS);
    quest_items.reserve(MAX_NR_QUEST_ITEMS);

    // Initial group is 0, top level set to True
    // ★★★ 核心调用：在这里完成所有物品 roll；命中的条目会回调 Loot::AddItem
    //   groupId=0 表示从模板根开始（不是引用指定组）
    //   isTopLevel=true 让 Process 在处理 Groups 时应用 RATE_DROP_ITEM_GROUP_AMOUNT
    tab->Process(*this, store, lootMode, lootOwner, 0, true);          // Processing is done there, callback via Loot::AddItem()

    // 脚本钩子：模板处理完毕（可在此最后修改 loot，例如清空/追加）
    sScriptMgr->OnAfterLootTemplateProcess(this, tab, store, lootOwner, personal, noEmptyError, lootMode);

    // Setting access rights for group loot case
    // 队伍场景：为每个附近队员构造可见性映射
    Group* group = lootOwner->GetGroup();
    if (!personal && group)
    {
        // ★ 初始 round-robin 持有者 = lootOwner（击杀者/首先开 loot 的人）
        //   后续每次有人捡完一个 round-robin 物品，持有者会切换到下一位
        roundRobinPlayer = lootOwner->GetGUID();

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Player* player = itr->GetSource()) // should actually be looted object instead of lootOwner but looter has to be really close so doesnt really matter
            {
                // 必须在 loot 奖励距离内（默认很近，超出范围不给 loot 权限）
                // ★ 这就是为什么离 BOSS 太远的队员看不到掉落
                if (player->IsAtLootRewardDistance(lootSource ? lootSource : lootOwner))
                {
                    FillNotNormalLootFor(player);
                }
            }
        }

        // 标记低于阈值物品（quality < threshold）：这些走 round-robin，不参与 ROLL
        // ★ threshold 由队长设置（默认 UNCOMMON，即绿色及以上才 ROLL）
        //   低于阈值的物品不进入 Group::GroupLoot 的 ROLL 流程，直接按 round-robin 轮流捡
        for (uint8 i = 0; i < items.size(); ++i)
        {
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(items[i].itemid))
                if (proto->Quality < uint32(group->GetLootThreshold()))
                    items[i].is_underthreshold = true;
        }
    }
    // ... for personal loot
    // 个人 loot：只为自己构造可见性映射（不涉及队伍规则）
    else
        FillNotNormalLootFor(lootOwner);

    return true;
}

// ============================================================================
// 为玩家构造三个可见性映射（任务/FFA/条件），并自动收取货币代币
// ============================================================================
void Loot::FillNotNormalLootFor(Player* player)
{
    ObjectGuid playerGuid = player->GetGUID();

    // 任务物品可见列表：还没构造过就构造
    QuestItemMap::const_iterator qmapitr = PlayerQuestItems.find(playerGuid);
    if (qmapitr == PlayerQuestItems.end())
        FillQuestLoot(player);

    // FFA 物品可见列表
    qmapitr = PlayerFFAItems.find(playerGuid);
    if (qmapitr == PlayerFFAItems.end())
        FillFFALoot(player);

    // 非 FFA、非任务的条件物品可见列表
    qmapitr = PlayerNonQuestNonFFAConditionalItems.find(playerGuid);
    if (qmapitr == PlayerNonQuestNonFFAConditionalItems.end())
        FillNonQuestNonFFAConditionalLoot(player);

    // Process currency items
    // 货币代币（Justice/Valor Points 等）特殊处理：自动收取，不进背包
    uint32 max_slot = GetMaxSlotInLootFor(player);
    LootItem const* item = nullptr;
    uint32 itemsSize = uint32(items.size());
    for (uint32 i = 0; i < max_slot; ++i)
    {
        if (i < items.size())
            item = &items[i];
        else
            item = &quest_items[i - itemsSize];

        if (!item->is_looted && item->freeforall && item->AllowedForPlayer(player, sourceWorldObjectGUID))
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->itemid))
                if (proto->IsCurrencyToken())
                {
                    InventoryResult msg;
                    player->StoreLootItem(i, this, msg);
                }
    }
}

// ============================================================================
// 构造玩家可见的 FFA 物品列表（ITEM_FLAG_MULTI_DROP 的物品，人人都能拿一份）
//
// ★ 与普通物品的核心差异：
//   普通物品：全队共享一份（一人捡了其他人就没了）
//   FFA 物品：每个符合条件的队员都能独立拿一份（典型：货币代币、节日物品）
//
// ★ unlootedCount 计数特点：
//   每个玩家对每个 FFA 物品都 +1（不防重计）
//   这意味着 5 人小队 + 2 个 FFA 物品 = unlootedCount 增加 10
//   （因为从系统视角看，这相当于还有 10 个待捡动作）
// ============================================================================
QuestItemList* Loot::FillFFALoot(Player* player)
{
    QuestItemList* ql = new QuestItemList();

    for (uint8 i = 0; i < items.size(); ++i)
    {
        LootItem& item = items[i];
        // 三条件：未捡 + 是 FFA + 该玩家 AllowedForPlayer 通过
        // ★ 注意：FFA 物品的 AllowedForPlayer source 参数传 containerGUID（物品容器场景）
        //   而其他场景下 AllowedForPlayer 传 sourceWorldObjectGUID
        if (!item.is_looted && item.freeforall && item.AllowedForPlayer(player, containerGUID))
        {
            ql->push_back(QuestItem(i));
            ++unlootedCount;                            // ★ FFA 物品按玩家计（每人独立一份，所以每个玩家都 +1）
        }
    }
    if (ql->empty())
    {
        delete ql;
        return nullptr;
    }

    PlayerFFAItems[player->GetGUID()] = ql;
    return ql;
}

// ============================================================================
// 构造玩家可见的任务物品列表
//
// ★ 两层限制：
//   1) items.size() 不能已达 18（客户端窗口装不下任务槽）
//   2) items.size() + ql->size() 不能超过 18（普通+任务总槽位限制）
//
// ★ ML 特殊处理：
//   主分配者（ML）即使没接任务也能看到任务物品（方便分配给真正需要的玩家）
//   但 ML 不算"允许拾取者"，不会 AddAllowedLooter
//
// ★ unlootedCount 计数规则：
//   - FFA 任务物品（freeforall=true）：每玩家独立 +1（同 FillFFALoot）
//   - 非 FFA 任务物品：用 is_counted 标志防重计，全队只 +1 次
// ============================================================================
QuestItemList* Loot::FillQuestLoot(Player* player)
{
    // ★ 客户端窗口硬限制：普通物品已满 18 则任务槽无法显示
    if (items.size() == MAX_NR_LOOT_ITEMS)
        return nullptr;

    QuestItemList* ql = new QuestItemList();

    // ML 即使没接任务也能看到任务物品（方便分配）
    bool isMasterLooter = player->GetGroup() && player->GetGroup()->GetLootMethod() == MASTER_LOOT && player->GetGroup()->GetMasterLooterGuid() == player->GetGUID();

    for (uint8 i = 0; i < quest_items.size(); ++i)
    {
        LootItem& item = quest_items[i];

        sScriptMgr->OnPlayerBeforeFillQuestLootItem(player, item);

        bool allowed = item.AllowedForPlayer(player, sourceWorldObjectGUID);

        // 不允许且不是 ML：跳过（玩家看不到这个任务物品）
        if (!allowed && !isMasterLooter)
            continue;

        ql->push_back(QuestItem(i));

        // Only add "allowed looter" if you are actually allowed to loot.
        // ★ ML 没接任务时不加入 allowedGUIDs（避免 ML 被误判为"可捡者"）
        if (allowed)
        {
            item.AddAllowedLooter(player);

            // unlootedCount 计数规则：
            // - FFA 任务物品：每玩家独立计（同一物品多人能各拿一份）
            // - 非 FFA 任务物品：用 is_counted 防重计，只算一次
            //   ★ is_counted 是 LootItem 的成员，全队共享（不按玩家），所以 5 人小队也只 +1 次
            if (item.freeforall)
            {
                ++unlootedCount;
            }
            else if (!item.is_counted)
            {
                ++unlootedCount;
                item.is_counted = true;                 // ★ 防止下一个队员再 +1
            }
        }

        // 限制总槽位数 ≤ 18（普通物品 + 当前任务槽）
        if (items.size() + ql->size() == MAX_NR_LOOT_ITEMS)
            break;
    }

    if (ql->empty())
    {
        delete ql;
        return nullptr;
    }

    PlayerQuestItems[player->GetGUID()] = ql;
    return ql;
}

// ============================================================================
// 构造玩家可见的"非 FFA、非任务、带条件"物品列表
//
// ★ 为什么单独搞一个映射：
//   普通物品（无条件）在 LootView 中直接按 permission 显示，不需要按玩家过滤
//   但条件物品（带 conditions）的可见性因人而异，必须按玩家单独构造
//
// ★ 与 FillFFALoot 的区别：
//   - 这里只处理 !freeforall 的物品（FFA 已在 FillFFALoot 处理）
//   - is_counted 防重计（每个物品全队只 +1 次，与 FillQuestLoot 同）
// ============================================================================
QuestItemList* Loot::FillNonQuestNonFFAConditionalLoot(Player* player)
{
    QuestItemList* ql = new QuestItemList();

    for (uint8 i = 0; i < items.size(); ++i)
    {
        LootItem& item = items[i];

        // 三条件：未捡 + 非 FFA + 该玩家 AllowedForPlayer 通过
        // ★ 注意：与 FillFFALoot 的 source 参数不同，这里传 sourceWorldObjectGUID
        if (!item.is_looted && !item.freeforall && item.AllowedForPlayer(player, sourceWorldObjectGUID))
        {
            item.AddAllowedLooter(player);

            // ★ 只对真正带 conditions 的物品建槽（普通物品无需走条件分支，由 LootView 直接显示）
            //   这样条件物品在 LootView 中通过 PlayerNonQuestNonFFAConditionalItems 取槽
            //   而普通物品直接在主循环里取（避免重复显示）
            if (!item.conditions.empty())
            {
                ql->push_back(QuestItem(i));
                if (!item.is_counted)
                {
                    ++unlootedCount;
                    item.is_counted = true;             // 防重计：5 人小队只 +1 次
                }
            }
        }
    }
    if (ql->empty())
    {
        delete ql;
        return nullptr;
    }

    PlayerNonQuestNonFFAConditionalItems[player->GetGUID()] = ql;
    return ql;
}

//===================================================

// ============================================================================
// 通知所有正在开 loot 窗口的玩家：某普通槽位物品被取走
// 用 SendLootRelease / 直接更新窗口
// ============================================================================
void Loot::NotifyItemRemoved(uint8 lootIndex)
{
    // notify all players that are looting this that the item was removed
    // convert the index to the slot the player sees
    PlayersLootingSet::iterator i_next;
    for (PlayersLootingSet::iterator i = PlayersLooting.begin(); i != PlayersLooting.end(); i = i_next)
    {
        i_next = i;
        ++i_next;
        if (Player* player = ObjectAccessor::FindPlayer(*i))
            player->SendNotifyLootItemRemoved(lootIndex);
        else
            PlayersLooting.erase(i);                    // 玩家已离线，顺便清理
    }
}

// 通知所有玩家：金钱被取走
void Loot::NotifyMoneyRemoved()
{
    // notify all players that are looting this that the money was removed
    PlayersLootingSet::iterator i_next;
    for (PlayersLootingSet::iterator i = PlayersLooting.begin(); i != PlayersLooting.end(); i = i_next)
    {
        i_next = i;
        ++i_next;
        if (Player* player = ObjectAccessor::FindPlayer(*i))
            player->SendNotifyLootMoneyRemoved();
        else
            PlayersLooting.erase(i);
    }
}

// 通知所有玩家：某任务槽位物品被取走（FFA 任务物品人人都要看到消失）
void Loot::NotifyQuestItemRemoved(uint8 questIndex)
{
    // when a free for all questitem is looted
    // all players will get notified of it being removed
    // (other questitems can be looted by each group member)
    // bit inefficient but isn't called often

    PlayersLootingSet::iterator i_next;
    for (PlayersLootingSet::iterator i = PlayersLooting.begin(); i != PlayersLooting.end(); i = i_next)
    {
        i_next = i;
        ++i_next;
        if (Player* player = ObjectAccessor::FindPlayer(*i))
        {
            // 该玩家的任务槽列表里找 questIndex 对应位置
            QuestItemMap::const_iterator pq = PlayerQuestItems.find(player->GetGUID());
            if (pq != PlayerQuestItems.end() && pq->second)
            {
                // find where/if the player has the given item in it's vector
                QuestItemList& pql = *pq->second;

                uint8 j;
                for (j = 0; j < pql.size(); ++j)
                    if (pql[j].index == questIndex)
                        break;

                // 客户端看到的槽位 = items.size() + j（任务槽在普通槽之后）
                if (j < pql.size())
                    player->SendNotifyLootItemRemoved(items.size() + j);
            }
        }
        else
            PlayersLooting.erase(i);
    }
}

// ============================================================================
// 金钱生成：在 [minAmount, maxAmount] 之间随机，应用 RATE_DROP_MONEY
//
// ★ 三段式算法（按范围大小分流，解决精度/性能问题）：
//   1) maxAmount <= minAmount：取 maxAmount（DB 写死或 min==max 的情况）
//   2) 范围 < 32700：直接 urand(min, max)，结果精确到 1 铜
//   3) 范围 >= 32700：先 >>8 摇再 <<8 还原
//      ★ 原因：urand 内部用 uint32，范围过大会让低 8 位几乎都是 0（精度损失）
//        右移 8 位等价于"以 256 铜为粒度"摇骰，虽然损失精度但避免溢出/分布异常
//        （比如 0~1000 金的怪物掉落，玩家不会在意最后 2 银 56 铜的精度）
// ============================================================================
void Loot::generateMoneyLoot(uint32 minAmount, uint32 maxAmount)
{
    if (maxAmount > 0)
    {
        if (maxAmount <= minAmount)
            gold = uint32(maxAmount * sWorld->getRate(RATE_DROP_MONEY));
        else if ((maxAmount - minAmount) < 32700)
            gold = uint32(urand(minAmount, maxAmount) * sWorld->getRate(RATE_DROP_MONEY));
        else
            // 范围过大：先 >>8 摇再 <<8 还原，等价粗粒度随机
            // 例：min=10000, max=1000000 → urand(39, 3906) × rate × 256
            gold = uint32(urand(minAmount >> 8, maxAmount >> 8) * sWorld->getRate(RATE_DROP_MONEY)) << 8;
    }
}

// ============================================================================
// 按客户端看到的槽位 lootSlot 查 LootItem（处理普通/任务/FFA/条件四种来源）
// 各输出参数返回对应的 QuestItem 句柄（用于标记已捡）
// 返回 nullptr 表示该槽已被捡或不属于该玩家
//
// ★ 客户端槽位编号规则：
//   [0, items.size())                          → 普通物品 / FFA / 条件物品（共用 items[]）
//   [items.size(), items.size()+questCount)    → 任务物品（quest_items[]）
//
// ★ 四种查找路径：
//   1) lootSlot >= items.size() → 任务物品路径，从 PlayerQuestItems 查
//   2) item.freeforall          → FFA 路径，从 PlayerFFAItems 查（每玩家独立 is_looted）
//   3) !conditions.empty()      → 条件路径，从 PlayerNonQuestNonFFAConditionalItems 查
//   4) 其他                     → 普通物品，直接 items[lootSlot]，is_looted 是全局的
//
// ★ 为什么 FFA/条件物品要从 per-player 映射查 is_looted：
//   普通物品：一人捡了全队都没了，用全局 item.is_looted
//   FFA/条件：每人独立可捡，每玩家有自己的"已捡"状态，存在 QuestItem.is_looted
// ============================================================================
LootItem* Loot::LootItemInSlot(uint32 lootSlot, Player* player, QuestItem * *qitem, QuestItem * *ffaitem, QuestItem * *conditem)
{
    LootItem* item = nullptr;
    bool is_looted = true;                                  // 默认已捡，只有找到具体来源才覆盖
    if (lootSlot >= items.size())
    {
        // 路径 1：任务槽（客户端槽位 = items.size() + questIndex）
        uint32 questSlot = lootSlot - items.size();
        QuestItemMap::const_iterator itr = PlayerQuestItems.find(player->GetGUID());
        if (itr != PlayerQuestItems.end() && questSlot < itr->second->size())
        {
            QuestItem* qitem2 = &itr->second->at(questSlot);
            if (qitem)
                *qitem = qitem2;
            item = &quest_items[qitem2->index];
            // pussywizard: follow_loot_rules 的任务物品加入每个玩家，但并非都允许，这里再校验
            // ★ follow_loot_rules 任务物品在 FillQuestLoot 时即使没接任务也可能进 ML 视图，
            //   但真正拾取时要二次校验 AllowedForPlayer
            if (item->follow_loot_rules && !item->AllowedForPlayer(player, sourceWorldObjectGUID)) // pussywizard: such items (follow_loot_rules) are added to every player, but not everyone is allowed, check it here
                return nullptr;
            is_looted = qitem2->is_looted;                  // 用 per-player 的 is_looted
        }
    }
    else
    {
        // 普通槽（lootSlot < items.size()）
        item = &items[lootSlot];
        is_looted = item->is_looted;                        // 默认用全局 is_looted
        if (item->freeforall)
        {
            // 路径 2：FFA 物品 → 从该玩家的 FFA 列表里查 per-player is_looted
            QuestItemMap::const_iterator itr = PlayerFFAItems.find(player->GetGUID());
            if (itr != PlayerFFAItems.end())
            {
                for (QuestItemList::const_iterator iter = itr->second->begin(); iter != itr->second->end(); ++iter)
                    if (iter->index == lootSlot)
                    {
                        QuestItem* ffaitem2 = (QuestItem*) & (*iter);
                        if (ffaitem)
                            *ffaitem = ffaitem2;
                        is_looted = ffaitem2->is_looted;    // 覆盖为 per-player 的 is_looted
                        break;
                    }
            }
        }
        else if (!item->conditions.empty())
        {
            // 路径 3：条件物品 → 从该玩家的条件列表里查 per-player is_looted
            QuestItemMap::const_iterator itr = PlayerNonQuestNonFFAConditionalItems.find(player->GetGUID());
            if (itr != PlayerNonQuestNonFFAConditionalItems.end())
            {
                for (QuestItemList::const_iterator iter = itr->second->begin(); iter != itr->second->end(); ++iter)
                {
                    if (iter->index == lootSlot)
                    {
                        QuestItem* conditem2 = (QuestItem*) & (*iter);
                        if (conditem)
                            *conditem = conditem2;
                        is_looted = conditem2->is_looted;   // 覆盖为 per-player 的 is_looted
                        break;
                    }
                }
            }
        }
        // 路径 4：普通物品 → 不进入任何分支，直接用全局 is_looted（上面已设）
    }

    if (is_looted)
        return nullptr;                                     // 已捡或不属于该玩家

    return item;
}

// 该玩家能看到的最大槽位数 = 普通物品数 + 自己的任务槽数
uint32 Loot::GetMaxSlotInLootFor(Player* player) const
{
    QuestItemMap::const_iterator itr = PlayerQuestItems.find(player->GetGUID());
    return items.size() + (itr != PlayerQuestItems.end() ?  itr->second->size() : 0);
}

// 是否还有任何"所有人都能看到"的物品（不含 FFA、不含条件、含金钱）
// 用于判断尸体是否完全空了
bool Loot::hasItemForAll() const
{
    // Gold is always lootable
    if (gold)
    {
        return true;
    }

    for (LootItem const& item : items)
        if (!item.is_looted && !item.freeforall && item.conditions.empty())
            return true;
    return false;
}

// return true if there is any FFA, quest or conditional item for the player.
// 是否还有该玩家专属可见的物品（任务/FFA/条件）
bool Loot::hasItemFor(Player* player) const
{
    // 任务物品
    QuestItemMap const& lootPlayerQuestItems = GetPlayerQuestItems();
    QuestItemMap::const_iterator q_itr = lootPlayerQuestItems.find(player->GetGUID());
    if (q_itr != lootPlayerQuestItems.end())
    {
        QuestItemList* q_list = q_itr->second;
        for (QuestItemList::const_iterator qi = q_list->begin(); qi != q_list->end(); ++qi)
        {
            const LootItem& item = quest_items[qi->index];
            if (!qi->is_looted && !item.is_looted)
                return true;
        }
    }

    // FFA 物品
    QuestItemMap const& lootPlayerFFAItems = GetPlayerFFAItems();
    QuestItemMap::const_iterator ffa_itr = lootPlayerFFAItems.find(player->GetGUID());
    if (ffa_itr != lootPlayerFFAItems.end())
    {
        QuestItemList* ffa_list = ffa_itr->second;
        for (QuestItemList::const_iterator fi = ffa_list->begin(); fi != ffa_list->end(); ++fi)
        {
            const LootItem& item = items[fi->index];
            if (!fi->is_looted && !item.is_looted)
                return true;
        }
    }

    // 条件物品
    QuestItemMap const& lootPlayerNonQuestNonFFAConditionalItems = GetPlayerNonQuestNonFFAConditionalItems();
    QuestItemMap::const_iterator nn_itr = lootPlayerNonQuestNonFFAConditionalItems.find(player->GetGUID());
    if (nn_itr != lootPlayerNonQuestNonFFAConditionalItems.end())
    {
        QuestItemList* conditional_list = nn_itr->second;
        for (QuestItemList::const_iterator ci = conditional_list->begin(); ci != conditional_list->end(); ++ci)
        {
            const LootItem& item = items[ci->index];
            if (!ci->is_looted && !item.is_looted)
                return true;
        }
    }

    return false;
}

// return true if there is any item over the group threshold (i.e. not underthreshold).
// 是否还有任何高于阈值的物品（决定是否触发队伍 ROLL）
bool Loot::hasOverThresholdItem() const
{
    for (uint8 i = 0; i < items.size(); ++i)
    {
        if (!items[i].is_looted && !items[i].is_underthreshold && !items[i].freeforall)
            return true;
    }

    return false;
}

// ============================================================================
// 序列化：把单个 LootItem 写入 ByteBuffer
// 字段：itemid, count, displayId, randomSuffix, randomPropertyId
// （slot_type 不在这里写，由 LootView operator<< 按权限决定）
// ============================================================================
ByteBuffer& operator<<(ByteBuffer& b, LootItem const& li)
{
    b << uint32(li.itemid);
    b << uint32(li.count);                                  // nr of items of this type
    b << uint32(sObjectMgr->GetItemTemplate(li.itemid)->DisplayInfoID);
    b << uint32(li.randomSuffix);
    b << uint32(li.randomPropertyId);
    //b << uint8(0);                                        // slot type - will send after this function call
    return b;
}

// ============================================================================
// LootView 序列化：SMSG_LOOT_RESPONSE 的核心
// 按 viewer 的 permission 决定每个槽位的 slot_type（ALLOW_LOOT/ROLL_ONGOING/MASTER/LOCKED/OWNER）
// 同一个 Loot，不同玩家看到的输出不同
//
// ★★★ slot_type 决策真值表（普通物品，GROUP/MASTER/RESTRICTED 权限）：
//   ┌──────────────────┬───────────────────────────────────────────────────────┐
//   │ 条件              │ slot_type                                              │
//   ├──────────────────┼───────────────────────────────────────────────────────┤
//   │ is_blocked + GROUP│ ROLL_ONGOING（ROLL 进行中）                            │
//   │ is_blocked + ML自己│ MASTER（ML 看到可分配槽）                              │
//   │ is_blocked + ML他人│ LOCKED（其他队员看到红色锁定）                         │
//   │ is_blocked + 受限 │ LOCKED                                                  │
//   │ rollWinnerGUID==自己│ OWNER（已 ROLL 到手，等背包空）                        │
//   │ rollWinnerGUID!=自己│ 隐藏（continue，整行不发）                            │
//   │ roundRobin空/是自己/高于阈值 │ ALLOW_LOOT（可捡）                       │
//   │ 其他（轮到别人）   │ 隐藏（continue）                                       │
//   └──────────────────┴───────────────────────────────────────────────────────┘
//
// ★ 物品分类与对应映射（避免重复显示）：
//   - 普通物品（无条件/非FFA）  → 主循环显示
//   - 任务物品                 → GetPlayerQuestItems 取
//   - FFA 物品                 → GetPlayerFFAItems 取（永远 ALLOW_LOOT）
//   - 条件物品（带 conditions） → GetPlayerNonQuestNonFFAConditionalItems 取
//
// ★ count_pos 占位回填技巧：
//   物品数要先发（客户端要读），但实际数量要遍历完才知道
//   所以先占 1 字节位置（b << uint8(0)），最后用 b.put<uint8>(pos, n) 回填
// ============================================================================
ByteBuffer& operator<<(ByteBuffer& b, LootView const& lv)
{
    // 无权限：发空 loot（金币 0 + 物品数 0）
    if (lv.permission == NONE_PERMISSION)
    {
        b << uint32(0);                                     //gold
        b << uint8(0);                                      // item count
        return b;                                           // nothing output more
    }

    Loot& l = lv.loot;

    uint8 itemsShown = 0;

    b << uint32(l.gold);                                    //gold

    std::size_t count_pos = b.wpos();                            // pos of item count byte
    b << uint8(0);                                          // item count placeholder    // 先占位，最后回填实际数量

    switch (lv.permission)
    {
        // ---------------------------------------------------------------------------
        // 队伍 / ML / 受限权限：处理阈值物品的 ROLL/ML 逻辑
        // 这是 loot 系统中最复杂的权限分支
        // ---------------------------------------------------------------------------
        case GROUP_PERMISSION:
        case MASTER_PERMISSION:
        case RESTRICTED_PERMISSION:
            {
                bool isMasterLooter = lv.viewer->GetGroup() && lv.viewer->GetGroup()->GetMasterLooterGuid() == lv.viewer->GetGUID();

                // if you are not the round-robin group looter, you can only see
                // blocked rolled items and quest items, and !ffa items
                // ★ 可见性过滤四条件：
                //   1) !is_looted（未捡）
                //   2) !freeforall（FFA 物品由专门的 PlayerFFAItems 处理，这里跳过避免重复）
                //   3) conditions.empty() || isMasterLooter（ML 可看到条件物品，其他人看不到他人的条件物品）
                //   4) AllowedForPlayer（阵营/任务/专业等通过）
                for (uint8 i = 0; i < l.items.size(); ++i)
                {
                    if (!l.items[i].is_looted && !l.items[i].freeforall && (l.items[i].conditions.empty() || isMasterLooter) && l.items[i].AllowedForPlayer(lv.viewer, l.sourceWorldObjectGUID))
                    {
                        uint8 slot_type = 0;

                        if (l.items[i].is_blocked) // for ML & restricted is_blocked = !is_underthreshold
                        {
                            // ★ is_blocked 含义：该物品被锁定（ROLL 中 / ML 持有 / 阈值之上待 ROLL）
                            //   Group::GroupLoot/NBG/MasterLoot 在创建 Roll 时设此标志
                            switch (lv.permission)
                            {
                                case GROUP_PERMISSION:
                                    slot_type = LOOT_SLOT_TYPE_ROLL_ONGOING;   // 队伍 ROLL 中（黄色图标）
                                    break;
                                case MASTER_PERMISSION:
                                    {
                                        // ML 模式：ML 本人看 MASTER（可点击分配），其他人看 LOCKED（红色）
                                        if (lv.viewer->GetGroup())
                                        {
                                            if (lv.viewer->GetGroup()->GetMasterLooterGuid() == lv.viewer->GetGUID())
                                                slot_type = LOOT_SLOT_TYPE_MASTER;
                                            else
                                                slot_type = LOOT_SLOT_TYPE_LOCKED;
                                        }
                                        break;
                                    }
                                case RESTRICTED_PERMISSION:
                                    slot_type = LOOT_SLOT_TYPE_LOCKED;          // 受限：红色锁定（非 ML 看到 ML 物品）
                                    break;
                                default:
                                    continue;
                            }
                        }
                        else if (l.items[i].rollWinnerGUID)
                        {
                            // ★ 已 ROLL 出胜者但物品还在 loot 里（典型：胜者背包满）
                            //   只有胜者能看（OWNER），其他人 continue 隐藏整行
                            if (l.items[i].rollWinnerGUID == lv.viewer->GetGUID())
                                slot_type = LOOT_SLOT_TYPE_OWNER;
                            else
                                continue;                           // 别人的战利品，隐藏
                        }
                        else if (!l.roundRobinPlayer || lv.viewer->GetGUID() == l.roundRobinPlayer || !l.items[i].is_underthreshold)
                        {
                            // no round robin owner or he has released the loot
                            // or it IS the round robin group owner
                            // => item is lootable
                            // ★ 三个允许捡的条件（满足任一即可）：
                            //   1) round-robin 持有者为空（已释放或没人接）
                            //   2) 自己就是 round-robin 持有者
                            //   3) 物品高于阈值（高阈值物品不走 round-robin，全队都能捡直到 ROLL 触发）
                            slot_type = LOOT_SLOT_TYPE_ALLOW_LOOT;
                        }
                        else
                            // item shall not be displayed.
                            continue;                           // 轮到别人的回合：隐藏（低于阈值的物品按 round-robin 轮流）

                        b << uint8(i) << l.items[i];
                        b << uint8(slot_type);
                        ++itemsShown;
                    }
                }
                break;
            }
        // ---------------------------------------------------------------------------
        // 轮流捡：只有 round-robin 持有者能看
        // ---------------------------------------------------------------------------
        case ROUND_ROBIN_PERMISSION:
            {
                for (uint8 i = 0; i < l.items.size(); ++i)
                {
                    if (!l.items[i].is_looted && !l.items[i].freeforall && l.items[i].conditions.empty() && l.items[i].AllowedForPlayer(lv.viewer, l.sourceWorldObjectGUID))
                    {
                        // 不是当前 round-robin 持有者 -> 隐藏
                        if (l.roundRobinPlayer && lv.viewer->GetGUID() != l.roundRobinPlayer)
                            // item shall not be displayed.
                            continue;

                        b << uint8(i) << l.items[i];
                        b << uint8(LOOT_SLOT_TYPE_ALLOW_LOOT);
                        ++itemsShown;
                    }
                }
                break;
            }
        // ---------------------------------------------------------------------------
        // FFA / 所有者：所有可见物品都能捡
        // ---------------------------------------------------------------------------
        case ALL_PERMISSION:
        case OWNER_PERMISSION:
            {
                // 所有者用 OWNER（避免绑定确认弹窗），FFA 用 ALLOW_LOOT
                uint8 slot_type = lv.permission == OWNER_PERMISSION ? LOOT_SLOT_TYPE_OWNER : LOOT_SLOT_TYPE_ALLOW_LOOT;
                for (uint8 i = 0; i < l.items.size(); ++i)
                {
                    if (!l.items[i].is_looted && !l.items[i].freeforall && l.items[i].conditions.empty() && l.items[i].AllowedForPlayer(lv.viewer, l.sourceWorldObjectGUID))
                    {
                        b << uint8(i) << l.items[i];
                        b << uint8(slot_type);
                        ++itemsShown;
                    }
                }
                break;
            }
        default:
            return b;
    }

    // 默认槽位类型：所有者用 OWNER，其他用 ALLOW_LOOT
    // 用于下面的任务/FFA/条件物品分支
    LootSlotType slotType = lv.permission == OWNER_PERMISSION ? LOOT_SLOT_TYPE_OWNER : LOOT_SLOT_TYPE_ALLOW_LOOT;

    // Xinef: items that do not follow loot rules need this
    // 队伍物品在 ML 模式下用 MASTER
    LootSlotType partySlotType = lv.permission == MASTER_PERMISSION ? LOOT_SLOT_TYPE_MASTER : slotType;

    // ---------------------------------------------------------------------------
    // 任务物品（在普通物品之后，客户端槽位 = items.size() + i）
    //
    // ★ 三种情况：
    //   1) 玩家没接任务 + showInLoot=false：完全隐藏（标记 is_looted 让客户端不再请求）
    //   2) 玩家没接任务 + showInLoot=true ：灰显（LOCKED 或 MASTER），让玩家知道"存在但捡不了"
    //   3) 玩家接了任务                  ：按 follow_loot_rules 决定 slot_type
    //
    // ★ follow_loot_rules 决策树（任务物品的两种行为）：
    //   follow_loot_rules=true：遵循队伍规则（按 permission 给 MASTER/LOCKED/ALLOW/ROLL）
    //     典型：BOSS 掉的任务物品也走 ML 分配
    //   follow_loot_rules=false：忽略队伍规则
    //     - 非 FFA：用 partySlotType（ML 模式下 = MASTER，其他 = slotType）
    //     - FFA：用 slotType（人人可捡）
    // ---------------------------------------------------------------------------
    QuestItemMap const& lootPlayerQuestItems = l.GetPlayerQuestItems();
    QuestItemMap::const_iterator q_itr = lootPlayerQuestItems.find(lv.viewer->GetGUID());
    if (q_itr != lootPlayerQuestItems.end())
    {
        QuestItemList* q_list = q_itr->second;
        for (QuestItemList::const_iterator qi = q_list->begin(); qi != q_list->end(); ++qi)
        {
            LootItem& item = l.quest_items[qi->index];
            if (!qi->is_looted && !item.is_looted)
            {
                bool showInLoot = true;
                // ★ HasQuestForItem 第 4 个参数 showInLoot 是输出参数：
                //   返回 false 时表示玩家没接任务，showInLoot 表示该物品是否还应该灰显给玩家看
                bool hasQuestForItem = lv.viewer->HasQuestForItem(item.itemid, 0, false, &showInLoot);
                if (!hasQuestForItem)
                {
                    // 玩家没接对应任务
                    if (!showInLoot)
                    {
                        // 情况 1：该物品对玩家完全不可见（showInLoot=false）
                        // ★ 直接标记 is_looted=true：让客户端认为已捡，不再发 CMSG 请求
                        //   这是一种"清理"操作，避免客户端反复请求一个永远捡不到的物品
                        const_cast<QuestItem*>(&(*qi))->is_looted = true;
                        if (!item.freeforall)
                        {
                            item.is_looted = true;         // 非 FFA：全局标记已捡
                        }
                        continue;
                    }

                    // 情况 2：灰显（让玩家看到但捡不了）
                    // 客户端槽位 = items.size() + (qi - q_list->begin())（任务槽索引）
                    b << uint8(l.items.size() + (qi - q_list->begin()));
                    b << item;
                    // ML 模式给 MASTER（ML 能分配），其他模式给 LOCKED（红色，不可捡）
                    b << uint8(lv.permission == MASTER_PERMISSION ? LOOT_SLOT_TYPE_MASTER : LOOT_SLOT_TYPE_LOCKED);
                }
                else
                {
                    // 情况 3：玩家接了任务，可以捡
                    b << uint8(l.items.size() + (qi - q_list->begin()));
                    b << item;

                    if (item.follow_loot_rules)
                    {
                        // ★ 遵循队伍规则的任务物品：和普通物品同样的权限分支
                        //   典型：BOSS 掉的关键任务物品也走 ML/ROLL
                        switch (lv.permission)
                        {
                            case MASTER_PERMISSION:
                                b << uint8(LOOT_SLOT_TYPE_MASTER);
                                break;
                            case RESTRICTED_PERMISSION:
                                // is_blocked 时 LOCKED，否则默认 slotType
                                b << (item.is_blocked ? uint8(LOOT_SLOT_TYPE_LOCKED) : uint8(slotType));
                                break;
                            case GROUP_PERMISSION:
                            case ROUND_ROBIN_PERMISSION:
                                // 队伍权限：is_blocked 表示 ROLL 中
                                if (!item.is_blocked)
                                    b << uint8(LOOT_SLOT_TYPE_ALLOW_LOOT);
                                else
                                    b << uint8(LOOT_SLOT_TYPE_ROLL_ONGOING);
                                break;
                            default:
                                b << uint8(slotType);      // ALL/OWNER 权限
                                break;
                        }
                    }
                    else if (!item.freeforall)
                        // ★ 不遵循规则的非 FFA 任务物品：用 partySlotType
                        //   ML 模式下 = MASTER，其他模式 = slotType
                        b << uint8(partySlotType);
                    else
                        // ★ FFA 任务物品：人人可捡，用 slotType
                        b << uint8(slotType);
                }

                ++itemsShown;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // FFA 物品（每个玩家独立可见，slot_type 永远 ALLOW_LOOT）
    // ★ 为什么 FFA 物品不按 permission 区分：
    //   FFA = Free For All（人人可拾取），不论队伍模式如何，符合 AllowedForPlayer 的玩家都能捡
    // ---------------------------------------------------------------------------
    QuestItemMap const& lootPlayerFFAItems = l.GetPlayerFFAItems();
    QuestItemMap::const_iterator ffa_itr = lootPlayerFFAItems.find(lv.viewer->GetGUID());
    if (ffa_itr != lootPlayerFFAItems.end())
    {
        QuestItemList* ffa_list = ffa_itr->second;
        for (QuestItemList::const_iterator fi = ffa_list->begin(); fi != ffa_list->end(); ++fi)
        {
            LootItem& item = l.items[fi->index];
            if (!fi->is_looted && !item.is_looted)
            {
                b << uint8(fi->index);                    // FFA 物品槽位 = items 中的下标
                b << item;
                // Xinef: Here are FFA items, so dont use owner permision
                // ★ FFA 永远 ALLOW_LOOT，即使 viewer 是 OWNER_PERMISSION 也一样
                b << uint8(LOOT_SLOT_TYPE_ALLOW_LOOT /*slotType*/);
                ++itemsShown;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // 条件物品（非任务、非 FFA，但带 conditions）
    // ★ 与任务物品 follow_loot_rules=true 分支的决策树完全相同（代码复用）
    // ---------------------------------------------------------------------------
    QuestItemMap const& lootPlayerNonQuestNonFFAConditionalItems = l.GetPlayerNonQuestNonFFAConditionalItems();
    QuestItemMap::const_iterator nn_itr = lootPlayerNonQuestNonFFAConditionalItems.find(lv.viewer->GetGUID());
    if (nn_itr != lootPlayerNonQuestNonFFAConditionalItems.end())
    {
        QuestItemList* conditional_list = nn_itr->second;
        for (QuestItemList::const_iterator ci = conditional_list->begin(); ci != conditional_list->end(); ++ci)
        {
            LootItem& item = l.items[ci->index];
            if (!ci->is_looted && !item.is_looted)
            {
                b << uint8(ci->index);
                b << item;
                if (item.follow_loot_rules)
                {
                    // 遵循队伍规则的条件物品：和任务物品同样的权限分支
                    switch (lv.permission)
                    {
                        case MASTER_PERMISSION:
                            b << uint8(LOOT_SLOT_TYPE_MASTER);
                            break;
                        case RESTRICTED_PERMISSION:
                            b << (item.is_blocked ? uint8(LOOT_SLOT_TYPE_LOCKED) : uint8(slotType));
                            break;
                        case GROUP_PERMISSION:
                        case ROUND_ROBIN_PERMISSION:
                            if (!item.is_blocked)
                                b << uint8(LOOT_SLOT_TYPE_ALLOW_LOOT);
                            else
                                b << uint8(LOOT_SLOT_TYPE_ROLL_ONGOING);
                            break;
                        default:
                            b << uint8(slotType);
                            break;
                    }
                }
                else if (!item.freeforall)
                    b << uint8(partySlotType);
                else
                    b << uint8(slotType);
                ++itemsShown;
            }
        }
    }

    //update number of items shown
    // 回填实际可见物品数（之前占位的位置）
    b.put<uint8>(count_pos, itemsShown);

    return b;
}

//
// --------- LootTemplate::LootGroup ---------
//

// 析构：释放组内所有 LootStoreItem
LootTemplate::LootGroup::~LootGroup()
{
    while (!ExplicitlyChanced.empty())
    {
        delete ExplicitlyChanced.back();
        ExplicitlyChanced.pop_back();
    }

    while (!EqualChanced.empty())
    {
        delete EqualChanced.back();
        EqualChanced.pop_back();
    }
}

// Adds an entry to the group (at loading stage)
// ★ 加载时分流：Chance > 0 进 ExplicitlyChanced，== 0 进 EqualChanced
// 为什么用两个 list 而非一个：Roll 算法对两类条目处理方式不同（显式累减 / 等概率随机）
void LootTemplate::LootGroup::AddEntry(LootStoreItem* item)
{
    if (item->chance != 0)
        ExplicitlyChanced.push_back(item);
    else
        EqualChanced.push_back(item);
}

// ============================================================================
// 模式二：组内单次摇骰选一个（核心算法）
//
// ★ 累减法（Cumulative Subtraction）原理：
//   想象一个 [0, 100) 的数轴，把每个显式条目的 chance 当作一段区间首尾相接：
//     [0, c1) | [c1, c1+c2) | [c1+c2, c1+c2+c3) | ...
//   一次 rand_chance() 抽一个点 r，看落在哪段区间，对应条目就是赢家。
//   代码实现：roll = rand_chance()，遍历时 roll -= chance，第一次 roll<0 的条目胜出。
//
// ★ 例子：3 个显式条目 chance=20/30/50（和=100）
//   抽到 r=45 → 减 20 得 25 (>0) → 减 30 得 -5 (<0) → 第 2 个胜出
//
// ★ 边界情况：
//   - 显式总和 < 100 且没命中任何段（r 落在 [总和, 100)）→ 进等概率回退
//   - 显式总和 > 100：永远不会触发等概率回退（设计错误，Verify 会警告）
//   - 某条目 chance >= 100：直接必中（不等其他条目）
//   - possibleLoot 为空（全被 InvalidSelector 过滤掉）→ 跳到等概率
//
// ★ 等概率回退（EqualChanced）：
//   Chance==0 的条目均匀随机选一个（SelectRandomContainerElement）
//   设计意图：让 DBA 不用精确算概率，写"5 个装备 chance=0 各 20%"即可
// ============================================================================
// Rolls an item from the group, returns nullptr if all miss their chances
LootStoreItem const* LootTemplate::LootGroup::Roll(Loot& loot, Player const* player, LootStore const& store, uint16 lootMode) const
{
    // 拷贝一份显式条目（remove_if 会修改副本，不影响原 list）
    LootStoreItemList possibleLoot = ExplicitlyChanced;
    possibleLoot.remove_if(LootGroupInvalidSelector(loot, lootMode));   // 过滤 lootmode 不符 + 重复超额

    if (!possibleLoot.empty())                             // First explicitly chanced entries are checked
    {
        float roll = (float)rand_chance();                // 整组只摇一次 [0, 100)（关键：整组共享一个随机数）

        for (LootStoreItemList::const_iterator itr = possibleLoot.begin(); itr != possibleLoot.end(); ++itr)   // check each explicitly chanced entry in the template and modify its chance based on quality.
        {
            LootStoreItem* item = *itr;
            float chance = item->chance;

            // 脚本钩子：可改 chance 或否决整组（return nullptr 让本次组 roll 落空）
            // ★ 注意：这里如果脚本否决会直接结束整组的 roll，不会继续后续条目
            if (!sScriptMgr->OnItemRoll(player, item, chance, loot, store))
                return nullptr;

            // 必中（chance >= 100）：立即返回，不等其他条目
            // ★ 这就是为什么 DB 里写 100 的条目总是优先掉
            if (chance >= 100.0f)
                return item;

            // 累减法核心：从 roll 中扣掉当前条目的 chance
            roll -= chance;
            if (roll < 0)
                return item;                              // 命中：当前条目就是赢家
        }
        // 循环正常结束 = 显式全部未中（roll 仍 >= 0），继续走等概率回退
    }

    // 显式全未中：脚本钩子，可否决等概率回退（让本组本次空掉）
    if (!sScriptMgr->OnBeforeLootEqualChanced(player, EqualChanced, loot, store))
        return nullptr;

    // 等概率池：均匀随机挑一个（每个候选概率 = 1/n）
    // ★ 注意这里也走 InvalidSelector 过滤，保证不会选到超额/模式不符的候选
    possibleLoot = EqualChanced;
    possibleLoot.remove_if(LootGroupInvalidSelector(loot, lootMode));
    if (!possibleLoot.empty())                              // If nothing selected yet - an item is taken from equal-chanced part
        return Acore::Containers::SelectRandomContainerElement(possibleLoot);

    return nullptr;                                            // Empty drop from the group    // 显式和等概率都没命中，本组本次什么都不掉
}

// True if group includes at least 1 quest drop entry
// 是否含任务掉落（递归查引用）
bool LootTemplate::LootGroup::HasQuestDrop(LootTemplateMap const& store) const
{
    // 显式条目
    for (LootStoreItemList::const_iterator i = ExplicitlyChanced.begin(); i != ExplicitlyChanced.end(); ++i)
    {
        LootStoreItem* item = *i;
        if (item->reference) // References
        {
            LootTemplateMap::const_iterator Referenced = store.find(std::abs(item->reference));
            if (Referenced == store.end())
            {
                continue; // Error message [should be] already printed at loading stage
            }

            if (Referenced->second->HasQuestDrop(store))
            {
                return true;
            }
        }
        else if (item->needs_quest)
        {
            return true;
        }
    }

    // 等概率条目
    for (LootStoreItemList::const_iterator i = EqualChanced.begin(); i != EqualChanced.end(); ++i)
    {
        LootStoreItem* item = *i;
        if (item->reference) // References
        {
            LootTemplateMap::const_iterator Referenced = store.find(std::abs(item->reference));
            if (Referenced == store.end())
            {
                continue; // Error message [should be] already printed at loading stage
            }

            if (Referenced->second->HasQuestDrop(store))
            {
                return true;
            }
        }
        else if (item->needs_quest)
        {
            return true;
        }
    }

    return false;
}

// True if group includes at least 1 quest drop entry for active quests of the player
// 是否含玩家当前任务需要的掉落
bool LootTemplate::LootGroup::HasQuestDropForPlayer(Player const* player, LootTemplateMap const& store) const
{
    for (LootStoreItemList::const_iterator i = ExplicitlyChanced.begin(); i != ExplicitlyChanced.end(); ++i)
    {
        LootStoreItem* item = *i;
        if (item->reference)                        // References processing
        {
            LootTemplateMap::const_iterator Referenced = store.find(std::abs(item->reference));
            if (Referenced == store.end())
            {
                continue;                                   // Error message already printed at loading stage
            }

            if (Referenced->second->HasQuestDropForPlayer(store, player))
            {
                return true;
            }
        }
        else if (player->HasQuestForItem(item->itemid))
        {
            return true;                                    // active quest drop found
        }
    }

    for (LootStoreItemList::const_iterator i = EqualChanced.begin(); i != EqualChanced.end(); ++i)
    {
        LootStoreItem* item = *i;
        if (item->reference)                        // References processing
        {
            LootTemplateMap::const_iterator Referenced = store.find(std::abs(item->reference));
            if (Referenced == store.end())
            {
                continue;                                   // Error message already printed at loading stage
            }

            if (Referenced->second->HasQuestDropForPlayer(store, player))
            {
                return true;
            }
        }
        else if (player->HasQuestForItem(item->itemid))
        {
            return true;                                    // active quest drop found
        }
    }

    return false;
}

// 清空组内所有条目的 conditions（reload 时用）
void LootTemplate::LootGroup::CopyConditions(ConditionList /*conditions*/)
{
    for (LootStoreItemList::iterator i = ExplicitlyChanced.begin(); i != ExplicitlyChanced.end(); ++i)
        (*i)->conditions.clear();

    for (LootStoreItemList::iterator i = EqualChanced.begin(); i != EqualChanced.end(); ++i)
        (*i)->conditions.clear();
}

// ============================================================================
// 模式三：组的入口处理器（在 LootTemplate::Process 中对每个组调用）
//
// ★ 四步处理：
//   1) 调 Roll() 选一个赢家
//   2) 引用条目 → 递归处理 reference_loot_template（maxcount × REFERENCED_AMOUNT 为递归次数）
//   3) 普通条目 → 脚本钩子 → loot.AddItem 落袋
//   4) 若 nonRefIterationsLeft > 1 且非任务物品 → 递归调本函数再 roll 一次
//
// ★ nonRefIterationsLeft 的来源：
//   - 顶层模板：sWorld->getRate(RATE_DROP_ITEM_GROUP_AMOUNT)（默认 1）
//   - 引用内部的组：恒为 0（防止引用无限放大组数）
//   设计意图：让运维通过配置让一个组掉多个物品（典型用例：BOSS 掉落多件装备）
//
// ★ 为什么任务物品不能多次 roll（!item->needs_quest 检查）：
//   一人接一个任务，若组内多次 roll 同一任务物品会让玩家一次拿多份，
//   破坏任务进度系统（HasQuestForItem 只检查数量是否够，多份会直接销毁浪费）
// ============================================================================
// Rolls an item from the group (if any takes its chance) and adds the item to the loot
void LootTemplate::LootGroup::Process(Loot& loot, Player const* player, LootStore const& store, uint16 lootMode, uint16 nonRefIterationsLeft) const
{
    // 步骤 1：摇骰选赢家（可能返回 nullptr = 本组本次空掉）
    if (LootStoreItem const* item = Roll(loot, player, store, lootMode))
    {
        bool rate = store.IsRatesAllowed();

        if (item->reference) // References processing
        {
            // 步骤 2a：引用条目 → 递归到 reference_loot_template
            // ★ 注意：组内理论上禁止放引用（IsValid 时报警告），这里仍兼容处理
            if (LootTemplate const* Referenced = LootTemplates_Reference.GetLootFor(std::abs(item->reference)))
            {
                // maxcount 作为引用递归次数倍数（× REFERENCED_AMOUNT rate）
                uint32 maxcount = uint32(float(item->maxcount) * sWorld->getRate(RATE_DROP_ITEM_REFERENCED_AMOUNT));
                sScriptMgr->OnAfterRefCount(player, loot, rate, lootMode, const_cast<LootStoreItem*>(item), maxcount, store);
                for (uint32 loop = 0; loop < maxcount; ++loop) // Ref multiplicator
                    // This reference needs to be processed further, but it is marked isTopLevel=false so that any groups inside
                    // the reference are not multiplied by Rate.Drop.Item.GroupAmount
                    // ★ isTopLevel=false 关键：防止引用内部的组又被 GroupAmount 放大（指数爆炸）
                    Referenced->Process(loot, store, lootMode, player, 0, false);
            }
        }
        else
        {
            // 步骤 2b：普通物品 → 脚本钩子 → 直接加入 loot
            // Plain entries (not a reference, not grouped)
            sScriptMgr->OnBeforeDropAddItem(player, loot, rate, lootMode, const_cast<LootStoreItem*>(item), store);
            loot.AddItem(*item); // Chance is already checked, just add    // Roll 已确认命中，直接落袋

            // 步骤 3：若还能再 roll 且非任务物品，递归本函数产生更多掉落
            // If we still have non-ref runs to do for this group AND this item wasn't a reference,
            // recursively call this function to produce more items for this group.
            // However, if this is a quest item we shouldn't multiply this group.
            // ★ 注意：递归时 nonRefIterationsLeft-1，最终会减到 1 停止
            //   每次 Roll 都会重新过滤 InvalidSelector，所以重复上限仍生效
            if (nonRefIterationsLeft > 1 && !item->needs_quest)
            {
                this->Process(loot, player, store, lootMode, nonRefIterationsLeft-1);
            }
        }
    }
}

// Overall chance for the group without equal chanced items
// ★ 显式条目（非任务）的概率总和
// 注意：任务物品的 chance 不计入（任务物品独立 roll，不与普通物品争抢概率）
float LootTemplate::LootGroup::RawTotalChance() const
{
    float result = 0;

    for (LootStoreItemList::const_iterator i = ExplicitlyChanced.begin(); i != ExplicitlyChanced.end(); ++i)
        if (!(*i)->needs_quest)
            result += (*i)->chance;

    return result;
}

// Overall chance for the group
// ★ 含等概率回退时的总概率：有等概率项且显式和 < 100 时返回 100（因为总会命中一个）
// 这就是为什么"显式 50% + 等概率 3 个"的实际效果是：
//   50% 概率掉显式条目，50% 概率从等概率 3 个中随机挑一个（每个实际概率 50%/3 ≈ 16.67%）
float LootTemplate::LootGroup::TotalChance() const
{
    float result = RawTotalChance();

    if (!EqualChanced.empty() && result < 100.0f)
        return 100.0f;                                  // 有等概率项时，剩余概率必然落到等概率池

    return result;
}

// 启动校验：组概率总和（> 101% 报错，等概率与显式并存且和>=100% 也报错）
// ★ 两条规则的含义：
//   1) 显式和 > 101%：DBA 写错概率（本应 ≤ 100），会让 roll 永远命中前面几个条目
//      ★ 用 101 而非 100 是容差（浮点精度 + 历史 DB 残留），TODO 注释说未来收紧到 100
//   2) 显式和 >= 100 且有等概率项：等概率项永远不可能被触发（设计冗余）
//      因为 Roll 中显式条目累减已让 roll<0，永远不会走到等概率分支
void LootTemplate::LootGroup::Verify(LootStore const& lootstore, uint32 id, uint8 group_id) const
{
    float chance = RawTotalChance();
    if (chance > 101.0f)                                    /// @todo: replace with 100% when DBs will be ready
    {
        LOG_ERROR("sql.sql", "Table '{}' entry {} group {} has total chance > 100% ({})", lootstore.GetName(), id, group_id, chance);
    }

    if (chance >= 100.0f && !EqualChanced.empty())
    {
        LOG_ERROR("sql.sql", "Table '{}' entry {} group {} has items with chance=0% but group total chance >= 100% ({})", lootstore.GetName(), id, group_id, chance);
    }
}

// 检查组内引用目标是否存在（mincount != maxcount 也报错）
void LootTemplate::LootGroup::CheckLootRefs(LootStore const& lootstore, uint32 Id, LootIdSet* ref_set) const
{
    for (LootStoreItemList::const_iterator ieItr = ExplicitlyChanced.begin(); ieItr != ExplicitlyChanced.end(); ++ieItr)
    {
        LootStoreItem* item = *ieItr;
        if (item->reference)
        {
            if (item->mincount != item->maxcount)
                LootTemplates_Reference.ReportInvalidCount(std::abs(item->reference), lootstore.GetName(), Id, item->itemid, item->mincount, item->maxcount);

            if (!LootTemplates_Reference.GetLootFor(std::abs(item->reference)))
                LootTemplates_Reference.ReportNonExistingId(std::abs(item->reference), lootstore.GetName(), item->itemid);
            else if (ref_set)
                ref_set->erase(std::abs(item->reference));   // 标记该引用已被使用
        }
    }

    for (LootStoreItemList::const_iterator ieItr = EqualChanced.begin(); ieItr != EqualChanced.end(); ++ieItr)
    {
        LootStoreItem* item = *ieItr;
        if (item->reference)
        {
            if (item->mincount != item->maxcount)
                LootTemplates_Reference.ReportInvalidCount(std::abs(item->reference), lootstore.GetName(), Id, item->itemid, item->mincount, item->maxcount);

            if (!LootTemplates_Reference.GetLootFor(std::abs(item->reference)))
                LootTemplates_Reference.ReportNonExistingId(std::abs(item->reference), lootstore.GetName(), item->itemid);
            else if (ref_set)
                ref_set->erase(std::abs(item->reference));
        }
    }
}

//
// --------- LootTemplate ---------
//

// 析构：释放所有 Entries 和 Groups
LootTemplate::~LootTemplate()
{
    while (!Entries.empty())
    {
        delete Entries.back();
        Entries.pop_back();
    }

    for (std::size_t i = 0; i < Groups.size(); ++i)
        delete Groups[i];
    Groups.clear();
}

// Adds an entry to the group (at loading stage)
// ★ 加载分流：groupid > 0 进 Groups[gid-1]，== 0 进 Entries
// 注意 groupid 与 Groups 下标的偏移：groupid=1 → Groups[0]
// （这样 groupid=0 可以保留为"未分组"的哨兵值）
void LootTemplate::AddEntry(LootStoreItem* item)
{
    if (item->groupid > 0)  // Group and grouped reference
    {
        // 按需扩容 Groups 数组（DB 行可能乱序到达，先看到 groupid=3 再看到 groupid=1）
        if (item->groupid >= Groups.size())
        {
            Groups.resize(item->groupid, nullptr);  // Adds new group the the loot template if needed
        }

        // 该组还没创建就 new 一个
        if (!Groups[item->groupid - 1])
        {
            Groups[item->groupid - 1] = new LootGroup();
        }

        // 加入组（组内再按 chance 分到 ExplicitlyChanced / EqualChanced）
        Groups[item->groupid - 1]->AddEntry(item);  // Adds new entry to the group
    }
    else                                            // Non-grouped entries
        // 未分组：直接进 Entries（模式一，每个独立 roll）
        Entries.push_back(item);
}

// 清空所有条目的 conditions（reload 用）
void LootTemplate::CopyConditions(ConditionList conditions)
{
    for (LootStoreItemList::iterator i = Entries.begin(); i != Entries.end(); ++i)
        (*i)->conditions.clear();

    for (LootGroups::iterator i = Groups.begin(); i != Groups.end(); ++i)
        if (LootGroup* group = *i)
            group->CopyConditions(conditions);
}

// ============================================================================
// 把条件复制到指定 LootItem（LootItemStorage 还原时用）
// 在 Entries / 各组的 ExplicitlyChanced / EqualChanced 中找 itemid 匹配的条目
// 引用条目则递归进引用模板找
// conditionLootId 用于精确定位（同名 itemid 多处出现时）
// ============================================================================
bool LootTemplate::CopyConditions(LootItem* li, uint32 conditionLootId) const
{
    // 在未分组条目中找
    for (LootStoreItemList::const_iterator _iter = Entries.begin(); _iter != Entries.end(); ++_iter)
    {
        LootStoreItem* item = *_iter;
        if (item->reference)
        {
            // 引用：递归进引用模板找
            if (LootTemplate const* Referenced = LootTemplates_Reference.GetLootFor(std::abs(item->reference)))
            {
                if (Referenced->CopyConditions(li, conditionLootId))
                {
                    return true;
                }
            }
        }
        else
        {
            // 物品 id 不匹配：跳过
            if (item->itemid != li->itemid)
            {
                continue;
            }

            // conditionLootId 不匹配（同名物品多模板出现时）：跳过
            if (!item->conditions.empty() && conditionLootId && conditionLootId != item->conditions.front()->SourceGroup)
            {
                continue;
            }

            li->conditions = item->conditions;
            return true;
        }
    }

    // 在各组的 ExplicitlyChanced / EqualChanced 中找（逻辑同上）
    for (LootGroups::const_iterator groupItr = Groups.begin(); groupItr != Groups.end(); ++groupItr)
    {
        LootGroup* group = *groupItr;
        if (!group)
            continue;

        LootStoreItemList* itemList = group->GetExplicitlyChancedItemList();
        for (LootStoreItemList::iterator i = itemList->begin(); i != itemList->end(); ++i)
        {
            LootStoreItem* item = *i;
            if (item->reference)
            {
                if (LootTemplate const* Referenced = LootTemplates_Reference.GetLootFor(std::abs(item->reference)))
                {
                    if (Referenced->CopyConditions(li, conditionLootId))
                    {
                        return true;
                    }
                }
            }
            else
            {
                if (item->itemid != li->itemid)
                {
                    continue;
                }

                if (!item->conditions.empty() && conditionLootId && conditionLootId != item->conditions.front()->SourceGroup)
                {
                    continue;
                }

                li->conditions = item->conditions;
                return true;
            }
        }

        itemList = group->GetEqualChancedItemList();
        for (LootStoreItemList::iterator i = itemList->begin(); i != itemList->end(); ++i)
        {
            LootStoreItem* item = *i;
            if (item->reference)
            {
                if (LootTemplate const* Referenced = LootTemplates_Reference.GetLootFor(std::abs(item->reference)))
                {
                    if (Referenced->CopyConditions(li, conditionLootId))
                    {
                        return true;
                    }
                }
            }
            else
            {
                if (item->itemid != li->itemid)
                {
                    continue;
                }

                if (!item->conditions.empty() && conditionLootId && conditionLootId != item->conditions.front()->SourceGroup)
                {
                    continue;
                }

                li->conditions = item->conditions;
                return true;
            }
        }
    }

    return false;
}

// ============================================================================
// 模板主处理器：完成所有 roll，命中的物品通过 loot.AddItem 落袋
// 这是整个掉落系统的"心脏"，由 Loot::FillLoot 在顶层调用一次
//
// ★ 三大分支（按 groupId 和条目类型分流）：
//   (A) groupId != 0：只处理指定组（引用指向具体组的情况，跳过 Entries 和其他组）
//   (B) groupId == 0 + 遍历 Entries：每个未分组条目独立 roll（模式一）
//   (C) groupId == 0 + 遍历 Groups：每组调 LootGroup::Process（模式二+三）
//
// ★ isTopLevel 参数的作用（防止指数放大）：
//   - true：当前是"顶层"模板（被 FillLoot 直接调用），可应用 RATE_DROP_ITEM_GROUP_AMOUNT
//   - false：当前是从引用递归进来的，组数倍率已由上层决定，这里不再放大
//   设计意图：避免"引用 → 组 → 引用 → 组"链路上 GroupAmount 被反复相乘
//
// ★ 引用条目的递归流程（关键路径）：
//   顶层 Entries 引用命中 → maxcount × REFERENCED_AMOUNT → 循环 N 次调
//     Referenced->Process(loot, store, lootMode, player, item->groupid, false)
//   ★ item->groupid 传进去：让引用可以选择"只跑引用模板的某个组"
//   ★ isTopLevel=false：组数不再放大
// ============================================================================
// Rolls for every item in the template and adds the rolled items the the loot
void LootTemplate::Process(Loot& loot, LootStore const& store, uint16 lootMode, Player const* player, uint8 groupId, bool isTopLevel) const
{
    bool rate = store.IsRatesAllowed();

    if (groupId)                                            // Group reference uses own processing of the group
    {
        // (A) 引用指定的某个组（item->groupid 非 0 时由引用路径传入）
        if (groupId > Groups.size())
            return;                                         // Error message already printed at loading stage

        if (!Groups[groupId - 1])
            return;

        // Rate.Drop.Item.GroupAmount is only in effect for the top loot template level
        // ★ 顶层时才传入 GroupAmount；否则传 0 表示只 roll 一次
        if (isTopLevel)
        {
            Groups[groupId - 1]->Process(loot, player, store, lootMode, sWorld->getRate(RATE_DROP_ITEM_GROUP_AMOUNT));
        }
        else
        {
            Groups[groupId - 1]->Process(loot, player, store, lootMode, 0);
        }
        return;                                             // ★ 直接 return，跳过 Entries 和其他 Groups
    }

    // Rolling non-grouped items
    // (B) 未分组条目：每个独立 roll（模式一）
    for (LootStoreItemList::const_iterator i = Entries.begin(); i != Entries.end(); ++i)
    {
        LootStoreItem* item = *i;
        if (!(item->lootmode & lootMode))                         // Do not add if mode mismatch
            continue;                                           // lootmode 位掩码不匹配（常用作英雄/普通难度区分）
        if (!item->Roll(rate, player, loot, store))
            continue;                                           // Bad luck for the entry    // 没摇中（rand_chance >= chance×品质系数）

        if (item->reference)                                    // References processing
        {
            // 引用命中：递归处理 reference_loot_template
            LootTemplate const* Referenced = LootTemplates_Reference.GetLootFor(std::abs(item->reference));
            if (!Referenced)
                continue;                                       // Error message already printed at loading stage

            // ★ 引用次数 = maxcount × RATE_DROP_ITEM_REFERENCED_AMOUNT
            //   典型用法：DBA 把"垃圾物品池"放进 reference_loot_template，
            //   多个怪物用相同 lootid 引用它，maxcount=2 表示跑两次池子
            uint32 maxcount = uint32(float(item->maxcount) * sWorld->getRate(RATE_DROP_ITEM_REFERENCED_AMOUNT));
            sScriptMgr->OnAfterRefCount(player, loot, rate, lootMode, item, maxcount, store);
            for (uint32 loop = 0; loop < maxcount; ++loop)      // Ref multiplicator
                // we're no longer in the top level, so isTopLevel is false
                // ★ item->groupid 传入：让引用可以选择只跑引用模板的某个组
                //   典型用法：引用一个"装备池"的某个稀有组
                Referenced->Process(loot, store, lootMode, player, item->groupid, false);
        }
        else
        {
            // Plain entries (not a reference, not grouped)
            // 普通物品：脚本钩子 → 直接落袋
            sScriptMgr->OnBeforeDropAddItem(player, loot, rate, lootMode, item, store);
            loot.AddItem(*item);                                // Chance is already checked, just add
        }
    }

    // Now processing groups
    // (C) 分组条目：每组只产出一个（除非 RATE_DROP_ITEM_GROUP_AMOUNT 让它多摇）
    // ★ 注意：与 (B) 不同，这里每个组只 roll 一次（不论是否命中），多组之间互不影响
    for (LootGroups::const_iterator i = Groups.begin(); i != Groups.end(); ++i)
        if (LootGroup* group = *i)
        {
            // Rate.Drop.Item.GroupAmount is only in effect for the top loot template level
            if (isTopLevel)
            {
                // 顶层：每组都尝试 roll GroupAmount 次（可掉多个非任务物品）
                uint32 groupAmount = sWorld->getRate(RATE_DROP_ITEM_GROUP_AMOUNT);
                sScriptMgr->OnAfterCalculateLootGroupAmount(player, loot, lootMode, groupAmount, store);
                group->Process(loot, player, store, lootMode, groupAmount);
            }
            else
            {
                // 非顶层（引用内部）：每组只 roll 一次
                group->Process(loot, player, store, lootMode, 0);
            }
        }
}

// True if template includes at least 1 quest drop entry
// 含任务掉落（递归查引用 + 各组）
bool LootTemplate::HasQuestDrop(LootTemplateMap const& store) const
{
    for (LootStoreItemList::const_iterator i = Entries.begin(); i != Entries.end(); ++i)
    {
        LootStoreItem* item = *i;
        if (item->reference)                                // References
        {
            LootTemplateMap::const_iterator Referenced = store.find(std::abs(item->reference));
            if (Referenced == store.end())
                continue;                                   // Error message [should be] already printed at loading stage

            if (Referenced->second->HasQuestDrop(store))
                return true;
        }
        else if (item->needs_quest)
            return true;                                    // quest drop found
    }

    // Now processing groups
    for (LootGroups::const_iterator i = Groups.begin(); i != Groups.end(); ++i)
    {
        if (LootGroup* group = *i)
        {
            if (group->HasQuestDrop(store))
            {
                return true;
            }
        }
    }

    return false;
}

// True if template includes at least 1 quest drop for an active quest of the player
// 含玩家当前任务需要的掉落
bool LootTemplate::HasQuestDropForPlayer(LootTemplateMap const& store, Player const* player) const
{
    // Checking non-grouped entries
    for (LootStoreItemList::const_iterator i = Entries.begin(); i != Entries.end(); ++i)
    {
        LootStoreItem* item = *i;
        if (item->reference)                                // References processing
        {
            LootTemplateMap::const_iterator Referenced = store.find(std::abs(item->reference));
            if (Referenced == store.end())
                continue;                                   // Error message already printed at loading stage
            if (Referenced->second->HasQuestDropForPlayer(store, player))
                return true;
        }
        else if (player->HasQuestForItem(item->itemid))
            return true;                                    // active quest drop found
    }

    // Now checking groups
    for (LootGroups::const_iterator i = Groups.begin(); i != Groups.end(); ++i)
    {
        if (LootGroup* group = *i)
        {
            if (group->HasQuestDropForPlayer(player, store))
            {
                return true;
            }
        }
    }

    return false;
}

// Checks integrity of the template
// 校验：调用每个组的 Verify（组概率和等）
void LootTemplate::Verify(LootStore const& lootstore, uint32 id) const
{
    // Checking group chances
    for (uint32 i = 0; i < Groups.size(); ++i)
        if (Groups[i])
            Groups[i]->Verify(lootstore, id, i + 1);

    /// @todo: References validity checks
}

// 检查模板内所有引用目标是否存在
void LootTemplate::CheckLootRefs(LootStore const& lootstore, uint32 Id, LootIdSet* ref_set) const
{
    for (LootStoreItemList::const_iterator ieItr = Entries.begin(); ieItr != Entries.end(); ++ieItr)
    {
        LootStoreItem* item = *ieItr;
        if (item->reference)
        {
            if (item->mincount != item->maxcount)
                LootTemplates_Reference.ReportInvalidCount(std::abs(item->reference), lootstore.GetName(), Id, item->itemid, item->mincount, item->maxcount);

            if (!LootTemplates_Reference.GetLootFor(std::abs(item->reference)))
                LootTemplates_Reference.ReportNonExistingId(std::abs(item->reference), lootstore.GetName(), item->itemid);
            else if (ref_set)
                ref_set->erase(std::abs(item->reference));
        }
    }

    for (LootGroups::const_iterator grItr = Groups.begin(); grItr != Groups.end(); ++grItr)
        if (LootGroup* group = *grItr)
            group->CheckLootRefs(lootstore, Id, ref_set);
}

// ============================================================================
// ConditionMgr 调用：把 cond 挂到模板内所有 itemid == cond->SourceEntry 的条目上
// 返回 true 表示至少挂到一个
// ============================================================================
bool LootTemplate::addConditionItem(Condition* cond)
{
    if (!cond || !cond->isLoaded())//should never happen, checked at loading
    {
        LOG_ERROR("condition", "LootTemplate::addConditionItem: condition is null");
        return false;
    }

    // 在未分组条目中找
    if (!Entries.empty())
    {
        for (LootStoreItemList::iterator i = Entries.begin(); i != Entries.end(); ++i)
        {
            if ((*i)->itemid == uint32(cond->SourceEntry))
            {
                (*i)->conditions.push_back(cond);
                return true;
            }
        }
    }

    // 在各组的 ExplicitlyChanced / EqualChanced 中找
    if (!Groups.empty())
    {
        for (LootGroups::iterator groupItr = Groups.begin(); groupItr != Groups.end(); ++groupItr)
        {
            LootGroup* group = *groupItr;
            if (!group)
                continue;

            LootStoreItemList* itemList = group->GetExplicitlyChancedItemList();
            if (!itemList->empty())
            {
                for (LootStoreItemList::iterator i = itemList->begin(); i != itemList->end(); ++i)
                {
                    if ((*i)->itemid == uint32(cond->SourceEntry))
                    {
                        (*i)->conditions.push_back(cond);
                        return true;
                    }
                }
            }

            itemList = group->GetEqualChancedItemList();
            if (!itemList->empty())
            {
                for (LootStoreItemList::iterator i = itemList->begin(); i != itemList->end(); ++i)
                {
                    if ((*i)->itemid == uint32(cond->SourceEntry))
                    {
                        (*i)->conditions.push_back(cond);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// 该 itemid 是否是引用（用于 ConditionMgr 判断条件目标是否合法）
bool LootTemplate::isReference(uint32 id) const
{
    for (LootStoreItemList::const_iterator ieItr = Entries.begin(); ieItr != Entries.end(); ++ieItr)
    {
        if ((*ieItr)->itemid == id && (*ieItr)->reference)
        {
            return true;
        }
    }

    return false;//not found or not reference
}

// ============================================================================
// 下面是 13 个 LoadLootTemplates_X() 加载函数
// 每个都遵循相同模式：
//   1) LoadAndCollectLootIds 载入 SQL + 收集 lootid
//   2) 遍历 owner 表（creature_template / item_template 等），交叉校验 lootid 是否存在
//   3) 移除已使用的 lootid，剩下的调 ReportUnusedIds 报告
//   4) 打印加载耗时
// ============================================================================

// 生物掉落：owner 表 = creature_template.lootid
void LoadLootTemplates_Creature()
{
    LOG_INFO("server.loading", "Loading Creature Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet, lootIdSetUsed;
    uint32 count = LootTemplates_Creature.LoadAndCollectLootIds(lootIdSet);

    // Remove real entries and check loot existence
    CreatureTemplateContainer const* ctc = sObjectMgr->GetCreatureTemplates();
    for (CreatureTemplateContainer::const_iterator itr = ctc->begin(); itr != ctc->end(); ++itr)
    {
        if (uint32 lootid = itr->second.lootid)
        {
            if (lootIdSet.find(lootid) == lootIdSet.end())
                LootTemplates_Creature.ReportNonExistingId(lootid, "Creature", itr->second.Entry);
            else
                lootIdSetUsed.insert(lootid);
        }
    }

    // 把已使用的从 lootIdSet 移除（剩下的就是没人引用的）
    for (LootIdSet::const_iterator itr = lootIdSetUsed.begin(); itr != lootIdSetUsed.end(); ++itr)
        lootIdSet.erase(*itr);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Creature.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} Creature Loot Templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 creature loot templates. DB table `creature_loot_template` is empty");

    LOG_INFO("server.loading", " ");
}

// 分解掉落：owner = item_template.DisenchantID
void LoadLootTemplates_Disenchant()
{
    LOG_INFO("server.loading", "Loading Disenchanting Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet, lootIdSetUsed;
    uint32 count = LootTemplates_Disenchant.LoadAndCollectLootIds(lootIdSet);

    ItemTemplateContainer const* its = sObjectMgr->GetItemTemplateStore();
    for (ItemTemplateContainer::const_iterator itr = its->begin(); itr != its->end(); ++itr)
    {
        if (uint32 lootid = itr->second.DisenchantID)
        {
            if (lootIdSet.find(lootid) == lootIdSet.end())
                LootTemplates_Disenchant.ReportNonExistingId(lootid);
            else
                lootIdSetUsed.insert(lootid);
        }
    }

    for (LootIdSet::const_iterator itr = lootIdSetUsed.begin(); itr != lootIdSetUsed.end(); ++itr)
        lootIdSet.erase(*itr);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Disenchant.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} disenchanting loot templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 disenchanting loot templates. DB table `disenchant_loot_template` is empty");
    LOG_INFO("server.loading", " ");
}

// 钓鱼掉落：owner = AreaTableEntry.ID（按区域）
void LoadLootTemplates_Fishing()
{
    LOG_INFO("server.loading", "Loading Fishing Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet;
    uint32 count = LootTemplates_Fishing.LoadAndCollectLootIds(lootIdSet);

    // remove real entries and check existence loot
    for (uint32 i = 1; i < sAreaTableStore.GetNumRows(); ++i)
        if (AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(i))
            if (lootIdSet.find(areaEntry->ID) != lootIdSet.end())
                lootIdSet.erase(areaEntry->ID);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Fishing.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} Fishing Loot Templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 fishing loot templates. DB table `fishing_loot_template` is empty");

    LOG_INFO("server.loading", " ");
}

// 游戏对象掉落：owner = GameObjectTemplate.GetLootId()
void LoadLootTemplates_Gameobject()
{
    LOG_INFO("server.loading", "Loading Gameobject Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet, lootIdSetUsed;
    uint32 count = LootTemplates_Gameobject.LoadAndCollectLootIds(lootIdSet);

    // remove real entries and check existence loot
    GameObjectTemplateContainer const* gotc = sObjectMgr->GetGameObjectTemplates();
    for (GameObjectTemplateContainer::const_iterator itr = gotc->begin(); itr != gotc->end(); ++itr)
    {
        if (uint32 lootid = itr->second.GetLootId())
        {
            if (lootIdSet.find(lootid) == lootIdSet.end())
                LootTemplates_Gameobject.ReportNonExistingId(lootid, "Gameobject", itr->second.entry);
            else
                lootIdSetUsed.insert(lootid);
        }
    }

    for (LootIdSet::const_iterator itr = lootIdSetUsed.begin(); itr != lootIdSetUsed.end(); ++itr)
        lootIdSet.erase(*itr);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Gameobject.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} Gameobject Loot Templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 gameobject loot templates. DB table `gameobject_loot_template` is empty");

    LOG_INFO("server.loading", " ");
}

// 物品容器掉落：owner = item_template.ItemId（带 ITEM_FLAG_HAS_LOOT）
void LoadLootTemplates_Item()
{
    LOG_INFO("server.loading", "Loading Item Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet;
    uint32 count = LootTemplates_Item.LoadAndCollectLootIds(lootIdSet);

    // remove real entries and check existence loot
    ItemTemplateContainer const* its = sObjectMgr->GetItemTemplateStore();
    for (ItemTemplateContainer::const_iterator itr = its->begin(); itr != its->end(); ++itr)
        if (lootIdSet.find(itr->second.ItemId) != lootIdSet.end() && itr->second.HasFlag(ITEM_FLAG_HAS_LOOT))
            lootIdSet.erase(itr->second.ItemId);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Item.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} item loot templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 item loot templates. DB table `item_loot_template` is empty");

    LOG_INFO("server.loading", " ");
}

// 研磨掉落：owner = item_template.ItemId（带 ITEM_FLAG_IS_MILLABLE，草药）
void LoadLootTemplates_Milling()
{
    LOG_INFO("server.loading", "Loading Milling Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet;
    uint32 count = LootTemplates_Milling.LoadAndCollectLootIds(lootIdSet);

    // remove real entries and check existence loot
    ItemTemplateContainer const* its = sObjectMgr->GetItemTemplateStore();
    for (ItemTemplateContainer::const_iterator itr = its->begin(); itr != its->end(); ++itr)
    {
        if (!itr->second.HasFlag(ITEM_FLAG_IS_MILLABLE))
            continue;

        if (lootIdSet.find(itr->second.ItemId) != lootIdSet.end())
            lootIdSet.erase(itr->second.ItemId);
    }

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Milling.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} milling loot templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 milling loot templates. DB table `milling_loot_template` is empty");

    LOG_INFO("server.loading", " ");
}

// 偷窃掉落：owner = creature_template.pickpocketLootId
void LoadLootTemplates_Pickpocketing()
{
    LOG_INFO("server.loading", "Loading Pickpocketing Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet, lootIdSetUsed;
    uint32 count = LootTemplates_Pickpocketing.LoadAndCollectLootIds(lootIdSet);

    // Remove real entries and check loot existence
    CreatureTemplateContainer const* ctc = sObjectMgr->GetCreatureTemplates();
    for (CreatureTemplateContainer::const_iterator itr = ctc->begin(); itr != ctc->end(); ++itr)
    {
        if (uint32 lootid = itr->second.pickpocketLootId)
        {
            if (lootIdSet.find(lootid) == lootIdSet.end())
                LootTemplates_Pickpocketing.ReportNonExistingId(lootid, "Creature", itr->second.Entry);
            else
                lootIdSetUsed.insert(lootid);
        }
    }

    for (LootIdSet::const_iterator itr = lootIdSetUsed.begin(); itr != lootIdSetUsed.end(); ++itr)
        lootIdSet.erase(*itr);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Pickpocketing.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} pickpocketing loot templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 pickpocketing loot templates. DB table `pickpocketing_loot_template` is empty");

    LOG_INFO("server.loading", " ");
}

// 选矿掉落：owner = item_template.ItemId（带 ITEM_FLAG_IS_PROSPECTABLE，矿石）
void LoadLootTemplates_Prospecting()
{
    LOG_INFO("server.loading", "Loading Prospecting Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet;
    uint32 count = LootTemplates_Prospecting.LoadAndCollectLootIds(lootIdSet);

    // remove real entries and check existence loot
    ItemTemplateContainer const* its = sObjectMgr->GetItemTemplateStore();
    for (ItemTemplateContainer::const_iterator itr = its->begin(); itr != its->end(); ++itr)
    {
        if (!itr->second.HasFlag(ITEM_FLAG_IS_PROSPECTABLE))
            continue;

        if (lootIdSet.find(itr->second.ItemId) != lootIdSet.end())
            lootIdSet.erase(itr->second.ItemId);
    }

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Prospecting.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} prospecting loot templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 prospecting loot templates. DB table `prospecting_loot_template` is empty");

    LOG_INFO("server.loading", " ");
}

// 邮件附件掉落：owner = mail_template.id
void LoadLootTemplates_Mail()
{
    LOG_INFO("server.loading", "Loading Mail Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet;
    uint32 count = LootTemplates_Mail.LoadAndCollectLootIds(lootIdSet);

    // remove real entries and check existence loot
    for (uint32 i = 1; i < sMailTemplateStore.GetNumRows(); ++i)
        if (sMailTemplateStore.LookupEntry(i))
            if (lootIdSet.find(i) != lootIdSet.end())
                lootIdSet.erase(i);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Mail.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} mail loot templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 mail loot templates. DB table `mail_loot_template` is empty");

    LOG_INFO("server.loading", " ");
}

// 剥皮掉落：owner = creature_template.SkinLootId
void LoadLootTemplates_Skinning()
{
    LOG_INFO("server.loading", "Loading Skinning Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet, lootIdSetUsed;
    uint32 count = LootTemplates_Skinning.LoadAndCollectLootIds(lootIdSet);

    // remove real entries and check existence loot
    CreatureTemplateContainer const* ctc = sObjectMgr->GetCreatureTemplates();
    for (CreatureTemplateContainer::const_iterator itr = ctc->begin(); itr != ctc->end(); ++itr)
    {
        if (uint32 lootid = itr->second.SkinLootId)
        {
            if (lootIdSet.find(lootid) == lootIdSet.end())
                LootTemplates_Skinning.ReportNonExistingId(lootid, "Creature", itr->second.Entry);
            else
                lootIdSetUsed.insert(lootid);
        }
    }

    for (LootIdSet::const_iterator itr = lootIdSetUsed.begin(); itr != lootIdSetUsed.end(); ++itr)
        lootIdSet.erase(*itr);

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Skinning.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} skinning loot templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 skinning loot templates. DB table `skinning_loot_template` is empty");

    LOG_INFO("server.loading", " ");
}

// 法术掉落：owner = spell_id（IsLootCrafting 的法术，如开包裹法术）
void LoadLootTemplates_Spell()
{
    LOG_INFO("server.loading", "Loading Spell Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet;
    uint32 count = LootTemplates_Spell.LoadAndCollectLootIds(lootIdSet);

    // remove real entries and check existence loot
    for (uint32 spell_id = 1; spell_id < sSpellMgr->GetSpellInfoStoreSize(); ++spell_id)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spell_id);
        if (!spellInfo)
            continue;

        // possible cases
        if (!spellInfo->IsLootCrafting())
            continue;

        if (lootIdSet.find(spell_id) == lootIdSet.end())
        {
            // not report about not trainable spells (optionally supported by DB)
            // ignore 61756 (Northrend Inscription Research (FAST QA VERSION) for example
            // 非可变形 / 是贸易技能：才报告缺失
            if (!spellInfo->HasAttribute(SPELL_ATTR0_NOT_SHAPESHIFTED) || spellInfo->HasAttribute(SPELL_ATTR0_IS_TRADESKILL))
            {
                LootTemplates_Spell.ReportNonExistingId(spell_id, "Spell", spellInfo->Id);
            }
        }
        else
            lootIdSet.erase(spell_id);
    }

    // output error for any still listed (not referenced from appropriate table) ids
    LootTemplates_Spell.ReportUnusedIds(lootIdSet);

    if (count)
        LOG_INFO("server.loading", ">> Loaded {} spell loot templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    else
        LOG_WARN("server.loading", ">> Loaded 0 spell loot templates. DB table `spell_loot_template` is empty");
    LOG_INFO("server.loading", " ");
}

// 玩家尸体掉落：owner = team id（无交叉校验，因为 player 死亡生成时直接用）
void LoadLootTemplates_Player()
{
    LOG_INFO("server.loading", "Loading Player Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet;
    uint32 count = LootTemplates_Player.LoadAndCollectLootIds(lootIdSet);

    if (count)
    {
        LOG_INFO("server.loading", ">> Loaded {} player loot templates in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    }
    else
    {
        LOG_WARN("server.loading", ">> Loaded 0 player loot templates. DB table `player_loot_template` is empty");
    }

    LOG_INFO("server.loading", " ");
}

// ============================================================================
// 引用掉落表加载：必须放在最后（其他表都加载完后才能查它们用了哪些引用）
// 流程：先收集所有引用 id，然后让其他每个 Store 调 CheckLootRefs 从中移除已用的
// 最后 lootIdSet 剩下的就是没人引用的孤立引用
// ============================================================================
void LoadLootTemplates_Reference()
{
    LOG_INFO("server.loading", "Loading Reference Loot Templates...");

    uint32 oldMSTime = getMSTime();

    LootIdSet lootIdSet;
    LootTemplates_Reference.LoadAndCollectLootIds(lootIdSet);

    // check references and remove used
    // 每张表都用同一个 lootIdSet，CheckLootRefs 会从中移除已使用的引用 id
    LootTemplates_Creature.CheckLootRefs(&lootIdSet);
    LootTemplates_Fishing.CheckLootRefs(&lootIdSet);
    LootTemplates_Gameobject.CheckLootRefs(&lootIdSet);
    LootTemplates_Item.CheckLootRefs(&lootIdSet);
    LootTemplates_Milling.CheckLootRefs(&lootIdSet);
    LootTemplates_Pickpocketing.CheckLootRefs(&lootIdSet);
    LootTemplates_Skinning.CheckLootRefs(&lootIdSet);
    LootTemplates_Disenchant.CheckLootRefs(&lootIdSet);
    LootTemplates_Prospecting.CheckLootRefs(&lootIdSet);
    LootTemplates_Mail.CheckLootRefs(&lootIdSet);
    LootTemplates_Reference.CheckLootRefs(&lootIdSet);   // 引用也可以引用引用（递归）

    // output error for any still listed ids (not referenced from any loot table)
    LootTemplates_Reference.ReportUnusedIds(lootIdSet);

    LOG_INFO("server.loading", ">> Loaded reference loot templates in {} ms", GetMSTimeDiffToNow(oldMSTime));
    LOG_INFO("server.loading", " ");
}
