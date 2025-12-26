#!/usr/bin/env python3
"""
Stability Benchmark v7 - F方案端到端测试 (27个用例)
MongoDB 3.4.2 + RocksDB 性能基准测试

使用原始 Wire Protocol，零客户端开销，精确测量服务端延迟。

Usage:
    python3 /tmp/stability_benchmark_v7.py [OPTIONS]

    --port PORT      目标端口 (默认 27020 分片集群, 可选 27019 单节点)
    --iterations N   测试迭代次数 (默认 2000)
    --warmup N       预热次数 (默认 200)
    --output FILE    JSON 输出文件 (默认 /tmp/stability_v7_result.json)
"""

import socket
import struct
import time
import json
import random
import argparse
import statistics
from datetime import datetime
from collections import OrderedDict

# ============================================================================
# 配置常量
# ============================================================================

DEFAULT_HOST = "localhost"
DEFAULT_PORT = 27020
DEFAULT_DB = "shardtest"
DEFAULT_ITERATIONS = 2000
DEFAULT_WARMUP = 200
OUTLIER_TRIM = 0.05  # 去除5%异常值
RESET_SLEEP = 3  # F方案重置后等待秒数

# MongoDB Wire Protocol opcodes
OP_REPLY = 1
OP_UPDATE = 2001
OP_INSERT = 2002
OP_QUERY = 2004
OP_DELETE = 2006

# BSON type codes
BSON_DOUBLE = 0x01
BSON_STRING = 0x02
BSON_DOCUMENT = 0x03
BSON_ARRAY = 0x04
BSON_BINARY = 0x05
BSON_OBJECTID = 0x07
BSON_BOOL = 0x08
BSON_DATETIME = 0x09
BSON_NULL = 0x0A
BSON_INT32 = 0x10
BSON_INT64 = 0x12

# 全局请求ID
_request_id = 0

def get_request_id():
    global _request_id
    _request_id += 1
    return _request_id

# ============================================================================
# BSON 编解码
# ============================================================================

def bson_encode(doc):
    """将Python字典编码为BSON二进制"""
    if doc is None:
        return b'\x05\x00\x00\x00\x00'  # 空文档

    body = b''
    for key, value in doc.items():
        key_bytes = key.encode('utf-8') + b'\x00'

        if value is None:
            body += bytes([BSON_NULL]) + key_bytes
        elif isinstance(value, bool):
            body += bytes([BSON_BOOL]) + key_bytes + (b'\x01' if value else b'\x00')
        elif isinstance(value, int):
            if -2147483648 <= value <= 2147483647:
                body += bytes([BSON_INT32]) + key_bytes + struct.pack('<i', value)
            else:
                body += bytes([BSON_INT64]) + key_bytes + struct.pack('<q', value)
        elif isinstance(value, float):
            body += bytes([BSON_DOUBLE]) + key_bytes + struct.pack('<d', value)
        elif isinstance(value, str):
            str_bytes = value.encode('utf-8') + b'\x00'
            body += bytes([BSON_STRING]) + key_bytes + struct.pack('<i', len(str_bytes)) + str_bytes
        elif isinstance(value, bytes):
            body += bytes([BSON_BINARY]) + key_bytes + struct.pack('<i', len(value)) + b'\x00' + value
        elif isinstance(value, dict):
            subdoc = bson_encode(value)
            body += bytes([BSON_DOCUMENT]) + key_bytes + subdoc
        elif isinstance(value, (list, tuple)):
            # 数组编码为特殊文档，键为索引字符串
            array_doc = OrderedDict()
            for i, item in enumerate(value):
                array_doc[str(i)] = item
            subdoc = bson_encode(array_doc)
            body += bytes([BSON_ARRAY]) + key_bytes + subdoc
        elif isinstance(value, ObjectId):
            body += bytes([BSON_OBJECTID]) + key_bytes + value.binary
        else:
            raise ValueError(f"Unsupported BSON type: {type(value)}")

    body += b'\x00'  # 文档结束符
    return struct.pack('<i', len(body) + 4) + body

