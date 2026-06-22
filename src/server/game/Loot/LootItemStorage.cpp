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

#include "LootItemStorage.h"
#include "DatabaseEnv.h"
#include "ObjectMgr.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "Timer.h"

// 私有构造（单例模式 —— instance() 内的 static 局部变量延迟初始化）
LootItemStorage::LootItemStorage()
{
}

LootItemStorage::~LootItemStorage()
{
}

// 单例访问入口：返回函数内 static 实例的指针
LootItemStorage* LootItemStorage::instance()
{
    static LootItemStorage instance;
    return &instance;
}

// 服务器启动时调用：从 character 库一次性载入全部"已存储掉落"到内存缓存
// 之所以要在内存缓存：玩家每次开包都查 DB 太慢，启动时全量加载到 unordered_map
void LootItemStorage::LoadStorageFromDB()
{
    uint32 oldMSTime = getMSTime();                  // 记录开始时间（用于打印耗时）
    lootItemStore.clear();                           // 清空缓存（重载安全）

    // CHAR_SEL_ITEMCONTAINER_ITEMS: SELECT * FROM item_instance_container_items
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_ITEMCONTAINER_ITEMS);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);
    if (!result)                                     // 表为空
    {
        LOG_WARN("server.loading", ">> Loaded 0 stored items!");
        LOG_INFO("server.loading", " ");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        // 字段顺序与 item_instance_container_items 列对应：
        //   [0] container_guid  [1] itemid      [2] itemIndex   [3] count
        //   [4] randomPropertyId [5] randomSuffix [6] follow_loot_rules
        //   [7] freeforall       [8] is_blocked    [9] is_counted
        //   [10] is_underthreshold [11] needs_quest [12] conditionLootId
        // 容器 GUID 由 item 的 lowGUID 构造（HighGuid::Item）
        StoredLootItemList& itemList = lootItemStore[ObjectGuid::Create<HighGuid::Item>(fields[0].Get<uint32>())];
        // 直接 emplace_back 构造 StoredLootItem（参数顺序见头文件注释）
        itemList.emplace_back(fields[1].Get<uint32>(), fields[2].Get<uint32>(), fields[3].Get<uint32>(), fields[4].Get<int32>(), fields[5].Get<uint32>(), fields[6].Get<bool>(),
            fields[7].Get<bool>(), fields[8].Get<bool>(), fields[9].Get<bool>(), fields[10].Get<bool>(), fields[11].Get<bool>(), fields[12].Get<uint32>());

        ++count;
    } while (result->NextRow());

    LOG_INFO("server.loading", ">> Loaded {} stored items in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
    LOG_INFO("server.loading", " ");
}

// 删除某个容器里的指定条目（直接走 DB）
// 调用方：RemoveStoredLootItem / RemoveStoredLootMoney 内部
void LootItemStorage::RemoveEntryFromDB(ObjectGuid containerGUID, uint32 itemid, uint32 count, uint32 itemIndex)
{
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();  // 开启事务

    // CHAR_DEL_ITEMCONTAINER_SINGLE_ITEM:
    //   DELETE FROM item_instance_container_items
    //   WHERE container_guid=? AND itemid=? AND count=? AND itemIndex=?
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEMCONTAINER_SINGLE_ITEM);
    stmt->SetData(0, containerGUID.GetCounter());   // 容器的 lowGUID
    stmt->SetData(1, itemid);                       // 物品 id（金钱条目为 0）
    stmt->SetData(2, count);                        // 数量（金钱条目为金额）
    stmt->SetData(3, itemIndex);                    // 槽位索引（金钱条目为 0）
    trans->Append(stmt);

    CharacterDatabase.CommitTransaction(trans);     // 提交事务
}

