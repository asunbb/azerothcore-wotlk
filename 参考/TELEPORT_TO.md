# Player::TeleportTo 函数详解

> **源文件**: `src/server/game/Entities/Player/Player.cpp:1353-1598`
> **声明**: `src/server/game/Entities/Player/Player.h`

---

## 1. 函数签名

```cpp
bool Player::TeleportTo(
    uint32 mapid,              // 目标地图 ID
    float x, float y, float z, // 目标世界坐标
    float orientation,          // 目标朝向
    uint32 options = 0,         // 传送选项位掩码
    Unit* target = nullptr,     // 传送目标单位（部分脚本用）
    bool newInstance = false    // 是否强制创建新实例
);
```

**返回值**: `true` 传送已启动（或延迟启动），`false` 传送被拒绝。

---

## 2. options 位掩码标志

| 标志 | 值 | 含义 |
|---|---|---|
| `TELE_TO_GM_MODE` | `0x01` | GM 模式，跳过 `PlayerCannotEnter` 检查 |
| `TELE_TO_NOT_LEAVE_TRANSPORT` | `0x02` | 不脱离载具（船/飞艇） |
| `TELE_TO_NOT_LEAVE_COMBAT` | `0x04` | 不脱离战斗状态 |
| `TELE_TO_NOT_UNSUMMON_PET` | `0x08` | 不解散宠物 |
| `TELE_TO_SPELL` | `0x10` | 法术传送，不中断施法中的法术 |
| `TELE_TO_NOT_LEAVE_VEHICLE` | `0x20` | 不退出坐骑/载具 |
| `TELE_TO_WITH_PET` | `0x40` | 主动解散宠物（临时，到目的地后重新召唤） |
| `TELE_TO_NOT_LEAVE_TAXI` | `0x80` | 不中断飞行 taxi |

---

## 3. 完整流程注释

### 第一阶段：前置校验（Player.cpp:1355-1404）

```
┌─────────────────────────────────────────────────────────────┐
│ 1. 坐标合法性校验                                            │
│    MapMgr::IsValidMapCoord(mapid, x, y, z, orientation)     │
│    确认坐标在地图有效范围 [-17066.17, +17066.17] 内           │
│    失败 → 记录错误日志，返回 false                           │
├─────────────────────────────────────────────────────────────┤
│ 2. 地图禁用检查                                              │
│    sDisableMgr->IsDisabledFor(DISABLE_TYPE_MAP, mapid, ...) │
│    检查目标地图是否被管理员禁用                                │
│    有 RBAC_PERM_SKIP_CHECK_DISABLE_MAP 权限可跳过            │
│    失败 → 发送 TRANSFER_ABORT_MAP_NOT_ALLOWED，返回 false   │
├─────────────────────────────────────────────────────────────┤
│ 3. 缓存当前宠物指针                                          │
│    Pet* pet = GetPet()                                      │
│    必须在传送前获取，传送后可能找不到                          │
├─────────────────────────────────────────────────────────────┤
│ 4. 战场/竞技场检查                                           │
│    未分配战场 ID 的玩家不允许进入战场地图                      │
│    失败 → 静默返回 false                                    │
├─────────────────────────────────────────────────────────────┤
│ 5. 竞技场观战者限制                                          │
│    观战者不允许从竞技场传送到副本                              │
│    失败 → TRANSFER_ABORT_MAP_NOT_ALLOWED                    │
├─────────────────────────────────────────────────────────────┤
│ 6. 客户端资料片版本检查                                      │
│    玩家资料片等级 < 地图要求的资料片等级                       │
│    特殊处理：如果在载具上，先移除乘客再传送去墓地              │
│    失败 → TRANSFER_ABORT_INSUF_EXPAN_LVL                    │
└─────────────────────────────────────────────────────────────────────┘
```

### 第二阶段：传送前状态清理（Player.cpp:1406-1451）

