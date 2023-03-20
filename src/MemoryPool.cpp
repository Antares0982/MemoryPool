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
#include <unordered_map>
#include <thread>
#include <memory_resource>
#include <atomic>
#include <utility>
#include <unordered_set>
#include <cassert>

namespace Antares::MemoryPool {
    namespace details {
        class SpinLock {
            std::atomic_flag locked = ATOMIC_FLAG_INIT;
        public:
            void lock() {
                while (locked.test_and_set(std::memory_order_acquire)) {}
            }

            void unlock() {
                locked.clear(std::memory_order_release);
            }
        };

        struct ResourceMap {
            std::unordered_map<std::thread::id, std::unique_ptr<ControlledResouce>> holder;
            SpinLock mutex;
        };

        static ResourceMap &GetResouce() {
            static ResourceMap resource;
            return resource;
        }

        static ResourceMap &GetBackupResouce() {
            static ResourceMap resource;
            return resource;
        }

        ControlledResouce *createResourceAndBindToResourceHolder(ResourceMap &resouce) {
            ControlledResouce *rtPtrPtr;

            {
                // lock this scope
                // exits fast so use spinlock
                std::lock_guard lk(resouce.mutex);
                auto &valPtrPtr = resouce.holder[std::this_thread::get_id()];
                if (valPtrPtr == nullptr) {
                    valPtrPtr = std::make_unique<ControlledResouce>();
                }
                rtPtrPtr = valPtrPtr.get();
            }

            assert(rtPtrPtr); // never nullptr
            auto &rtPtr = *rtPtrPtr;
            if (rtPtr == nullptr) rtPtr = std::make_unique<Resource>();

            return rtPtrPtr;
        }

        Resource *GetResourcePointer() {
            thread_local const auto controlledResourcePtrPtr = createResourceAndBindToResourceHolder(GetResouce());
            return controlledResourcePtrPtr->get();
        }

        Resource *GetTempResourcePointer() {
            thread_local const auto controlledResourcePtrPtr = createResourceAndBindToResourceHolder(GetBackupResouce());
            return controlledResourcePtrPtr->get();
        }
    }

    using namespace details;

    void *Malloc(size_t size, size_t align) {
        return GetResourcePointer()->allocate(size, align);
    }

    void *MallocTemp(size_t size, size_t align) {
        return GetTempResourcePointer()->allocate(size, align);
    }

    std::function<void()> &GetGCAlgorithm() {
        static std::function<void()> gc;
        return gc;
    }

    void SwapResource() {
        std::unordered_set<std::thread::id> idSetAll;
        for (auto &pair: GetResouce().holder) {
            idSetAll.insert(pair.first);
        }
        for (auto &pair: GetBackupResouce().holder) {
            idSetAll.insert(pair.first);
        }
        for (auto id: idSetAll) {
            auto &resourcePtrPtr = GetResouce().holder[id];
            if (!resourcePtrPtr) {
                resourcePtrPtr = std::make_unique<ControlledResouce>();
            }
            if (!resourcePtrPtr->get()) { // NOLINT(readability-redundant-smartptr-get)
                *resourcePtrPtr = std::make_unique<Resource>();
            }
            auto &backupResourcePtrPtr = GetBackupResouce().holder[id];
            if (!backupResourcePtrPtr) {
                backupResourcePtrPtr = std::make_unique<ControlledResouce>();
            }
            if (!backupResourcePtrPtr->get()) { // NOLINT(readability-redundant-smartptr-get)
                *backupResourcePtrPtr = std::make_unique<Resource>();
            }

            std::swap(*resourcePtrPtr, *backupResourcePtrPtr);
        }
    }

    void RegisterGC(std::function<void()> gc) {
        GetGCAlgorithm() = std::move(gc);
    }

    void GC() {
        auto &gcAlgorithm = GetGCAlgorithm();
        if (!gcAlgorithm) {
            Clean();
            return;
        }
        CleanTemp();
        SwapResource();
        gcAlgorithm();
        CleanTemp();
    }

    void CleanTemp() {
        for (auto &pair: GetBackupResouce().holder) {
            pair.second->get()->release();
        }
    }

    void Clean() {
        CleanTemp();
        for (auto &pair: GetResouce().holder) {
            pair.second->get()->release();
        }
    }
} // Antares
