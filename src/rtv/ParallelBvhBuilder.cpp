#include "rtv/ParallelBvhBuilder.h"

#include <algorithm>
#include <future>
#include <thread>

namespace rtv {

namespace {

ParallelBvhBuildResult buildTask(const ParallelBvhBuildTask& task) {
    const auto start = std::chrono::high_resolution_clock::now();
    ParallelBvhBuildResult result;

    if (task.vertices == nullptr || task.indices == nullptr || task.faceMaterials == nullptr) {
        return result;
    }

    result.bvh = buildBvh(
        *task.vertices,
        *task.indices,
        *task.faceMaterials,
        task.texcoords,
        task.normals,
        task.tangents,
        task.quality);

    const auto end = std::chrono::high_resolution_clock::now();
    result.buildTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
    return result;
}

} // namespace

std::vector<ParallelBvhBuildResult> ParallelBvhBuilder::buildAll(
    const std::vector<ParallelBvhBuildTask>& tasks,
    uint32_t maxThreads) {
    std::vector<ParallelBvhBuildResult> results(tasks.size());

    if (tasks.empty()) {
        return results;
    }

    if (tasks.size() == 1) {
        results[0] = buildTask(tasks[0]);
        return results;
    }

    const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t workerCount = (maxThreads > 0) ? std::min(maxThreads, hardwareThreads) : hardwareThreads;
    const uint32_t clampedWorkers = std::min(workerCount, static_cast<uint32_t>(tasks.size()));

    std::atomic<uint32_t> nextTask{0};
    std::vector<std::future<void>> workers;
    workers.reserve(clampedWorkers);

    for (uint32_t w = 0; w < clampedWorkers; ++w) {
        workers.push_back(std::async(std::launch::async, [&]() {
            while (true) {
                uint32_t taskIndex = nextTask.fetch_add(1);
                if (taskIndex >= tasks.size()) {
                    break;
                }
                results[taskIndex] = buildTask(tasks[taskIndex]);
            }
        }));
    }

    for (auto& worker : workers) {
        worker.get();
    }

    return results;
}

} // namespace rtv
