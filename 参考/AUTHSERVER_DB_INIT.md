# authserver 数据库自动加载与建表流程

> **核心文件**:
> - `src/server/apps/authserver/Main.cpp` — 入口
> - `src/server/database/Database/DatabaseLoader.cpp` — 四阶段加载器
> - `src/server/database/Updater/DBUpdater.cpp` — 建库、填充、更新
> - `src/server/database/Updater/UpdateFetcher.cpp` — 增量更新匹配算法
> - `src/server/database/Database/DatabaseWorkerPool.cpp` — 连接池管理
> - `data/sql/base/db_auth/` — 基础表结构 SQL（18 个文件）

---

## 1. 总体流程图

```
main()
  │
  ├─ sConfigMgr->LoadAppConfigs()          加载 authserver.conf
  ├─ sLog->Initialize()                     初始化日志
  │
  └─ StartDB()                              ← 核心入口
       │
       ├─ MySQL::Library_Init()              初始化 MySQL C 库
       │
       ├─ DatabaseLoader loader(...)
       │    └─ .AddDatabase(LoginDatabase, "Login")
       │         ├─ _open 队列  ← 注册连接 lambda
       │         ├─ _populate 队列 ← 注册填充 lambda
       │         ├─ _update 队列  ← 注册更新 lambda
       │         └─ _prepare 队列 ← 注册预编译 lambda
       │
       └─ loader.Load()
            │
            ├─ [阶段1] OpenDatabases()         连接数据库（不存在则创建）
            ├─ [阶段2] PopulateDatabases()     首次运行：执行 base SQL 建表
            ├─ [阶段3] UpdateDatabases()       每次启动：应用增量 SQL 更新
            └─ [阶段4] PrepareStatements()     预编译 SQL 语句
```

---

## 2. StartDB() — 入口函数

**文件**: `Main.cpp:207-224`

```cpp
bool StartDB()
{
    MySQL::Library_Init();                    // 初始化 MySQL 线程安全

    DatabaseLoader loader("server.authserver");
    loader
        .AddDatabase(LoginDatabase, "Login"); // 只加载 Login 数据库

    if (!loader.Load())
        return false;

    LOG_INFO("server.authserver", "Started auth database connection pool.");
    sLog->SetRealmId(0);
    return true;
}
```

authserver 只操作 **一个数据库**：`LoginDatabase`（对应 `acore_auth`）。

`LoginDatabase` 的类型是 `DatabaseWorkerPool<LoginDatabaseConnection>`（`DatabaseEnv.cpp:22`）。

---

## 3. DatabaseLoader::AddDatabase() — 注册四个阶段

**文件**: `DatabaseLoader.cpp:52-171`

`AddDatabase` 向四个队列各推入一个 lambda：

| 队列 | Lambda 职责 | 条件 |
|---|---|---|
| `_open` | 读取配置、连接数据库、数据库不存在时自动创建 | 始终注册 |
| `_populate` | 首次运行时执行 base SQL 建表 | `Updates.EnableDatabases` 包含对应位 |
| `_update` | 每次启动时应用增量更新 | 同上 |
| `_prepare` | 预编译所有 prepared statements | 始终注册 |

此外在 `_open` 成功后，向 `_close` 栈推入 `pool.Close()` 以便失败时回滚。

### _open lambda 的关键逻辑

```
1. 从配置读取连接字符串: LoginDatabaseInfo
   默认值: "127.0.0.1;3306;acore;acore;acore_auth"
   格式: "host;port;user;password;database"

2. pool.SetConnectionInfo(dbString, asyncThreads, synchThreads)

3. pool.Open()
   ├─ 成功 (error == 0) → 继续
   ├─ CR_CONNECTION_ERROR → 重试（最多 20 次，间隔 15 秒）
   └─ ER_BAD_DB_ERROR (1049) → 数据库不存在
       └─ 如果 AutoSetup 启用:
           ├─ DBUpdater<T>::Create(pool)  → 执行 CREATE DATABASE
           └─ pool.Open() 重试
```

---

## 4. DatabaseLoader::Load() — 四阶段流水线

**文件**: `DatabaseLoader.cpp:173-191`

```cpp
bool DatabaseLoader::Load()
{
    if (!OpenDatabases())       // 阶段1
        return false;
    if (!PopulateDatabases())   // 阶段2
        return false;
    if (!UpdateDatabases())     // 阶段3
        return false;
    if (!PrepareStatements())   // 阶段4
        return false;
    return true;
}
```

每个阶段调用 `Process(queue)` 遍历执行所有 lambda。任一失败则回滚所有已打开的连接池。

---

