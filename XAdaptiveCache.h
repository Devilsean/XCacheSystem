#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <iostream>
#include <iomanip>
#include "XCachePolicy.h"
#include "XLRUCache.h"
#include "XLFUCache.h"
#include "XArcCache/XArcCache.h"

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
            LFU_AGING,
            ARC
        };

        XAdaptiveCache(int capacity) : capacity(capacity), currentStrategy(Strategy::LFU_AGING)
        {
            // 初始化四种缓存策略
            lruCache = std::make_unique<XLRUCache<Key, Value>>(capacity);
            lfuCache = std::make_unique<XLFUCache<Key, Value>>(capacity);
            lfuAgingCache = std::make_unique<XLFUCache<Key, Value>>(capacity, 8000, 1000, 0.5);
            arcCache = std::make_unique<XCache::XArcCache<Key, Value>>(capacity);
            
            // 性能统计
            strategyPerformance.resize(4, {0, 0}); // 4种策略
            lastEvaluationTime = std::chrono::steady_clock::now();
            evaluationInterval = std::chrono::milliseconds(500); // 每0.5秒评估一次，更频繁
            switchThreshold = 0.02; // 2%的切换阈值，更敏感
        }

        ~XAdaptiveCache() override = default;

        void put(Key key, Value value) override
        {
            std::lock_guard<std::mutex> lock(mtx);
            
            // 所有策略都执行put操作
            lruCache->put(key, value);
            lfuCache->put(key, value);
            lfuAgingCache->put(key, value);
            arcCache->put(key, value);
        }

        bool get(Key key, Value &value) override
        {
            std::lock_guard<std::mutex> lock(mtx);
            
            // 让所有策略都执行get操作，收集性能数据
            std::vector<bool> hits(4);
            std::vector<Value> values(4);
            
            hits[0] = lruCache->get(key, values[0]);
            hits[1] = lfuCache->get(key, values[1]);
            hits[2] = lfuAgingCache->get(key, values[2]);
            hits[3] = arcCache->get(key, values[3]);
            
            // 更新所有策略的性能统计
            for (int i = 0; i < 4; ++i)
            {
                strategyPerformance[i].total++;
                if (hits[i])
                    strategyPerformance[i].hits++;
            }
            
            // 返回当前策略的结果
            int strategyIndex = static_cast<int>(currentStrategy);
            value = values[strategyIndex];
            
            // 定期评估并切换策略
            evaluateAndSwitchStrategy();
            
            return hits[strategyIndex];
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
            static int evaluationCount = 0;
            if (++evaluationCount % 1000 != 0) // 每1000次get调用评估一次
                return;
            
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
            if (bestHitRate > currentHitRate + switchThreshold) // 使用可配置的切换阈值
            {
                std::string oldStrategy = getStrategyName(currentStrategy);
                currentStrategy = static_cast<Strategy>(bestStrategy);
                std::string newStrategy = getStrategyName(currentStrategy);
                // 不重置统计，保持连续性
            }
        }
        
        std::string getStrategyName(Strategy strategy) const
        {
            switch (strategy)
            {
            case Strategy::LRU: return "LRU";
            case Strategy::LFU: return "LFU";
            case Strategy::LFU_AGING: return "LFU-Aging";
            case Strategy::ARC: return "ARC";
            default: return "Unknown";
            }
        }

    private:
        int capacity;
        Strategy currentStrategy;
        
        std::unique_ptr<XLRUCache<Key, Value>> lruCache;
        std::unique_ptr<XLFUCache<Key, Value>> lfuCache;
        std::unique_ptr<XLFUCache<Key, Value>> lfuAgingCache;
        std::unique_ptr<XCache::XArcCache<Key, Value>> arcCache;
        
        std::vector<PerformanceStats> strategyPerformance;
        std::chrono::steady_clock::time_point lastEvaluationTime;
        std::chrono::milliseconds evaluationInterval;
        double switchThreshold; // 可配置的切换阈值
        
        mutable std::mutex mtx;
    };
}
