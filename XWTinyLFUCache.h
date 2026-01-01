#pragma once

#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

#include "XCachePolicy.h"
#include "XLRUCache.h"

namespace XCache {
// Count-Min Sketch频率估算器
template <typename Key> class FrequencySketch {
private:
  struct Counter {
    uint8_t count;
    Counter() : count(0) {}
  };

  std::vector<std::vector<Counter>> counters;
  std::vector<std::hash<Key>> hashFunctions;
  std::vector<uint64_t> hashSeeds;
  int width;
  int depth;
  size_t sampleSize;
  std::mutex mtx;

public:
  FrequencySketch(int width = 256, int depth = 4, size_t sampleSize = 10000)
      : width(width), depth(depth), sampleSize(sampleSize) {
    counters.resize(depth, std::vector<Counter>(width));

    // 创建不同的哈希函数
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    for (int i = 0; i < depth; ++i) {
      hashFunctions.emplace_back();
      hashSeeds.push_back(dis(gen)); // 为每个哈希函数生成不同的种子
    }
  }

  void increment(Key key) {
    std::lock_guard<std::mutex> lock(mtx);
    for (int i = 0; i < depth; ++i) {
      size_t hash = hashFunctions[i](key) ^ hashSeeds[i]; // 使用种子增加随机性
      size_t index = (hash % width + width) % width;
      if (counters[i][index].count < 255) {
        counters[i][index].count++;
      }
    }
  }

  uint32_t frequency(Key key) {
    std::lock_guard<std::mutex> lock(mtx);
    uint32_t minCount = UINT32_MAX;

    for (int i = 0; i < depth; ++i) {
      size_t hash = hashFunctions[i](key) ^ hashSeeds[i]; // 使用种子增加随机性
      size_t index = (hash % width + width) % width;
      minCount =
          std::min(minCount, static_cast<uint32_t>(counters[i][index].count));
    }

    return minCount;
  }

  void decay() {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto &row : counters) {
      for (auto &counter : row) {
        if (counter.count > 0) {
          counter.count = counter.count / 2; // 减半衰减
        }
      }
    }
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto &row : counters) {
      for (auto &counter : row) {
        counter.count = 0;
      }
    }
  }

  size_t getSampleSize() const { return sampleSize; }
};

// W-TinyLFU主缓存实现
template <typename Key, typename Value>
class XWTinyLFUCache : public XCachePolicy<Key, Value> {
private:
  // Window Cache - 处理新访问的条目
  std::unique_ptr<XLRUCache<Key, Value>> windowCache;

  // Victim Cache - 存储较老的条目
  std::unique_ptr<XLRUCache<Key, Value>> victimCache;

  // 频率估算器
  std::unique_ptr<FrequencySketch<Key>> frequencySketch;

  // 配置参数
  size_t totalCapacity;
  size_t windowCapacity;
  size_t victimCapacity;
  double windowRatio;

  // 统计信息
  mutable std::mutex statsMutex;
  size_t accessCount = 0;
  size_t hitCount = 0;
  size_t windowHits = 0;
  size_t victimHits = 0;

  // W-TinyLFU 特有统计
  size_t admissionWins = 0;   // 新条目战胜旧条目的次数
  size_t admissionLosses = 0; // 新条目败给旧条目的次数
  size_t operationCount = 0;  // 用于触发衰减的操作计数

  // 主锁
  std::mutex mainMutex;

public:
  XWTinyLFUCache(size_t capacity, double windowRatio = 0.01)
      : totalCapacity(capacity), windowRatio(windowRatio) {
    windowCapacity = static_cast<size_t>(capacity * windowRatio);
    victimCapacity = capacity - windowCapacity;

    if (windowCapacity == 0)
      windowCapacity = 1;
    if (victimCapacity == 0)
      victimCapacity = capacity - 1;

    windowCache = std::make_unique<XLRUCache<Key, Value>>(windowCapacity);
    victimCache = std::make_unique<XLRUCache<Key, Value>>(victimCapacity);

    // Frequency Sketch的宽度约为总容量的4倍
    int sketchWidth = std::max(256, static_cast<int>(capacity * 4));
    frequencySketch =
        std::make_unique<FrequencySketch<Key>>(sketchWidth, 4, capacity);
  }

  ~XWTinyLFUCache() override = default;

  void put(Key key, Value value) override {
    if (totalCapacity == 0)
      return;

    std::lock_guard<std::mutex> lock(mainMutex);

    // 更新频率统计
    frequencySketch->increment(key);

    // 检查是否已在缓存中
    Value existingValue;
    bool inWindow = windowCache->get(key, existingValue);
    bool inVictim = false;

    if (!inWindow) {
      inVictim = victimCache->get(key, existingValue);
    }

    if (inWindow || inVictim) {
      // 更新现有值
      if (inWindow) {
        windowCache->put(key, value);
      } else {
        // 不要移到Window，直接更新Victim
        victimCache->put(key, value);
      }
      return;
    }

    // 新条目处理
    ensureWindowCapacity();
    windowCache->put(key, value);
  }