## 5. 阶段1：OpenDatabases — 连接与建库

### 5.1 pool.Open()

**文件**: `DatabaseWorkerPool.cpp:86-110`

```
pool.Open()
  ├─ OpenConnections(IDX_ASYNC, asyncThreads)   创建异步连接（带工作队列）
  └─ OpenConnections(IDX_SYNCH, synchThreads)   创建同步连接
```

`OpenConnections()` (`DatabaseWorkerPool.cpp:417-457`) 为每个连接：
1. 创建 `LoginDatabaseConnection` 实例
2. 调用 `connection->Open()` → 内部执行 `mysql_real_connect()`
3. 检查 MySQL 版本 ≥ 8.0
4. 失败则清空所有连接并返回错误码

### 5.2 数据库不存在时的自动创建

**文件**: `DatabaseLoader.cpp:106-113`

```cpp
if ((error == ER_BAD_DB_ERROR) && updatesEnabledForThis && _autoSetup)
{
    if (DBUpdater<T>::Create(pool) && (!pool.Open()))
        error = 0;
}
```

### 5.3 DBUpdater::Create()

**文件**: `DBUpdater.cpp:173-221`

```
1. 提示用户确认（非 dry-run 且未设置 AC_DISABLE_INTERACTIVE=1）
2. 写临时文件 create_table.sql:
     CREATE DATABASE `acore_auth` DEFAULT CHARACTER SET UTF8MB4
     COLLATE utf8mb4_general_ci;
3. 调用 ApplyFile() 通过 mysql CLI 执行
4. 删除临时文件
5. 重试 pool.Open()
```

此时数据库已存在，但**完全为空**（无任何表）。

---

## 6. 阶段2：PopulateDatabases — 首次建表

### 6.1 DBUpdater::Populate()

**文件**: `DBUpdater.cpp:359-430`

**核心守卫**：通过 `SHOW TABLES` 检查数据库是否有表：

```cpp
QueryResult const result = Retrieve(pool, "SHOW TABLES");
if (result && (result->GetRowCount() > 0))
    return true;   // ← 已有表，跳过填充（非首次运行）
```

如果数据库**完全为空**（0 行），则执行填充：

```
1. 获取 base SQL 目录: data/sql/base/db_auth/
2. 遍历目录中所有 .sql 文件
3. 按文件名排序（字母序）
4. 逐个 ApplyFile() 执行
```

### 6.2 base SQL 文件清单

**目录**: `data/sql/base/db_auth/`（共 18 个文件）

```
account.sql                 ← 账号表（核心）
account_access.sql          ← 账号权限/GM等级
account_banned.sql          ← 账号封禁
account_muted.sql           ← 账号禁言
autobroadcast.sql           ← 自动广播
autobroadcast_locale.sql    ← 自动广播多语言
build_info.sql              ← 构建版本信息
ip_banned.sql               ← IP 封禁
logs.sql                    ← 日志表
logs_ip_actions.sql         ← IP 操作日志
motd.sql                    ← 每日消息
motd_localized.sql          ← 每日消息多语言
realmcharacters.sql         ← 角色数量统计
realmlist.sql               ← 服务器列表
secret_digest.sql           ← 密钥摘要
updates.sql                 ← ★ 更新追踪表
updates_include.sql         ← ★ 更新目录注册表
uptime.sql                  ← 服务器运行时间
```

### 6.3 两个关键追踪表

**updates 表** (`updates.sql`)：
```sql
CREATE TABLE `updates` (
  `name` varchar(200) NOT NULL,       -- SQL 文件名（含扩展名）
  `hash` char(40) DEFAULT '',         -- 文件内容的 SHA-1 哈希
  `state` enum('RELEASED','CUSTOM',   -- 更新生命周期状态
               'MODULE','ARCHIVED',
               'PENDING') NOT NULL DEFAULT 'RELEASED',
  `timestamp` timestamp NOT NULL,     -- 应用时间
  `speed` int unsigned NOT NULL,      -- 执行耗时(ms)
  PRIMARY KEY (`name`)
);
```
并预填充所有已合并的更新记录。

**updates_include 表** (`updates_include.sql`)：
```sql
CREATE TABLE `updates_include` (
  `path` varchar(200) NOT NULL,       -- SQL 目录路径（$ = 源码根目录）
  `state` enum('RELEASED','ARCHIVED',
               'CUSTOM','PENDING') NOT NULL DEFAULT 'RELEASED',
  PRIMARY KEY (`path`)
);

INSERT INTO `updates_include` VALUES
  ('$/data/sql/archive/db_auth','ARCHIVED'),        -- 已归档更新
  ('$/data/sql/custom/db_auth','CUSTOM'),            -- 自定义更新
  ('$/data/sql/updates/db_auth','RELEASED'),         -- 正式更新
  ('$/data/sql/updates/pending_db_auth','PENDING');  -- 待合并更新
```