def bson_decode(data, offset=0):
    """从BSON二进制解码为Python字典"""
    if len(data) < offset + 4:
        return {}, offset

    doc_len = struct.unpack_from('<i', data, offset)[0]
    end = offset + doc_len
    pos = offset + 4
    result = OrderedDict()

    while pos < end - 1:
        type_byte = data[pos]
        pos += 1

        # 读取键名
        key_end = data.index(b'\x00', pos)
        key = data[pos:key_end].decode('utf-8')
        pos = key_end + 1

        if type_byte == BSON_DOUBLE:
            value = struct.unpack_from('<d', data, pos)[0]
            pos += 8
        elif type_byte == BSON_STRING:
            str_len = struct.unpack_from('<i', data, pos)[0]
            pos += 4
            value = data[pos:pos + str_len - 1].decode('utf-8')
            pos += str_len
        elif type_byte == BSON_DOCUMENT:
            value, pos = bson_decode(data, pos)
        elif type_byte == BSON_ARRAY:
            arr_dict, pos = bson_decode(data, pos)
            value = list(arr_dict.values())
        elif type_byte == BSON_BINARY:
            bin_len = struct.unpack_from('<i', data, pos)[0]
            pos += 5  # 4 bytes len + 1 byte subtype
            value = data[pos:pos + bin_len]
            pos += bin_len
        elif type_byte == BSON_OBJECTID:
            value = ObjectId(data[pos:pos + 12])
            pos += 12
        elif type_byte == BSON_BOOL:
            value = data[pos] != 0
            pos += 1
        elif type_byte == BSON_DATETIME:
            value = struct.unpack_from('<q', data, pos)[0]
            pos += 8
        elif type_byte == BSON_NULL:
            value = None
        elif type_byte == BSON_INT32:
            value = struct.unpack_from('<i', data, pos)[0]
            pos += 4
        elif type_byte == BSON_INT64:
            value = struct.unpack_from('<q', data, pos)[0]
            pos += 8
        else:
            raise ValueError(f"Unknown BSON type: {type_byte} at pos {pos}")

        result[key] = value

    return result, end

class ObjectId:
    """简单的 ObjectId 实现"""
    _counter = random.randint(0, 0xFFFFFF)
    _machine = random.randbytes(3)
    _pid = random.randint(0, 0xFFFF)

    def __init__(self, oid=None):
        if oid is None:
            # 生成新的 ObjectId
            timestamp = struct.pack('>I', int(time.time()))
            ObjectId._counter = (ObjectId._counter + 1) & 0xFFFFFF
            counter = struct.pack('>I', ObjectId._counter)[1:]
            self.binary = timestamp + ObjectId._machine + struct.pack('>H', ObjectId._pid) + counter
        elif isinstance(oid, bytes):
            self.binary = oid
        else:
            raise ValueError("ObjectId must be bytes or None")

    def __str__(self):
        return self.binary.hex()

    def __repr__(self):
        return f"ObjectId('{self}')"

# ============================================================================
# MongoDB Wire Protocol
# ============================================================================

def make_header(msg_len, request_id, response_to, opcode):
    """构建消息头 (16字节)"""
    return struct.pack('<iiii', msg_len, request_id, response_to, opcode)

def send_message(sock, opcode, body):
    """发送 Wire Protocol 消息并接收响应"""
    request_id = get_request_id()
    header = make_header(16 + len(body), request_id, 0, opcode)
    sock.sendall(header + body)

    # 接收响应头
    resp_header = sock.recv(16)
    if len(resp_header) < 16:
        raise IOError("Failed to receive response header")

    resp_len, resp_id, resp_to, resp_opcode = struct.unpack('<iiii', resp_header)

    # 接收响应体
    body_len = resp_len - 16
    resp_body = b''
    while len(resp_body) < body_len:
        chunk = sock.recv(body_len - len(resp_body))
        if not chunk:
            raise IOError("Connection closed while receiving response")
        resp_body += chunk

    return resp_opcode, resp_body

def parse_op_reply(body):
    """解析 OP_REPLY 响应"""
    flags, cursor_id, starting_from, num_returned = struct.unpack_from('<iqii', body, 0)

    documents = []
    offset = 20
    for _ in range(num_returned):
        doc, offset = bson_decode(body, offset)
        documents.append(doc)

    return {
        'flags': flags,
        'cursor_id': cursor_id,
        'starting_from': starting_from,
        'num_returned': num_returned,
        'documents': documents
    }

