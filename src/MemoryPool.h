//
// Created by antares on 3/15/23.
//

#ifndef MEMORYPOOL_MEMORYPOOL_H
#define MEMORYPOOL_MEMORYPOOL_H

#include <functional>
#include <memory_resource>
#include <memory>

namespace Antares::MemoryPool {
    using Resource = std::pmr::monotonic_buffer_resource;
    using ControlledResouce = std::unique_ptr<Resource>;

    template<typename T= std::byte>
    using Allocator = std::pmr::polymorphic_allocator<T>;

    [[nodiscard]] void *Malloc(size_t size, size_t align);

    [[nodiscard]] void *MallocTemp(size_t size, size_t align);

    constexpr void Free(void *) {}

    ControlledResouce *GetResoucePointer();

    ControlledResouce *GetTempResoucePointer();

    template<typename T= std::byte>
    Allocator<T> GetAllocator() {
        return Allocator<T>(GetResoucePointer());
    }

    template<typename T = std::byte>
    Allocator<T> GetTempAllocator() {
        return Allocator<T>(GetTempResoucePointer());
    }

    template<typename T, typename ... Args>
    [[nodiscard]] T *New(Args &&... args) {
        auto ptr = Malloc(sizeof(T), alignof(T));
        return new(ptr) T(std::forward<Args>(args)...);
    }

    template<typename T>
    [[nodiscard]] T *NewArray(size_t size) {
        auto ptr = Malloc(sizeof(T) * size, alignof(T));
        return new(ptr) T[size];
    }

    template<typename T, typename ... Args>
    [[nodiscard]] T *NewTemp(Args &&... args) {
        auto ptr = MallocTemp(sizeof(T), alignof(T));
        return new(ptr) T(std::forward<Args>(args)...);
    }

    template<typename T>
    [[nodiscard]] T *NewTempArray(size_t size) {
        auto ptr = MallocTemp(sizeof(T) * size, alignof(T));
        return new(ptr) T[size];
    }

    template<typename T>
    void Delete(T *ptr) {
        ptr->~T();
    }

    void RegisterGC(std::function<void()> gc);

    /// NOT thread safe
    /// Please freeze all memory allocation before calling it
    void GC();

    /// NOT thread safe
    /// Please freeze all memory allocation before calling it
    void CleanTemp();

    /// NOT thread safe
    /// Please freeze all memory allocation before calling it
    void Clean();
} // Antares

#endif //MEMORYPOOL_MEMORYPOOL_H
