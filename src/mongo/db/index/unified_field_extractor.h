/**
 * unified_field_extractor.h
 *
 * 统一字段提取器 - 优化多索引+摘要场景的字段提取性能
 *
 * 问题背景:
 *   - 文档: 70个字段
 *   - 索引: 7个索引 × 10个字段 = 70次访问
 *   - 摘要: 40个字段
 *   - 原始方法: 每次访问都遍历文档，总计110次遍历
 *
 * 优化方案:
 *   - 预注册所有需要的字段（索引+摘要），自动去重
 *   - 一次遍历文档，使用4字节签名快速匹配
 *   - 匹配的字段存入固定槽位，后续O(1)访问
 *
 * 性能提升:
 *   - 遍历次数: 110次 → 1次
 *   - 字段访问: O(N)字符串比较 → O(1)槽位索引
 *   - 预计: 7000ns → 300ns (23x提升)
 */

#pragma once

#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/bson/dotted_path_support.h"

namespace mongo {

/**
 * 统一字段提取器
 *
 * 使用流程:
 * 1. 初始化阶段（启动时一次）:
 *    - registerIndex() 注册索引字段
 *    - registerDigest() 注册摘要字段
 *    - finalize() 完成注册
 *
 * 2. 提取阶段（每个文档）:
 *    - extract(doc) 一次遍历提取所有字段
 *    - get(slot) O(1)访问字段
 *
 * 线程安全: 非线程安全，每个线程应有独立实例
 */
class UnifiedFieldExtractor {
public:
    // 最大支持字段数
    static const size_t kMaxFields = 256;

    // 无效槽位标记
    static const uint8_t kInvalidSlot = 255;

    UnifiedFieldExtractor() = default;

    // ========================================================================
    // 注册阶段 API
    // ========================================================================

    /**
     * 注册单个字段
     *
     * @param fieldPath 字段路径（支持嵌套如 "a.b.c"）
     * @return 槽位ID，用于后续O(1)访问
     *
     * 重复注册同一字段返回相同槽位（自动去重）
     */
    uint8_t registerField(const StringData& fieldPath) {
        if (_finalized) {
            return kInvalidSlot;
        }

        uint32_t sig = makeSignature(fieldPath.rawData(), fieldPath.size());

        // 检查是否已注册
        auto it = _sigToSlot.find(sig);
        if (it != _sigToSlot.end()) {
            // 验证不是哈希冲突
            if (_fields[it->second] == fieldPath) {
                return it->second;
            }
            // 哈希冲突，使用全字符串匹配
            for (size_t i = 0; i < _fields.size(); ++i) {
                if (_fields[i] == fieldPath) {
                    return static_cast<uint8_t>(i);
                }
            }
        }

        // 检查是否已在冲突列表中
        auto collIt = _collisionSlots.find(sig);
        if (collIt != _collisionSlots.end()) {
            for (uint8_t slot : collIt->second) {
                if (_fields[slot] == fieldPath) {
                    return slot;  // 已注册
                }
            }
        }

        // 新字段
        if (_fields.size() >= kMaxFields - 1) {
            return kInvalidSlot;  // 超出容量
        }

        uint8_t slot = static_cast<uint8_t>(_fields.size());
        _fields.push_back(fieldPath.toString());

        // 如果已存在同签名的字段，存入冲突列表，不覆盖 _sigToSlot
        if (it != _sigToSlot.end()) {
            _collisionSlots[sig].push_back(slot);
        } else {
            _sigToSlot[sig] = slot;
        }

        // 分类：顶级字段 vs 嵌套路径
        size_t dotPos = fieldPath.find('.');
        if (dotPos == std::string::npos) {
            _topLevelSlots.push_back(slot);
            _isNested.push_back(false);
        } else {
            _nestedSlots.push_back(slot);
            _isNested.push_back(true);
            // 提取顶级前缀 - 存储到 _nestedPrefixes (与 _nestedSlots 并行)
            _nestedPrefixes.push_back(fieldPath.substr(0, dotPos).toString());
        }

        return slot;
    }

    /**
     * 批量注册索引字段
     *
     * @param indexName 索引名称（用于后续按名称获取）
     * @param fields 字段列表
     * @return 槽位ID列表
     */
    std::vector<uint8_t> registerIndex(const std::string& indexName,
                                        const std::vector<std::string>& fields) {
        std::vector<uint8_t> slots;
        slots.reserve(fields.size());

        for (const auto& f : fields) {
            uint8_t slot = registerField(f);
            if (slot != kInvalidSlot) {
                slots.push_back(slot);
            }
        }

        _indexSlots[indexName] = slots;
        return slots;
    }

    /**
     * 批量注册摘要字段
     *
     * @param digestName 摘要名称
     * @param fields 字段列表
     * @return 槽位ID列表
     */
    std::vector<uint8_t> registerDigest(const std::string& digestName,
                                         const std::vector<std::string>& fields) {
        std::vector<uint8_t> slots;
        slots.reserve(fields.size());

        for (const auto& f : fields) {
            uint8_t slot = registerField(f);
            if (slot != kInvalidSlot) {
                slots.push_back(slot);
            }
        }

        _digestSlots[digestName] = slots;
        return slots;
    }