---

## 7. 阶段3：UpdateDatabases — 增量更新

### 7.1 DBUpdater::Update()

**文件**: `DBUpdater.cpp:223-296`

**每次启动都会执行**。流程：

```
1. CheckExecutable() — 检查 mysql CLI 可执行文件是否可用

2. 自愈检查 — 确保 updates 和 updates_include 表存在:
   SHOW TABLES LIKE 'updates'
   SHOW TABLES LIKE 'updates_include'
   如果缺失 → 从 base SQL 重新创建（自愈机制）

3. 创建 UpdateFetcher，调用 updateFetcher.Update():
   - 传入配置项:
     - Updates.Redundancy (默认 true)
     - Updates.AllowRehash (默认 true)
     - Updates.ArchivedRedundancy (默认 false)
     - Updates.CleanDeadRefMaxCount (默认 3)
```

### 7.2 UpdateFetcher::Update() — 增量匹配算法

**文件**: `UpdateFetcher.cpp:240-446`

```
步骤1: GetFileList() — 收集所有可用的 SQL 文件
  ├─ ReceiveIncludedDirectories()
  │    从 updates_include 表读取目录列表
  │    追加 modules/<mod>/data/sql/ 下的模块目录
  └─ FillFileListRecursively()
       递归扫描每个目录，收集 .sql 文件
       每个文件标记状态 (RELEASED/ARCHIVED/CUSTOM/PENDING/MODULE)
       检查文件名是否重复（全局唯一）

步骤2: ReceiveAppliedFiles() — 读取已应用的更新
  SELECT * FROM updates → 构建已应用文件映射

步骤3: 遍历所有可用 SQL 文件，逐一比对:

  对每个文件:
  ├─ 计算文件内容的 SHA-1 哈希
  │
  ├─ 如果文件已在 updates 表中:
  │   ├─ 跳过冗余检查且已应用 → 跳过
  │   ├─ 归档文件且标记一致 → 跳过
  │   ├─ 哈希为空且允许 rehash → 更新哈希 (MODE_REHASH)
  │   ├─ 哈希不同 → 重新应用 (文件被修改了)
  │   └─ 哈希相同 → 跳过（已应用且未变）
  │
  └─ 如果文件不在 updates 表中:
      ├─ 查找相同哈希的其他文件 → 重命名处理
      └─ 全新文件 → 应用 (MODE_APPLY)

  应用顺序:
  1. 先应用 RELEASED / ARCHIVED 更新
  2. 再应用 PENDING / CUSTOM / MODULE 更新

步骤4: 清理孤立条目
  updates 表中存在但文件系统中不存在的条目
  根据 CleanDeadRefMaxCount 配置决定是否自动删除
```

### 7.3 SQL 文件生命周期状态

| 状态 | 目录 | 含义 |
|---|---|---|
| `RELEASED` | `data/sql/updates/db_auth/` | 已合并的正式更新 |
| `ARCHIVED` | `data/sql/archive/db_auth/` | 已归档的历史更新 |
| `PENDING` | `data/sql/updates/pending_db_auth/` | 待合并的开发中更新 |
| `CUSTOM` | `data/sql/custom/db_auth/` | 用户自定义更新（gitignored） |
| `MODULE` | `modules/<mod>/data/sql/db_auth/` | 模块提供的更新 |

---

## 8. 阶段4：PrepareStatements — 预编译语句

**文件**: `DatabaseWorkerPool.cpp:137-178`

```
pool.PrepareStatements()
  ├─ 遍历所有异步连接 → connection->PrepareStatements()
  ├─ 遍历所有同步连接 → connection->PrepareStatements()
  └─ 每个连接调用 LoginDatabaseConnection::DoPrepareStatements()
       定义在 Implementation/LoginDatabase.cpp
       预编译 90+ 个 prepared statement（LOGIN_SEL_*、LOGIN_UPD_* 等）
```

---

## 9. ApplyFile() — SQL 文件的实际执行方式

**文件**: `DBUpdater.cpp:451-535`

AzerothCore **不通过 MySQL C API 执行 SQL 文件**，而是调用 mysql CLI：

