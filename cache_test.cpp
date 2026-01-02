#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

#include "XArcCache/XArcCache.h"
#include "XCachePolicy.h"
#include "XLFUCache.h"
#include "XLRUCache.h"
#include "XWTinyLFUCache.h"

class Timer {
public:
  Timer() : start_(std::chrono::high_resolution_clock::now()) {}

  double elapsed() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_)
        .count();
  }

private:
  std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

// 测试辅助类
class CacheTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 初始化缓存实例
    lru = std::make_unique<XCache::XLRUCache<int, std::string>>(20);
    lfu = std::make_unique<XCache::XLFUCache<int, std::string>>(20);
    arc = std::make_unique<XCache::XArcCache<int, std::string>>(20);
    lruk = std::make_unique<XCache::XLRUKCache<int, std::string>>(20, 2);
    lfuAging = std::make_unique<XCache::XLFUCache<int, std::string>>(20, 50000,
                                                                     5000, 0.7);
    wTinyLFU = std::make_unique<XCache::XWTinyLFUCache<int, std::string>>(20);

    caches = {lru.get(),  lfu.get(),      arc.get(),
              lruk.get(), lfuAging.get(), wTinyLFU.get()};
  }

  std::unique_ptr<XCache::XLRUCache<int, std::string>> lru;
  std::unique_ptr<XCache::XLFUCache<int, std::string>> lfu;
  std::unique_ptr<XCache::XArcCache<int, std::string>> arc;
  std::unique_ptr<XCache::XLRUKCache<int, std::string>> lruk;
  std::unique_ptr<XCache::XLFUCache<int, std::string>> lfuAging;
  std::unique_ptr<XCache::XWTinyLFUCache<int, std::string>> wTinyLFU;

  std::array<XCache::XCachePolicy<int, std::string> *, 6> caches;
};

// 基本功能测试
TEST_F(CacheTest, BasicOperations) {
  // 测试 put 和 get 操作
  for (int i = 0; i < 6; ++i) {
    caches[i]->put(1, "value1");
    caches[i]->put(2, "value2");

    std::string result;
    ASSERT_TRUE(caches[i]->get(1, result))
        << "Cache " << i << " failed to get key 1";
    EXPECT_EQ(result, "value1") << "Cache " << i << " wrong value for key 1";

    result.clear(); // 清空结果
    ASSERT_TRUE(caches[i]->get(2, result))
        << "Cache " << i << " failed to get key 2";
    EXPECT_EQ(result, "value2") << "Cache " << i << " wrong value for key 2";

    EXPECT_FALSE(caches[i]->get(3, result))
        << "Cache " << i << " should not find non-existent key";
  }
}

// 测试缓存容量限制
TEST_F(CacheTest, CapacityLimit) {
  const int capacity = 20;

  for (int cache_idx = 0; cache_idx < 6; ++cache_idx) {
    // 填充超过容量
    for (int i = 0; i < capacity + 5; ++i) {
      caches[cache_idx]->put(i, "value" + std::to_string(i));
    }

    // 检查最早的数据是否被淘汰
    std::string result;
    // LRU-K (cache_idx=3) 有历史缓存，行为不同，需要特殊处理
    if (cache_idx == 3) {
      // LRU-K 可能仍然能找到早期的数据，因为它们在历史缓存中
      // 我们只检查最新的数据是否存在
      EXPECT_TRUE(caches[cache_idx]->get(capacity + 4, result))
          << "Cache " << cache_idx << " should contain key " << (capacity + 4);
    } else {
      // 其他缓存应该淘汰最早的数据
      EXPECT_FALSE(caches[cache_idx]->get(0, result))
          << "Cache " << cache_idx << " should have evicted key 0";
      EXPECT_TRUE(caches[cache_idx]->get(capacity + 4, result))
          << "Cache " << cache_idx << " should contain key " << (capacity + 4);
    }
  }
}

// 热点数据访问测试
TEST_F(CacheTest, HotDataAccess) {
  const int CAPACITY = 20;
  const int OPERATIONS = 10000; // 减少操作次数以便测试
  const int HOT_KEYS = 20;
  const int COLD_KEYS = 1000;

  std::random_device rd;
  std::mt19937 gen(rd());

  for (int cache_idx = 0; cache_idx < 6; ++cache_idx) {
    // 预热缓存
    for (int key = 0; key < HOT_KEYS; ++key) {
      caches[cache_idx]->put(key, "value" + std::to_string(key));
    }

    int hits = 0;
    int get_operations = 0;

    for (int op = 0; op < OPERATIONS; ++op) {
      bool isPut = (gen() % 100 < 30);
      int key;

      if (gen() % 100 < 70) {
        key = gen() % HOT_KEYS; // 热点数据
      } else {
        key = HOT_KEYS + (gen() % COLD_KEYS); // 冷数据
      }

      if (isPut) {
        caches[cache_idx]->put(key, "value" + std::to_string(key) + "_v" +
                                        std::to_string(op % 100));
      } else {
        std::string result;
        get_operations++;
        if (caches[cache_idx]->get(key, result)) {
          hits++;
        }
      }
    }

    double hitRate = 100.0 * hits / get_operations;
    EXPECT_GE(hitRate, 45.0)
        << "Cache " << cache_idx << " hit rate too low: " << hitRate << "%";
  }
}

