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

#ifdef TEST_MEMORYPOOL_MAIN


#include <array>
#include <thread>
#include "MemoryPool.h"
#include <deque>
#include <iostream>
#include <cassert>
#include <atomic>
#include <future>

#ifdef __linux__

#include <malloc.h>

#endif

struct TreeNode {
	int val;
	TreeNode* sons[4];

	explicit TreeNode(int x) : val(x), sons{} {}

	TreeNode(const TreeNode& o) = delete;
};

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

/// @brief A simple class to help manage async tasks
struct BaseAsyncData {
	std::atomic<size_t> counter = 0;
	std::promise<void> pr;

	void finish_one() {
		size_t counting = counter.fetch_sub(1);
		assert(counting != 0);
		if (counting == 1) {
			pr.set_value();
		}
	}

	void finish_n(size_t n) {
		size_t counting = counter.fetch_sub(n);
		assert(counting >= n);
		if (counting == n) {
			pr.set_value();
		}
	}

	void wait() {
		pr.get_future().wait();
	}
};

int main() {
	using MemoryPool = Antares::MemoryPool;
	MemoryPool pool;
	std::array<std::thread, 4> threads;
	std::array<std::mutex, 4> mutexes;

	std::array<std::deque<TreeNode*>, 4> dequeues;

	constexpr size_t total = 1000000;

	std::atomic<size_t> counter = 1 + 4;

	/// first task: create a tree with 4 sons for each node, using memory pool to allocate memory
	/// the total number of nodes except the root is `total`

	auto root = pool.New<TreeNode>(0);

	for (size_t i = 0; i < 4; i++) {
		root->sons[i] = pool.New<TreeNode>(i + 1);
		dequeues[i].push_back(root->sons[i]);
	}
	BaseAsyncData async_data;
	async_data.counter = 4;
	auto fu = async_data.pr.get_future();
	SpinLock lk0;
	lk0.lock();
	for (size_t i = 0; i < 4; i++) {
		threads[i] = std::thread([i, &pool, &lk0, &mutexes, &dequeues, &counter, total, &async_data, &fu] {
			lk0.lock();
			lk0.unlock();
			// get one node from queue, then create 4 nodes and send them to queue
			auto& mtx = mutexes[i];
			auto& dq = dequeues[i];

			while (counter < total) {
				TreeNode* node;
				{
					std::lock_guard lk(mtx);
					if (dq.empty()) {
						continue;
					}
					node = dq.front();
					dq.pop_front();
				}

				for (size_t j = 0; j < 4; j++) {
					auto counting = counter++;
					if (counting > total) {
						async_data.finish_one();
						while (async_data.counter != 0)
						{
							fu.wait_for(std::chrono::milliseconds(100));
						}
						return;
					}
					auto ptr = pool.New<TreeNode>(counting);
					node->sons[j] = ptr;
					auto& mutex = mutexes[j];
					auto& deque = dequeues[j];
					std::lock_guard lk(mutex);
					deque.push_back(ptr);
				}
			}
			async_data.finish_one();
			while (async_data.counter != 0)
			{
				fu.wait_for(std::chrono::milliseconds(100));
			}
			});
	}
	lk0.unlock();
	for (auto& thread : threads) {
		thread.join();
	}

	for (int i = 0; i < 4; ++i) {
		dequeues[i].clear();
	}

	std::cout << "done" << std::endl;
	std::cout << "current root node address: " << root << std::endl << std::endl;


	/// second task: gc the old tree, and create a new tree with the same structure
	std::function < void() > gc = [&pool, &mutexes, &dequeues, &root, &counter, total]() {
		// copy root
		auto newroot = pool.New<TreeNode>(root->val);
		for (size_t i = 0; i < 4; i++) {
			newroot->sons[i] = root->sons[i];
		}
		// delete old root
		pool.Delete(root);
		root = newroot;
		// start recursive gc
		dequeues[0].push_back(newroot);
		std::array<std::thread, 4> threads;
		BaseAsyncData async_data;
		async_data.counter = 4;
		auto fu = async_data.pr.get_future();
		counter = total;
		SpinLock lk1;
		lk1.lock();
		for (size_t i = 0; i < 4; i++) {
			threads[i] = std::thread([i, &pool, &lk1, &mutexes, &dequeues, &counter, &async_data, &fu] {
				lk1.lock();
				lk1.unlock();
				auto& mutex = mutexes[i];
				auto& deque = dequeues[i];
				while (counter > 0) {
					TreeNode* node;
					{
						std::lock_guard lk(mutex);
						if (deque.empty()) {
							continue;
						}
						node = deque.front();
						deque.pop_front();
					}
					for (size_t j = 0; j < 4; j++) {
						if (node->sons[j] != nullptr) {
							auto& mutexj = mutexes[j];
							auto& dequej = dequeues[j];
							auto nson = pool.New<TreeNode>(node->sons[j]->val);
							for (size_t k = 0; k < 4; k++) {
								nson->sons[k] = node->sons[j]->sons[k];
							}
							pool.Delete(node->sons[j]);
							{
								std::lock_guard lk(mutexj);
								dequej.push_back(nson);
							}
							counter--;
						}
					}
				}
				async_data.finish_one();
				while (async_data.counter != 0)
				{
					fu.wait_for(std::chrono::milliseconds(100));
				}
				});
		}
		lk1.unlock();
		for (auto& thread : threads) {
			thread.join();
		}
		};

#ifdef __linux__
	std::this_thread::sleep_for(std::chrono::seconds(1));
	malloc_stats();
	std::this_thread::sleep_for(std::chrono::seconds(1));
#endif

	auto oldRoot = root;
	std::cout << "Before GC, current value in old root: " << oldRoot->val << std::endl << std::endl;
	pool.RegisterGC(gc);
	pool.GC();

#ifdef __linux__
	std::this_thread::sleep_for(std::chrono::seconds(1));
	malloc_stats();
	std::this_thread::sleep_for(std::chrono::seconds(1));
#endif

	std::cout << std::endl << "GC done" << std::endl;
	std::cout << "current root node address: " << root << std::endl;

	std::cout << "counter: " << counter.load() << std::endl;
	std::cout << "Current value in new root: " << root->val << std::endl;
	std::cout << "Current value in old root (invalid read): " << oldRoot->val << ", since GCed it may not be 0"
		<< std::endl << std::endl;

	constexpr size_t bufferSize = 1024;
	static size_t buffer[bufferSize];
	static std::atomic<size_t> buffer_counter = 0;
	struct Test {
		size_t cc;

		Test() {
			cc = buffer_counter++;
			buffer[cc] = cc;
		}

		~Test() {
			buffer[cc] = bufferSize;
			buffer_counter--;
		}
	};

	{
		std::cout << "Testing new array..." << std::endl;
		auto testPtrArray = pool.NewArray<Test>(bufferSize);
		std::cout << "Expecting buffer_counter = " << bufferSize << ", got " << buffer_counter.load() << std::endl;
		assert(buffer_counter == bufferSize);
		pool.DeleteArray(testPtrArray, bufferSize);
		assert(buffer_counter == 0);
		std::cout << "Expecting buffer_counter = 0, got " << buffer_counter.load() << std::endl;
	}

	{
		MemoryPool::Allocator <Test> allocatorTemplate(&pool);
		std::cout << "Testing Allocator..." << std::endl;
		std::vector<Test, MemoryPool::Allocator < Test>>
			testVec(allocatorTemplate);
		testVec.reserve(bufferSize);
		for (size_t i = 0; i < bufferSize; i++) {
			testVec.emplace_back();
		}
		std::cout << "Expecting buffer_counter = " << bufferSize << ", got " << buffer_counter.load() << std::endl;
		assert(buffer_counter == bufferSize);
		testVec.resize(bufferSize / 2);
		std::cout << "Expecting buffer_counter = " << bufferSize / 2 << ", got " << buffer_counter.load() << std::endl;
		assert(buffer_counter == bufferSize / 2);
		testVec.resize(bufferSize);
		std::cout << "Expecting buffer_counter = " << bufferSize << ", got " << buffer_counter.load() << std::endl;
		assert(buffer_counter == bufferSize);
		testVec.clear();
		std::cout << "Expecting buffer_counter = 0, got " << buffer_counter.load() << std::endl;
	}

	{
		std::cout << "Testing ThreadLocalAllocator..." << std::endl;
		std::vector<Test, MemoryPool::ThreadLocalAllocator < Test>>
			testVec(
				pool.GetThreadLocalAllocator<Test, Antares::MemoryPool::Temporary>()
			);
		testVec.reserve(bufferSize);
		for (size_t i = 0; i < bufferSize; i++) {
			testVec.emplace_back();
		}
		std::cout << "Expecting buffer_counter = " << bufferSize << ", got " << buffer_counter.load() << std::endl;
		assert(buffer_counter == bufferSize);
		testVec.resize(bufferSize / 2);
		std::cout << "Expecting buffer_counter = " << bufferSize / 2 << ", got " << buffer_counter.load() << std::endl;
		assert(buffer_counter == bufferSize / 2);
		testVec.resize(bufferSize);
		std::cout << "Expecting buffer_counter = " << bufferSize << ", got " << buffer_counter.load() << std::endl;
		assert(buffer_counter == bufferSize);
		testVec.clear();
		std::cout << "Expecting buffer_counter = 0, got " << buffer_counter.load() << std::endl;
	}

	pool.Clean();

	MemoryPool* pool2 = new MemoryPool();
	MemoryPool* pool3 = new MemoryPool();

	{
		std::cout << "Testing multiple pools..." << std::endl;

		pool2->NewArray<size_t>(bufferSize);
		pool3->NewArray<size_t>(bufferSize);
		pool2->NewTempArray<size_t>(bufferSize);
		pool3->NewTempArray<size_t>(bufferSize);
		delete pool2;
		pool2 = new MemoryPool();
		pool2->NewArray<size_t>(bufferSize);
		pool2->NewTempArray<size_t>(bufferSize);
		pool.NewArray<size_t>(bufferSize);
		pool.NewTempArray<size_t>(bufferSize);
		delete pool3;
		pool3 = new MemoryPool();
		pool3->NewArray<size_t>(bufferSize);
		pool3->NewTempArray<size_t>(bufferSize);
		delete pool2;
		delete pool3;
		pool.Clean();
	}

	std::cout << "All memory cleaned" << std::endl;

#ifdef __linux__
	std::this_thread::sleep_for(std::chrono::seconds(1));
	malloc_stats();
	std::cerr.flush();
#endif

	return 0;
		}

#endif