// 把一个新生成的容器 Loot 写入内存缓存 + character 库
// 调用时机：玩家打开可拾取物品容器（包裹/礼物）后，loot 已生成完毕
void LootItemStorage::AddNewStoredLoot(Loot* loot, Player* /*player*/)
{
    // 防御性：如果该容器已存在存储记录，跳过（避免重复插入）
    // 通常意味着之前的清理逻辑漏了，记日志以便排查
    if (lootItemStore.find(loot->containerGUID) != lootItemStore.end())
    {
        LOG_INFO("misc", "LootItemStorage::AddNewStoredLoot (A1) - {}!", loot->containerGUID.ToString());
        return;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();  // 整个容器的写入用一次事务
    CharacterDatabasePreparedStatement* stmt = nullptr;

    // 在内存缓存中创建该容器的条目列表（[] 操作符会自动插入）
    StoredLootItemList& itemList = lootItemStore[loot->containerGUID];

    // 第一步：如果有金钱，先存金钱条目（itemid == 0 是金钱的特殊标记）
    if (loot->gold)
    {
        // 内存：用 itemid=0 表示金钱，count 字段存金额
        itemList.emplace_back(0, 0, loot->gold, 0, 0, false, false, false, false, false, false, 0);

        // DB：插入一行金钱记录
        uint8 index = 0;
        // CHAR_INS_ITEMCONTAINER_SINGLE_ITEM: INSERT INTO item_instance_container_items (...) VALUES (?...)
        stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_ITEMCONTAINER_SINGLE_ITEM);
        stmt->SetData(index++, loot->containerGUID.GetCounter());  // 容器 lowGUID
        stmt->SetData(index++, 0);                                 // itemid=0 标记金钱
        stmt->SetData(index++, 0);                                 // itemIndex=0
        stmt->SetData(index++, loot->gold);                        // count = 金额
        stmt->SetData(index++, 0);                                 // randomPropertyId
        stmt->SetData(index++, 0);                                 // randomSuffix
        stmt->SetData(index++, false);                             // follow_loot_rules
        stmt->SetData(index++, false);                             // freeforall
        stmt->SetData(index++, false);                             // is_blocked
        stmt->SetData(index++, false);                             // is_counted
        stmt->SetData(index++, false);                             // is_underthreshold
        stmt->SetData(index++, false);                             // needs_quest
        stmt->SetData(index++, 0);                                 // conditionLootId
        trans->Append(stmt);
    }

    // 第二步：如果整个 loot 还没被捡光，逐个保存物品
    if (!loot->isLooted())
        for (LootItemList::const_iterator li = loot->items.begin(); li != loot->items.end(); li++)
        {
            // 注意：这里特意不调用 li->AllowedForPlayer(player)
            // 原因：当前玩家可能拿不到该物品（条件不符），但也许之后会把容器
            //       交易给另一个满足条件的玩家；如果此时不存，重登后就永远丢失。
            //if (!li->AllowedForPlayer(player))
            //    continue;

            // 跳过不存在的物品模板和"货币代币"（代币不持久化容器内）
            ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(li->itemid);
            if (!itemTemplate || itemTemplate->IsCurrencyToken())
                continue;

            // 还原 conditions 时要用到 conditionLootId
            // 取 conditions.front()->SourceGroup（即关联的 loot 模板 entry）
            uint32 conditionLootId = 0;
            if (!li->conditions.empty())
            {
                conditionLootId = li->conditions.front()->SourceGroup;
            }

            // 内存：构造条目
            itemList.emplace_back(li->itemid, li->itemIndex, li->count, li->randomPropertyId, li->randomSuffix, li->follow_loot_rules, li->freeforall, li->is_blocked, li->is_counted,
                li->is_underthreshold, li->needs_quest, conditionLootId);

            // DB：插入一行物品记录
            uint8 index = 0;
            stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_ITEMCONTAINER_SINGLE_ITEM);
            stmt->SetData(index++, loot->containerGUID.GetCounter());  // 容器 lowGUID
            stmt->SetData(index++, li->itemid);                        // 物品 id
            stmt->SetData(index++, li->itemIndex);                     // 槽位索引
            stmt->SetData(index++, li->count);                         // 数量
            stmt->SetData (index++, li->randomPropertyId);             // 随机属性
            stmt->SetData(index++, li->randomSuffix);                  // 随机附魔
            stmt->SetData(index++, li->follow_loot_rules);             // 遵循规则
            stmt->SetData(index++, li->freeforall);                    // FFA
            stmt->SetData(index++, li->is_blocked);                    // 锁定
            stmt->SetData(index++, li->is_counted);                    // 已计数
            stmt->SetData(index++, li->is_underthreshold);             // 低于阈值
            stmt->SetData(index++, li->needs_quest);                   // 任务物品
            stmt->SetData(index++, conditionLootId);                   // 条件 lootid

            trans->Append(stmt);
        }

    CharacterDatabase.CommitTransaction(trans);     // 一次性提交所有 INSERT
}

