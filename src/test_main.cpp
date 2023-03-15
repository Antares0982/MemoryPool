//
// Created by antares on 3/15/23.
//

#include <array>
#include <thread>
#include "MemoryPool.h"
#include <deque>
#include <iostream>

#ifdef __linux__

#include <malloc.h>

#endif

struct TreeNode {
    int val;
    TreeNode *sons[4];

    TreeNode(int x) : val(x), sons{} {}
};


int main() {
    using namespace Antares::MemoryPool;
    std::array<std::thread, 4> threads;
    std::array<std::mutex, 4> mutexes;

    std::array<std::deque<TreeNode *>, 4> dequeues;

    constexpr size_t total = 1000000;

    std::atomic<size_t> counter = 1 + 4;

    TreeNode *root = New<TreeNode>(0);

    for (size_t i = 0; i < 4; i++) {
        root->sons[i] = New<TreeNode>(i + 1);
    }
    for (size_t i = 0; i < 4; i++) {
        dequeues[i].push_back(root->sons[i]);
    }
    for (size_t i = 0; i < 4; i++) {
        threads[i] = std::thread([i, &mutexes, &dequeues, &counter] {
            // get one node from queue, and then create 4 nodes and send to queue
            auto &mtx = mutexes[i];
            auto &dq = dequeues[i];

            while (counter < total) {
                TreeNode *node;
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
                        return;
                    }
                    auto ptr = New<TreeNode>(counting);
                    node->sons[j] = ptr;
                    auto &mutex = mutexes[j];
                    auto &deque = dequeues[j];
                    std::lock_guard lk(mutex);
                    deque.push_back(ptr);
                }
                for (int j = 0; j < 4; j++) {

                }
            }
        });
    }

    for (auto &thread: threads) {
        thread.join();
    }

    for (int i = 0; i < 4; ++i) {
        dequeues[i].clear();
    }

    std::cout << "done" << std::endl;
    std::cout << "current root node address: " << root << std::endl << std::endl;

    std::function<void()> gc = [&mutexes, &dequeues, &root, &counter]() {
        // copy root
        auto newroot = New<TreeNode>(root->val);
        for (size_t i = 0; i < 4; i++) {
            newroot->sons[i] = root->sons[i];
        }
        // delete old root
        Delete(root);
        root = newroot;
        // start recursive gc
        dequeues[0].push_back(newroot);
        std::array<std::thread, 4> threads;
        counter = total;
        for (size_t i = 0; i < 4; i++) {
            threads[i] = std::thread([i, &mutexes, &dequeues, &counter] {
                auto &mutex = mutexes[i];
                auto &deque = dequeues[i];
                while (counter > 0) {
                    TreeNode *node;
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
                            auto &mutex = mutexes[j];
                            auto &deque = dequeues[j];
                            auto nson = New<TreeNode>(node->sons[j]->val);
                            for (size_t k = 0; k < 4; k++) {
                                nson->sons[k] = node->sons[j]->sons[k];
                            }
                            Delete(node->sons[j]);
                            {
                                std::lock_guard lk(mutex);
                                deque.push_back(nson);
                            }
                            counter--;
                        }
                    }
                }
            });
        }
        for (auto &thread: threads) {
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
    RegisterGC(gc);
    GC();

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

    Clean();

    std::cout << "All memory cleaned" << std::endl;

#ifdef __linux__
    std::this_thread::sleep_for(std::chrono::seconds(1));
    malloc_stats();
#endif

    return 0;
}
