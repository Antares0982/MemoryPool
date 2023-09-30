//
// Created by Antares on 2023/9/16.
//

#ifndef THREAD_LOCAL_WRAPPER_THL_WRAPPER_H
#define THREAD_LOCAL_WRAPPER_THL_WRAPPER_H

#ifdef WORKAROUND_BROKEN_MINGW_TLS

#include <unordered_map>
#include <mutex>
#include <utility>


namespace Antares {
    template<typename T>
    class ThreadLocalWrapper;

    template<typename T>
    class ThreadLocalManager {
        friend class ThreadLocalWrapper<T>;

        std::unordered_map<void *, T *> threadLocalObjects;
        std::mutex mtx;

        static auto &get() {
            static ThreadLocalManager mgr;
            return mgr;
        };

        size_t erase(void *key) {
            std::lock_guard<std::mutex> lock(mtx);
            return threadLocalObjects.erase(key);
        }

        auto insert(void *key, T *val) {
            std::lock_guard<std::mutex> lock(mtx);
            return threadLocalObjects.insert({key, val});
        }
    };

    template<typename T>
    class ThreadLocalWrapper {
        T *object = nullptr;

        ThreadLocalWrapper() = default;

        explicit ThreadLocalWrapper(T *in_ptr) : object(in_ptr) {
            ThreadLocalManager<T>::get().insert((void *) this, object);
        }

    public:
        ThreadLocalWrapper(const ThreadLocalWrapper &) = delete;

        ThreadLocalWrapper(ThreadLocalWrapper &&) = delete;

        template<typename ...Args>
        static ThreadLocalWrapper make(Args &&...args) {
            auto object = new T(std::forward<Args>(args)...);
            return ThreadLocalWrapper(object);
        }

        /// for MinGW compatibility, do not access `object` directly
        ~ThreadLocalWrapper() {
            auto key = (void *) this;
            auto &manager = ThreadLocalManager<T>::get();
            manager.erase(key);
            delete object;
        }

        T *operator->() {
            return object;
        }

        const T *operator->() const {
            return object;
        }

        T *get() {
            return object;
        }

        [[nodiscard]] const T *get() const {
            return object;
        }

        T &operator*() {
            return *object;
        }

        [[nodiscard]] const T &operator*() const {
            return *object;
        }

        auto &operator=(const ThreadLocalWrapper &) = delete;

        auto &operator=(const T &val) {
            *object = val;
            return *this;
        }

        auto &operator=(T &&val) {
            *object = std::move(val);
            return *this;
        }
    };

    template<typename T>
    auto &tls_get() {
        thread_local auto ptr = ThreadLocalWrapper<T>::make();
        return *ptr;
    }
}

#else

namespace Antares {
    template<typename T>
    auto &tls_get() {
        thread_local T obj;
        return obj;
    }
}

#endif //WORKAROUND_BROKEN_MINGW_TLS

#endif //THREAD_LOCAL_WRAPPER_THL_WRAPPER_H