```
┌─────────────────────────────────────────────────────────────┐
│ 7. 中断 Taxi 飞行                                           │
│    除非有 TELE_TO_NOT_LEAVE_TAXI 标志                        │
│    → MovementExpired() 清除移动生成器                        │
│    → CleanupAfterTaxiFlight() 清理飞行状态                   │
├─────────────────────────────────────────────────────────────┤
│ 8. 退出载具/坐骑                                            │
│    除非有 TELE_TO_NOT_LEAVE_VEHICLE 标志                     │
│    → ExitVehicle()                                          │
├─────────────────────────────────────────────────────────────┤
│ 9. 重置移动标志                                              │
│    仅保留 MOVEMENTFLAG_MASK_HAS_PLAYER_STATUS_OPCODE 位      │
│    → DisableSpline() 禁用样条移动                            │
├─────────────────────────────────────────────────────────────┤
│ 10. 移除控制类 Aura（跨地图或距离 > 100 码时）               │
│     - SPELL_AURA_MOD_STUN     （昏迷）                       │
│     - SPELL_AURA_MOD_FEAR     （恐惧）                       │
│     - SPELL_AURA_MOD_CONFUSE  （迷惑）                       │
│     - SPELL_AURA_MOD_ROOT     （定身）                       │
│     - AURA_INTERRUPT_FLAG_TELEPORTED 标记的 Aura             │
│     注意：小范围传送（如闪现）不触发                           │
├─────────────────────────────────────────────────────────────┤
│ 11. 载具乘客处理                                             │
│     TELE_TO_NOT_LEAVE_TRANSPORT → 保留载具标志               │
│     否则 → RemovePassenger，清除 transport 数据              │
├─────────────────────────────────────────────────────────────┤
│ 12. 跨地图决斗判定                                           │
│     目标地图 ≠ 当前地图且正在决斗 → DuelComplete(DUEL_FLED)  │
│     必须在传送前检查，否则 ObjectAccessor 找不到决斗旗        │
├─────────────────────────────────────────────────────────────┤
│ 13. 脚本钩子 OnPlayerBeforeTeleport                         │
│     允许脚本拦截传送，返回 false 则取消                       │
└─────────────────────────────────────────────────────────────┘
```

### 第三阶段：执行传送（分支）

根据 `GetMapId() == mapid && !newInstance` 分为两条路径：

---

#### 路径 A：同地图传送（Near Teleport）（Player.cpp:1453-1500）

