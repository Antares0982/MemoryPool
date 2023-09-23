//
// Created by antares on 3/15/23.
//
// MIT License
//
// Copyright (c) 2023 Antares
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "MemoryPool.h"
#include "tls_wrapper.h"
#include <thread>
#include <memory_resource>
#include <atomic>
#include <utility>
#include <unordered_set>
#include <cassert>
#include <stdexcept>
#include <iostream>
#include <set>


namespace Antares {
    namespace details {
        /// @brief A simple spin lock
        class SpinLock {
            std::atomic_flag locked = ATOMIC_FLAG_INIT;
        public:
            void lock() {
                while (locked.test_and_set(std::memory_order_acquire)) {}
            }

            void unlock() {
                locked.clear(std::memory_order_release);
            }

            bool try_lock() {
                return !locked.test_and_set(std::memory_order_acquire);
            }

            void wait() {
#if __cplusplus <= 201703L
                while (locked.test_and_set(std::memory_order_acquire)) {}
#else
                while (locked.test(std::memory_order_acquire)) {}
#endif
            }
        };

        /// @brief A controlled resource
        struct CriticalControlledResource {
            ControlledResource resource = nullptr;
            std::mutex protectMtx;

            void resource_create() {
                std::lock_guard lk(protectMtx);
                if (nullptr == resource) {
                    resource = std::make_unique<Resource>();
#ifdef  _DEBUG
                    debug::totalBufferObj.fetch_add(1, std::memory_order_relaxed);
#endif
                    std::cout << "Resource obj create" << std::endl;
                }
            }

            void resource_release() {
                std::lock_guard lk(protectMtx);
                if (nullptr != resource) {
#ifdef  _DEBUG
                    debug::totalBufferObj.fetch_sub(1, std::memory_order_relaxed);
#endif
                    std::cout << "Resource obj dec" << std::endl;
                    resource.reset();
                }
            }

            [[nodiscard]] auto get() const {
                return resource.get();
            }

            ~CriticalControlledResource() {
                if (resource) resource_release();
            }
        };

        /// @brief A pair of controlled resource pointer
        struct PairCriticalControlledResource {
            CriticalControlledResource pointers[2];
        };

        struct TLSResourceArray;

        struct TLSResourceArrayCollection {
            std::unordered_set<TLSResourceArray *> tlsResourceArraySet;
            SpinLock collectionMutex;

            void insert(TLSResourceArray *array) {
                std::lock_guard lk(collectionMutex);
                tlsResourceArraySet.insert(array);
            }

            void erase(TLSResourceArray *array) {
                std::lock_guard lk(collectionMutex);
                tlsResourceArraySet.erase(array);
            }

            void apply(const std::function<void(TLSResourceArray *)> &func) {
                std::lock_guard lk(collectionMutex);
                for (auto array: tlsResourceArraySet) {
                    func(array);
                }
            }
        };

        static TLSResourceArrayCollection &GetTLSResourceArraySet() {
            static TLSResourceArrayCollection tlsResourceArraySet;
            return tlsResourceArraySet;
        }

        /// @brief An array of paired controlled resource pointer
        struct TLSResourceArray {
            PairCriticalControlledResource resourceArray[MAX_MEMORYPOOL_COUNT];

            TLSResourceArray() {
                GetTLSResourceArraySet().insert(this);
            }

            auto begin() {
                return resourceArray;
            }

            auto end() {
                return resourceArray + MAX_MEMORYPOOL_COUNT;
            }

            ~TLSResourceArray() {
                GetTLSResourceArraySet().erase(this);
                for (auto &pair: resourceArray) {
                    for (auto &resource: pair.pointers) {
                        resource.resource_release();
                    }
                }
            }

            PairCriticalControlledResource &operator[](size_t index) {
                return resourceArray[index];
            }

            const PairCriticalControlledResource &operator[](size_t index) const {
                return resourceArray[index];
            }
        };

        TLSResourceArray &GetThreadLocalResourcePointerArray() {
            return tls_get<TLSResourceArray>();
        }

        template<typename T, typename = std::enable_if_t<std::is_unsigned_v<T> && std::is_same_v<T, std::decay_t<T>>>>
        struct ResourceIdCollection {
            std::unordered_set<T> ids;
            std::set<T> backupIds;
            std::mutex mutex;

            ResourceIdCollection() {
                for (T i = 0; i < MAX_MEMORYPOOL_COUNT; ++i) {
                    backupIds.insert(i);
                }
            }

