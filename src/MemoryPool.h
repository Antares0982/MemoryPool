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

        Resource *GetResourcePointer();

        Resource *GetTempResourcePointer();

        template<typename T, typename = std::enable_if_t<!std::is_void_v<T>>>
        constexpr void ConstructArray(T *where, size_t size) {
            for (size_t i = 0; i < size; ++i) {
                new(where + i) T();
            }
        }

        template<typename T, typename = std::enable_if_t<!std::is_void_v<T>>>
        constexpr void ConstructArray(T *where, size_t size, const T &prototype) {
            for (size_t i = 0; i < size; ++i) {
                new(where + i) T(prototype);
            }
        }

        template<typename T, typename Trait>
        struct Allocator : public std::allocator<T> {
            Allocator() = default;

            template<typename T2>
            Allocator(const Allocator<T2, Trait> &) {} // NOLINT(google-explicit-constructor)

            template<typename T2>
            Allocator(Allocator<T2, Trait> &&) noexcept {} // NOLINT(google-explicit-constructor) }

            ~Allocator() = default;

            [[nodiscard]] T *allocate(size_t n
#if __cplusplus <= 201703L
                    const void* hint = nullptr
#endif
            ) {
                auto result = (T *) Trait::Malloc(n * sizeof(T));
                if (!result)std::__throw_bad_array_new_length();
                return result;
            }

#if __cplusplus >= 202106L
            std::allocation_result<T*, std::size_t> allocate_at_least( std::size_t n ){
            auto result = allocate(n);
            return {result, n};
        }
#endif

            constexpr void deallocate(T *, std::size_t) {}
        };
    }


    /// @brief malloc a memory block. Thread safe.
    [[nodiscard]] void *Malloc(size_t size, size_t align = sizeof(void *));

    /// @brief malloc a temp memory block. Thread safe.
    [[nodiscard]] void *MallocTemp(size_t size, size_t align = sizeof(void *));

    /// @brief Only for memory traits compatibility. Literally do nothing.
    constexpr void Free(void *) {}

    namespace details {
        struct DefaultTrait {
            static void *Malloc(size_t size, size_t align = sizeof(void *)) {
                return ::Antares::MemoryPool::Malloc(size, align);
            }

            static void Free(void *) {
            }
        };

        struct TempTrait {
            static void *Malloc(size_t size, size_t align = sizeof(void *)) {
                return MallocTemp(size, align);
            }

            static void Free(void *) {}
        };
    }

    enum AllocatePolicy {
        Default,
        Temporary,
    };

    template<AllocatePolicy P = Default>
    void *MallocWithPolicy(size_t size, size_t align = sizeof(void *)) {
        if constexpr (P == Default)
            return Malloc(size, align);
        else if constexpr (P == Temporary)
            return MallocTemp(size, align);
        else
            static_assert(P == Default || P == Temporary, "Unknown policy");
    }

    /// @brief Allocator meets the standard `Allocator` requirements. Thread safe.
    template<typename T>
    using Allocator = details::Allocator<T, details::DefaultTrait>;

    /// @brief Temporary allocator meets the standard `Allocator` requirements. Thread safe.
    template<typename T>
    using TempAllocator = details::Allocator<T, details::TempTrait>;

    /// @brief Allocator allocates memory thread locally. Usually faster, but not thread safe.
    template<typename T>
    using ThreadLocalAllocator = std::pmr::polymorphic_allocator<T>;

    /// @brief Get a thread local allocator object for containers.
    template<typename T, AllocatePolicy P = Default>
    auto GetThreadLocalAllocator() {
        if constexpr (P == Default)
            return ThreadLocalAllocator<T>(details::GetResourcePointer());
        else
            return ThreadLocalAllocator<T>(details::GetTempResourcePointer());
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
        auto ptr = (T *) Malloc(sizeof(T) * size, alignof(T));
        details::ConstructArray(ptr, size);
        return ptr;
    }

    /// @brief Create a new array using prototype. Thread safe.
    template<typename T>
    [[nodiscard]] T *NewArray(size_t size, const T &prototype) {
        auto ptr = (T *) Malloc(sizeof(T) * size, alignof(T));
        details::ConstructArray(ptr, size, prototype);
        return ptr;
    }

    /// @brief Create a temporary object, which can be deallocated right before GC. Thread safe.
    template<typename T, typename ... Args>
    [[nodiscard]] T *NewTemp(Args &&... args) {
        auto ptr = MallocTemp(sizeof(T), alignof(T));
        return new(ptr) T(std::forward<Args>(args)...);
    }

    /// @brief Create a temporary array, which will be deleted right before GC. Thread safe.
    template<typename T>
    [[nodiscard]] T *NewTempArray(size_t size) {
        auto ptr = (T *) MallocTemp(sizeof(T) * size, alignof(T));
        details::ConstructArray(ptr, size);
        return ptr;
    }

    /// @brief Create a temporary array with given prototype, which will be deleted right before GC. Thread safe.
    template<typename T>
    [[nodiscard]] T *NewTempArray(size_t size, const T &prototype) {
        auto ptr = (T *) MallocTemp(sizeof(T) * size, alignof(T));
        details::ConstructArray(ptr, size, prototype);
        return ptr;
    }

    /// @brief Construct an object at given address
    template<typename T, typename ... Args>
    void AllocateAt(T *ptr, Args &&... args) {
        new(ptr) T(std::forward<Args>(args)...);
    }

    /// @brief Delete an object
    template<typename T>
    void Delete(T *ptr) {
        ptr->~T();
    }

    /// @brief Delete an array with given length
    template<typename T>
    void DeleteArray(T *ptr, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            (ptr + i)->~T();
        }
    }

    /// @brief Deleter for std::unique_ptr
    /// @note there is no deleter for array
    template<typename T>
    struct Deleter {
        constexpr void operator()(T *ptr) { DeleteObject(ptr); }
    };

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
