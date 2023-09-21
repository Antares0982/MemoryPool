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
#include <map>
#include <stdexcept>


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
            SpinLock protectMtx; // this mutex protects per id per thread
        };

        /// @brief A pair of controlled resource pointer
        struct PairCriticalControlledResource {
            CriticalControlledResource pointers[2];
        };

        void removeRefFromPool(size_t index, ControlledResource *, bool useFront);

        /// @brief An array of paired controlled resource pointer
        struct TLSResourceArray {
            PairCriticalControlledResource resourceArray[MAX_MEMORYPOOL_COUNT];

            auto begin() {
                return resourceArray;
            }

            auto end() {
                return resourceArray + MAX_MEMORYPOOL_COUNT;
            }

            ~TLSResourceArray() {
                for (size_t i = 0; i < MAX_MEMORYPOOL_COUNT; ++i) {
                    auto &pair = resourceArray[i];
                    removeRefFromPool(i, &pair.pointers[0].resource, true);
                    removeRefFromPool(i, &pair.pointers[1].resource, false);
                }
            }

            PairCriticalControlledResource &operator[](size_t index) {
                return resourceArray[index];
            }

            const PairCriticalControlledResource &operator[](size_t index) const {
                return resourceArray[index];
            }
        };

        struct ResourceMap {
            using ThreadResourcesCollectionMap = std::unordered_set<ControlledResource *>;
            struct CriticalMap {
                ThreadResourcesCollectionMap resourceMap;
                SpinLock collectionMutex;
            };
            CriticalMap resourceMaps[2];
            std::function<void()> gc;

            void BindResource(ControlledResource *resource, bool useFront) {
                auto &useResourceMap = useFront ? resourceMaps[0] : resourceMaps[1];
                auto &useMutex = useResourceMap.collectionMutex;
                std::lock_guard lk(useMutex);
                useResourceMap.resourceMap.insert(resource);
            }

            void RemoveResource(ControlledResource *resource, bool useFront) {
                auto &useResourceMap = useFront ? resourceMaps[0] : resourceMaps[1];
                auto &useMutex = useResourceMap.collectionMutex;
                std::lock_guard lk(useMutex);
                useResourceMap.resourceMap.erase(resource);
            }

            void CleanAll() {
                for (auto &rMap: resourceMaps) {
                    for (auto resource: rMap.resourceMap) {
                        resource->reset();
                    }
                }
            }

            void CleanTemp(bool useFront) {
                auto &tempResourceMap = useFront ? resourceMaps[1] : resourceMaps[0];
                for (auto resource: tempResourceMap.resourceMap) {
                    resource->reset();
                }
            }

            std::function<void()> &GetGCAlgorithm() {
                return gc;
            }
        };

        struct ResourceIdCollection {
            std::map<unsigned int, ResourceMap *> resourceCollection;
            std::mutex mutex;
        };

        static ResourceIdCollection &GetResourceCollection() {
            static ResourceIdCollection resourceCollection;
            return resourceCollection;
        }

        void removeRefFromPool(size_t index, ControlledResource *resource, bool useFront) {
            auto &resourceIdCollection = GetResourceCollection();
            auto &collection = resourceIdCollection.resourceCollection;
            auto &mutex = resourceIdCollection.mutex;
            std::lock_guard lk(mutex);
            auto it = collection.find(index);
            if (it == collection.end()) {
                return;
            }
            auto &resourceMap = it->second;
            resourceMap->RemoveResource(resource, useFront);
        }

        unsigned int QueryFreeMemoryPoolId(ResourceMap *resourceMap) {
            auto &resourceIdCollection = GetResourceCollection();
            auto &collection = resourceIdCollection.resourceCollection;
            auto &mutex = resourceIdCollection.mutex;
            {
                std::lock_guard lk(mutex);
                if (collection.empty()) {
                    collection.insert(std::make_pair(0, resourceMap));
                    return 0;
                }
                if (collection.size() == 1) {
                    auto result = (collection.begin()->first == 0) ? 1 : 0;
                    collection.insert(std::make_pair(result, resourceMap));
                    return result;
                }
                if (collection.size() == MAX_MEMORYPOOL_COUNT) {
                    throw std::runtime_error("Too many memory pools created");
                }
                auto result = collection.rbegin()->first + 1;
                if (result == MAX_MEMORYPOOL_COUNT) {
                    // under this case, we have to run over the whole collection to find a free id
                    auto it = collection.begin();
                    unsigned int lastKnown = it->first;
                    ++it;
                    for (; it != collection.end(); ++it) {
                        unsigned int current = it->first;
                        if (current - lastKnown > 1) {
                            result = lastKnown + 1;
                            collection.insert(std::make_pair(result, resourceMap));
                            return result;
                        }
                        lastKnown = current;
                    }
                } else {
                    collection.insert(std::make_pair(result, resourceMap));
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

        TLSResourceArray &GetThreadLocalResourcePointerArray() {
            return tls_get<TLSResourceArray>();
        }

        template<MemoryPool::AllocatePolicy P = MemoryPool::AllocatePolicy::Default>
        Resource *GetResourcePointer(ResourceMap *bindingResourceMap, unsigned int id, bool useFront) {
            auto &resourcePointerArray = GetThreadLocalResourcePointerArray();
            auto &resourcesPair = resourcePointerArray[id];
            if constexpr (P == MemoryPool::AllocatePolicy::Temporary) {
                useFront = !useFront;
            }
            auto &currentCriticalResource = useFront ? resourcesPair.pointers[0] : resourcesPair.pointers[1];
            auto rawResource = currentCriticalResource.resource.get();
            if (nullptr == rawResource) [[unlikely]] {// these codes only run once per memory pool per thread
                std::lock_guard lk(currentCriticalResource.protectMtx); // this exits fast, so use spin lock
                if (nullptr == currentCriticalResource.resource) {
                    currentCriticalResource.resource = std::make_unique<Resource>();
                    bindingResourceMap->BindResource(&currentCriticalResource.resource, useFront);
                }
                rawResource = currentCriticalResource.resource.get();
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
            id(QueryFreeMemoryPoolId(pImpl.get())) {
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