def send_op_query(sock, db, collection, query, fields=None, skip=0, limit=0, flags=0):
    """发送 OP_QUERY 并返回结果文档"""
    full_name = f"{db}.{collection}".encode('utf-8') + b'\x00'
    query_bson = bson_encode(query)
    fields_bson = bson_encode(fields) if fields else b''

    body = struct.pack('<i', flags) + full_name + struct.pack('<ii', skip, limit) + query_bson + fields_bson

    opcode, resp_body = send_message(sock, OP_QUERY, body)

    if opcode != OP_REPLY:
        raise ValueError(f"Expected OP_REPLY, got {opcode}")

    reply = parse_op_reply(resp_body)
    return reply['documents']

def send_op_insert(sock, db, collection, documents, flags=0):
    """发送 OP_INSERT"""
    full_name = f"{db}.{collection}".encode('utf-8') + b'\x00'

    docs_bson = b''
    for doc in documents:
        docs_bson += bson_encode(doc)

    body = struct.pack('<i', flags) + full_name + docs_bson

    # OP_INSERT 不返回响应，发送后直接返回
    request_id = get_request_id()
    header = make_header(16 + len(body), request_id, 0, OP_INSERT)
    sock.sendall(header + body)

def send_op_update(sock, db, collection, selector, update, flags=0):
    """发送 OP_UPDATE"""
    full_name = f"{db}.{collection}".encode('utf-8') + b'\x00'
    selector_bson = bson_encode(selector)
    update_bson = bson_encode(update)

    body = struct.pack('<i', 0) + full_name + struct.pack('<i', flags) + selector_bson + update_bson

    request_id = get_request_id()
    header = make_header(16 + len(body), request_id, 0, OP_UPDATE)
    sock.sendall(header + body)

def send_op_delete(sock, db, collection, selector, flags=0):
    """发送 OP_DELETE"""
    full_name = f"{db}.{collection}".encode('utf-8') + b'\x00'
    selector_bson = bson_encode(selector)

    body = struct.pack('<i', 0) + full_name + struct.pack('<i', flags) + selector_bson

    request_id = get_request_id()
    header = make_header(16 + len(body), request_id, 0, OP_DELETE)
    sock.sendall(header + body)

# ============================================================================
# 命令执行
# ============================================================================

def run_cmd(sock, db, cmd):
    """执行数据库命令"""
    docs = send_op_query(sock, db, "$cmd", cmd, limit=1)
    if docs:
        return docs[0]
    return {}

def get_last_error(sock, db):
    """获取最后一次操作的错误信息"""
    return run_cmd(sock, db, {"getLastError": 1})

# ============================================================================
# F方案重置
# ============================================================================

def f_scheme_reset(sock, db):
    """F方案重置 - drop + fsync + sleep"""
    print("  F-Scheme reset: drop collections...")

    # 删除测试集合
    run_cmd(sock, db, {"drop": "simple_docs"})
    run_cmd(sock, db, {"drop": "complex_docs"})

    # 强制刷盘
    print("  F-Scheme reset: fsync...")
    run_cmd(sock, "admin", {"fsync": 1})

    # 等待 RocksDB 稳定
    print(f"  F-Scheme reset: sleep {RESET_SLEEP}s...")
    time.sleep(RESET_SLEEP)

# ============================================================================
# 集合设置
# ============================================================================

def setup_simple_collection(sock, db):
    """设置简单表 - 只有分片键索引"""
    print("  Setting up simple_docs collection...")

    # 创建集合
    run_cmd(sock, db, {"create": "simple_docs"})

    # 创建分片键索引
    run_cmd(sock, db, {
        "createIndexes": "simple_docs",
        "indexes": [{"key": {"userId": 1}, "name": "userId_1"}]
    })

    # 插入初始数据用于查询测试
    initial_docs = []
    for i in range(1000):
        initial_docs.append({
            "userId": i,
            "name": f"user_{i}",
            "age": 20 + (i % 50),
            "status": ["active", "inactive", "pending"][i % 3],
            "score": random.randint(0, 100)
        })

    # 分批插入
    batch_size = 100
    for i in range(0, len(initial_docs), batch_size):
        batch = initial_docs[i:i+batch_size]
        send_op_insert(sock, db, "simple_docs", batch)

    get_last_error(sock, db)  # 确保写入完成
    print(f"    Inserted {len(initial_docs)} initial documents")

