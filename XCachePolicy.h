#pragma once

namespace XCache
{

    template <typename Key, typename Value>
    class XCachePolicy
    {
    public:
        virtual ~XCachePolicy() {};

        virtual void put(Key key, Value value) = 0;
        virtual bool get(Key key, Value &value) = 0;
        virtual Value get(Key key) = 0;
    }; // 此处需要添加分号
} // namespace XCache