#pragma once

#include <mutex>
#include <unordered_map>
#include <map>

#include "XArcCacheNode.h"

namespace XCache
{
    template <typename Key, typename Value>
    class XArcLFUpart
    {
        using NodeType = ArcNode<Key, Value>;//ARC算法节点
        using NodePtr = std::shared_ptr<NodeType>;//指向ARC算法节点的智能指针
        using NodeMap = std::unordered_map<Key, NodePtr>;//存储智能指针的哈希表
        using FreqMap = std::map<size_t, std::list<NodePtr>>;//存储频率到节点列表的映射

    public:
        explicit XArcLFUpart(size_t capacity, size_t transformThreshold)
            : capacity(capacity), ghostCapacity(capacity),
              transformThreshold(transformThreshold), minFreq(0)
        {
            initializeList();
        }

        ~XArcLFUpart() = default;

        bool put(Key key, Value value)
        {
            if (capacity == 0)
                return false;
            std::lock_guard<std::mutex> lock(mtx);//可能需要修改数据，需要加锁
            auto it = mainCache.find(key);
            if (it != mainCache.end())
            {
                return updateExistingNode(it->second, value);//更新已存在节点的值
            }
            return addNewNode(key, value);
        }

        bool get(Key key, Value &value)
        {
            std::lock_guard<std::mutex> lock(mtx);//可能需要修改数据，需要加锁
            auto it = mainCache.find(key);
            if (it != mainCache.end())
            {
                updateNodeFreq(it->second);//更新节点的访问频率
                value = it->second->value;
                return true;
            }
            return false;
        }

        bool contain(Key key)
        {
            return mainCache.find(key) != mainCache.end();
        }

        bool checkGhost(Key key) // 检查并删除幽灵缓存中的节点
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

        void increaseCapacity()
        {
            capacity++;
        }

        bool decreaseCapacity()
        {
            if (capacity <= 0)
                return false;
            if (mainCache.size() == capacity) // 如果主缓存已满，则尝试从幽灵缓存中移除节点
            {
                evictLeastFrequentNode();//从幽灵缓存中移除最近最少访问频率的节点
            }
            --capacity;
            return true;
        }

    private:
        void initializeList()
        {
            ghostHead = std::make_shared<NodeType>();
            ghostTail = std::make_shared<NodeType>();
            ghostHead->next = ghostTail;
            ghostTail->prev = ghostHead;
        }

        bool updateExistingNode(NodePtr node, const Value &value) // 更新已存在节点的值
        {
            node->setValue(value);
            updateNodeFreq(node);
            return true;
        }

        void addToGhost(NodePtr node) // 将节点添加到幽灵缓存中
        {
            node->prev = ghostTail->prev;
            node->next = ghostTail;
            if (auto prevNode = node->prev.lock())
            {
                prevNode->next = node;
            }
            ghostTail->prev = node;
            ghostCache[node->getKey()] = node;
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

        void updateNodeFreq(NodePtr node) // 更新节点频率
        {
            size_t oldFreq = node->getAccessCount();
            node->incrementAccessCount();
            size_t newFreq = node->getAccessCount();
            // 从旧频率列表中移除节点
            auto &oldList = freqMap[oldFreq];
            oldList.remove(node);
            if (oldList.empty())
            {
                freqMap.erase(oldFreq);
                if (minFreq == oldFreq)
                {
                    minFreq = newFreq;
                }
            }
            // 添加到新的频率列表
            if (freqMap.find(newFreq) == freqMap.end())
            {
                freqMap[newFreq] = std::list<NodePtr>();
            }
            freqMap[newFreq].push_back(node);
        }

        void evictLeastFrequentNode() // 移除最小频率节点
        {
            if (freqMap.empty())
                return;
            // 获取最小频率列表
            auto &minFreqList = freqMap[minFreq];
            if (minFreqList.empty())
                return;
            // 获取最小频率列表中的第一个节点
            NodePtr node = minFreqList.front();
            minFreqList.pop_front();
            // 如果最小频率列表为空，则从频率映射中移除
            if (minFreqList.empty())
            {
                freqMap.erase(minFreq);
                if (!freqMap.empty())
                {
                    minFreq = freqMap.begin()->first;
                }
            }
            // 将节点移动到幽灵缓存
            if (ghostCache.size() >= ghostCapacity)
            {
                removeOldestGhost();
            }
            addToGhost(node);
            // 从主缓存中移除节点
            mainCache.erase(node->getKey());
        }

        void removeOldestGhost() // 移除最旧的幽灵节点
        {
            NodePtr node = ghostHead->next;
            if (node != ghostTail)
            {
                removeFromGhost(node);
                ghostCache.erase(node->getKey());
            }
        }

        bool addNewNode(const Key &key, const Value &value)
        {
            if (mainCache.size() >= capacity)
            {
                evictLeastFrequentNode();
            }
            NodePtr node = std::make_shared<NodeType>(key, value);
            mainCache[key] = node;
            if (mainCache.find(1) == mainCache.end())
            {
                freqMap[1] = std::list<NodePtr>();
            }
            freqMap[1].push_back(node);
            minFreq = 1;
            return true;
        }

    private:
        size_t capacity;           // 主缓存容量
        size_t ghostCapacity;      // 幽灵缓存容量
        size_t transformThreshold; // 转换阈值
        size_t minFreq;            // 最小频率
        std::mutex mtx;

        NodeMap mainCache;  // 主缓存
        NodeMap ghostCache; // 幽灵缓存
        FreqMap freqMap;    // 频率到节点的映射

        NodePtr ghostHead; // 幽灵链表头
        NodePtr ghostTail; // 幽灵链表尾
    };
}