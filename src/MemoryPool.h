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

#ifndef MEMORYPOOL_MEMORYPOOL_H
#define MEMORYPOOL_MEMORYPOOL_H

#include <functional>
#include <memory_resource>
#include <memory>

namespace Antares::MemoryPool {
    namespace details {
        using Resource = std::pmr::monotonic_buffer_resource;
        using ControlledResouce = std::unique_ptr<Resource>;

        ControlledResouce *GetResoucePointer();

        ControlledResouce *GetTempResoucePointer();
    }

    /// @brief Allocator meets the C++20 MemoryResource requirements.
    template<typename T= std::byte>
    using Allocator = std::pmr::polymorphic_allocator<T>;

    /// @brief malloc a memory block. Thread safe.
    [[nodiscard]] void *Malloc(size_t size, size_t align = sizeof(void *));

    /// @brief malloc a temp memory block. Thread safe.
    [[nodiscard]] void *MallocTemp(size_t size, size_t align = sizeof(void *));

    /// @brief Only for memory traits compatibility. Literally do nothing.
    constexpr void Free(void *) {}

    /// @brief Get an allocator object for containers.
    template<typename T= std::byte>
    Allocator<T> GetAllocator() {
        return Allocator<T>(details::GetResoucePointer());
    }

    /// @brief Get a temporary allocator object for containers.
    template<typename T = std::byte>
    Allocator<T> GetTempAllocator() {
        return Allocator<T>(details::GetTempResoucePointer());
    }

    /// @brief Create a new object. Thread safe.
    template<typename T, typename ... Args>
    [[nodiscard]] T *New(Args &&... args) {
        auto ptr = Malloc(sizeof(T), alignof(T));
        return new(ptr) T(std::forward<Args>(args)...);
    }

    /// @brief Create a new array. Thread safe.
    template<typename T>
    [[nodiscard]] T *NewArray(size_t size) {
        auto ptr = Malloc(sizeof(T) * size, alignof(T));
        return new(ptr) T[size];
    }

    /// @brief Create a temporary object, which cannot be retrieved by GC function. Thread safe.
    template<typename T, typename ... Args>
    [[nodiscard]] T *NewTemp(Args &&... args) {
        auto ptr = MallocTemp(sizeof(T), alignof(T));
        return new(ptr) T(std::forward<Args>(args)...);
    }

    /// @brief Create a temporary array, which cannot be retrieved by GC function. Thread safe.
    template<typename T>
    [[nodiscard]] T *NewTempArray(size_t size) {
        auto ptr = MallocTemp(sizeof(T) * size, alignof(T));
        return new(ptr) T[size];
    }

    /// @brief Delete an object
    template<typename T>
    void Delete(T *ptr) {
        ptr->~T();
    }

    /// @brief Register a GC function
    /// @param gc The GC function
    /// @note The GC function will be called when GC() is called.
    ///  When GC() happens, all the temp memories will be freed first, and all
    ///  memories allocated before will be freed right after the registered gc function is called.
    ///  So gc function should use New() to copy all necessary data and call Delete() for old objects,
    ///  before the memories become invalid.
    void RegisterGC(std::function<void()> gc);

    /// @brief Start a GC.
    /// @note NOT thread safe. First GC() cleans all temp memories, and then call the registered GC function.
    ///  After that, all memories allocated before calling GC() will be freed.
    ///  If no gc function is registered, all memories will be freed.
    void GC();

    /// @brief Clean all temp memories.
    /// @note NOT thread safe.
    ///  Please freeze all temp memory allocation before calling it.
    void CleanTemp();

    /// @brief Clean all memories.
    /// @note NOT thread safe.
    ///  Please freeze all memory allocation before calling it.
    void Clean();
} // Antares

#endif //MEMORYPOOL_MEMORYPOOL_H
