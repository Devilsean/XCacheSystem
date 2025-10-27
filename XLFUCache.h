#pragma once

#include <mutex>
#include <vector>
#include <unordered_map>
#include <thread>
#include <memory>
#include <cmath>

#include "XCachePolicy.h"

namespace XCache
{
    template <typename Key, typename Value>
    class XLFUCache; // 这是一个向前声明，用于解决循环依赖问题

    template <typename Key, typename Value>
    class Freqlist
    {
    private:
        struct Node
        {
            Key key;
            Value value;
            int freq;
            std::weak_ptr<Node> prev; // 前一个节点的弱引用，避免循环引用
            std::shared_ptr<Node> next;
            Node() : freq(1), next(nullptr) {}                                 // 无参构造，初始化频率为1
            Node(Key k, Value v) : key(k), value(v), freq(1), next(nullptr) {} // 有参构造，传入键值对，初始化频率为1
        };

        using NodePtr = std::shared_ptr<Node>;
        int freq;
        NodePtr head;
        NodePtr tail;

    public:
        explicit Freqlist(int f) : freq(f) // 设置缓存队列
        {
            head = std::make_shared<Node>();
            tail = std::make_shared<Node>();
            head->next = tail;
            tail->prev = head;
        }

        bool isEmpty() const // 判断队列是否为空
        {
            return head->next == tail;
        }

        void addNode(NodePtr node) // 添加节点到队列尾部
        {
            if (!node || !head || !tail)
                return;
            node->prev = tail->prev;
            node->next = tail;
            tail->prev.lock()->next = node; // 需要使用lock()函数将弱引用转换为强引用
            tail->prev = node;
        }

        void removeNode(NodePtr node) // 从队列中移除节点
        {
            if (!node || !head || !tail)
                return;
            if (node->prev.expired() || !node->next)
                return;
            auto prev = node->prev.lock();
            prev->next = node->next;
            node->next->prev = prev;
            node->prev.reset();   // 清除弱引用
            node->next = nullptr; // 清除强引用
        }

        NodePtr getfirstNode() const // 获取频率列表的第一个节点
        {
            return head->next;
        }

        friend class XLFUCache<Key, Value>; // 声明友元类，以支持访问私有成员
    };

    template <typename Key, typename Value>
    class XLFUCache : public XCachePolicy<Key, Value>
    {
    public:
        using Node = typename Freqlist<Key, Value>::Node; //
        using NodePtr = std::shared_ptr<Node>;
        using NodeMap = std::unordered_map<Key, NodePtr>;

        XLFUCache(int capacity_, int maxAvgFreq = 1000000) : capacity(capacity_), maxAverageFreq(maxAvgFreq), curAverageFreq(0), curTotalFreq(0), minFreq(INT8_MAX) {}
        ~XLFUCache() override = default;
        void put(Key key, Value value) override
        {
            if (capacity == 0)
                return;
            std::lock_guard<std::mutex> lock(mtx);
            auto it = nodeMap.find(key);
            if (it != nodeMap.end())
            {
                it->second->value = value;
                putInternal(key, value);
                return;
            }
            putInternal(key, value);
        }

        bool get(Key key, Value &value) override
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = nodeMap.find(key);
            if (it != nodeMap.end())
            {
                getInternal(it->second, value);
                return true;
            }
            return false;
        }
        Value get(Key key) override
        {
            Value value;
            get(key, value);
            return value;
        }

        void purge()
        {
            nodeMap.clear();
            freqMap.clear();
        }

    private:
        void putInternal(Key key, Value value);       // 存入缓存
        void getInternal(NodePtr node, Value &value); // 从缓存中获取数据

        void kickout();                        // 移除缓存中的过期数据
        void addToFreqlist(NodePtr node);      // 将节点添加到频率列表中
        void removeFromFreqlist(NodePtr node); // 将节点从频率列表中移除