def setup_complex_collection(sock, db):
    """设置复杂表 - 分片键 + 5个二级索引"""
    print("  Setting up complex_docs collection...")

    # 创建集合
    run_cmd(sock, db, {"create": "complex_docs"})

    # 创建索引
    run_cmd(sock, db, {
        "createIndexes": "complex_docs",
        "indexes": [
            {"key": {"userId": 1}, "name": "userId_1"},
            {"key": {"email": 1}, "name": "email_1"},
            {"key": {"age": 1}, "name": "age_1"},
            {"key": {"status": 1}, "name": "status_1"},
            {"key": {"createdAt": 1}, "name": "createdAt_1"},
            {"key": {"category": 1, "score": -1}, "name": "category_score"}
        ]
    })

    # 插入初始数据
    initial_docs = []
    for i in range(1000):
        initial_docs.append({
            "userId": i,
            "email": f"user{i}@example.com",
            "name": f"User {i}",
            "age": 20 + (i % 50),
            "status": ["active", "inactive", "pending"][i % 3],
            "category": ["A", "B", "C", "D"][i % 4],
            "score": random.randint(0, 100),
            "createdAt": int(time.time()) - random.randint(0, 86400 * 30),
            "tags": [f"tag{j}" for j in range(i % 5)],
            "metadata": {"version": 1, "source": "benchmark"}
        })

    batch_size = 100
    for i in range(0, len(initial_docs), batch_size):
        batch = initial_docs[i:i+batch_size]
        send_op_insert(sock, db, "complex_docs", batch)

    get_last_error(sock, db)
    print(f"    Inserted {len(initial_docs)} initial documents")

# ============================================================================
# 测试用例
# ============================================================================

# === Simple 表测试 (12个) ===

def test_simple_insert_single(sock, db, coll, i):
    """单条插入"""
    doc = {"userId": 10000 + i, "name": f"test_{i}", "age": 25, "score": i % 100}
    send_op_insert(sock, db, coll, [doc])
    get_last_error(sock, db)

def test_simple_insert_batch(sock, db, coll, i):
    """批量10条插入"""
    docs = [{"userId": 20000 + i * 10 + j, "name": f"batch_{i}_{j}", "age": 25 + j, "score": j * 10}
            for j in range(10)]
    send_op_insert(sock, db, coll, docs)
    get_last_error(sock, db)

def test_simple_query_exact(sock, db, coll, i):
    """精确匹配分片键"""
    send_op_query(sock, db, coll, {"userId": i % 1000}, limit=1)

def test_simple_query_gt(sock, db, coll, i):
    """范围查询 $gt"""
    send_op_query(sock, db, coll, {"userId": {"$gt": i % 900}}, limit=10)

def test_simple_query_or(sock, db, coll, i):
    """OR 条件查询"""
    send_op_query(sock, db, coll, {"$or": [{"userId": i % 500}, {"userId": (i + 100) % 500}]}, limit=10)

def test_simple_query_in(sock, db, coll, i):
    """IN 查询"""
    ids = [(i + j * 100) % 1000 for j in range(5)]
    send_op_query(sock, db, coll, {"userId": {"$in": ids}}, limit=10)

def test_simple_query_and(sock, db, coll, i):
    """AND 条件查询"""
    send_op_query(sock, db, coll, {"$and": [{"age": {"$gte": 25}}, {"age": {"$lte": 40}}]}, limit=10)

def test_simple_query_partial(sock, db, coll, i):
    """部分键匹配"""
    send_op_query(sock, db, coll, {"age": 25 + (i % 20)}, limit=10)

def test_simple_update_single(sock, db, coll, i):
    """单条更新"""
    send_op_update(sock, db, coll, {"userId": i % 1000}, {"$set": {"score": i % 100}})
    get_last_error(sock, db)

def test_simple_update_multi(sock, db, coll, i):
    """多条更新"""
    send_op_update(sock, db, coll, {"age": 25 + (i % 10)}, {"$inc": {"score": 1}}, flags=2)  # MULTI flag
    get_last_error(sock, db)