```
1. 写临时配置文件 mysql_ac.conf:
   [client]
   password = "xxx"

2. 构建 mysql CLI 命令行参数:
   --defaults-extra-file=mysql_ac.conf
   -h<host> -u<user> -P<port>
   --default-character-set=utf8
   --max-allowed-packet=1GB
   [--ssl-mode=REQUIRED]  (如果启用 SSL)
   <database>

3. Acore::StartProcess(mysql可执行路径, args, ...)
   将 .sql 文件内容通过 stdin 管道传给 mysql 进程

4. 检查退出码:
   EXIT_SUCCESS → 成功
   否则 → 抛出 UpdateException，终止启动
```

---

## 10. 完整启动时序图（首次运行 vs 后续运行）

### 首次运行（数据库完全不存在）

```
main()
  └─ StartDB()
       └─ loader.Load()
            │
            ├─ [阶段1] OpenDatabases
            │    ├─ pool.Open() → ER_BAD_DB_ERROR (1049)
            │    ├─ DBUpdater::Create()
            │    │    └─ CREATE DATABASE acore_auth ...    ← 数据库被创建
            │    └─ pool.Open() → 成功连接空数据库
            │
            ├─ [阶段2] PopulateDatabases
            │    ├─ SHOW TABLES → 0 行（空库）
            │    └─ 逐个执行 data/sql/base/db_auth/*.sql  ← 18 个表被创建
            │         ├─ account.sql
            │         ├─ account_access.sql
            │         ├─ ...
            │         ├─ updates.sql              ← 更新追踪表
            │         └─ updates_include.sql       ← 更新目录注册
            │
            ├─ [阶段3] UpdateDatabases
            │    ├─ 检查 updates/updates_include 表存在 ✓
            │    └─ UpdateFetcher::Update()
            │         ├─ 读取 updates_include → 4 个目录
            │         ├─ 扫描目录中的 .sql 文件
            │         ├─ 与 updates 表对比哈希
            │         └─ 应用新增/变更的更新文件
            │
            └─ [阶段4] PrepareStatements
                 └─ 预编译 90+ 个 prepared statement
```

### 后续运行（数据库已存在且最新）

```
main()
  └─ StartDB()
       └─ loader.Load()
            │
            ├─ [阶段1] OpenDatabases
            │    └─ pool.Open() → 直接成功
            │
            ├─ [阶段2] PopulateDatabases
            │    └─ SHOW TABLES → >0 行 → 跳过
            │
            ├─ [阶段3] UpdateDatabases
            │    ├─ 自愈检查 updates/updates_include ✓
            │    └─ UpdateFetcher::Update()
            │         ├─ 所有文件哈希匹配 → 无需更新
            │         └─ ">> Login database is up-to-date!"
            │
            └─ [阶段4] PrepareStatements
                 └─ 预编译 prepared statement
```

---

## 11. 关键配置项

| 配置项 | 默认值 | 含义 |
|---|---|---|
| `LoginDatabaseInfo` | `127.0.0.1;3306;acore;acore;acore_auth` | Login 数据库连接字符串 |
| `LoginDatabase.WorkerThreads` | `1` | 异步连接数 |
| `LoginDatabase.SynchThreads` | `1` | 同步连接数 |
| `Updates.AutoSetup` | `true` | 数据库不存在时自动创建 |
| `Updates.EnableDatabases` | 默认全部启用 | 控制哪些数据库启用自动更新 |
| `Updates.Redundancy` | `true` | 对已应用文件进行冗余哈希校验 |
| `Updates.AllowRehash` | `true` | 允许重新计算空哈希条目 |
| `Updates.ArchivedRedundancy` | `false` | 是否对归档文件进行冗余检查 |
| `Updates.CleanDeadRefMaxCount` | `3` | 清理孤立条目的最大数量 |
| `MySQLExecutable` | 自动检测 | mysql CLI 可执行文件路径 |
| `SourceDirectory` | 自动检测 | 源码根目录路径 |

---

## 12. 自愈机制

`DBUpdater::Update()` 中有一个关键的自愈逻辑（`DBUpdater.cpp:240-263`）：

```cpp
auto CheckUpdateTable = [&](std::string const& tableName)
{
    auto checkTable = DBUpdater<T>::Retrieve(pool, "SHOW TABLES LIKE '{}'", tableName);
    if (!checkTable)
    {
        // 表不存在！从 base SQL 重建
        Path const temp(GetBaseFilesDirectory() + tableName + ".sql");
        DBUpdater<T>::ApplyFile(pool, temp);
    }
    return true;
};

if (!CheckUpdateTable("updates") || !CheckUpdateTable("updates_include"))
    return false;
```

即使有人手动删除了 `updates` 或 `updates_include` 表，服务器也能在启动时自动从 base SQL 重建，确保更新系统不中断。
