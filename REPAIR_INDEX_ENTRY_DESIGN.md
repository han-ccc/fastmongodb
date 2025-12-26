# repairIndexEntry 索引修复命令设计

## 背景问题

| 场景 | 问题 |
|------|------|
| 文档存在，索引缺失 | 某些 bug 导致文档写入后索引条目未创建 |
| 文档已删，索引残留 | 文档删除后索引条目未清理（孤儿索引） |

## 命令格式

```javascript
db.adminCommand({
    repairIndexEntry: "<collection>",
    action: "insert" | "remove",           // 必填
    indexName: "<index_name>",             // 必填

    // 定位条件（可组合使用，确保唯一定位）
    _id: <value>,                          // 文档 _id
    shardKey: {...},                       // shard key 值
    indexKey: {...},                       // 索引键值
    recordId: <int64>,                     // 底层 RecordId

    // 常见组合：
    // - { _id } - 通过 _id 唯一定位文档
    // - { shardKey, _id } - 分片场景定位文档
    // - { indexKey, recordId } - 唯一定位索引条目（用于孤儿清理）
    // - { shardKey, indexKey, recordId } - 完整定位

    dryRun: <bool>                         // 可选: 仅验证不执行
})
```

**定位逻辑**：
- 有 `_id` → 通过 _id 索引查找 RecordId，获取文档
- 有 `recordId` → 直接使用（用于索引条目不存在文档的场景）
- `shardKey` 用于：1) ShardKey 锁定位  2) 验证文档归属

## 使用示例

```javascript
// 1. 添加缺失索引（通过 _id 定位，简单场景）
db.adminCommand({
    repairIndexEntry: "users",
    action: "insert",
    indexName: "email_1",
    _id: ObjectId("...")
})

// 2. 添加缺失索引（分片场景，shardKey + _id）
db.adminCommand({
    repairIndexEntry: "orders",
    action: "insert",
    indexName: "status_1",
    shardKey: { userId: 12345 },
    _id: ObjectId("...")
})

// 3. 删除孤儿索引（indexKey 非唯一，需 recordId 精确定位）
db.adminCommand({
    repairIndexEntry: "products",
    action: "remove",
    indexName: "category_1",
    shardKey: { storeId: 100 },
    indexKey: { category: "electronics" },
    recordId: NumberLong(12345678)
})

// 4. 删除孤儿索引（完整定位）
db.adminCommand({
    repairIndexEntry: "logs",
    action: "remove",
    indexName: "timestamp_1",
    shardKey: { appId: "app1" },
    indexKey: { timestamp: ISODate("2025-01-01") },
    recordId: NumberLong(99999)
})
```

---

## 执行流程

```
1. 参数解析与验证
   ├─ 解析 action, indexName
   └─ 解析定位条件: _id, shardKey, indexKey, recordId
   ↓
2. 验证集合与索引存在
   ↓
3. 获取 ShardKey 锁（如果提供了 shardKey）
   ↓
4. 定位文档/记录（多条件组合）
   ├─ 有 _id → 通过 _id 索引查找 RecordId
   ├─ 有 recordId → 直接使用
   ├─ 有 shardKey → 验证文档归属
   └─ 有 indexKey → 用于校验/定位索引条目
   ↓
5. 校验（在锁保护下）
   ├─ [insert] 校验：文档存在 + 索引缺失
   └─ [remove] 校验：文档不存在 + 索引存在
   ↓
6. 执行索引操作 (WriteUnitOfWork)
   ├─ [insert] → accessMethod->insert(doc, recordId)
   └─ [remove] → accessMethod->remove / unindex
   ↓
7. 提交事务 → 释放锁 → 返回结果
```

## 锁策略

**使用 ShardKey 锁 + 标准共享锁，不使用 Collection 排他锁**

| 锁类型 | 模式 | 说明 |
|-------|------|------|
| ScopedTransaction | MODE_IX | 全局意向排他锁（标准） |
| DBLock | MODE_IX | 数据库级意向排他锁（标准） |
| CollectionLock | MODE_IX | 集合级意向排他锁（**非 MODE_X**） |
| ShardKeyLock | - | 锁住特定 shardkey 值（自定义） |