def test_simple_delete_single(sock, db, coll, i):
    """单条删除"""
    send_op_delete(sock, db, coll, {"userId": 30000 + i}, flags=1)  # SINGLE flag
    get_last_error(sock, db)

def test_simple_delete_multi(sock, db, coll, i):
    """多条删除"""
    send_op_delete(sock, db, coll, {"userId": {"$gte": 40000 + i * 5, "$lt": 40000 + i * 5 + 3}})
    get_last_error(sock, db)

# === Complex 表测试 (15个) ===

def test_complex_insert_single(sock, db, coll, i):
    """单条插入(5索引)"""
    doc = {
        "userId": 10000 + i,
        "email": f"test{i}@example.com",
        "name": f"Test User {i}",
        "age": 25 + (i % 30),
        "status": "active",
        "category": ["A", "B", "C", "D"][i % 4],
        "score": i % 100,
        "createdAt": int(time.time()),
        "tags": ["benchmark"],
        "metadata": {"version": 1}
    }
    send_op_insert(sock, db, coll, [doc])
    get_last_error(sock, db)

def test_complex_insert_batch(sock, db, coll, i):
    """批量10条插入"""
    docs = [{
        "userId": 20000 + i * 10 + j,
        "email": f"batch{i}_{j}@example.com",
        "name": f"Batch User {i}_{j}",
        "age": 25 + j,
        "status": ["active", "inactive"][j % 2],
        "category": ["A", "B", "C", "D"][j % 4],
        "score": j * 10,
        "createdAt": int(time.time()),
        "tags": [f"tag{j}"],
        "metadata": {"version": 1}
    } for j in range(10)]
    send_op_insert(sock, db, coll, docs)
    get_last_error(sock, db)

def test_complex_query_exact(sock, db, coll, i):
    """精确匹配"""
    send_op_query(sock, db, coll, {"userId": i % 1000}, limit=1)

def test_complex_query_gt(sock, db, coll, i):
    """范围查询 $gt"""
    send_op_query(sock, db, coll, {"userId": {"$gt": i % 900}}, limit=10)

def test_complex_query_or(sock, db, coll, i):
    """OR 条件查询"""
    send_op_query(sock, db, coll, {"$or": [{"status": "active"}, {"category": "A"}]}, limit=10)

def test_complex_query_range(sock, db, coll, i):
    """范围扫描"""
    send_op_query(sock, db, coll, {"age": {"$gte": 25, "$lte": 35}}, limit=20)

def test_complex_query_in(sock, db, coll, i):
    """IN 查询"""
    ids = [(i + j * 100) % 1000 for j in range(5)]
    send_op_query(sock, db, coll, {"userId": {"$in": ids}}, limit=10)

def test_complex_query_and(sock, db, coll, i):
    """AND 条件查询"""
    send_op_query(sock, db, coll, {"$and": [{"status": "active"}, {"age": {"$gte": 30}}]}, limit=10)

def test_complex_query_partial(sock, db, coll, i):
    """部分键匹配"""
    send_op_query(sock, db, coll, {"category": ["A", "B", "C", "D"][i % 4]}, limit=10)

def test_complex_query_by_index(sock, db, coll, i):
    """二级索引查询"""
    send_op_query(sock, db, coll, {"email": f"user{i % 1000}@example.com"}, limit=1)

def test_complex_update_single(sock, db, coll, i):
    """单条更新"""
    send_op_update(sock, db, coll, {"userId": i % 1000}, {"$set": {"score": i % 100}})
    get_last_error(sock, db)

def test_complex_update_indexed(sock, db, coll, i):
    """更新索引字段"""
    send_op_update(sock, db, coll, {"userId": i % 1000}, {"$set": {"status": ["active", "inactive"][i % 2]}})
    get_last_error(sock, db)

def test_complex_update_multi_idx(sock, db, coll, i):
    """多索引更新"""
    send_op_update(sock, db, coll, {"userId": i % 1000},
                   {"$set": {"age": 25 + (i % 30), "category": ["A", "B", "C", "D"][i % 4]}})
    get_last_error(sock, db)

