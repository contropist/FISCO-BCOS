/**
 *  Copyright (C) 2023 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * @file BucketMap.h
 * @author: jimmyshi
 * @date 2023/3/3
 */
#pragma once

#include "Common.h"
#include "bcos-task/Generator.h"
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/cache_aligned_allocator.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_for_each.h>
#include <concepts>
#include <random>
#include <range/v3/iterator/operations.hpp>
#include <range/v3/range/concepts.hpp>
#include <range/v3/range/traits.hpp>
#include <range/v3/view/addressof.hpp>
#include <range/v3/view/chunk_by.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/map.hpp>
#include <range/v3/view/transform.hpp>
#include <type_traits>
#include <unordered_map>

namespace bcos
{

class EmptyType
{
};

struct StringHash
{
    using is_transparent = void;

    template <std::convertible_to<std::string_view> T>
    std::size_t operator()(T&& str) const
    {
        return std::hash<std::decay_t<T>>{}(str);
    }
};

template <class KeyType, class ValueType,
    class MapType = std::conditional_t<std::is_same_v<KeyType, std::string>,
        std::unordered_map<std::string, ValueType, StringHash, std::equal_to<>>,
        std::unordered_map<KeyType, ValueType>>>
class Bucket : public std::enable_shared_from_this<Bucket<KeyType, ValueType, MapType>>
{
public:
    using Ptr = std::shared_ptr<Bucket>;

    Bucket() = default;
    Bucket(const Bucket&) = default;
    Bucket(Bucket&&) = default;
    Bucket& operator=(const Bucket&) = default;
    Bucket& operator=(Bucket&&) noexcept = default;
    ~Bucket() noexcept = default;

    class WriteAccessor
    {
    public:
        WriteAccessor() = default;
        bool emplaceLock(auto&&... args)
        {
            if (!m_writeGuard || !m_writeGuard->owns_lock())
            {
                m_writeGuard.emplace(std::forward<decltype(args)>(args)...);
            }
            return m_writeGuard->owns_lock();
        }
        void setIterator(typename MapType::iterator iterator, Bucket* bucket)
        {
            m_iterator = iterator;
            m_bucket = bucket;
        }
        auto iterator() const { return m_iterator; }
        Bucket* bucket() const { return m_bucket; }

        const KeyType& key() const { return m_iterator->first; }
        ValueType& value() { return m_iterator->second; }

    private:
        typename MapType::iterator m_iterator;
        Bucket* m_bucket;
        std::optional<WriteGuard> m_writeGuard;
    };

    class ReadAccessor
    {
    public:
        ReadAccessor() = default;
        bool emplaceLock(auto&&... args)
        {
            if (!m_readGuard)
            {
                m_readGuard.emplace(std::forward<decltype(args)>(args)...);
            }
            return m_readGuard->owns_lock();
        }
        void setIterator(typename MapType::const_iterator iterator, const Bucket* bucket)
        {
            m_iterator = iterator;
            m_bucket = bucket;
        }
        auto iterator() const { return m_iterator; }
        const Bucket* bucket() const { return m_bucket; }

        const KeyType& key() const { return m_iterator->first; }
        const ValueType& value() { return m_iterator->second; }

    private:
        typename MapType::const_iterator m_iterator;
        const Bucket* m_bucket;
        std::optional<ReadGuard> m_readGuard;
    };

    // return true if the lock has acquired
    template <class Accessor>
        requires std::same_as<Accessor, ReadAccessor> || std::same_as<Accessor, WriteAccessor>
    bool acquireAccessor(Accessor& accessor, bool wait)
    {
        return wait ? accessor.emplaceLock(m_mutex) :
                      accessor.emplaceLock(m_mutex, boost::try_to_lock);
    }

    // return true if found
    template <class AccessorType>
    bool find(AccessorType& accessor, const KeyType& key)
    {
        accessor.emplaceLock(m_mutex);

        auto it = m_values.find(key);
        if (it == m_values.end())
        {
            return false;
        }

        accessor.setIterator(it, this);
        return true;
    }

    // return true if insert happen
    bool insert(WriteAccessor& accessor, const std::pair<KeyType, ValueType>& keyValue)
    {
        accessor.emplaceLock(m_mutex);

        auto [it, inserted] = m_values.try_emplace(keyValue.first, keyValue.second);
        accessor.setIterator(it, this);
        return inserted;
    }

    void remove(WriteAccessor& accessor)
    {
        auto it = accessor.iterator();
        m_values.erase(it);
    }

    size_t size() { return m_values.size(); }
    bool contains(const auto& key)
    {
        ReadGuard guard(m_mutex);
        return m_values.contains(key);
    }

    void clear(WriteAccessor& accessor,
        std::function<void(bool, const KeyType&, WriteAccessor)> onRemove = nullptr)
    {
        accessor.emplaceLock(m_mutex);

        for (auto it = m_values.begin(); it != m_values.end(); it++)
        {
            if (onRemove)
            {
                onRemove(true, it->first, accessor);
            }
        }

        m_values.clear();
    }

