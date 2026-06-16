#include "rtv/BvhBuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

struct MeshData {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> faceMaterials;
    std::vector<glm::vec2> texcoords;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;
};

struct TimingStats {
    double minMs = std::numeric_limits<double>::max();
    double totalMs = 0.0;
};

uint32_t parseUintArg(const char* text, uint32_t fallback) {
    if (text == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || value == 0ul || value > static_cast<unsigned long>(std::numeric_limits<uint32_t>::max())) {
        return fallback;
    }
    return static_cast<uint32_t>(value);
}

MeshData makeGridMesh(uint32_t requestedTriangles) {
    const uint32_t cells = std::max(2u, static_cast<uint32_t>(std::sqrt(static_cast<double>(requestedTriangles) * 0.5)));
    const uint32_t vertexSide = cells + 1u;

    MeshData mesh;
    mesh.positions.reserve(static_cast<size_t>(vertexSide) * vertexSide);
    mesh.texcoords.reserve(static_cast<size_t>(vertexSide) * vertexSide);
    mesh.normals.reserve(static_cast<size_t>(vertexSide) * vertexSide);
    mesh.tangents.reserve(static_cast<size_t>(vertexSide) * vertexSide);
    mesh.indices.reserve(static_cast<size_t>(cells) * cells * 6u);
    mesh.faceMaterials.reserve(static_cast<size_t>(cells) * cells * 2u);

    for (uint32_t y = 0; y < vertexSide; ++y) {
        for (uint32_t x = 0; x < vertexSide; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(cells);
            const float fy = static_cast<float>(y) / static_cast<float>(cells);
            const float z = std::sin(fx * 18.0f) * std::cos(fy * 11.0f) * 0.05f;
            mesh.positions.push_back({fx * 40.0f, z, fy * 40.0f});
            mesh.texcoords.push_back({fx, fy});
            mesh.normals.push_back({0.0f, 1.0f, 0.0f});
            mesh.tangents.push_back({1.0f, 0.0f, 0.0f, 1.0f});
        }
    }

    for (uint32_t y = 0; y < cells; ++y) {
        for (uint32_t x = 0; x < cells; ++x) {
            const uint32_t v00 = y * vertexSide + x;
            const uint32_t v10 = v00 + 1u;
            const uint32_t v01 = v00 + vertexSide;
            const uint32_t v11 = v01 + 1u;
            mesh.indices.insert(mesh.indices.end(), {v00, v10, v11, v00, v11, v01});
            mesh.faceMaterials.push_back((x + y) & 3u);
            mesh.faceMaterials.push_back((x + y + 1u) & 3u);
        }
    }

    return mesh;
}

TimingStats runBenchmark(const MeshData& mesh, rtv::BvhBuildQuality quality, uint32_t iterations, size_t& checksum) {
    TimingStats stats;
    for (uint32_t i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const rtv::BvhBuildResult bvh = rtv::buildBvh(
            mesh.positions,
            mesh.indices,
            mesh.faceMaterials,
            &mesh.texcoords,
            &mesh.normals,
            &mesh.tangents,
            quality);
        const auto end = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        stats.minMs = std::min(stats.minMs, ms);
        stats.totalMs += ms;
        checksum ^= bvh.triangles.size() + (bvh.binaryNodes.size() << 1u) + (bvh.packedNodes.size() << 2u);
    }
    return stats;
}

void printStats(std::string_view name, const TimingStats& stats, uint32_t iterations) {
    std::cout << std::left << std::setw(12) << name
              << " min_ms=" << std::fixed << std::setprecision(3) << stats.minMs
              << " avg_ms=" << (stats.totalMs / static_cast<double>(iterations)) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    uint32_t triangles = 131072u;
    uint32_t iterations = 3u;
    if (argc > 1) {
        triangles = parseUintArg(argv[1], triangles);
    }
    if (argc > 2) {
        iterations = parseUintArg(argv[2], iterations);
    }

    const MeshData mesh = makeGridMesh(triangles);
    const uint32_t actualTriangles = static_cast<uint32_t>(mesh.indices.size() / 3u);
    std::cout << "BVH benchmark triangles=" << actualTriangles
              << " vertices=" << mesh.positions.size()
              << " iterations=" << iterations << '\n';

    size_t checksum = 0;
    printStats("MortonFast", runBenchmark(mesh, rtv::BvhBuildQuality::MortonFast, iterations, checksum), iterations);
    printStats("BinnedSah", runBenchmark(mesh, rtv::BvhBuildQuality::BinnedSah, iterations, checksum), iterations);
    std::cout << "checksum=" << checksum << '\n';
    return checksum == 0u ? 1 : 0;
}