    /**
     * 完成注册，准备提取
     * 调用后不能再注册新字段
     */
    void finalize() {
        _slots.resize(_fields.size());
        _hasArrayAlongPath.resize(_fields.size(), false);
        _finalized = true;

        // 预计算顶级字段的签名集合，用于快速判断
        for (uint8_t slot : _topLevelSlots) {
            uint32_t sig = makeSignature(_fields[slot].c_str(), _fields[slot].size());
            _topLevelSigs.insert(sig);
        }

        // 预计算嵌套字段的顶级前缀签名
        for (size_t i = 0; i < _nestedSlots.size(); ++i) {
            uint8_t slot = _nestedSlots[i];
            const std::string& prefix = _nestedPrefixes[i];  // Use index i, not slot
            uint32_t sig = makeSignature(prefix.c_str(), prefix.size());
            _nestedPrefixSigs[sig].push_back(slot);
        }
    }

    // ========================================================================
    // 提取阶段 API
    // ========================================================================

    /**
     * 一次遍历提取所有注册的字段
     *
     * @param doc 要提取的文档
     *
     * 时间复杂度: O(N) 其中N是文档字段数
     */
    void extract(const BSONObj& doc) {
        // 清空槽位
        std::fill(_slots.begin(), _slots.end(), BSONElement());
        std::fill(_hasArrayAlongPath.begin(), _hasArrayAlongPath.end(), false);
        _doc = doc;
        _extractedCount = 0;

        // 一次遍历文档
        for (BSONObjIterator it(doc); it.more(); ) {
            BSONElement elem = it.next();
            const char* fieldName = elem.fieldName();
            size_t fieldNameLen = elem.fieldNameSize() - 1;  // 不含'\0'
            uint32_t sig = makeSignature(fieldName, fieldNameLen);

            // 检查是否是需要的顶级字段（快速路径）
            auto slotIt = _sigToSlot.find(sig);
            if (slotIt != _sigToSlot.end()) {
                uint8_t slot = slotIt->second;
                // 验证字段名匹配（防止签名冲突）
                if (!_isNested[slot] &&
                    _fields[slot].size() == fieldNameLen &&
                    std::memcmp(_fields[slot].c_str(), fieldName, fieldNameLen) == 0) {
                    _slots[slot] = elem;
                    ++_extractedCount;
                }
            }

            // 检查冲突列表中的顶级字段（仅在有冲突时触发）
            auto collIt = _collisionSlots.find(sig);
            if (collIt != _collisionSlots.end()) {
                for (uint8_t slot : collIt->second) {
                    if (!_isNested[slot] &&
                        _fields[slot].size() == fieldNameLen &&
                        std::memcmp(_fields[slot].c_str(), fieldName, fieldNameLen) == 0) {
                        _slots[slot] = elem;
                        ++_extractedCount;
                        break;
                    }
                }
            }

            // 检查是否是嵌套字段的顶级前缀
            if (elem.type() == Object || elem.type() == Array) {
                auto prefixIt = _nestedPrefixSigs.find(sig);
                if (prefixIt != _nestedPrefixSigs.end()) {
                    // 提取嵌套字段
                    for (uint8_t slot : prefixIt->second) {
                        if (_slots[slot].eoo()) {
                            const std::string& fullPath = _fields[slot];
                            size_t dotPos = fullPath.find('.');
                            const char* subPath = fullPath.c_str() + dotPos + 1;

                            if (elem.type() == Object) {
                                // 对象类型：使用 extractElementAtPathOrArrayAlongPath 处理嵌套数组
                                BSONObj subObj = elem.Obj();
                                BSONElement result = dotted_path_support::extractElementAtPathOrArrayAlongPath(
                                    subObj, subPath);
                                _slots[slot] = result;
                                // 如果 subPath 被修改（不为空），说明遇到了数组，标记为需要特殊处理
                                if (*subPath != '\0') {
                                    _hasArrayAlongPath[slot] = true;
                                }
                            } else {
                                // 数组类型：直接返回数组元素，让调用者处理 multikey
                                _slots[slot] = elem;
                                _hasArrayAlongPath[slot] = true;
                            }

                            if (!_slots[slot].eoo()) {
                                ++_extractedCount;
                            }
                        }
                    }
                }
            }
        }
    }

    // ========================================================================
    // 访问阶段 API
    // ========================================================================

    /**
     * O(1) 按槽位访问字段
     *
     * @param slot 槽位ID（registerField返回值）
     * @return BSONElement，未找到返回eoo
     */
    BSONElement get(uint8_t slot) const {
        if (slot >= _slots.size()) {
            return BSONElement();
        }
        return _slots[slot];
    }

