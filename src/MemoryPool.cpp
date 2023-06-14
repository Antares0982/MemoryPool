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
#include <thread>
#include <memory_resource>
#include <atomic>
#include <utility>
#include <unordered_set>
#include <cassert>
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
                while (locked.test(std::memory_order_acquire)) {}
            }
        };

        /// @brief A controlled resource, which is trivially destructible
        struct CriticalControlledResourcePointer {
            ControlledResouce *resource = nullptr;
            SpinLock protectMtx; // this mutex protects per id per thread
        };

        /// @brief A pair of controlled resource pointer, which is trivially destructible
        struct PairCriticalControlledResourcePointer {
            CriticalControlledResourcePointer pointers[2];
            std::mutex pairMutex;
        };

        /// @brief An array of controlled resource pointer, which is trivially destructible
        typedef PairCriticalControlledResourcePointer ControlledResourceArray[MAX_MEMORYPOOL_COUNT];

        struct ResourceMap {
            using ThreadResourcesCollectionMap = std::unordered_set<ControlledResouce *>;

            ThreadResourcesCollectionMap resourceMap[2];
            SpinLock collectionMutex[2]; // this mutex protects resource per id
            std::function<void()> gc;

            void BindResource(ControlledResouce *resource, bool useFront) {
                auto &useResourceMap = useFront ? resourceMap[0] : resourceMap[1];
                auto &useMutex = useFront ? collectionMutex[0] : collectionMutex[1];
                std::lock_guard lk(useMutex);
                useResourceMap.insert(resource);
            }

            void CleanAll() {
                for (auto &rMap: resourceMap) {
                    for (auto resource: rMap) {
                        resource->reset();
                    }
                }
            }

            void CleanTemp(bool useFront) {
                auto &tempResourceMap = useFront ? resourceMap[1] : resourceMap[0];
                for (auto resource: tempResourceMap) {
                    resource->reset();
                }
            }

            std::function<void()> &GetGCAlgorithm() {
                return gc;
            }
        };

        /// @brief A helper class to destroy resources when program exits. Mainly for bullshit MinGW
        class ThreadLocalResourceDestructionHelper {
            std::forward_list<ControlledResouce> resourceList;
            SpinLock listMutex;

        public:
            std::pair<ControlledResouce *, ControlledResouce *> GetTwoNewResources() {
                std::lock_guard lk(listMutex);
                resourceList.emplace_front();
                auto result1 = &resourceList.front();
                resourceList.emplace_front();
                auto result2 = &resourceList.front();
                return {result1, result2};
            }
        };

        struct ResourceIdCollection {
            std::set<unsigned int> resourceCollection;
            std::mutex mutex;
        };

        static ResourceIdCollection &GetResourceCollection() {
            static ResourceIdCollection resourceCollection;
            return resourceCollection;
        }

        static ThreadLocalResourceDestructionHelper &GetThreadLocalResourceDestructionHelper() {
            static ThreadLocalResourceDestructionHelper helper;
            return helper;
        }

        unsigned int QueryFreeMemoryPoolId() {
            auto &resourceIdCollection = GetResourceCollection();
            auto &collection = resourceIdCollection.resourceCollection;
            auto &mutex = resourceIdCollection.mutex;
            {
                std::lock_guard lk(mutex);
                if (collection.empty()) {
                    collection.insert(0);
                    return 0;
                }
                if (collection.size() == 1) {
                    auto result = (*collection.begin() == 0) ? 1 : 0;
                    collection.insert(result);
                    return result;
                }
                if (collection.size() == MAX_MEMORYPOOL_COUNT) {
                    throw std::runtime_error("Too many memory pools created");
                }
                auto result = *collection.rbegin() + 1;
                if (result == MAX_MEMORYPOOL_COUNT) {
                    // under this case, we have to run over the whole collection to find a free id
                    auto it = collection.begin();
                    unsigned int lastKnown = *it;
                    ++it;
                    for (; it != collection.end(); ++it) {
                        unsigned int current = *it;
                        if (current - lastKnown > 1) {
                            result = lastKnown + 1;
                            collection.insert(result);
                            return result;
                        }
                        lastKnown = current;
                    }
                } else {
                    collection.insert(result);
                    return result;
                }
            }
            throw std::runtime_error("Too many memory pools created");
        }

        void FreeMemoryPoolId(unsigned int id) {
            auto &resourceIdCollection = GetResourceCollection();
            auto &collection = resourceIdCollection.resourceCollection;
            auto &mutex = resourceIdCollection.mutex;
            std::lock_guard lk(mutex);
            collection.erase(id);
        }

        PairCriticalControlledResourcePointer *GetThreadLocalResourcePointerArray() {
            thread_local ControlledResourceArray controlledResourceArray{};
            return controlledResourceArray;
        }

        template<MemoryPool::AllocatePolicy P = MemoryPool::AllocatePolicy::Default>
        Resource *GetResourcePointer(ResourceMap *bindingResourceMap, unsigned int id, bool useFront) {
            auto resourcePointerArray = GetThreadLocalResourcePointerArray();
            auto &resourcesPair = resourcePointerArray[id];
            if constexpr (P == MemoryPool::AllocatePolicy::Temporary) {
                useFront = !useFront;
            }
            auto &currentCriticalResource = useFront ? resourcesPair.pointers[0] : resourcesPair.pointers[1];
            if (currentCriticalResource.resource ==
                nullptr) [[unlikely]] { // these codes only run once per thread per id
                std::lock_guard lk(resourcesPair.pairMutex);
                if (currentCriticalResource.resource == nullptr) {
                    auto &anotherCriticalResource = useFront ? resourcesPair.pointers[1]
                                                             : resourcesPair.pointers[0];
                    assert(anotherCriticalResource.resource == nullptr);
                    auto [resource1, resource2] = GetThreadLocalResourceDestructionHelper().GetTwoNewResources();
                    currentCriticalResource.resource = resource1;
                    anotherCriticalResource.resource = resource2;
                }
                assert(currentCriticalResource.resource != nullptr);
            }
            auto rawResource = currentCriticalResource.resource->get();
            if (rawResource == nullptr) [[unlikely]] {// these codes only run once per memory pool per thread
                std::lock_guard lk(currentCriticalResource.protectMtx); // this exits fast, so use spin lock
                if (nullptr == currentCriticalResource.resource->get()) {
                    auto &controlledResource = *currentCriticalResource.resource;
                    controlledResource = std::make_unique<Resource>();
                    bindingResourceMap->BindResource(currentCriticalResource.resource, useFront);
                }
                rawResource = currentCriticalResource.resource->get();
                assert(rawResource != nullptr);
            }
            return rawResource;
        }

        void *Malloc(MemoryPool *inPool, size_t size, size_t align) {
            return inPool->Malloc(size, align);
        }

        void *MallocTemp(MemoryPool *inPool, size_t size, size_t align) {
            return inPool->MallocTemp(size, align);
        }
    }

    using namespace details;

    MemoryPool::MemoryPool() :
            pImpl(std::make_unique<ResourceMap>()),
            id(QueryFreeMemoryPoolId()) {
    }

    MemoryPool::~MemoryPool() {
        pImpl->CleanAll();
        FreeMemoryPoolId(id);
    }

    void *MemoryPool::Malloc(size_t size, size_t align) const {
        return details::GetResourcePointer(pImpl.get(), id, useFront)->allocate(size, align);
    }

    void *MemoryPool::MallocTemp(size_t size, size_t align) const {
        return details::GetResourcePointer<Temporary>(pImpl.get(), id, useFront)->allocate(size, align);
    }

    void MemoryPool::RegisterGC(std::function<void()> gc) {
        pImpl->GetGCAlgorithm() = std::move(gc);
    }

    //
    void MemoryPool::GC() {
        auto &gcAlgorithm = pImpl->GetGCAlgorithm();
        if (!gcAlgorithm) {
            pImpl->CleanAll();
            return;
        }
        CleanTemp();
        useFront = !useFront;
        gcAlgorithm();
        CleanTemp();
    }

    //
    void MemoryPool::CleanTemp() {
        pImpl->CleanTemp(useFront);
    }

    //
    void MemoryPool::Clean() {
        pImpl->CleanAll();
    }

    details::Resource *MemoryPool::GetResourcePointer() {
        return details::GetResourcePointer(pImpl.get(), id, useFront);
    }

    details::Resource *MemoryPool::GetTempResourcePointer() {
        return details::GetResourcePointer<Temporary>(pImpl.get(), id, useFront);
    }
} // Antares