        void addFreqNum();             // 增加频率
        void decreaseFreqNum(int num); // 减少频率
        void HaddleOverMaxAvgFreq();   // 处理超过最大平均频率的情况
        void updateMinFreq();          // 更新最小频率

    private:
        int capacity;
        int minFreq;
        int maxAverageFreq;
        int curAverageFreq;
        int curTotalFreq;
        std::mutex mtx;
        NodeMap nodeMap; // key到节点的映射
        std::unordered_map<int, Freqlist<Key, Value> *> freqMap;
    };

    template <typename Key, typename Value>
    void XLFUCache<Key, Value>::getInternal(NodePtr node, Value &value)
    {
        value = node->value;
        removeFromFreqlist(node);
        node->freq++;
        addToFreqlist(node);
        if (node->freq - 1 == minFreq && freqMap[node->freq - 1]->isEmpty())
        {
            minFreq++;
        }
        addFreqNum();
    }

    template <typename Key, typename Value>
    void XLFUCache<Key, Value>::putInternal(Key key, Value value)
    {
        if (nodeMap.size() == capacity)
        {
            kickout();
        }
        NodePtr node = std::make_shared<Node>(key, value);
        nodeMap[key] = node;
        addToFreqlist(node);
        addFreqNum();
        minFreq = std::min(minFreq, 1);
    }

    template <typename Key, typename Value>
    void XLFUCache<Key, Value>::kickout()
    {
        NodePtr node = freqMap[minFreq]->getfirstNode();
        removeFromFreqlist(node);
        nodeMap.erase(node->key);
        decreaseFreqNum(node->freq);
    }

    template <typename Key, typename Value>
    void XLFUCache<Key, Value>::addToFreqlist(NodePtr node)
    {
        if (!node)
            return;
        if (freqMap.find(node->freq) == freqMap.end()) // 如果频率列表不存在，则创建一个新的频率列表
        {
            freqMap[node->freq] = new Freqlist<Key, Value>(node->freq);
        }
        freqMap[node->freq]->addNode(node); // 将节点添加到对应频率的频率列表中
    }

    template <typename Key, typename Value>
    void XLFUCache<Key, Value>::removeFromFreqlist(NodePtr node)
    {
        if (!node)
            return;
        freqMap[node->freq]->removeNode(node);
    }

    template <typename Key, typename Value>
    void XLFUCache<Key, Value>::addFreqNum()
    {
        curTotalFreq++;
        if (nodeMap.empty())
            curAverageFreq = 0;
        else
            curAverageFreq = curTotalFreq / nodeMap.size();
        if (curAverageFreq > maxAverageFreq)
            HaddleOverMaxAvgFreq();
    }

    template <typename Key, typename Value>
    void XLFUCache<Key, Value>::decreaseFreqNum(int num)
    {
        curTotalFreq -= num;
        if (nodeMap.size() == 0)
            curAverageFreq = 0;
        else
            curAverageFreq = curTotalFreq / nodeMap.size();
    }

    template <typename Key, typename Value>
    void XLFUCache<Key, Value>::HaddleOverMaxAvgFreq()
    {
        if (nodeMap.empty())
            return;
        for (auto it = nodeMap.begin(); it != nodeMap.end(); ++it)
        {
            if (!it->second)
                continue;
            NodePtr node = it->second;
            removeFromFreqlist(node);
            node->freq -= maxAverageFreq / 2;
            if (node->freq < 1)
                node->freq = 1;
            addToFreqlist(node);
        }
        updateMinFreq();
    }

    template <typename Key, typename Value>
    void XLFUCache<Key, Value>::updateMinFreq()
    {
        minFreq = INT8_MAX;
        for (const auto &pair : freqMap)
        {
            if (pair.second && !pair.second->isEmpty())
            {
                minFreq = std::min(minFreq, pair.first);
            }
        }
        if (minFreq == INT8_MAX)
            minFreq = 1;
    }
} // namespace XCache