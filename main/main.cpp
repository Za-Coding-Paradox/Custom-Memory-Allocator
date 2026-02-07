#include <chrono>
#include <iostream>
#include <modules/allocator_engine.h>
#include <thread>
#include <vector>

using namespace Allocator;

struct Particle
{
    float pos[3];
    uint32_t id;
};

void RunCorrectnessTest(AllocatorEngine& engine)
{
    std::cout << "[LOG] Entering Correctness Test..." << std::endl;

    std::cout << "[LOG] Attempting Linear Allocation (FrameLoad)..." << std::endl;
    void* p1 = engine.Allocate<FrameLoad>(1024);
    if (!p1) {
        std::cerr << "[CRIT] Linear Allocation returned NULL!" << std::endl;
        return;
    }
    std::cout << "[LOG] Linear Allocation Success at: " << p1 << std::endl;

    std::cout << "[LOG] Attempting Pool Allocation (Particle)..." << std::endl;
    Handle h1 = engine.AllocateWithHandle<Particle>();
    if (!h1.IsValid()) {
        std::cerr << "[CRIT] Pool Allocation failed (Invalid Handle)!" << std::endl;
        return;
    }

    std::cout << "[LOG] Attempting Handle Resolution..." << std::endl;
    Particle* part = engine.ResolveHandle<Particle>(h1);
    if (!part) {
        std::cerr << "[CRIT] Handle Resolution returned NULL!" << std::endl;
        return;
    }

    part->id = 42;
    std::cout << "[LOG] Handle Resolution Success. ID set to 42." << std::endl;

    std::cout << "[LOG] Attempting FreeHandle..." << std::endl;
    engine.FreeHandle<Particle>(h1);

    if (engine.ResolveHandle<Particle>(h1) == nullptr) {
        std::cout << ">> Correctness: PASSED" << std::endl;
    }
    else {
        std::cerr << "[CRIT] Stale Handle Check FAILED!" << std::endl;
    }
}

int main()
{
    std::cout << "[SYSTEM] Initializing Engine..." << std::endl;

    // Safety check for Arena Size
    size_t slabSize = 64 * 1024;
    size_t arenaSize = 1024 * 1024 * 1024;

    std::cout << "[SYSTEM] Parameters: Slab=" << slabSize << " Arena=" << arenaSize << std::endl;

    try {
        AllocatorEngine engine(slabSize, arenaSize);

        std::cout << "[SYSTEM] Calling engine.Initialize()..." << std::endl;
        engine.Initialize();
        std::cout << "[SYSTEM] Engine Initialized Successfully." << std::endl;

        RunCorrectnessTest(engine);

        std::cout << "[SYSTEM] Shutting down..." << std::endl;
        engine.Shutdown();
    }
    catch (const std::exception& e) {
        std::cerr << "[FATAL] Exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::cerr << "[FATAL] Unknown crash during initialization!" << std::endl;
    }

    return 0;
}