```
┌─────────────────────────────────────────────────────────────┐
│ 适用条件: 目标地图 == 当前地图 且 不强制创建新实例             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│ A1. 重置远传送信号量                                         │
│     SetSemaphoreTeleportFar(0)                              │
│     清除可能残留的跨地图传送状态                              │
│                                                             │
│ A2. 延迟传送检查                                            │
│     MustDelayTeleport() → 如果在 Unit::Update() 中施放传送   │
│     法术，需要等到 Update 结束后再执行                        │
│     → 保存目标到 teleportStore_dest，设置延迟标志，返回 true │
│                                                             │
│ A3. 宠物处理                                                │
│     TELE_TO_WITH_PET → 临时解散宠物                         │
│     否则 → 如果宠物超出可见范围，临时解散                     │
│                                                             │
│ A4. 脱离战斗                                                │
│     除非有 TELE_TO_NOT_LEAVE_COMBAT 标志                     │
│                                                             │
│ A5. 保存传送数据                                            │
│     teleportStore_dest = WorldLocation(mapid, x, y, z, o)  │
│     SetFallInformation() → 记录起始高度（坠落检测用）        │
│                                                             │
│ A6. 设置近传送信号量                                         │
│     SetSemaphoreTeleportNear(当前时间)                       │
│     → IsBeingTeleportedNear() 返回 true                     │
│     → 阻止在此期间的移动包处理                               │
│                                                             │
│ A7. 执行重定位并通知客户端                                   │
│     (仅在线玩家)                                             │
│     a. SetCanTeleport(true)                                 │
│     b. 保存旧位置 oldPos                                     │
│     c. Relocate(x, y, z, orientation) → 更新服务端位置       │
│     d. SendTeleportAckPacket() → 发送传送确认包              │
│     e. SendTeleportPacket(oldPos) → 广播移动包               │
│        （内部会临时回到 oldPos 以在正确位置广播）             │
│                                                             │
│ A8. 等待客户端确认                                           │
│     客户端回复 MSG_MOVE_TELEPORT_ACK                        │
│     → WorldSession::HandleMovementOpcodes() 完成传送        │
│     → SetSemaphoreTeleportNear(0) 清除信号量                │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

#### 路径 B：跨地图传送（Far Teleport）（Player.cpp:1501-1596）

```
┌─────────────────────────────────────────────────────────────┐
│ 适用条件: 目标地图 ≠ 当前地图 或 newInstance == true          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│ B1. 死亡骑士起始区域限制                                     │
│     DK 在 Ebon Hold 且未学会 50977（传送技能）               │
│     → TRANSFER_ABORT_UNIQUE_MESSAGE，返回 false             │
│                                                             │
│ B2. 获取旧地图指针                                           │
│     Map* oldmap = GetMap()                                  │
│                                                             │
│ B3. 地图进入权限检查                                         │
│     sMapMgr->PlayerCannotEnter(mapid, this, false)          │
│     GM 模式（TELE_TO_GM_MODE）可跳过                         │
│     检查内容：等级、团队、副本锁定、人数上限等                │
│                                                             │
│ B4. 重置近传送信号量                                         │
│     SetSemaphoreTeleportNear(0)                             │
│                                                             │
│ B5. 延迟传送检查（同 A2）                                    │
│     MustDelayTeleport() → 保存目标，设置远传送信号量          │
│                                                             │
│ B6. 清除选择目标                                             │
│     SetSelection(ObjectGuid::Empty)                         │
│                                                             │
│ B7. 脱离战斗                                                │
│     CombatStop()（无条件，跨地图必须脱战）                    │
│                                                             │
│ B8. 解散宠物（无条件）                                       │
│     UnsummonPetTemporaryIfAny()                             │
│                                                             │
│ B9. 移除所有动态对象                                         │
│     RemoveAllDynObjects()                                   │
│                                                             │
│ B10. 中断施法                                                │
│      除非有 TELE_TO_SPELL 标志（传送法术本身不中断）          │
│      → InterruptNonMeleeSpells(true)                        │
│                                                             │
│ B11. 移除地图切换相关 Aura                                   │
│      AURA_INTERRUPT_FLAG_CHANGE_MAP                         │
│      AURA_INTERRUPT_FLAG_MOVE                               │
│      AURA_INTERRUPT_FLAG_TURNING                            │
│                                                             │
│ B12. 递增地图切换计数器                                       │
│      SetMapChangeOrderCounter()                             │
│      用于追踪传送顺序，处理并发传送                           │
│                                                             │
│ B13. 发送 SMSG_TRANSFER_PENDING（仅在线玩家）                │
│      告知客户端即将切换地图                                  │
│      包含: 目标 mapId, [载具 entry + 当前 mapId]             │
│      客户端显示加载画面                                      │
│                                                             │
│ B14. 从旧地图移除玩家                                        │
│      oldmap->RemovePlayerFromMap(this, false)                │
│      false = 不立即删除，保留对象                            │
│                                                             │
│ B15. 保存传送目标数据                                        │
│      teleportStore_dest = WorldLocation(mapid, x, y, z, o) │
│      SetFallInformation() → 记录目标高度（坠落检测）         │
│      此位置也用于传送完成前的 SaveToDB（防止下线丢失）        │
│                                                             │
│ B16. 发送 SMSG_NEW_WORLD（仅在线玩家）                       │
│      包含: 目标 mapId, 目标 x/y/z/o                          │
│      客户端收到后开始加载新地图                               │
│      同时发送 SendSavedInstances() 副本锁定信息              │
│                                                             │
│ B17. 设置远传送信号量                                         │
│      SetSemaphoreTeleportFar(当前时间)                       │
│      → IsBeingTeleportedFar() 返回 true                     │
│      → 阻止其他传送请求                                      │
│                                                             │
│ B18. 等待客户端确认（异步）                                   │
│      客户端加载完新地图后发送 MSG_MOVE_WORLDPORT_ACK          │
│      → WorldSession::HandleMoveWorldportAck()               │
│        执行以下步骤:                                         │
│        a. 从 teleportStore_dest 取出目标坐标                 │
│        b. 坐标合法性二次校验                                  │
│        c. 创建目标地图: sMapMgr->CreateMap(mapId, player)   │
│        d. Relocate(x, y, z, o) 更新服务端位置                │
│        e. SetMap(newMap) 切换到新地图                        │
│        f. newMap->AddPlayerToMap(player) 加入新地图          │
│        g. oldMap->AfterPlayerUnlinkFromMap() 清理旧地图      │
│        h. SetSemaphoreTeleportFar(0) 清除信号量              │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. 流程总图