**说明**：共享锁/意向锁正常持有，只是不使用 Collection MODE_X 排他锁，改用 ShardKey 锁实现细粒度并发控制。

### ShardKey 锁实现（新增）

```cpp
// 文件: src/mongo/db/s/shard_key_lock.h

class ShardKeyLock {
public:
    // 获取 shardkey 锁
    static std::unique_ptr<ShardKeyLock> acquire(
        OperationContext* txn,
        const NamespaceString& nss,
        const BSONObj& shardKeyValue);

    ~ShardKeyLock();  // RAII 释放锁

private:
    // 内部使用 mutex map: collection -> shardKeyValue -> mutex
    static stdx::mutex _globalMutex;
    static std::map<std::string, std::map<BSONObj, std::unique_ptr<stdx::mutex>>> _lockMap;
};
```

### 使用方式

```cpp
// 在 repairIndexEntry 命令中
auto shardKeyLock = ShardKeyLock::acquire(txn, nss, shardKeyValue);

// 获取锁后进行校验
// ... 校验逻辑 ...

// 执行修复操作
// ... 索引操作 ...

// 离开作用域自动释放锁
```

---

## 校验机制（关键）

**在 shardkey 锁住后，必须校验状态再操作：**

### insert 操作校验

```cpp
// 1. 校验文档存在
Snapshotted<BSONObj> doc;
if (!collection->findDoc(txn, recordId, &doc)) {
    return {ErrorCodes::DocumentNotFound, "Document does not exist"};
}

// 2. 校验索引条目确实缺失
BSONObjSet keys;
accessMethod->getKeys(doc.value(), GetKeysMode::kEnforceConstraints, &keys, nullptr);
for (const auto& key : keys) {
    if (accessMethod->exists(txn, key, recordId)) {
        return {ErrorCodes::IndexEntryAlreadyExists,
                "Index entry already exists, no repair needed"};
    }
}

// 3. 校验通过，执行 insert
```

### remove 操作校验

```cpp
// 1. 校验文档确实不存在（如果通过 indexKey 定位）
if (hasIndexKey) {
    Snapshotted<BSONObj> doc;
    if (collection->findDoc(txn, recordId, &doc)) {
        return {ErrorCodes::DocumentStillExists,
                "Document still exists, cannot remove orphan index"};
    }
}

// 2. 校验索引条目确实存在
if (!accessMethod->exists(txn, indexKey, recordId)) {
    return {ErrorCodes::IndexEntryNotFound,
            "Index entry does not exist, no repair needed"};
}

// 3. 校验通过，执行 remove
```

---

## 错误处理

| 错误场景 | 错误码 | 错误消息 |
|---------|--------|---------|
| 集合不存在 | NamespaceNotFound | Collection not found |
| 索引不存在 | IndexNotFound | Index not found |
| 文档不存在（insert时） | DocumentNotFound | Document not found |
| 参数无效 | InvalidOptions | Must specify _id or indexKey |
| 非主节点 | NotMaster | Not primary |
| **匹配多条记录** | **AmbiguousMatch** | **Multiple documents/index entries matched, need more specific conditions** |

### 多条匹配检测（关键）

**必须确保操作只影响一条记录，否则报错：**

```cpp
// 检测文档匹配数量
if (hasIndexKey && !hasRecordId) {
    // 通过 indexKey 查找，可能匹配多条
    auto cursor = accessMethod->newCursor(txn);
    cursor->seek(indexKey);

    int matchCount = 0;
    while (cursor->next() && matchCount < 2) {
        matchCount++;
    }

    if (matchCount > 1) {
        return {ErrorCodes::AmbiguousMatch,
                "Multiple index entries matched, please provide recordId"};
    }
}

// 检测索引条目匹配数量（非唯一索引）
if (action == "insert") {
    BSONObjSet keys;
    accessMethod->getKeys(doc.value(), ..., &keys);

    if (keys.size() > 1) {
        return {ErrorCodes::AmbiguousMatch,
                "Document generates multiple index keys, please specify indexKey"};
    }
}
```

