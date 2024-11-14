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
#include "bcos-utilities/BoostLog.h"
#include <oneapi/tbb/parallel_sort.h>
#include <concepts>
#include <queue>
#include <range/v3/iterator/operations.hpp>
#include <range/v3/range/concepts.hpp>
#include <range/v3/view/addressof.hpp>
#include <range/v3/view/chunk_by.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/repeat.hpp>
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
        void setLock(WriteGuard guard)
        {
            if (!m_writeGuard || !m_writeGuard->owns_lock())
            {
                m_writeGuard.emplace(std::move(guard));
            }
        }
        void emplaceLock(auto&&... args)
        {
            if (!m_writeGuard || !m_writeGuard->owns_lock())
            {
                m_writeGuard.emplace(std::forward<decltype(args)>(args)...);
            }
        }
        void setValue(typename MapType::iterator it) { m_it = it; };
        const KeyType& key() const { return m_it->first; }
        ValueType& value() { return m_it->second; }

    private:
        typename MapType::iterator m_it;
        std::optional<WriteGuard> m_writeGuard;
    };

    class ReadAccessor
    {
    public:
        ReadAccessor() = default;
        void setLock(ReadGuard guard)
        {
            if (!m_readGuard)
            {
                m_readGuard.emplace(std::move(guard));
            }
        }
        void emplaceLock(auto&&... args)
        {
            if (!m_readGuard)
            {
                m_readGuard.emplace(std::forward<decltype(args)>(args)...);
            }
        }
        void setValue(typename MapType::iterator it) { m_it = it; };
        const KeyType& key() const { return m_it->first; }
        const ValueType& value() { return m_it->second; }

    private:
        typename MapType::iterator m_it;
        std::optional<ReadGuard> m_readGuard;
    };

    // return true if the lock has acquired
    bool acquireAccessor(WriteAccessor& accessor, bool wait)
    {
        if (WriteGuard guard = wait ? WriteGuard(m_mutex) : WriteGuard(m_mutex, boost::try_to_lock);
            guard.owns_lock())
        {
            accessor.setLock(std::move(guard));  // acquire lock here
            return true;
        }
        return false;
    }

    // return true if the lock has acquired
    bool acquireAccessor(ReadAccessor& accessor, bool wait)
    {
        if (ReadGuard guard = wait ? ReadGuard(m_mutex) : ReadGuard(m_mutex, boost::try_to_lock);
            guard.owns_lock())
        {
            accessor.setLock(std::move(guard));  // acquire lock here
            return true;
        }
        return false;
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

        accessor.setValue(it);
        return true;
    }

    // return true if insert happen
    bool insert(WriteAccessor& accessor, const std::pair<KeyType, ValueType>& keyValue)
    {
        accessor.emplaceLock(m_mutex);

        auto [it, inserted] = m_values.try_emplace(keyValue.first, keyValue.second);
        accessor.setValue(it);
        return inserted;
    }

    // return {} if not exists before remove
    ValueType remove(const KeyType& key)
    {
        bcos::WriteGuard guard(m_mutex);

        auto it = m_values.find(key);
        if (it == m_values.end())
        {
            return {};
        }

        ValueType ret = std::move(it->second);
        m_values.erase(it);
        return ret;
    }


    // return true if remove success
    std::pair<bool, ValueType> remove(WriteAccessor& accessor, const KeyType& key)
    {
        accessor.emplaceLock(m_mutex);

        auto it = m_values.find(key);
        if (it == m_values.end())
        {
            if (c_fileLogLevel == LogLevel::DEBUG) [[unlikely]]
            {
                BCOS_LOG(DEBUG) << LOG_DESC("Remove tx, but transaction not found! ")
                                << LOG_KV("key", key);
            }
            return {false, {}};
        }

        auto value = std::move(it->second);
        m_values.erase(it);
        return {true, std::move(value)};
    }

    size_t size()
    {
        ReadGuard guard(m_mutex);
        return m_values.size();
    }
    bool contains(const auto& key)
    {
        ReadGuard guard(m_mutex);
        return m_values.contains(key);
    }

    // return true if need continue
    template <class AccessorType>  // handler return isContinue
    bool forEach(std::function<bool(AccessorType&)> handler, AccessorType& accessor)
    {
        accessor.emplaceLock(m_mutex);
        for (auto it = m_values.begin(); it != m_values.end(); it++)
        {
            accessor.setValue(it);
            if (!handler(accessor))
            {
                return false;
            }
        }
        return true;
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

    // handler: accessor is nullptr if not found, handler return false to break to find
    template <class AccessorType>
    void batchFind(const auto& keys, std::function<bool(const KeyType&, AccessorType*)> handler)
    {
        forEach<AccessorType>(keys, [handler = std::move(handler)](const KeyType& key,
                                        typename BucketType::Ptr bucket, AccessorType& accessor) {
            bool has = bucket->template find<AccessorType>(accessor, key);
            return handler(key, has ? std::addressof(accessor) : nullptr);
        });
    }

    void batchInsert(
        const auto& kvs, std::function<void(bool, const KeyType&, WriteAccessor)> onInsert)
    {
        forEach<WriteAccessor>(kvs, [onInsert = std::move(onInsert)](decltype(kvs.front()) kv,
                                        typename BucketType::Ptr bucket, WriteAccessor& accessor) {
            bucket->insert(accessor, kv);
            return true;
        });
    }

    void batchInsert(const auto& kvs)
    {
        batchInsert(kvs, [](bool, const KeyType&, WriteAccessor) {});
    }

    template <class Keys, bool returnRemoved>
        requires ::ranges::random_access_range<Keys> && ::ranges::sized_range<Keys>
    auto batchRemove(
        const Keys& keys) -> std::conditional_t<returnRemoved, std::vector<ValueType>, void>
    {
        auto count = ::ranges::size(keys);
        auto sortedKeys =
            ::ranges::views::enumerate(keys) | ::ranges::views::transform([&](const auto& tuple) {
                auto&& [index, key] = tuple;
                static_assert(std::is_lvalue_reference_v<decltype(key)>);
                if constexpr (returnRemoved)
                {
                    return std::make_tuple(std::addressof(key), getBucketIndex(key), index);
                }
                else
                {
                    return std::make_tuple(std::addressof(key), getBucketIndex(key));
                }
            }) |
            ::ranges::to<std::vector>();
        tbb::parallel_sort(sortedKeys.begin(), sortedKeys.end(),
            [](const auto& lhs, const auto& rhs) { return std::get<1>(lhs) < std::get<1>(rhs); });

        std::conditional_t<returnRemoved, std::vector<ValueType>, EmptyType> values;
        if constexpr (returnRemoved)
        {
            values.resize(count);
        }

        auto chunks = ::ranges::views::chunk_by(sortedKeys, [](const auto& lhs, const auto& rhs) {
            return std::get<1>(lhs) == std::get<1>(rhs);
        }) | ::ranges::to<std::vector>();
        tbb::parallel_for(tbb::blocked_range(0LU, chunks.size()), [&](auto const& range) {
            for (auto i = range.begin(); i != range.end(); ++i)
            {
                auto& chunk = chunks[i];
                auto bucketIndex = std::get<1>(chunk.front());
                auto& bucket = m_buckets[bucketIndex];

                WriteAccessor accessor;
                bucket->acquireAccessor(accessor, true);

                auto& datas = bucket->m_values;
                if constexpr (returnRemoved)
                {
                    for (auto&& [key, _, index] : chunk)
                    {
                        if (auto it = datas.find(*key); it != datas.end())
                        {
                            values[index] = std::move(it->second);
                            datas.erase(it);
                        }
                    }
                }
                else
                {
                    for (auto&& [key, _] : chunk)
                    {
                        datas.erase(*key);
                    }
                }
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

    ValueType remove(const KeyType& key)
    {
        auto idx = getBucketIndex(key);
        auto& bucket = m_buckets[idx];
        return bucket->remove(key);
    }

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
        if (!onRemove) [[likely]]
        {
            for (size_t i = 0; i < m_buckets.size(); i++)
            {
                m_buckets[i] = std::make_shared<BucketType>();
            }
        }
        else
        {
            // idx and bucket
            std::queue<std::pair<size_t, typename BucketType::Ptr>> bucket2Remove;
            for (size_t i = 0; i < m_buckets.size(); i++)
            {
                bucket2Remove.emplace(i, std::move(m_buckets[i]));
                m_buckets[i] = std::make_shared<BucketType>();
            }

            while (!bucket2Remove.empty())
            {
                auto it = std::move(bucket2Remove.front());
                auto idx = it.first;
                auto& bucket = it.second;
                bucket2Remove.pop();
                ReadAccessor accessor;
                bool needWait = bucket2Remove.empty();  // wait if is last bucket
                bool acquired = bucket->acquireAccessor(accessor, needWait);
                if (acquired) [[likely]]
                {
                    bucket->template forEach<ReadAccessor>(
                        [&](ReadAccessor& accessor) {
                            onRemove(true, accessor.key(), accessor.value());
                            return true;
                        },
                        accessor);
                }
                else
                {
                    bucket2Remove.template emplace(std::move(it));
                }
            }
        }
    }

    template <class AccessorType>  // handler return isContinue
    void forEach(std::function<bool(AccessorType&)> handler)
    {
        forEachByStartIndex<AccessorType>(std::rand() % m_buckets.size(), std::move(handler));
    }

    template <class AccessorType>  // handler return isContinue
    void forEach(const KeyType& startAfter, std::function<bool(AccessorType&)> handler)
    {
        auto startIdx = (getBucketIndex(startAfter) + 1) % m_buckets.size();
        forEachByStartIndex<AccessorType>(startIdx, std::move(handler));
    }

    template <class AccessorType>  // handler return isContinue
    void forEach(const KeyType& startAfter, size_t eachBucketLimit,
        std::function<std::pair<bool, bool>(AccessorType& accessor)> handler)
    {
        size_t startIdx = (getBucketIndex(startAfter) + 1) % m_buckets.size();
        size_t bucketsSize = m_buckets.size();

        auto indexes =
            ::ranges::views::iota(startIdx, startIdx + bucketsSize) |
            ::ranges::views::transform([bucketsSize](size_t i) { return i % bucketsSize; });

        forEachBucket<AccessorType>(
            indexes, [eachBucketLimit, handler = std::move(handler)](
                         size_t, typename BucketType::Ptr bucket, AccessorType& accessor) {
                size_t count = 0;
                bool needBucketContinue = true;
                bucket->template forEach<AccessorType>(
                    [&count, &needBucketContinue, eachBucketLimit, handler = std::move(handler)](
                        AccessorType& accessor) {
                        auto [needContinue, isValid] = handler(accessor);
                        needBucketContinue = needContinue;
                        if (isValid)
                        {
                            count++;
                        }
                        if (count >= eachBucketLimit)
                        {
                            return false;
                        }
                        return needContinue;
                    },
                    accessor);
                return needBucketContinue;
            });
    }

    template <class AccessorType>  // handler return isContinue
    void forEachByStartIndex(size_t startIdx, std::function<bool(AccessorType&)> handler)
    {
        size_t x = startIdx;
        size_t bucketsSize = m_buckets.size();

        auto indexes =
            ::ranges::views::iota(startIdx, startIdx + bucketsSize) |
            ::ranges::views::transform([bucketsSize](size_t i) { return i % bucketsSize; });

        forEachBucket<AccessorType>(
            indexes, [handler = std::move(handler)](
                         size_t, typename BucketType::Ptr bucket, AccessorType& accessor) {
                return bucket->template forEach<AccessorType>(handler, accessor);
            });
    }

    template <class AccessorType>  // handler return isContinue
    void forEachBucket(const auto& bucketIndexes,
        std::function<bool(size_t idx, typename BucketType::Ptr, AccessorType&)> handler)
    {
        // idx and bucket
        std::queue<std::pair<size_t, typename BucketType::Ptr>> bucket2Process;

        for (size_t idx : bucketIndexes)
        {
            bucket2Process.template emplace(idx, m_buckets[idx]);
        }

        while (!bucket2Process.empty())
        {
            auto it = std::move(bucket2Process.front());
            auto idx = it.first;
            auto& bucket = it.second;
            bucket2Process.pop();
            AccessorType accessor;
            bool needWait = bucket2Process.empty();  // wait if is last bucket
            bool acquired = bucket->acquireAccessor(accessor, needWait);
            if (acquired) [[likely]]
            {
                if (!handler(idx, bucket, accessor))
                {
                    break;
                }
            }
            else
            {
                bucket2Process.template emplace(std::move(it));
            }
        }
    }

    template <class AccessorType>  // handler return isContinue
    void forEach(const auto& objs,
        std::function<bool(decltype(objs.front()), typename BucketType::Ptr, AccessorType&)>
            handler)
    {
        auto batches = objs | ::ranges::views::chunk_by([this](const auto& a, const auto& b) {
            return this->getBucketIndex(a) == this->getBucketIndex(b);
        });

        std::queue<std::tuple<size_t, decltype(batches.front()), typename BucketType::Ptr>>
            bucket2Process;

        for (const auto& batch : batches)
        {
            AccessorType accessor;
            size_t idx = getBucketIndex(batch.front());
            auto& bucket = m_buckets[idx];
            bool acquired = bucket->acquireAccessor(accessor, false);

            if (acquired) [[likely]]
            {
                for (const auto& obj : batch)
                {
                    if (!handler(obj, bucket, accessor))
                    {
                        return;
                    }
                }
            }
            else
            {
                bucket2Process.template emplace(idx, batch, bucket);
            }
        }

        while (!bucket2Process.empty())
        {
            auto it = std::move(bucket2Process.front());
            auto idx = std::get<0>(it);
            const auto& batch = std::get<1>(it);
            auto& bucket = std::get<2>(it);
            bucket2Process.pop();

            AccessorType accessor;
            bool needWait = bucket2Process.empty();  // wait if is last bucket
            bool acquired = bucket->acquireAccessor(accessor, bucket2Process.empty());

            if (acquired) [[likely]]
            {
                for (const auto& obj : batch)
                {
                    if (!handler(obj, bucket, accessor))
                    {
                        return;
                    }
                }
            }
            else
            {
                bucket2Process.template emplace(std::move(it));
            }
        }
    }

protected:
    int getBucketIndex(const std::pair<KeyType, ValueType>& keyValue)
    {
        return getBucketIndex(keyValue.first);
    }

    int getBucketIndex(const KeyType& key)
    {
        auto hash = BucketHasher{}(key);
        return hash % m_buckets.size();
    }
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

    void batchInsert(const auto& keys,
        std::function<void(bool, const KeyType&, typename BucketSet::WriteAccessor*)> onInsert)
    {
        BucketSet::template forEach<typename BucketSet::WriteAccessor>(
            keys, [onInsert = std::move(onInsert)](const KeyType& key,
                      typename Bucket<KeyType, EmptyType>::Ptr bucket,
                      typename BucketSet::WriteAccessor& accessor) {
                bool success = bucket->insert(accessor, {key, EmptyType()});
                onInsert(success, key, success ? std::addressof(accessor) : nullptr);
                return true;
            });
    }

    void batchInsert(const auto& keys)
    {
        batchInsert(keys, [](bool, const KeyType&, typename BucketSet::WriteAccessor*) {});
    }
};

}  // namespace bcos