```
Player::TeleportTo(mapid, x, y, z, orientation, options)
│
├─── [阶段1] 前置校验
│    ├─ 坐标合法性 ──────────────────── 失败 → return false
│    ├─ 地图禁用检查 ────────────────── 失败 → return false
│    ├─ 缓存宠物指针
│    ├─ 战场/竞技场检查 ─────────────── 失败 → return false
│    ├─ 观战者限制 ──────────────────── 失败 → return false
│    └─ 资料片版本检查 ──────────────── 失败 → return false
│
├─── [阶段2] 状态清理
│    ├─ 中断 Taxi / 退出载具
│    ├─ 重置移动标志
│    ├─ 移除控制类 Aura（远距离时）
│    ├─ 载具乘客处理
│    ├─ 跨地图决斗判定
│    └─ 脚本钩子 OnPlayerBeforeTeleport ── 拦截 → return false
│
├─── [阶段3] 执行传送
│    │
│    ├── 同地图 (Near Teleport)
│    │    ├─ 延迟检查 → MustDelayTeleport → 保存目标, return true
│    │    ├─ 宠物/战斗处理
│    │    ├─ 保存传送数据
│    │    ├─ Relocate() 更新位置
│    │    ├─ SendTeleportAckPacket() + SendTeleportPacket()
│    │    └─ 等待 MSG_MOVE_TELEPORT_ACK → 完成
│    │
│    └── 跨地图 (Far Teleport)
│         ├─ DK 起始区限制 ─────────────── 失败 → return false
│         ├─ 地图进入权限 ──────────────── 失败 → return false
│         ├─ 延迟检查 → MustDelayTeleport → 保存目标, return true
│         ├─ 清除选择/战斗/宠物/施法/Aura
│         ├─ Send SMSG_TRANSFER_PENDING
│         ├─ RemovePlayerFromMap（旧地图）
│         ├─ 保存传送数据
│         ├─ Send SMSG_NEW_WORLD
│         ├─ 设置远传送信号量
│         └─ 等待 MSG_MOVE_WORLDPORT_ACK → HandleMoveWorldportAck()
│              ├─ 创建新地图
│              ├─ Relocate() 更新位置
│              ├─ SetMap(newMap)
│              ├─ AddPlayerToMap
│              └─ 清除信号量 → 完成
│
└─── return true
```

---

## 5. 关键数据结构

### 信号量机制

| 信号量 | 设置时机 | 清除时机 | 含义 |
|---|---|---|---|
| `mSemaphoreTeleport_Near` | 同地图传送启动时 | 客户端回复 `MSG_MOVE_TELEPORT_ACK` | 正在同地图传送中 |
| `mSemaphoreTeleport_Far` | 跨地图传送启动时 | 客户端回复 `MSG_MOVE_WORLDPORT_ACK` | 正在跨地图传送中 |

- `IsBeingTeleported()` = Near 或 Far 任一信号量非零
- 传送期间阻止：移动包处理、新传送请求、宠物召唤等

### teleportStore_dest

```cpp
WorldLocation teleportStore_dest;  // 保存传送目标
uint32 teleportStore_options;       // 保存传送选项
```

- 在 `TeleportTo()` 中保存目标坐标
- 在 `HandleMoveWorldportAck()` 中取出使用
- 也用于传送完成前玩家下线的 `SaveToDB()`，防止位置丢失

---

## 6. 坐标系要点

- 每张地图的坐标系**独立**，原点在各自地图中心
- 传送时目标坐标是**直接指定**的（来自数据库/DBC/脚本），不做地图间换算
- 坐标有效范围：`[-17066.17, +17066.17]`（即 `±MAP_HALFSIZE - 0.5`）
- `NormalizeMapCoord()` 将坐标钳制到此范围

---

## 7. 调用者一览

| 调用场景 | 典型调用方式 |
|---|---|
| GM 命令 `.go` | `player->TeleportTo({ mapId, pos })` |
| AreaTrigger 副本入口 | `player->TeleportTo(at->target_mapId, at->target_X/Y/Z/O, TELE_TO_NOT_LEAVE_TRANSPORT)` |
| 传送门 GameObject | `player->TeleportTo(mapid, x, y, z, o)` |
| 法术传送/闪现 | `NearTeleportTo()` → 内部调 `TeleportTo(同地图, ...)` |
| 飞行点 Taxi | `TeleportTo(node->map_id, x, y, z, o, TELE_TO_NOT_LEAVE_TAXI)` |
| 墓地复活 | `TeleportTo(grave->Map, x, y, z, o)` |
| 炉石/召回 | `TeleportTo(m_homebindMapId, ...)` |
| 副本匹配 LFG | `TeleportTo(mapid, x, y, z, o, 0, nullptr, 同地图判断)` |
| 载具传送 | `TeleportTo(transport->GetMapId(), worldX/Y/Z/O, TELE_TO_NOT_LEAVE_TRANSPORT)` |