---

## 需要修改的文件

| # | 文件路径 | 修改内容 |
|---|----------|---------|
| 1 | `src/mongo/db/s/shard_key_lock.h` | **新增** - ShardKey 锁头文件 |
| 2 | `src/mongo/db/s/shard_key_lock.cpp` | **新增** - ShardKey 锁实现 |
| 3 | `src/mongo/db/commands/repair_index_entry_cmd.cpp` | **新增** - 命令实现 |
| 4 | `src/mongo/db/auth/action_types.txt` | 添加 `repairIndexEntry` 权限类型 |
| 5 | `src/mongo/db/s/SConscript` | 添加 shard_key_lock 编译规则 |
| 6 | `src/mongo/db/commands/SConscript` | 添加命令编译规则 |

---

## 关键实现代码

### 命令骨架

```cpp
class CmdRepairIndexEntry : public Command {
public:
    CmdRepairIndexEntry() : Command("repairIndexEntry") {}

    bool slaveOk() const override { return false; }
    bool supportsWriteConcern(const BSONObj&) const override { return true; }

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int options,
             string& errmsg,
             BSONObjBuilder& result) override;
};
```

### 核心 API 调用

```cpp
// 1. 获取索引
IndexDescriptor* desc = indexCatalog->findIndexByName(txn, indexName);
IndexAccessMethod* accessMethod = indexCatalog->getIndex(desc);

// 2. 通过 _id 查找 RecordId
IndexDescriptor* idIndex = indexCatalog->findIdIndex(txn);
RecordId recordId = indexCatalog->getIndex(idIndex)->findSingle(txn, idKey);

// 3. 获取文档
Snapshotted<BSONObj> doc;
collection->findDoc(txn, recordId, &doc);

// 4. 索引操作
InsertDeleteOptions options;
int64_t numAffected = 0;

// insert
accessMethod->insert(txn, doc.value(), recordId, options, &numAffected);

// remove
accessMethod->remove(txn, doc.value(), recordId, options, &numAffected);
```

---

## 实现步骤

### Phase 1: ShardKey 锁
- [ ] 1. 创建 `shard_key_lock.h` - 定义 ShardKeyLock 类
- [ ] 2. 创建 `shard_key_lock.cpp` - 实现 RAII 锁机制
- [ ] 3. 更新 `src/mongo/db/s/SConscript` 添加编译规则

### Phase 2: 命令实现
- [ ] 4. 在 `action_types.txt` 添加 `repairIndexEntry` 权限
- [ ] 5. 创建 `repair_index_entry_cmd.cpp` 命令框架
- [ ] 6. 实现参数解析（action, indexName, 定位方式）
- [ ] 7. 集成 ShardKeyLock 获取锁

### Phase 3: 校验逻辑
- [ ] 8. insert 校验：文档存在 + 索引缺失
- [ ] 9. remove 校验：文档不存在 + 索引存在

### Phase 4: 索引操作
- [ ] 10. 实现 insert 操作（accessMethod->insert）
- [ ] 11. 实现 remove 操作（accessMethod->remove）
- [ ] 12. 实现 indexKey+recordId 直接删除

### Phase 5: 单元测试
- [ ] 13. 创建 `shard_key_lock_test.cpp` - ShardKey 锁测试
- [ ] 14. 创建 `repair_index_entry_cmd_test.cpp` - 命令测试

### Phase 6: 集成
- [ ] 15. 更新 SConscript 编译配置
- [ ] 16. 编译验证
- [ ] 17. 端到端功能测试

---

## 单元测试设计

### 1. ShardKey 锁测试 (`shard_key_lock_test.cpp`)