    SharedMutex& getMutex() { return m_mutex; }

    MapType m_values;
    mutable SharedMutex m_mutex;
};

template <class KeyType, class ValueType, class BucketHasher = std::hash<KeyType>,
    class BucketType = Bucket<KeyType, ValueType>>
class BucketMap
{
public:
    using Ptr = std::shared_ptr<BucketMap>;
    using WriteAccessor = typename BucketType::WriteAccessor;
    using ReadAccessor = typename BucketType::ReadAccessor;

    BucketMap(const BucketMap&) = default;
    BucketMap(BucketMap&&) noexcept = default;
    BucketMap& operator=(const BucketMap&) = default;
    BucketMap& operator=(BucketMap&&) noexcept = default;
    BucketMap(size_t bucketSize)
    {
        m_buckets.reserve(bucketSize);
        for (size_t i = 0; i < bucketSize; i++)
        {
            m_buckets.emplace_back(std::make_shared<BucketType>());
        }
    }
    ~BucketMap() noexcept = default;

    // return true if found
    template <class AccessorType>
    bool find(AccessorType& accessor, const KeyType& key)
    {
        auto idx = getBucketIndex(key);
        auto& bucket = m_buckets[idx];
        return bucket->template find<AccessorType>(accessor, key);
    }

    template <class Accessor, bool parallel, class Keys, class Handler>
        requires ::ranges::random_access_range<Keys> && ::ranges::sized_range<Keys> &&
                 std::is_lvalue_reference_v<::ranges::range_reference_t<Keys>> &&
                 std::invocable<Handler, Accessor&, size_t, BucketType&>
    auto traverse(const Keys& keys, Handler handler)
    {
        auto sortedKeys = ::ranges::views::enumerate(keys) |
                          ::ranges::views::transform([&](const auto& tuple) {
                              auto&& [index, key] = tuple;
                              return std::make_tuple(index, getBucketIndex(key));
                          }) |
                          ::ranges::to<std::vector>();
        std::sort(sortedKeys.begin(), sortedKeys.end(),
            [](const auto& lhs, const auto& rhs) { return std::get<1>(lhs) < std::get<1>(rhs); });
        auto chunks = ::ranges::views::chunk_by(sortedKeys,
            [](const auto& lhs, const auto& rhs) { return std::get<1>(lhs) == std::get<1>(rhs); });

        constexpr static auto innerHandler = [](auto& buckets, auto& chunk, auto& handler) {
            auto bucketIndex = std::get<1>(chunk.front());
            auto& bucket = buckets[bucketIndex];

            Accessor accessor;
            bucket->acquireAccessor(accessor, true);

            for (auto&& [index, _] : chunk)
            {
                handler(accessor, index, *bucket);
            }
        };

        if constexpr (parallel)
        {
            auto chunksVec = ::ranges::to<std::vector>(chunks);
            tbb::parallel_for(tbb::blocked_range(0LU, chunksVec.size()), [&](auto const& range) {
                for (auto i = range.begin(); i != range.end(); ++i)
                {
                    auto& chunk = chunksVec[i];
                    innerHandler(m_buckets, chunk, handler);
                }
            });
        }
        else
        {
            for (auto& chunk : chunks)
            {
                innerHandler(m_buckets, chunk, handler);
            }
        }
    }

    template <class Keys>
        requires ::ranges::random_access_range<Keys> && ::ranges::sized_range<Keys> &&
                 std::is_lvalue_reference_v<::ranges::range_reference_t<Keys>>
    void batchInsert(const Keys& kvs)
    {
        traverse<WriteAccessor, true>(::ranges::views::keys(kvs),
            [&](WriteAccessor& accessor, auto index, BucketType& bucket) {
                bucket.insert(accessor, kvs[index]);
            });
    }

    // handler: accessor is nullptr if not found, handler return false to break to find
    template <class AccessorType>
    auto batchFind(const auto& keys)
    {
        std::vector<std::optional<ValueType>,
            tbb::cache_aligned_allocator<std::optional<ValueType>>>
            values(::ranges::size(keys));
        traverse<AccessorType, true>(
            keys, [&](AccessorType& accessor, auto index, BucketType& bucket) {
                if (bucket.find(accessor, keys[index]))
                {
                    values[index].emplace(accessor.value());
                }
            });
        return values;
    }

    template <class Keys, bool returnRemoved>
        requires ::ranges::random_access_range<Keys> && ::ranges::sized_range<Keys>
    auto batchRemove(const Keys& keys)
    {
        std::conditional_t<returnRemoved,
            std::vector<std::optional<ValueType>,
                tbb::cache_aligned_allocator<std::optional<ValueType>>>,
            EmptyType>
            values;
        if constexpr (returnRemoved)
        {
            values.resize(::ranges::size(keys));
        }
        traverse<WriteAccessor, true>(
            keys, [&](WriteAccessor& accessor, auto index, BucketType& bucket) {
                if (bucket.find(accessor, keys[index]))
                {
                    if constexpr (returnRemoved)
                    {
                        values[index].emplace(std::move(accessor.value()));
                    }
                    bucket.remove(accessor);
                }
            });

        if constexpr (returnRemoved)
        {
            return values;
        }
    }

