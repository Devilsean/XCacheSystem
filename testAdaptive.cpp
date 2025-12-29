#include <iostream>
#include <string>
#include <chrono>
#include <vector>
#include <iomanip>
#include <random>
#include <algorithm>

#include "XCachePolicy.h"
#include "XLFUCache.h"
#include "XLRUCache.h"
#include "XArcCache/XArcCache.h"
#include "XAdaptiveCache.h"

void testWorkloadShift()
{
    std::cout << "\n=== 自适应缓存工作负载变化测试 ===" << std::endl;

    const int CAPACITY = 30;
    const int OPERATIONS = 20000; // 减少操作数以便观察

    XCache::XAdaptiveCache<int, std::string> adaptive(CAPACITY);

    std::random_device rd;
    std::mt19937 gen(rd());
    const int PHASE_LENGTH = OPERATIONS / 5;

    std::cout << "开始测试..." << std::endl;

    // 进行多阶段测试
    for (int op = 0; op < OPERATIONS; ++op)
    {
        int phase = op / PHASE_LENGTH;
        int putProbability;
        int keyRange;

        switch (phase)
        {
        case 0:
            putProbability = 15;
            keyRange = 50;
            break; // 阶段1: 热点访问
        case 1:
            putProbability = 25;
            keyRange = 200;
            break; // 阶段2: 扩展访问
        case 2:
            putProbability = 35;
            keyRange = 100;
            break; // 阶段3: 中等范围
        case 3:
            putProbability = 20;
            keyRange = 300;
            break; // 阶段4: 大范围访问
        default:
            putProbability = 30;
            keyRange = 80;
            break; // 阶段5: 回到中等范围
        }

        bool isPut = (gen() % 100 < putProbability);
        int key = gen() % keyRange;

        if (isPut)
        {
            std::string value = "value" + std::to_string(key) + "_v" + std::to_string(op % 100);
            adaptive.put(key, value);
        }
        else
        {
            std::string result;
            adaptive.get(key, result);
        }

        // 每100次操作强制评估一次
        if (op % 100 == 0)
        {
            // 这里可以添加一些调试输出
        }

        // 每1000次操作输出一次当前策略
        if (op % 1000 == 0)
        {
            auto currentStrategy = adaptive.getCurrentStrategy();
            std::string strategyName;
            switch (currentStrategy)
            {
            case XCache::XAdaptiveCache<int, std::string>::Strategy::LRU:
                strategyName = "LRU";
                break;
            case XCache::XAdaptiveCache<int, std::string>::Strategy::LFU:
                strategyName = "LFU";
                break;
            case XCache::XAdaptiveCache<int, std::string>::Strategy::LFU_AGING:
                strategyName = "LFU-Aging";
                break;
            case XCache::XAdaptiveCache<int, std::string>::Strategy::ARC:
                strategyName = "ARC";
                break;
            }
            std::cout << "操作 " << op << ": 当前策略 = " << strategyName << std::endl;
        }
    }

    std::cout << "测试完成" << std::endl;
}

int main()
{
    testWorkloadShift();
    return 0;
}
