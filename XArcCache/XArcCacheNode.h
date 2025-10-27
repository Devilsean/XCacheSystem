#pragma once

#include <memory>

namespace XCache
{
    template <typename Key, typename Value>
    class ArcNode
    {
    private:
        Key key;
        Value value;
        size_t accessCount; // 访问频率
        std::weak_ptr<ArcNode> prev;
        std::shared_ptr<ArcNode> next;

    public:
        ArcNode() : accessCount(1), next(nullptr) {}
        ArcNode(Key k, Value v) : key(k), value(v), accessCount(1), next(nullptr) {}

        Key getKey() const { return key; }
        Value getValue() const { return value; }
        void setValue(Value v) { value = v; }
        void incrementAccessCount() { accessCount++; }
        size_t getAccessCount() const { return accessCount; }

        ~ArcNode() = default;

        template <typename K, typename V>
        friend class XArcLFUpart;
        template <typename K, typename V>
        friend class XArcLRUpart;
    };
} // namespace XCache