    bool insert(WriteAccessor& accessor, std::pair<KeyType, ValueType> kv)
    {
        auto idx = getBucketIndex(kv.first);
        auto& bucket = m_buckets[idx];
        return bucket->insert(accessor, std::move(kv));
    }

    void remove(WriteAccessor& accessor) { accessor.bucket()->remove(accessor); }

    size_t size() const
    {
        size_t size = 0;
        for (const auto& bucket : m_buckets)
        {
            size += bucket->size();
        }
        return size;
    }

    bool empty() const { return size() == 0; }

    bool contains(const KeyType& key)
    {
        auto idx = getBucketIndex(key);
        auto& bucket = m_buckets[idx];
        return bucket->contains(key);
    }
    bool contains(const auto& key)
    {
        auto idx = getBucketIndex(key);
        auto& bucket = m_buckets[idx];
        return bucket->contains(key);
    }

    void clear(std::function<void(bool, const KeyType&, const ValueType&)> onRemove = {})
    {
        if (onRemove)
        {
            for (auto& accessor : range<ReadAccessor>())
            {
                onRemove(true, accessor.key(), accessor.value());
            }
        }

        for (size_t i = 0; i < m_buckets.size(); i++)
        {
            m_buckets[i] = std::make_shared<BucketType>();
        }
    }

    template <class AccessorType>
    task::Generator<AccessorType&> range(size_t startIndex) const
    {
        for (auto& bucket : ::ranges::views::iota(startIndex, startIndex + m_buckets.size()) |
                                ::ranges::views::transform([&](auto index) -> auto& {
                                    return m_buckets[index % m_buckets.size()];
                                }))
        {
            AccessorType accessor;
            bucket->acquireAccessor(accessor, true);

            for (auto it = bucket->m_values.begin(); it != bucket->m_values.end(); ++it)
            {
                accessor.setIterator(it, bucket.get());
                co_yield accessor;
            }
        }
    }

    template <class AccessorType>
    task::Generator<AccessorType&> range()
    {
        static thread_local std::mt19937 random(std::random_device{}());
        auto startIndex = random() % m_buckets.size();
        return range<AccessorType>(startIndex);
    }

    template <class AccessorType>
    task::Generator<AccessorType&> rangeByKey(const KeyType& startKey)
    {
        auto startIndex = getBucketIndex(startKey);
        return range<AccessorType>(startIndex);
    }

protected:
    int getBucketIndex(auto const& key)
    {
        auto hash = BucketHasher{}(key);
        return hash % m_buckets.size();
    }

    std::vector<typename BucketType::Ptr> m_buckets;
};

template <class KeyType, class BucketHasher = std::hash<KeyType>>
class BucketSet : public BucketMap<KeyType, EmptyType, BucketHasher>
{
public:
    BucketSet(const BucketSet&) = default;
    BucketSet(BucketSet&&) noexcept = default;
    BucketSet& operator=(const BucketSet&) = default;
    BucketSet& operator=(BucketSet&&) noexcept = default;
    ~BucketSet() noexcept = default;

    BucketSet(size_t bucketSize) : BucketMap<KeyType, EmptyType, BucketHasher>(bucketSize){};

    using WriteAccessor = typename BucketMap<KeyType, EmptyType, BucketHasher>::WriteAccessor;
    using ReadAccessor = typename BucketMap<KeyType, EmptyType, BucketHasher>::ReadAccessor;

    bool insert(BucketSet::WriteAccessor& accessor, KeyType key)
    {
        return BucketMap<KeyType, EmptyType, BucketHasher>::insert(
            accessor, {std::move(key), EmptyType()});
    }

    template <bool returnInsertResult>
    auto batchInsert(const auto& keys)
    {
        std::conditional_t<returnInsertResult,
            std::vector<int8_t, tbb::cache_aligned_allocator<int8_t>>, EmptyType>
            results;
        if constexpr (returnInsertResult)
        {
            results.resize(::ranges::size(keys));
        }
        BucketMap<KeyType, EmptyType, BucketHasher>::template traverse<WriteAccessor, true>(
            keys, [&](WriteAccessor& accessor, auto index, auto& bucket) {
                bool inserted = bucket.insert(accessor, {keys[index], EmptyType()});
                if constexpr (returnInsertResult)
                {
                    if (inserted)
                    {
                        results[index] = true;
                    }
                }
            });
        if constexpr (returnInsertResult)
        {
            return results;
        }
    }

    void batchInsert(const auto& keys) { batchInsert<false>(keys); }
};

}  // namespace bcos