    /**
     * 获取索引的所有字段值
     *
     * @param indexName 索引名称
     * @param out 输出向量
     */
    void getIndexFields(const std::string& indexName,
                        std::vector<BSONElement>& out) const {
        auto it = _indexSlots.find(indexName);
        if (it == _indexSlots.end()) {
            out.clear();
            return;
        }

        const auto& slots = it->second;
        out.clear();
        out.reserve(slots.size());
        for (uint8_t s : slots) {
            out.push_back(_slots[s]);
        }
    }

    /**
     * 获取摘要的所有字段值
     *
     * @param digestName 摘要名称
     * @param out 输出向量
     */
    void getDigestFields(const std::string& digestName,
                         std::vector<BSONElement>& out) const {
        auto it = _digestSlots.find(digestName);
        if (it == _digestSlots.end()) {
            out.clear();
            return;
        }

        const auto& slots = it->second;
        out.clear();
        out.reserve(slots.size());
        for (uint8_t s : slots) {
            out.push_back(_slots[s]);
        }
    }

    /**
     * 获取索引的槽位列表
     */
    const std::vector<uint8_t>* getIndexSlots(const std::string& indexName) const {
        auto it = _indexSlots.find(indexName);
        return (it != _indexSlots.end()) ? &it->second : nullptr;
    }

    /**
     * 获取摘要的槽位列表
     */
    const std::vector<uint8_t>* getDigestSlots(const std::string& digestName) const {
        auto it = _digestSlots.find(digestName);
        return (it != _digestSlots.end()) ? &it->second : nullptr;
    }

    // ========================================================================
    // 统计信息
    // ========================================================================

    /** 总唯一字段数 */
    size_t totalUniqueFields() const { return _fields.size(); }

    /** 顶级字段数 */
    size_t topLevelCount() const { return _topLevelSlots.size(); }

    /** 嵌套字段数 */
    size_t nestedCount() const { return _nestedSlots.size(); }

    /** 已提取字段数（最近一次extract） */
    size_t extractedCount() const { return _extractedCount; }

    /** 索引数量 */
    size_t indexCount() const { return _indexSlots.size(); }

    /** 摘要数量 */
    size_t digestCount() const { return _digestSlots.size(); }

    /** 获取字段名 */
    const std::string& getFieldName(uint8_t slot) const {
        static const std::string empty;
        return (slot < _fields.size()) ? _fields[slot] : empty;
    }

    /** 是否已完成注册 */
    bool isFinalized() const { return _finalized; }

    /**
     * 检查指定槽位的字段路径中是否遇到了数组
     * 如果为 true，表示需要 multikey 处理
     */
    bool hasArrayAlongPath(uint8_t slot) const {
        return slot < _hasArrayAlongPath.size() && _hasArrayAlongPath[slot];
    }

    /** 冲突字段数量 */
    size_t collisionCount() const {
        size_t count = 0;
        for (const auto& pair : _collisionSlots) {
            count += pair.second.size();
        }
        return count;
    }

private:
    /**
     * 计算字段名签名
     *
     * 签名 = 长度(8bit) + 首字符(8bit) + 末字符(8bit) + 哈希(8bit)
     * 4字节，可用单条CPU指令比较
     */
    static uint32_t makeSignature(const char* s, size_t len) {
        if (len == 0) return 0;

        uint8_t hash = 0;
        for (size_t i = 0; i < len; ++i) {
            hash = static_cast<uint8_t>(hash * 31 + static_cast<uint8_t>(s[i]));
        }

        return (static_cast<uint32_t>(len & 0xFF) << 24) |
               (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(s[len - 1])) << 8) |
               hash;
    }

private:
    // ===== 注册信息 =====
    std::unordered_map<uint32_t, uint8_t> _sigToSlot;   // 签名 → 槽位（快速路径）
    std::unordered_map<uint32_t, std::vector<uint8_t>> _collisionSlots;  // 签名冲突字段
    std::vector<std::string> _fields;                    // 槽位 → 字段名
    std::vector<bool> _isNested;                         // 槽位 → 是否嵌套
    std::vector<std::string> _nestedPrefixes;            // 嵌套字段的顶级前缀（与_nestedSlots并行）

    std::vector<uint8_t> _topLevelSlots;                 // 顶级字段槽位列表
    std::vector<uint8_t> _nestedSlots;                   // 嵌套字段槽位列表

    std::set<uint32_t> _topLevelSigs;                    // 顶级字段签名集合
    std::unordered_map<uint32_t, std::vector<uint8_t>> _nestedPrefixSigs;  // 前缀签名 → 嵌套槽位

    // ===== 索引/摘要映射 =====
    std::unordered_map<std::string, std::vector<uint8_t>> _indexSlots;
    std::unordered_map<std::string, std::vector<uint8_t>> _digestSlots;

    // ===== 提取结果 =====
    std::vector<BSONElement> _slots;
    std::vector<bool> _hasArrayAlongPath;                // 标记路径中是否遇到数组
    BSONObj _doc;
    size_t _extractedCount = 0;
    bool _finalized = false;
};

}  // namespace mongo
