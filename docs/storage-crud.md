# Phase 4E：公共 Storage CRUD 集成与 Phase 4 收尾

## 1. 事实来源与边界

公共 API 继续以 `src/include/tinydbms/storage.h` 为准，没有增加或修改 Request/Result。
storage.cpp 仅协调 lifecycle、Catalog 查找、cursor restriction、私有模块调用与错误转换。
物理链路保持 `Storage API → HeapTable → BufferPool → RecordPage → SlottedPage/RecordCodec`；
storage.cpp 没有 fetch/页内 insert/erase，也不认识 SQL/WHERE/Filter/Project。

MiniOB 的 Table/RecordFileHandler 分层及 record scanner 是架构参考；本项目使用独立
CursorRegistry、进程级单调 ID、owned Record 和无长期 pin 的扫描，不移植事务或执行器。

## 2. 公共 INSERT / DELETE

严格顺序：Open → 表存在 → 空批次成功 → 同表未 close cursor 检查 → 临时 HeapTable batch。
空批次仍校验 lifecycle 和表存在，不算 mutation，即使 cursor 存在也成功。
非空批次不提前整体校验；逐项遇错停止，返回成功 RID 前缀或 uint64 deleted_count，
不回滚。不创建长期 HeapTable registry，PageFile poison 不因临时对象重建而失效。

## 3. 公共 Cursor / SCAN

open_table 校验 lifecycle 和表存在，委托 CursorRegistry.create，由 HeapTable.begin_scan
冻结当前 page_count；不 fetch，空表也返回 CursorId。多个同表读 cursor 允许。
scan_next 委托 registry，后者拥有经过创建时解析的不可变 schema 副本并按需构造 HeapTable；
当前不支持 DROP/ALTER，因此无需在每次扫描中重复解析 schema 或引入第二套 cursor 管理。

ScanNextResult 三态：Record/无 error、无 Record/无 error（EOF）、无 Record/有 error。
Record 拥有自身 values。EOF 重复 EOF，Failed 重复 saved_error；两者都保留 registry 条目。
有效 close_cursor 删除条目；重复 close 或不存在的 ID 返回 kCursorInvalid，不幂等成功。

CursorId 0 合法，进程级单调，不因成功关闭或重开重置；最大值可签发一次，此后明确
kInvalidRequest（exhausted），没有 wrap。没有新的公共物理 ID 或 sentinel。

## 4. Cursor mutation restriction

只要同表 registry 尚有 Active、Eof 或 Failed cursor，就拒绝非空 INSERT/DELETE，
返回 kInvalidRequest。空批次允许；其他表 mutation 允许。检查只在 storage.cpp，
HeapTable 不依赖 registry。所有 cursor close 后恢复该表 mutation 能力。

## 5. 错误

统一 data_error 重载按枚举映射，不检查字符串：Heap invalid→kInvalidRequest，
too-large→kValueTooLarge，no-victim→kInvalidRequest（消息注明无 unpinned replacement frame），
I/O→kIoError，corrupt→kCorrupt，cursor invalid→kCursorInvalid。表查找失败→kTableNotFound。
公共错误种类、batch 成功前缀和扫描错误语义不变。

## 6. Lifecycle 与 ownership

StorageState 持有 FileManager、BufferPool、CursorRegistry（按逆序销毁），以及 tables/lifecycle。
registry 不保存 guard 或物理裸指针。只有 Open 可以正常操作。

| 状态 | insert/open_table/delete_records | scan_next/close_cursor |
|---|---|---|
| Closed | kInvalidRequest | 保留既有 kCursorInvalid 语义 |
| Open | 执行业务契约 | 执行业务契约 |
| Closing | kInvalidRequest | kInvalidRequest，不访问 registry/已关闭池 |

close_storage：BufferPool.close → FileManager.close_all → metadata → reset_state。
最后 reset 才销毁当前 registry，但不重置全局 CursorId allocator。
池关闭失败恢复 Open，registry 原样保留；文件/metadata 收尾失败保持 Closing，
禁止正常操作，后续 close 重试成功后清理。未关闭 cursor 本身不持 pin，因此不阻止关闭。
Closed 重复 close_storage 仍成功 no-op。

## 7. 测试与验收证据

`storage_crud_test.cpp` 通过公共 API 完成：open/create → 多页 INSERT → scan 收集 RID →
close cursor → DELETE → 再 scan → close/reopen → 核对存活值和原 RID。
另覆盖 insert/delete 部分成功与超限、generation 复用后旧 RID 拒绝、空表与缺表、
双 cursor 独立进度、EOF/Failed 写入限制、空批次例外、不同表写入、重复 close、
free hole/retired 恢复跳过、corrupt/I/O/no-victim、pin 释放、CursorId 0/单调/耗尽、
成功 close 清理以及失败恢复 Open/保持 Closing 的行为。

故障使用已有 StorageTestAccess 配置（补充可选 ReadPage adapter）和真实 PageFile 读写，
不新增公共测试 API；retired/free fixture 只在从未驻留的新页 bootstrap，正常 CRUD 无缓存旁路。
旧 storage_contract 测试改为明确缺表/未知 cursor 断言，并补合法空表 EOF，没有弱化旧契约。

## 8. 非目标

Phase 4 完成的是 typed Storage CRUD，不是 SQL 数据库全链路完成。
没有 SQL WHERE、Parser、Executor/Filter/Projection、Index、FSM、Transaction、WAL 或 MVCC。
不提供断电恢复或事务原子性，不继续 Phase 5。
