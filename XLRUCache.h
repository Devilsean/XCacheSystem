#pragma once

#include <cmath>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "XCachePolicy.h"

namespace XCache {
template <typename Key, typename Value> class XLRUCache;

template <typename Key, typename Value> class LRUNode {
private:
  Key key;
  Value value;
  size_t accesscount;
  std::weak_ptr<LRUNode<Key, Value>> prev;
  std::shared_ptr<LRUNode<Key, Value>> next;

public:
  LRUNode(Key k, Value v) : key(k), value(v), accesscount(1) {}

  Key getKey() const { return key; }
  Value getValue() const { return value; }
  size_t getAccessCount() const { return accesscount; }

  void setValue(const Value &v) { value = v; }
  void incrementAccessCount() { accesscount++; }
  ~LRUNode() = default;

  friend class XLRUCache<Key, Value>;
};

template <typename Key, typename Value>
class XLRUCache : public XCachePolicy<Key, Value> {
  using LRUNodeType = LRUNode<Key, Value>;
  using NodePtr = std::shared_ptr<LRUNodeType>;
  using NodeMap = std::unordered_map<Key, NodePtr>;

public:
  XLRUCache(int capacity) : capacity(capacity) { initializeList(); }

  ~XLRUCache() override = default;

  void put(Key key, Value value) override {
    if (capacity <= 0)
      return;
    std::lock_guard<std::mutex> lock(mtx);
    auto it = nodeMap.find(key);
    if (it != nodeMap.end()) {
      updateExistingNode(it->second, value);
      return;
    }
    addNewNode(key, value);
  }

  bool get(Key key, Value &value) override {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = nodeMap.find(key);
    if (it != nodeMap.end()) {
      moveToMostRecent(it->second);
      value = it->second->getValue();
      return true;
    }
    return false;
  }

  Value get(Key key) override {
    Value v{}; // 值初始化，避免找不到值的时候返回垃圾值
    get(key, v);
    return v;
  }

  void remove(Key key) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = nodeMap.find(key);
    if (it != nodeMap.end()) {
      removeNode(it->second);
      nodeMap.erase(it);
    }
  }

  size_t size() {
    std::lock_guard<std::mutex> lock(mtx);
    return nodeMap.size();
  }

  Key getOldestKey() {
    std::lock_guard<std::mutex> lock(mtx);
    if (dummyHead->next && dummyHead->next != dummyTail) {
      return dummyHead->next->getKey();
    }
    return Key{};
  }

private:
  void initializeList() {
    dummyHead = std::make_shared<LRUNodeType>(Key(), Value());
    dummyTail = std::make_shared<LRUNodeType>(Key(), Value());
    dummyHead->next = dummyTail;
    dummyTail->prev = dummyHead;
  }

  void updateExistingNode(NodePtr node, const Value &value) {
    node->setValue(value);
    moveToMostRecent(node);
  }

  void addNewNode(Key key, const Value &value) {
    if (nodeMap.size() >= capacity) {
      evictLeastRecent();
    }
    NodePtr newNode = std::make_shared<LRUNodeType>(key, value);
    insertNode(newNode);
    nodeMap[key] = newNode;
  }

  void moveToMostRecent(NodePtr node) {
    removeNode(node);
    insertNode(node);
  }

  void insertNode(NodePtr node) {
    node->prev = dummyTail->prev;
    node->next = dummyTail;
    dummyTail->prev.lock()->next = node;
    dummyTail->prev = node;
  }

  void removeNode(NodePtr node) {
    if (!node->prev.expired() && node->next) {
      node->prev.lock()->next = node->next;
      node->next->prev = node->prev;
      node->next = nullptr;
    }
  }

  void evictLeastRecent() {
    NodePtr node = dummyHead->next;
    removeNode(node);
    nodeMap.erase(node->getKey());
  }

  int capacity;
  NodeMap nodeMap;
  std::mutex mtx;
  NodePtr dummyHead;
  NodePtr dummyTail;
};