```cpp
// 测试用例:
TEST(ShardKeyLockTest, BasicAcquireRelease) {
    // 获取锁 → 验证持有 → 释放 → 验证释放
}

TEST(ShardKeyLockTest, ConcurrentSameKey) {
    // 两个线程竞争同一 shardKey → 第二个阻塞
}

TEST(ShardKeyLockTest, ConcurrentDifferentKeys) {
    // 两个线程获取不同 shardKey → 都成功（不互斥）
}

TEST(ShardKeyLockTest, RAIIRelease) {
    // 锁对象离开作用域 → 自动释放
}
```

### 2. 命令测试 (`repair_index_entry_cmd_test.cpp`)

```cpp
// === 参数验证测试 ===
TEST(RepairIndexEntryCmdTest, MissingAction) {
    // 缺少 action → InvalidOptions
}

TEST(RepairIndexEntryCmdTest, MissingIndexName) {
    // 缺少 indexName → InvalidOptions
}

TEST(RepairIndexEntryCmdTest, InvalidAction) {
    // action 不是 insert/remove → InvalidOptions
}

TEST(RepairIndexEntryCmdTest, NoLocationParams) {
    // 没有任何定位参数 → InvalidOptions
}

// === insert 操作测试 ===
TEST(RepairIndexEntryCmdTest, InsertDocNotFound) {
    // 文档不存在 → DocumentNotFound
}

TEST(RepairIndexEntryCmdTest, InsertIndexAlreadyExists) {
    // 索引条目已存在 → IndexEntryAlreadyExists (校验失败)
}

TEST(RepairIndexEntryCmdTest, InsertSuccess) {
    // 文档存在 + 索引缺失 → 成功添加
}

// === remove 操作测试 ===
TEST(RepairIndexEntryCmdTest, RemoveDocStillExists) {
    // 文档仍存在 → DocumentStillExists (校验失败)
}

TEST(RepairIndexEntryCmdTest, RemoveIndexNotFound) {
    // 索引条目不存在 → IndexEntryNotFound
}

TEST(RepairIndexEntryCmdTest, RemoveOrphanSuccess) {
    // 文档不存在 + 索引存在 → 成功删除孤儿索引
}

// === 多条件定位测试 ===
TEST(RepairIndexEntryCmdTest, LocateByIdOnly) {
    // 仅 _id → 成功定位
}

TEST(RepairIndexEntryCmdTest, LocateByShardKeyAndId) {
    // shardKey + _id → 成功定位
}

TEST(RepairIndexEntryCmdTest, LocateByIndexKeyAndRecordId) {
    // indexKey + recordId → 成功定位索引条目
}

// === 锁集成测试 ===
TEST(RepairIndexEntryCmdTest, ShardKeyLockAcquired) {
    // 验证命令执行时获取了 shardKey 锁
}

// === 多条匹配测试 ===
TEST(RepairIndexEntryCmdTest, AmbiguousIndexKeyMatch) {
    // indexKey 匹配多条索引条目，无 recordId → AmbiguousMatch
}

TEST(RepairIndexEntryCmdTest, AmbiguousDocumentMatch) {
    // 文档生成多个索引键（multikey），无 indexKey → AmbiguousMatch
}

TEST(RepairIndexEntryCmdTest, UniqueMatchWithRecordId) {
    // indexKey 可能匹配多条，但提供 recordId → 成功唯一定位
}
```

### 3. 测试文件清单

| # | 文件路径 | 说明 |
|---|----------|------|
| 1 | `src/mongo/db/s/shard_key_lock_test.cpp` | ShardKey 锁单元测试 |
| 2 | `src/mongo/db/commands/repair_index_entry_cmd_test.cpp` | 命令单元测试 |

---

## 参考文件

| 文件 | 用途 |
|------|------|
| `src/mongo/db/commands/validate.cpp` | 锁策略和权限检查参考 |
| `src/mongo/db/catalog/index_catalog.cpp` | 索引操作 API |
| `src/mongo/db/index/index_access_method.cpp` | insert/remove 实现 |
| `src/mongo/db/catalog/collection.h` | findDoc 接口 |
