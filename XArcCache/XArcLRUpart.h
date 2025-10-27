#pragma once

#include <unordered_map>
#include <mutex>
#include <memory>
#include "XArcCacheNode.h"

namespace XCache
{
    template <typename Key, typename Value>
    class XArcLRUpart
    {
        using NodeType = ArcNode<Key, Value>;
        using NodePtr = std::shared_ptr<NodeType>;
        using NodeMap = std::unordered_map<Key, NodePtr>;

    public:
        explicit XArcLRUpart(size_t capacity, size_t transformThreshold)
            : capacity(capacity), ghostCapacity(capacity), transformThreshold(transformThreshold)
        {
            initializeList();
        }

        ~XArcLRUpart() = default;

        bool put(Key key, Value value) // 向主缓存中添加或更新节点
        {
            if (capacity == 0)
                return false;
            std::lock_guard<std::mutex> lock(mtx);
            auto it = mainCache.find(key);
            if (it != mainCache.end())
            {
                return updateExistingNode(it->second, value);
            }
            return addNewNode(key, value);
        }

        bool get(Key key, Value &value, bool &shouldTransform) // 从主缓存中获取指定键对应的值及判断是否需要转换
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = mainCache.find(key);
            if (it != mainCache.end())
            {
                shouldTransform = updateNodeAccess(it->second);
                value = it->second->getValue();
                return true;
            }
            return false;
        }

        bool checkGhost(Key key) // 检查幽灵缓存中是否存在指定的节点并移除
        {
            auto it = ghostCache.find(key);
            if (it != ghostCache.end())
            {
                removeFromGhost(it->second);
                ghostCache.erase(it);
                return true;
            }
            return false;
        }

        void increaseCapacity() { capacity++; }

        bool decreaseCapacity()
        {
            if (capacity <= 0)
                return false;
            if (mainCache.size() == capacity) // 如果主缓存已满，移除最近最少使用的节点
            {
                evictLeastRecent();
            }
            --capacity;
            return true;
        }

    private:
        size_t capacity;           // 缓存容量
        size_t ghostCapacity;      // 幽灵缓存容量
        size_t transformThreshold; // 转换阈值
        std::mutex mtx;

        NodeMap mainCache;  // 主缓存
        NodeMap ghostCache; // 幽灵缓存

        NodePtr mainHead;
        NodePtr mainTail;

        NodePtr ghostHead;
        NodePtr ghostTail;

        void initializeList()
        {
            mainHead = std::make_shared<NodeType>();
            mainTail = std::make_shared<NodeType>();
            mainHead->next = mainTail;
            mainTail->prev = mainHead;
            ghostHead = std::make_shared<NodeType>();
            ghostTail = std::make_shared<NodeType>();
            ghostHead->next = ghostTail;
            ghostTail->prev = ghostHead;
        }

        bool updateExistingNode(NodePtr node, const Value &value) // 更新主缓存中已存在节点的值
        {
            node->setValue(value);
            moveToFront(node);
            return true;
        }

        bool updateNodeAccess(NodePtr node) // 更新节点的状态并判断节点是否达到转换阈值
        {
            moveToFront(node);
            node->incrementAccessCount();
            return node->getAccessCount() >= transformThreshold;
        }

        bool addNewNode(const Key &key, const Value &value) // 向主缓存中添加新节点
        {
            // 若缓存以满，则移除最近最少使用的节点
            if (mainCache.size() >= capacity)
            {
                evictLeastRecent();
            }
            // 创建新节点并添加到主缓存
            NodePtr newNode = std::make_shared<NodeType>(key, value);
            mainCache[key] = newNode;
            addToFront(newNode);
            return true;
        }

        void evictLeastRecent() // 从主缓存中移除最近最少使用的节点
        {
            NodePtr leastUseNode = mainTail->prev.lock();
            if (!leastUseNode || leastUseNode == mainTail)
            {
                return;
            }
            // 从主链表中移除
            removeFromMain(leastUseNode);
            // 添加到幽灵缓存
            if (ghostCache.size() >= ghostCapacity)
            {
                removeOldestGhost();
            }
            addToGhost(leastUseNode);
            // 从主缓存映射中移除
            mainCache.erase(leastUseNode->getKey());
        }

        void moveToFront(NodePtr node) // 将节点移动到主缓存的前端
        {
            // 从当前位置移除节点
            if (!node->prev.expired() && node->next)
            {
                auto prevNode = node->prev.lock();
                prevNode->next = node->next;
                node->next->prev = prevNode;
                node->next = nullptr;
            }
            // 将节点添加到主缓存前端
            addToFront(node);
        }

        void addToFront(NodePtr node) // 将节点添加到主缓存的前端
        {
            node->prev = mainHead;
            node->next = mainHead->next;
            mainHead->next->prev = node;
            mainHead->next = node;
        }

        void removeFromMain(NodePtr node) // 从主缓存中移除节点
        {
            if (auto prevNode = node->prev.lock(); prevNode && node->next)
            {
                prevNode->next = node->next;
                node->next->prev = prevNode;
                node->next = nullptr;
            }
        }

        void addToGhost(NodePtr node) // 将节点添加到幽灵缓存
        {
            node->accessCount = 1; // 重置访问计数
            node->prev = ghostHead;
            node->next = ghostHead->next;
            ghostHead->next->prev = node;
            ghostHead->next = node;
            ghostCache[node->getKey()] = node;
        }

        void removeOldestGhost() // 从幽灵缓存中移除最旧的节点
        {
            NodePtr oldestGhost = ghostTail->prev.lock();
            if (!oldestGhost || oldestGhost == ghostTail)
            {
                return;
            }
            // 从幽灵链表中移除
            removeFromGhost(oldestGhost);
            // 从幽灵缓存映射中移除
            ghostCache.erase(oldestGhost->getKey());
        }

        void removeFromGhost(NodePtr node) // 从幽灵缓存中移除节点
        {
            if (auto prevNode = node->prev.lock(); prevNode && node->next)
            {
                prevNode->next = node->next;
                node->next->prev = prevNode;
                node->next = nullptr;
            }
        }
    };
}