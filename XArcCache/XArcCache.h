#pragma once

#include "../XCachePolicy.h"
#include "XArcLFUpart.h"
#include "XArcLRUpart.h"

#include <memory>

namespace XCache
{
    template <typename Key, typename Value>
    class XArcCache : public XCachePolicy<Key, Value>
    {
    public:
        explicit XArcCache(size_t capacity_ = 10, size_t transformThreshold_ = 2)
            : capacity(capacity_), transformThreshold(transformThreshold_),
              lfupart(std::make_unique<XArcLFUpart<Key, Value>>(capacity_, transformThreshold_)),
              lrupart(std::make_unique<XArcLRUpart<Key, Value>>(capacity_, transformThreshold_)) {};

        ~XArcCache() override = default;

        void put(Key key, Value value) override
        {
            checkGhostCaches(key);
            bool inLFU = lfupart->contain(key);
            lrupart->put(key, value);
            if (inLFU)
            {
                lfupart->put(key, value);
            }
        }

        bool get(Key key, Value &value) override
        {
            checkGhostCaches(key);
            bool shouldTransForm = false;
            if (lrupart->get(key, value, shouldTransForm))
            {
                if (shouldTransForm)
                {
                    lfupart->put(key, value);
                }
                return true;
            }
            return lfupart->get(key, value);
        }

        Value get(Key key) override
        {
            Value value{};
            get(key, value);
            return value;
        }

    private:
        bool checkGhostCaches(Key key)
        {
            bool inGhost = false;
            if (lfupart->checkGhost(key))
            {
                if (lrupart->decreaseCapacity())
                    lfupart->increaseCapacity();
                inGhost = true;
            }
            else if (lrupart->checkGhost(key))
            {
                if (lfupart->decreaseCapacity())
                    lrupart->increaseCapacity();
                inGhost = true;
            }
            return inGhost;
        }

    private:
        size_t capacity;
        size_t transformThreshold;
        std::unique_ptr<XArcLFUpart<Key, Value>> lfupart;
        std::unique_ptr<XArcLRUpart<Key, Value>> lrupart;
    };
}