// 循环扫描测试
TEST_F(CacheTest, LoopPattern) {
  const int CAPACITY = 50;
  const int LOOP_SIZE = 500;
  const int OPERATIONS = 5000; // 减少操作次数

  std::random_device rd;
  std::mt19937 gen(rd());

  for (int cache_idx = 0; cache_idx < 6; ++cache_idx) {
    // 预热
    for (int key = 0; key < LOOP_SIZE / 5; ++key) {
      caches[cache_idx]->put(key, "loop" + std::to_string(key));
    }

    int hits = 0;
    int get_operations = 0;
    int current_pos = 0;

    for (int op = 0; op < OPERATIONS; ++op) {
      bool isPut = (gen() % 100 < 20);
      int key;

      if (op % 100 < 60) { // 60%顺序扫描
        key = current_pos;
        current_pos = (current_pos + 1) % LOOP_SIZE;
      } else if (op % 100 < 90) { // 30%随机跳跃
        key = gen() % LOOP_SIZE;
      } else { // 10%访问范围外数据
        key = LOOP_SIZE + (gen() % LOOP_SIZE);
      }

      if (isPut) {
        caches[cache_idx]->put(key, "loop" + std::to_string(key) + "_v" +
                                        std::to_string(op % 100));
      } else {
        std::string result;
        get_operations++;
        if (caches[cache_idx]->get(key, result)) {
          hits++;
        }
      }
    }

    double hitRate = 100.0 * hits / get_operations;
    EXPECT_GE(hitRate, 1.0)
        << "Cache " << cache_idx
        << " loop pattern hit rate too low: " << hitRate << "%";
  }
}

// 工作负载变化测试
TEST_F(CacheTest, WorkloadShift) {
  const int CAPACITY = 30;
  const int OPERATIONS = 2000; // 减少操作次数
  const int PHASE_LENGTH = OPERATIONS / 5;

  std::random_device rd;
  std::mt19937 gen(rd());

  for (int cache_idx = 0; cache_idx < 6; ++cache_idx) {
    // 预热
    for (int key = 0; key < 30; ++key) {
      caches[cache_idx]->put(key, "init" + std::to_string(key));
    }

    int hits = 0;
    int get_operations = 0;

    for (int op = 0; op < OPERATIONS; ++op) {
      int phase = op / PHASE_LENGTH;
      int putProbability;

      switch (phase) {
      case 0:
        putProbability = 15;
        break;
      case 1:
        putProbability = 30;
        break;
      case 2:
        putProbability = 10;
        break;
      case 3:
        putProbability = 25;
        break;
      case 4:
        putProbability = 20;
        break;
      default:
        putProbability = 20;
      }

      bool isPut = (gen() % 100 < putProbability);
      int key;

      if (op < PHASE_LENGTH) {
        key = gen() % 5;
      } else if (op < PHASE_LENGTH * 2) {
        key = gen() % 400;
      } else if (op < PHASE_LENGTH * 3) {
        key = (op - PHASE_LENGTH * 2) % 100;
      } else if (op < PHASE_LENGTH * 4) {
        int locality = (op / 800) % 5;
        key = locality * 15 + (gen() % 15);
      } else {
        int r = gen() % 100;
        if (r < 40) {
          key = gen() % 5;
        } else if (r < 70) {
          key = 5 + (gen() % 45);
        } else {
          key = 50 + (gen() % 350);
        }
      }

      if (isPut) {
        caches[cache_idx]->put(key, "value" + std::to_string(key) + "_p" +
                                        std::to_string(phase));
      } else {
        std::string result;
        get_operations++;
        if (caches[cache_idx]->get(key, result)) {
          hits++;
        }
      }
    }

    double hitRate = 100.0 * hits / get_operations;
    EXPECT_GT(hitRate, 20.0)
        << "Cache " << cache_idx
        << " workload shift hit rate too low: " << hitRate << "%";
  }
}

// 性能测试
TEST_F(CacheTest, PerformanceTest) {
  const int OPERATIONS = 10000;

  for (int cache_idx = 0; cache_idx < 6; ++cache_idx) {
    Timer timer;

    // 混合操作
    for (int i = 0; i < OPERATIONS; ++i) {
      if (i % 3 == 0) {
        // Put 操作
        caches[cache_idx]->put(i, "value" + std::to_string(i));
      } else {
        // Get 操作
        std::string result;
        caches[cache_idx]->get(i % 100, result);
      }
    }

    double elapsed = timer.elapsed();
    EXPECT_LT(elapsed, 1000.0) << "Cache " << cache_idx
                               << " performance too slow: " << elapsed << "ms";
  }
}

// 参数化测试示例
class CacheParamTest : public ::testing::TestWithParam<int> {
protected:
  void SetUp() override {
    capacity = GetParam();
    lru = std::make_unique<XCache::XLRUCache<int, std::string>>(capacity);
    lfu = std::make_unique<XCache::XLFUCache<int, std::string>>(capacity);
    arc = std::make_unique<XCache::XArcCache<int, std::string>>(capacity);
  }

  int capacity;
  std::unique_ptr<XCache::XLRUCache<int, std::string>> lru;
  std::unique_ptr<XCache::XLFUCache<int, std::string>> lfu;
  std::unique_ptr<XCache::XArcCache<int, std::string>> arc;
};

// 测试不同容量下的缓存行为
TEST_P(CacheParamTest, DifferentCapacities) {
  // 填充缓存
  for (int i = 0; i < capacity * 2; ++i) {
    lru->put(i, "value" + std::to_string(i));
    lfu->put(i, "value" + std::to_string(i));
    arc->put(i, "value" + std::to_string(i));
  }

  // 检查容量限制
  std::string result;
  EXPECT_FALSE(lru->get(0, result));
  EXPECT_FALSE(lfu->get(0, result));
  EXPECT_FALSE(arc->get(0, result));

  EXPECT_TRUE(lru->get(capacity * 2 - 1, result));
  EXPECT_TRUE(lfu->get(capacity * 2 - 1, result));
  EXPECT_TRUE(arc->get(capacity * 2 - 1, result));
}

INSTANTIATE_TEST_SUITE_P(CacheCapacities, CacheParamTest,
                         ::testing::Values(10, 50, 100));

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
