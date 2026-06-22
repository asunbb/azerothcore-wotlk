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

#ifndef ACORE_LOOTITEMSTORAGE_H
#define ACORE_LOOTITEMSTORAGE_H

#include "Item.h"
#include "LootMgr.h"
#include <list>

// 已存储的掉落物品条目（用于"物品容器"类掉落 —— 例如可打开的包裹/礼物盒等）
// 这类容器内部生成的 Loot 必须能在服务器重启后还原，所以需要持久化到 character 库
struct StoredLootItem
{
    // 构造函数：从数据库读出 / 或在生成新 loot 时构造
    // i                 -> 物品 id（== 0 表示这是金钱条目，金额放在 count 字段）
    // idx               -> 该物品在 Loot::items / quest_items 中的索引
    // c                 -> 数量（金额条目时为金钱数）
    // ri                -> 随机属性 id（item_template 的 RandomProperty）
    // rs                -> 随机后缀（ItemEnchantment）
    // follow_loot_rules -> 是否遵循队伍 loot 规则（ITEM_FLAGS_CU_FOLLOW_LOOT_RULES）
    // freeforall        -> 是否人人可拾取（ITEM_FLAG_MULTI_DROP）
    // is_blocked        -> 是否被锁定（ROLL 中 / ML 持有 / 阈值之上）
    // is_counted        -> 是否已计入 unlootedCount
    // is_underthreshold -> 是否低于队伍 loot 阈值（走 round-robin）
    // needs_quest       -> 是否为任务物品
    // conditionLootId   -> 关联的 conditions 表 SourceGroup（用于还原条件）
    StoredLootItem(uint32 i, uint32 idx, uint32 c, int32 ri, uint32 rs, bool follow_loot_rules, bool freeforall,
        bool is_blocked, bool is_counted, bool is_underthreshold, bool needs_quest, uint32 conditionLootId) : itemid(i), itemIndex(idx),
        count(c), randomPropertyId(ri), randomSuffix(rs), follow_loot_rules(follow_loot_rules), freeforall(freeforall), is_blocked(is_blocked),
        is_counted(is_counted), is_underthreshold(is_underthreshold), needs_quest(needs_quest), conditionLootId(conditionLootId) { }

    // If itemid == 0 - money amount is stored in count value
    uint32 itemid;                  // 物品 id；== 0 表示这是"金钱条目"
    uint32 itemIndex;               // 在 Loot 中的槽位索引
    uint32 count;                   // 物品数量（或金钱数）
    int32 randomPropertyId;         // 随机属性 id
    uint32 randomSuffix;            // 随机附魔后缀
    bool follow_loot_rules;         // 遵循队伍 loot 规则
    bool freeforall;                // FFA（人人可拾取）
    bool is_blocked;                // 槽位被锁定
    bool is_counted;                // 已计入 unlootedCount
    bool is_underthreshold;         // 低于队伍阈值
    bool needs_quest;               // 任务物品
    uint32 conditionLootId;         // 关联的 loot 模板 id（conditions.SourceGroup）
};

// 单个容器内的所有已存储条目（含金钱 + 物品）
typedef std::list<StoredLootItem> StoredLootItemList;
// 全局缓存：容器 GUID -> 该容器内已存储的条目列表
// 容器 GUID 使用 HighGuid::Item（即物品实例的 GUID，对应 item_instance.guid）
typedef std::unordered_map<ObjectGuid, StoredLootItemList> LootItemContainer;

// 单例：负责"物品容器"类 Loot 的持久化（item_instance_container_items 表）
// 普通尸体/GO 的 Loot 只活在内存，不需要这个类
class LootItemStorage
{
private:
    LootItemStorage();             // 私有构造（单例模式）
    ~LootItemStorage();

public:
    // 单例访问入口（sLootItemStorage 宏的底层）
    static LootItemStorage* instance();

    // 服务器启动时调用：从 character 库一次性载入全部已存储条目到内存
    void LoadStorageFromDB();
    // 删除某个容器里的指定条目（按 itemid + count + itemIndex 定位）
    void RemoveEntryFromDB(ObjectGuid containerGUID, uint32 itemid, uint32 count, uint32 itemIndex);

    // 把一个新生成的容器 Loot 写入内存缓存 + character 库
    // 玩家打开可拾取物品容器（如包裹/礼物）生成 loot 后会调用
    void AddNewStoredLoot(Loot* loot, Player* player);
    // 从内存缓存还原某个 Item 的 Loot 到运行时（玩家重新登录 / 重新打开容器时）
    // 返回 true 表示成功还原
    bool LoadStoredLoot(Item* item, Player* player);

    // 玩家从容器中拿走某物品后调用：从内存 + DB 同时删除该条目
    void RemoveStoredLootItem(ObjectGuid containerGUID, uint32 itemid, uint32 count, Loot* loot, uint32 itemIndex);
    // 玩家从容器中拿走金钱后调用
    void RemoveStoredLootMoney(ObjectGuid containerGUID, Loot* loot);
    // 整个容器被销毁时调用：删除该容器的所有条目
    void RemoveStoredLoot(ObjectGuid containerGUID);

private:
    LootItemContainer lootItemStore;  // 内存缓存：所有容器的所有条目
};

// 全局访问宏：用 sLootItemStorage->XXX() 形式调用
#define sLootItemStorage LootItemStorage::instance()

#endif
