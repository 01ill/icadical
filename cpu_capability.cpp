#include <thread>
#include <iostream>

int main (int argc, char ** argv) {
    int numThreads = std::thread::hardware_concurrency();
    std::cout << "available threads: " << numThreads << "\n";
}