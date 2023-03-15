# MemoryPool
A simple memory pool using `std::pmr::monotonic_buffer_resource` to allocate memory, providing safe asynchronous destruction.

## Why use it

Generally, directly calling `operator new` or `malloc()` to allocate a block of memory on the heap takes much time. If memory needs to be frequently allocated, the use of `operator new` or `malloc()` can greatly affect performance. Memory resources reduce the time of allocation by allocating a big block of memory at one time and then using an algorithm to provide the required size of memory to the program. However, if multiple threads request or release memory in parallel from a lock-free memory memory resource, it may cause memory conflicts; if a lock is used, it will greatly affect the speed of memory allocation. We can solve the problem of allocating in parallel by using memory resources in thread local storage, which means the memory will only be allocated using a memory resource in current thread. However, the deallocation of memory may happen in a completely different thread. The deallocation may still cause conflicts when multiple threads request deallocations.

The purpose of this memory pool is to avoid the conflict when memory is allocated in one thread but deallocated in another. If you don't worry about this problem, this memory pool maybe not for you; you can use thread local `std::pmr::unsynchronized_pool_resource` or `std::pmr::synchronized_pool_resource`. But if you have the problem, then this pool maybe the simplest and easiest pool to use.

## How to use it

You can create an object using `New`:

```c++
using namespace Antares::MemoryPool;
T* object = New<T>(arg1, arg2); // T* object = new T(arg1, arg2);
```

Create array:

```c++
T* arr = NewArray<T>(size); 
// T* arr = new T[size];
T* arr2 = NewArray<T>(size, arr[0]); // without calling the default constructor
// T* arr2 = new T[size]; for(size_t i = 0;i<size;++i) arr2[i] = arr[0];
```

Delete object:

```c++
Delete(object); // only call constructor
DeleteArray(arr, size); // you have to specify the length of array
```