def test_complex_delete_single(sock, db, coll, i):
    """单条删除"""
    send_op_delete(sock, db, coll, {"userId": 30000 + i}, flags=1)
    get_last_error(sock, db)

def test_complex_delete_by_index(sock, db, coll, i):
    """按索引删除"""
    send_op_delete(sock, db, coll, {"email": f"todelete{i}@example.com"}, flags=1)
    get_last_error(sock, db)

# ============================================================================
# 测试用例定义
# ============================================================================

SIMPLE_TESTS = [
    ("INSERT (single)", test_simple_insert_single),
    ("INSERT (batch 10)", test_simple_insert_batch),
    ("QUERY (exact key)", test_simple_query_exact),
    ("QUERY ($gt)", test_simple_query_gt),
    ("QUERY ($or)", test_simple_query_or),
    ("QUERY ($in)", test_simple_query_in),
    ("QUERY ($and)", test_simple_query_and),
    ("QUERY (partial key)", test_simple_query_partial),
    ("UPDATE (single)", test_simple_update_single),
    ("UPDATE (multi)", test_simple_update_multi),
    ("DELETE (single)", test_simple_delete_single),
    ("DELETE (multi)", test_simple_delete_multi),
]

COMPLEX_TESTS = [
    ("INSERT (single)", test_complex_insert_single),
    ("INSERT (batch 10)", test_complex_insert_batch),
    ("QUERY (exact key)", test_complex_query_exact),
    ("QUERY ($gt)", test_complex_query_gt),
    ("QUERY ($or)", test_complex_query_or),
    ("QUERY (range scan)", test_complex_query_range),
    ("QUERY ($in)", test_complex_query_in),
    ("QUERY ($and)", test_complex_query_and),
    ("QUERY (partial key)", test_complex_query_partial),
    ("QUERY (by index)", test_complex_query_by_index),
    ("UPDATE (single)", test_complex_update_single),
    ("UPDATE (indexed)", test_complex_update_indexed),
    ("UPDATE (multi idx)", test_complex_update_multi_idx),
    ("DELETE (single)", test_complex_delete_single),
    ("DELETE (by index)", test_complex_delete_by_index),
]

# ============================================================================
# 统计计算
# ============================================================================

def calc_stats(times):
    """计算统计指标 (去除异常值)"""
    if not times:
        return None

    # 排序
    sorted_times = sorted(times)
    n = len(sorted_times)

    # 去除两端 OUTLIER_TRIM 的异常值
    trim_count = int(n * OUTLIER_TRIM)
    if trim_count > 0:
        trimmed = sorted_times[trim_count:-trim_count]
    else:
        trimmed = sorted_times

    if not trimmed:
        trimmed = sorted_times

    avg = statistics.mean(trimmed)
    p50 = statistics.median(trimmed)
    p95 = trimmed[int(len(trimmed) * 0.95)] if len(trimmed) > 1 else trimmed[0]
    p99 = trimmed[int(len(trimmed) * 0.99)] if len(trimmed) > 1 else trimmed[0]
    min_val = min(trimmed)
    max_val = max(trimmed)
    stddev = statistics.stdev(trimmed) if len(trimmed) > 1 else 0

    return {
        "avg": round(avg, 2),
        "p50": round(p50, 2),
        "p95": round(p95, 2),
        "p99": round(p99, 2),
        "min": round(min_val, 2),
        "max": round(max_val, 2),
        "stddev": round(stddev, 2),
        "count": len(times)
    }

# ============================================================================
# 测试运行
# ============================================================================

def run_warmup(sock, db, coll, test_func, warmup_count):
    """预热"""
    for i in range(warmup_count):
        try:
            test_func(sock, db, coll, i)
        except Exception:
            pass

def run_test(sock, db, coll, test_func, iterations):
    """运行测试并收集延迟"""
    times = []
    for i in range(iterations):
        start = time.perf_counter()
        try:
            test_func(sock, db, coll, i)
            elapsed = (time.perf_counter() - start) * 1_000_000  # 转换为微秒
            times.append(elapsed)
        except Exception as e:
            # 记录错误但继续
            pass
    return times

