//
// Created by antares on 3/15/23.
//

#include "MemoryPool.h"
#include <unordered_map>
#include <thread>
#include <memory_resource>
#include <atomic>
#include <utility>
#include <unordered_set>
#include <cassert>

namespace Antares ::MemoryPool {
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
        SpinLock mutex;
        std::unordered_map<std::thread::id, std::unique_ptr<ControlledResouce>> holder;
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

    ControlledResouce *GetResoucePointer() {
        thread_local auto controlledResourcePtrPtr = createResourceAndBindToResourceHolder(GetResouce());
        return controlledResourcePtrPtr;
    }

    ControlledResouce *GetTempResoucePointer() {
        thread_local auto controlledResourcePtrPtr = createResourceAndBindToResourceHolder(GetBackupResouce());
        return controlledResourcePtrPtr;
    }

    void *Malloc(size_t size, size_t align) {
        return GetResoucePointer()->get()->allocate(size, align);
    }

    void *MallocTemp(size_t size, size_t align) {
        return GetTempResoucePointer()->get()->allocate(size, align);
    }

    std::function<void()> &GetGCAlgorithm() {
        static std::function<void()> gc;
        return gc;
    }

    /// NOT thread safe
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

    /// NOT thread safe
    void GC() {
        CleanTemp();
        SwapResource();
        auto &gcAlgorithm = GetGCAlgorithm();
        if (gcAlgorithm) {
            gcAlgorithm();
        }
        CleanTemp();
    }

    /// NOT thread safe
    void CleanTemp() {
        for (auto &pair: GetBackupResouce().holder) {
            pair.second->get()->release();
        }
    }

    /// NOT thread safe
    void Clean() {
        for (auto &pair: GetResouce().holder) {
            pair.second->get()->release();
        }
        CleanTemp();
    }
} // Antares