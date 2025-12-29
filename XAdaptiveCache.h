#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include "XCachePolicy.h"
#include "XLRUCache.h"
#include "XLFUCache.h"

namespace XCache
{
    template <typename Key, typename Value>
    class XAdaptiveCache : public XCachePolicy<Key, Value>
    {
    public:
        enum class Strategy
        {
            LRU,
            LFU,
            LFU_AGING
        };

        XAdaptiveCache(int capacity) : capacity(capacity), currentStrategy(Strategy::LFU_AGING)
        {
            // 初始化三种缓存策略
            lruCache = std::make_unique<XLRUCache<Key, Value>>(capacity);
            lfuCache = std::make_unique<XLFUCache<Key, Value>>(capacity);
            lfuAgingCache = std::make_unique<XLFUCache<Key, Value>>(capacity, 8000, 1000, 0.5);
            
            // 性能统计
            strategyPerformance.resize(3, {0, 0}); // hits, total
            lastEvaluationTime = std::chrono::steady_clock::now();
            evaluationInterval = std::chrono::seconds(10); // 每10秒评估一次
        }

        ~XAdaptiveCache() override = default;

        void put(Key key, Value value) override
        {
            std::lock_guard<std::mutex> lock(mtx);
            
            // 所有策略都执行put操作
            lruCache->put(key, value);
            lfuCache->put(key, value);
            lfuAgingCache->put(key, value);
        }

        bool get(Key key, Value &value) override
        {
            std::lock_guard<std::mutex> lock(mtx);
            
            // 根据当前策略选择使用哪个缓存
            bool hit = false;
            switch (currentStrategy)
            {
            case Strategy::LRU:
                hit = lruCache->get(key, value);
                break;
            case Strategy::LFU:
                hit = lfuCache->get(key, value);
                break;
            case Strategy::LFU_AGING:
                hit = lfuAgingCache->get(key, value);
                break;
            }
            
            // 更新性能统计
            int strategyIndex = static_cast<int>(currentStrategy);
            strategyPerformance[strategyIndex].total++;
            if (hit)
                strategyPerformance[strategyIndex].hits++;
            
            // 定期评估并切换策略
            evaluateAndSwitchStrategy();
            
            return hit;
        }

        Value get(Key key) override
        {
            Value value{};
            get(key, value);
            return value;
        }

        Strategy getCurrentStrategy() const 
        { 
            std::lock_guard<std::mutex> lock(mtx);
            return currentStrategy; 
        }

        // 获取各策略的性能统计
        std::vector<double> getStrategyPerformance() const
        {
            std::lock_guard<std::mutex> lock(mtx);
            std::vector<double> performance;
            for (const auto& stats : strategyPerformance)
            {
                double hitRate = stats.total > 0 ? 
                    static_cast<double>(stats.hits) / stats.total : 0.0;
                performance.push_back(hitRate);
            }
            return performance;
        }

    private:
        struct PerformanceStats
        {
            int hits = 0;
            int total = 0;
        };

        void evaluateAndSwitchStrategy()
        {
            auto now = std::chrono::steady_clock::now();
            if (now - lastEvaluationTime < evaluationInterval)
                return;
            
            lastEvaluationTime = now;
            
            // 计算各策略的命中率
            std::vector<double> hitRates;
            for (const auto& stats : strategyPerformance)
            {
                double hitRate = stats.total > 0 ? 
                    static_cast<double>(stats.hits) / stats.total : 0.0;
                hitRates.push_back(hitRate);
            }
            
            // 找到最佳策略
            int bestStrategy = 0;
            double bestHitRate = hitRates[0];
            for (int i = 1; i < hitRates.size(); ++i)
            {
                if (hitRates[i] > bestHitRate)
                {
                    bestHitRate = hitRates[i];
                    bestStrategy = i;
                }
            }
            
            // 切换到最佳策略（如果提升显著）
            double currentHitRate = hitRates[static_cast<int>(currentStrategy)];
            if (bestHitRate > currentHitRate + 0.05) // 5%的提升阈值
            {
                currentStrategy = static_cast<Strategy>(bestStrategy);
                // 重置统计，以便重新评估
                strategyPerformance[bestStrategy] = {0, 0};
            }
        }

    private:
        int capacity;
        Strategy currentStrategy;
        
        std::unique_ptr<XLRUCache<Key, Value>> lruCache;
        std::unique_ptr<XLFUCache<Key, Value>> lfuCache;
        std::unique_ptr<XLFUCache<Key, Value>> lfuAgingCache;
        
        std::vector<PerformanceStats> strategyPerformance;
        std::chrono::steady_clock::time_point lastEvaluationTime;
        std::chrono::seconds evaluationInterval;
        
        mutable std::mutex mtx;
    };
}