def run_all_tests(sock, db, coll, tests, iterations, warmup):
    """运行所有测试"""
    results = []
    total = len(tests)

    for idx, (name, func) in enumerate(tests):
        print(f"  {idx+1}/{total} {name}...", end=" ", flush=True)

        # 预热
        run_warmup(sock, db, coll, func, warmup)

        # 正式测试
        times = run_test(sock, db, coll, func, iterations)
        stats = calc_stats(times)

        if stats:
            print(f"avg: {stats['avg']:.1f}us  p50: {stats['p50']:.1f}us")
            results.append({"name": name, **stats})
        else:
            print("SKIP (no data)")
            results.append({"name": name, "error": "no data"})

    return results

# ============================================================================
# 结果输出
# ============================================================================

def print_summary(simple_results, complex_results):
    """打印摘要"""
    print("\n" + "=" * 60)
    print("SUMMARY")
    print("=" * 60)

    print("\n[Simple Table]")
    print(f"{'Test':<25} {'avg':>10} {'p50':>10} {'p95':>10} {'stddev':>10}")
    print("-" * 65)
    for r in simple_results:
        if "error" not in r:
            print(f"{r['name']:<25} {r['avg']:>10.1f} {r['p50']:>10.1f} {r['p95']:>10.1f} {r['stddev']:>10.1f}")

    print("\n[Complex Table]")
    print(f"{'Test':<25} {'avg':>10} {'p50':>10} {'p95':>10} {'stddev':>10}")
    print("-" * 65)
    for r in complex_results:
        if "error" not in r:
            print(f"{r['name']:<25} {r['avg']:>10.1f} {r['p50']:>10.1f} {r['p95']:>10.1f} {r['stddev']:>10.1f}")

def save_json(simple_results, complex_results, output_file):
    """保存JSON结果"""
    result = {
        "simple": simple_results,
        "complex": complex_results,
        "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "config": {
            "iterations": DEFAULT_ITERATIONS,
            "warmup": DEFAULT_WARMUP,
            "outlier_trim": OUTLIER_TRIM
        }
    }

    with open(output_file, 'w') as f:
        json.dump(result, f, indent=2)

    print(f"\nResults saved to: {output_file}")

# ============================================================================
# 主程序
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="MongoDB Stability Benchmark v7")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="MongoDB port")
    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS, help="Test iterations")
    parser.add_argument("--warmup", type=int, default=DEFAULT_WARMUP, help="Warmup iterations")
    parser.add_argument("--output", default="/tmp/stability_v7_result.json", help="Output JSON file")
    args = parser.parse_args()

    iterations = args.iterations
    warmup = args.warmup

    print("=" * 60)
    print("Stability Benchmark v7 - F-Scheme E2E Test")
    print("=" * 60)
    print(f"Target: {DEFAULT_HOST}:{args.port}")
    print(f"Iterations: {args.iterations}, Warmup: {args.warmup}")
    print(f"Outlier trim: {OUTLIER_TRIM * 100}%")
    print()

    # 连接
    print("Connecting...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((DEFAULT_HOST, args.port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    # 测试连接
    result = run_cmd(sock, "admin", {"ping": 1})
    if result.get("ok") != 1:
        print("ERROR: Failed to ping server")
        return
    print("Connected successfully\n")

    # === Simple 表测试 ===
    print("[Phase 1] Simple Table Tests (12 tests)")
    print("-" * 40)
    f_scheme_reset(sock, DEFAULT_DB)
    setup_simple_collection(sock, DEFAULT_DB)
    print()
    simple_results = run_all_tests(sock, DEFAULT_DB, "simple_docs", SIMPLE_TESTS, args.iterations, args.warmup)

    # === Complex 表测试 ===
    print("\n[Phase 2] Complex Table Tests (15 tests)")
    print("-" * 40)
    f_scheme_reset(sock, DEFAULT_DB)
    setup_complex_collection(sock, DEFAULT_DB)
    print()
    complex_results = run_all_tests(sock, DEFAULT_DB, "complex_docs", COMPLEX_TESTS, args.iterations, args.warmup)

    # 输出结果
    print_summary(simple_results, complex_results)
    save_json(simple_results, complex_results, args.output)

    sock.close()
    print("\nBenchmark completed!")

if __name__ == "__main__":
    main()
