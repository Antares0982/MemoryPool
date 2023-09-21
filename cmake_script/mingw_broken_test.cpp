//
// Created by Antares on 2023/9/16.
//

#include <memory>
#include <thread>


void f() {
    thread_local std::shared_ptr<int> ptr = std::make_shared<int>(0);
}

int main() {
    std::thread th(f);
    th.join();
    return 0;
}