// 从内存缓存还原某个 Item 的 Loot 到运行时（玩家重新登录 / 重新打开容器时）
// 返回 true 表示成功还原（容器 GUID 在缓存里有记录）
bool LootItemStorage::LoadStoredLoot(Item* item, Player* player)
{
    // 物品模板必须存在
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->GetEntry());
    if (!proto)
    {
        return false;
    }

    Loot* loot = &item->loot;                                         // 该物品实例的 Loot 成员
    LootItemContainer::iterator itr = lootItemStore.find(loot->containerGUID);
    if (itr == lootItemStore.end())                                   // 该容器没存过（全新生成）
        return false;

    // 遍历该容器内所有已存储条目，逐个填回运行时 Loot
    StoredLootItemList& itemList = itr->second;
    for (StoredLootItemList::iterator it2 = itemList.begin(); it2 != itemList.end(); ++it2)
    {
        // 金钱条目：itemid == 0，金额存在 count 里
        if (it2->itemid == 0)
        {
            loot->gold = it2->count;
            continue;
        }

        // 必须能找到该物品在 item_loot_template 中的模板（用于还原 conditions）
        if (LootTemplate const* lt = LootTemplates_Item.GetLootFor(item->GetEntry()))
        {
            // 构造一个运行时 LootItem，把存储的字段逐一还原
            LootItem li;
            li.itemid = it2->itemid;
            li.itemIndex = it2->itemIndex;
            li.count = it2->count;
            li.follow_loot_rules = it2->follow_loot_rules;
            li.freeforall = it2->freeforall;
            li.is_blocked = it2->is_blocked;
            li.is_counted = it2->is_counted;
            li.is_underthreshold = it2->is_underthreshold;
            li.is_looted = false;                  // 默认未拾取
            li.needs_quest = it2->needs_quest;
            li.randomPropertyId = it2->randomPropertyId;
            li.randomSuffix = it2->randomSuffix;
            li.rollWinnerGUID = ObjectGuid::Empty; // 重置 ROLL 胜者（重启后失效）
            li.groupid = 0;                        // 组 id 不持久化，置 0

            // 从 item_loot_template 把对应的 conditions 复制回来
            // （条件本身没存数据库，只存了 conditionLootId 作为索引）
            lt->CopyConditions(&li, it2->conditionLootId);

            // 按 needs_quest 分流到 quest_items / items 两个 vector
            if (li.needs_quest)
            {
                loot->quest_items.push_back(li);
            }
            else
            {
                loot->items.push_back(li);
            }

            // unlootedCount 计数规则（与 Loot::AddItem 完全一致）：
            //  - 非任务、无条件、非 FFA 的"普通个人可见"物品 → 计数
            //  - FFA 物品：在 FillFFALoot() 中按玩家计数
            //  - 非 FFA 条件物品：在 FillNonQuestNonFFAConditionalLoot() 中计数
            //  - is_counted=true 的物品强制计数（恢复存储时的状态）
            if ((!li.needs_quest && li.conditions.empty() && !proto->HasFlag(ITEM_FLAG_MULTI_DROP)) || li.is_counted)
            {
                ++loot->unlootedCount;
            }
        }
    }

    // 如果还有未拾取内容，按玩家构造可见性映射（FFA / 任务 / 条件）
    if (loot->unlootedCount)
    {
        loot->FillNotNormalLootFor(player);
    }

    // 标记物品"loot 已生成"，避免玩家下次再打开时被 FillLoot 二次生成
    item->m_lootGenerated = true;
    return true;
}

// 玩家从容器中拿走某物品后调用：内存 + DB 同步删除该条目
void LootItemStorage::RemoveStoredLootItem(ObjectGuid containerGUID, uint32 itemid, uint32 count, Loot* loot, uint32 itemIndex)
{
    LootItemContainer::iterator itr = lootItemStore.find(containerGUID);
    if (itr == lootItemStore.end())                 // 该容器不在缓存里，直接返回
        return;

    // 在条目列表中找到 (itemid + count) 匹配的第一个，删除
    // 注意：itemIndex 不参与匹配判断，只是传给 DB 删除语句
    StoredLootItemList& itemList = itr->second;
    for (StoredLootItemList::iterator it2 = itemList.begin(); it2 != itemList.end(); ++it2)
        if (it2->itemid == itemid && it2->count == count)
        {
            RemoveEntryFromDB(containerGUID, itemid, count, itemIndex);  // 先删 DB
            itemList.erase(it2);                                         // 再删内存
            break;                                                      // 只删第一个匹配项
        }

    // 如果 loot 已被捡光（unlootedCount==0 且 gold==0），从缓存中删整个容器
    // 注意：unlootedCount>0 但 itemList 空了的容器不能删 ——
    //       那种情况意味着还有玩家可见的任务/FFA/条件物品，需要等玩家手动操作或交易走
    if (!loot->unlootedCount && !loot->gold)
        lootItemStore.erase(itr);
}

// 玩家从容器中拿走金钱后调用：内存 + DB 同步删除金钱条目
void LootItemStorage::RemoveStoredLootMoney(ObjectGuid containerGUID, Loot* loot)
{
    LootItemContainer::iterator itr = lootItemStore.find(containerGUID);
    if (itr == lootItemStore.end())
        return;

    // 找到 itemid == 0 的金钱条目并删除
    StoredLootItemList& itemList = itr->second;
    for (StoredLootItemList::iterator it2 = itemList.begin(); it2 != itemList.end(); ++it2)
        if (it2->itemid == 0)
        {
            RemoveEntryFromDB(containerGUID, 0, it2->count, 0);
            itemList.erase(it2);
            break;
        }

    // 如果 loot 已被捡光（这里只检查 unlootedCount，因为金钱已清），删整个容器
    if (!loot->unlootedCount)
        lootItemStore.erase(itr);
}

// 整个容器被销毁时调用：内存 + DB 一次性删除该容器的全部条目
void LootItemStorage::RemoveStoredLoot(ObjectGuid containerGUID)
{
    lootItemStore.erase(containerGUID);              // 内存：直接 erase（不存在也安全）

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    // CHAR_DEL_ITEMCONTAINER_CONTAINER:
    //   DELETE FROM item_instance_container_items WHERE container_guid=?
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEMCONTAINER_CONTAINER);
    stmt->SetData(0, containerGUID.GetCounter());
    trans->Append(stmt);

    CharacterDatabase.CommitTransaction(trans);
}