template <typename Key, typename Value>
class XLRUKCache
    : public XLRUCache<
          Key,
          Value> // LRU优化的K版本，只有在历史访问次数达到K次时，才会将节点移动到主缓存中
{
public:
  XLRUKCache(int capacity, int _k = 2, double historyRatio = 2.5)
      : XLRUCache<Key, Value>(capacity), // 初始化主缓存的容量
        historyList(std::make_unique<XLRUCache<Key, size_t>>(static_cast<int>(
            capacity * historyRatio))), // 自动设置历史缓存为容量的2.5倍
        k(_k) {}

  ~XLRUKCache() = default;

  Value get(Key key) {
    Value value{}; // 值初始化，避免找不到值的时候返回垃圾值
    bool inMainCache = XLRUCache<Key, Value>::get(key, value);

    size_t historycount = historyList->get(key);
    historycount++;
    historyList->put(key, historycount);

    if (inMainCache) {
      return value;
    }

    if (historycount >= k) // 如果历史访问次数达到K次，将节点移动到主缓存中
    {
      std::lock_guard<std::mutex> lock(historyMtx);
      auto it = historyMap.find(key);
      if (it != historyMap.end()) {
        Value storedValue = it->second;
        historyList->remove(key);
        historyMap.erase(it);
        XLRUCache<Key, Value>::put(key, storedValue);
        return storedValue;
      }
    }

    return Value{};
  }

  void put(Key key, Value value) override {
    Value existingValue{};
    bool inMainCache = XLRUCache<Key, Value>::get(key, existingValue);
    if (inMainCache) {
      XLRUCache<Key, Value>::put(key, value);
      return;
    }

    size_t historyCount = historyList->get(key);
    historyCount++;

    // 存储访问次数，value需要另外存储
    historyList->put(key, historyCount);

    {
      std::lock_guard<std::mutex> lock(historyMtx);
      historyMap[key] = value;
    }

    if (historyCount >= k) {
      historyList->remove(key);
      {
        std::lock_guard<std::mutex> lock(historyMtx);
        auto it = historyMap.find(key);
        if (it != historyMap.end()) {
          Value storedValue = it->second;
          historyMap.erase(it);
          XLRUCache<Key, Value>::put(key, storedValue);
        }
      }
    }
  }

private:
  int k;
  std::unique_ptr<XLRUCache<Key, size_t>> historyList;
  std::unordered_map<Key, Value> historyMap;
  std::mutex historyMtx; // 为historyMap添加独立的互斥锁
};

template <typename Key, typename Value>
class XHashLRUCaches // 对LRU进行分片操作，提高高并发使用的性能
{
public:
  XHashLRUCaches(int cacheSize, int sliceNum)
      : cacheSize(cacheSize), sliceNum(sliceNum) {
    size_t sliceCapacity = std::ceil(cacheSize / static_cast<double>(sliceNum));
    for (int i = 0; i < sliceNum; ++i) {
      sliceCaches.emplace_back(new XLRUCache<Key, Value>(sliceCapacity));
    }
  }

  ~XHashLRUCaches() = default;

  void put(Key key, Value value) {
    size_t sliceIndex = Hash(key) % sliceNum;
    sliceCaches[sliceIndex]->put(key, value);
  }

  bool get(Key key, Value &value) {
    size_t sliceIndex = Hash(key) % sliceNum;
    return sliceCaches[sliceIndex]->get(key, value);
  }

  Value get(Key key) {
    Value value{}; // 值初始化，避免找不到值的时候返回垃圾值
    get(key, value);
    return value;
  }

private:
  size_t Hash(Key key) // 对key进行哈希，得到一个哈希值
  {
    std::hash<Key> hf;
    return hf(key);
  }

private:
  size_t cacheSize; // 总容量
  int sliceNum;     // 切片数量
  std::vector<std::unique_ptr<XLRUCache<Key, Value>>> sliceCaches; // 切片缓存
};
} // namespace XCache