            T GetId() {
                std::lock_guard lk(mutex);
                if (backupIds.empty()) {
                    throw std::runtime_error("Too many memory pools created");
                }
                auto it = backupIds.begin();
                auto result = *it;
                backupIds.erase(it);
                ids.insert(result);
                return result;
            }

            void FreeId(T id) {
                std::lock_guard lk(mutex);
                auto it = ids.find(id);
                if (it == ids.end()) {
                    return;
                }
                ids.erase(it);
                backupIds.insert(id);
            }
        };

        using ResourceIdType = unsigned int;

        ResourceIdCollection<ResourceIdType> &GetResourceCollection() {
            static ResourceIdCollection<ResourceIdType> resourceCollection;
            return resourceCollection;
        }

        ResourceIdType QueryFreeMemoryPoolId() {
            return GetResourceCollection().GetId();
        }

        void FreeMemoryPoolId(ResourceIdType id) {
            GetResourceCollection().FreeId(id);
        }

        template<MemoryPool::AllocatePolicy P = MemoryPool::AllocatePolicy::Default>
        Resource *GetResourcePointer(ResourceIdType id, bool useFront) {
            //
            auto &resourcesPair = GetThreadLocalResourcePointerArray()[id];
            //
            if constexpr (P == MemoryPool::AllocatePolicy::Temporary) {
                useFront = !useFront;
            }
            //
            auto &currentCriticalResource = useFront ? resourcesPair.pointers[0] : resourcesPair.pointers[1];
            auto rawResource = currentCriticalResource.get();
            //
            if (nullptr == rawResource) [[unlikely]] {// these codes only run once per memory pool per thread
                currentCriticalResource.resource_create();
                rawResource = currentCriticalResource.get();
            }
            return rawResource;
        }

        void CleanAll(ResourceIdType id) {
            GetTLSResourceArraySet().apply([id](TLSResourceArray *array) {
                auto &resourcesPair = (*array)[id];
                for (auto &resource: resourcesPair.pointers) {
                    resource.resource_release();
                }
            });
        }

        void CleanTemp(ResourceIdType id, bool useFront) {
            GetTLSResourceArraySet().apply([id, useFront](TLSResourceArray *array) {
                auto &resourcesPair = (*array)[id];
                auto &tempResource = resourcesPair.pointers[useFront]; // true -> 1
                tempResource.resource_release();
            });
        }

        /// -------------------------------------------------------------
        void *Malloc(MemoryPool *inPool, size_t size, size_t align) {
            return inPool->Malloc(size, align);
        }

        void *MallocTemp(MemoryPool *inPool, size_t size, size_t align) {
            return inPool->MallocTemp(size, align);
        }
        /// -------------------------------------------------------------
    }

    using namespace details;

    MemoryPool::MemoryPool() :
            id(QueryFreeMemoryPoolId()) {
    }

    MemoryPool::~MemoryPool() {
        Clean();
        FreeMemoryPoolId(id);
    }

    void *MemoryPool::Malloc(size_t size, size_t align) const {
#ifdef  _DEBUG
        auto mod = size % align;
        if (mod != 0) {
            debug::totalBytes.fetch_add(size + align - mod);
        } else {
            debug::totalBytes.fetch_add(size);
        }
#endif
        return details::GetResourcePointer(id, useFront)->allocate(size, align);
    }

    void *MemoryPool::MallocTemp(size_t size, size_t align) const {
#ifdef  _DEBUG
        auto mod = size % align;
        if (mod != 0) {
            debug::totalBytes.fetch_add(size + align - mod);
        } else {
            debug::totalBytes.fetch_add(size);
        }
#endif
        return details::GetResourcePointer<Temporary>(id, useFront)->allocate(size, align);
    }

    void MemoryPool::RegisterGC(std::function<void()> gc) {
        gc_func = std::move(gc);
    }

    //
    void MemoryPool::GC() {
        if (!gc_func) {
            Clean();
            return;
        }
        CleanTemp();
        useFront = !useFront;
        gc_func();
        CleanTemp();
    }

    //
    void MemoryPool::CleanTemp() {
        details::CleanTemp(id, useFront);
    }

    //
    void MemoryPool::Clean() {
        details::CleanAll(id);
    }

    details::Resource *MemoryPool::GetResourcePointer() {
        return details::GetResourcePointer(id, useFront);
    }

    details::Resource *MemoryPool::GetTempResourcePointer() {
        return details::GetResourcePointer<Temporary>(id, useFront);
    }
} // Antares