  bool get(Key key, Value &value) override {
    if (totalCapacity == 0)
      return false;

    std::lock_guard<std::mutex> lock(mainMutex);

    // 更新频率统计
    frequencySketch->increment(key);

    // 先在Window Cache中查找
    if (windowCache->get(key, value)) {
      updateStats(true, true);
      // 策略：留在Window，等它自然淘汰时再通过Admission进入Victim
      return true;
    }

    // 再在Victim Cache中查找
    if (victimCache->get(key, value)) {
      updateStats(true, false);
      // 不要移动到Window！
      // XLRUCache::get内部已经包含LRU提升逻辑（移到链表头部）
      // 所以这里什么都不用做，让热点数据安稳地待在Victim Cache里
      return true;
    }

    updateStats(false, false);
    return false;
  }

  Value get(Key key) override {
    Value value{};
    get(key, value);
    return value;
  }

  void remove(Key key) {
    std::lock_guard<std::mutex> lock(mainMutex);
    windowCache->remove(key);
    victimCache->remove(key);
  }

  // 获取统计信息
  double getHitRate() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return accessCount > 0 ? static_cast<double>(hitCount) / accessCount : 0.0;
  }

  double getWindowHitRate() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return hitCount > 0 ? static_cast<double>(windowHits) / hitCount : 0.0;
  }

  double getVictimHitRate() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return hitCount > 0 ? static_cast<double>(victimHits) / hitCount : 0.0;
  }

  size_t getAccessCount() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return accessCount;
  }

  size_t getWindowSize() const { return windowCapacity; }

  size_t getVictimSize() const { return victimCapacity; }

  // W-TinyLFU 特有统计信息
  double getAdmissionWinRate() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    size_t total = admissionWins + admissionLosses;
    return total > 0 ? static_cast<double>(admissionWins) / total : 0.0;
  }

  size_t getAdmissionWins() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return admissionWins;
  }

  size_t getAdmissionLosses() const {
    std::lock_guard<std::mutex> lock(statsMutex);
    return admissionLosses;
  }

  void resetStats() {
    std::lock_guard<std::mutex> lock(statsMutex);
    accessCount = 0;
    hitCount = 0;
    windowHits = 0;
    victimHits = 0;
    admissionWins = 0;
    admissionLosses = 0;
    operationCount = 0;
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mainMutex);
    windowCache = std::make_unique<XLRUCache<Key, Value>>(windowCapacity);
    victimCache = std::make_unique<XLRUCache<Key, Value>>(victimCapacity);
    frequencySketch->reset();
    resetStats();
  }

private:
  void ensureWindowCapacity() {
    // 如果Window Cache满了，将最老的条目移到Victim Cache
    if (windowCache->size() >= windowCapacity) {
      // 获取Window中最老的条目（LRU的头部）
      Key windowVictimKey = windowCache->getOldestKey();
      Value windowVictimValue;

      if (windowCache->get(windowVictimKey, windowVictimValue)) {
        windowCache->remove(windowVictimKey);

        // 确保Victim Cache有容量
        ensureVictimCapacity(windowVictimKey, windowVictimValue);
      }
    }
  }

  void ensureVictimCapacity(Key newKey, Value newValue) {
    operationCount++;

    // 定期衰减频率计数器（每1000次操作）
    if (operationCount % 1000 == 0) {
      frequencySketch->decay();
    }

    // 如果Victim Cache不满，直接添加
    if (victimCache->size() < victimCapacity) {
      victimCache->put(newKey, newValue);
      return;
    }

    // Victim Cache满了，需要执行admission policy
    // 获取Victim Cache中最老的条目作为候选淘汰者
    Key victimCandidateKey = victimCache->getOldestKey();
    Value victimCandidateValue;
    if (!victimCache->get(victimCandidateKey, victimCandidateValue)) {
      victimCache->put(newKey, newValue);
      return;
    }

    // 获取两个候选者的频率
    uint32_t newKeyFreq = frequencySketch->frequency(newKey);
    uint32_t victimFreq = frequencySketch->frequency(victimCandidateKey);

    // W-TinyLFU核心逻辑：频率比较
    if (newKeyFreq >= victimFreq) {
      // 新条目频率更高或相等，替换旧条目
      victimCache->remove(victimCandidateKey);
      victimCache->put(newKey, newValue);
      admissionWins++;
    } else {
      // 旧条目频率更高，拒绝新条目
      admissionLosses++;
      // 保留victimCandidateKey，不添加newKey
    }
  }

  // 简化的Victim Cache淘汰逻辑（仅用于直接清理）
  void evictLowestFrequencyFromVictim() {
    if (victimCache->size() == 0)
      return;

    // 简单实现：移除最老的条目
    Key oldestKey = victimCache->getOldestKey();
    victimCache->remove(oldestKey);
  }

  void updateStats(bool hit, bool windowHit) {
    std::lock_guard<std::mutex> lock(statsMutex);
    accessCount++;
    if (hit) {
      hitCount++;
      if (windowHit) {
        windowHits++;
      } else {
        victimHits++;
      }
    }
  }
};
} // namespace XCache
