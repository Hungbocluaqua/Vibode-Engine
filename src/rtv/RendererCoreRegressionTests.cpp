#include "rtv/RendererCoreRegressionTests.h"

#include "rtv/FreeListAllocator.h"
#include "rtv/RenderGraph.h"
#include "rtv/RtxdiRuntime.h"
#include "rtv/passes/RestirGIPass.h"

#include <Volk/volk.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace rtv {

namespace {

bool check(bool condition, std::string_view message, std::ostream& output) {
    if (!condition) {
        output << "FAIL: " << message << '\n';
    }
    return condition;
}

bool testFreeListAllocator(std::ostream& output) {
    FreeListAllocator<uint32_t> allocator(2u);
    bool ok = true;
    ok &= check(allocator.allocate() == 0u, "free-list first allocation", output);
    ok &= check(allocator.allocate() == 1u, "free-list second allocation", output);
    ok &= check(allocator.allocate() == UINT32_MAX, "free-list exhaustion", output);
    ok &= check(allocator.allocatedCount() == 2u, "free-list allocated count at capacity", output);

    allocator.free(0u);
    allocator.free(0u);
    ok &= check(allocator.allocatedCount() == 1u, "free-list duplicate free is ignored", output);
    ok &= check(allocator.allocate() == 0u, "free-list reuses a released slot", output);
    ok &= check(allocator.allocate() == UINT32_MAX, "free-list does not allocate a live slot twice", output);
    return ok;
}

RenderGraphResource makeExternalBuffer(const char* name) {
    RenderGraphResource resource{};
    resource.type = RenderGraphResource::Type::Buffer;
    resource.lifetime = RenderGraphResource::Lifetime::Persistent;
    resource.size = 256u;
    resource.bufferUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    resource.external = true;
    resource.debugName = name;
    return resource;
}

bool testRenderGraphReaderOrdering(std::ostream& output) {
    RenderGraph graph(nullptr, false);
    const RenderGraphResourceId target = graph.createBuffer(makeExternalBuffer("target"));
    const RenderGraphResourceId dependency = graph.createBuffer(makeExternalBuffer("dependency"));
    const RenderGraphResourceId outputA = graph.createBuffer(makeExternalBuffer("output a"));
    const RenderGraphResourceId outputB = graph.createBuffer(makeExternalBuffer("output b"));

    graph.addPass("initial write").addStorageWrite(target, PipelineDomain::Transfer);
    graph.addPass("delayed compute reader")
        .addStorageRead(target, PipelineDomain::Compute)
        .addStorageRead(dependency, PipelineDomain::Compute)
        .addStorageWrite(outputA, PipelineDomain::Compute);
    graph.addPass("ray reader")
        .addStorageRead(target, PipelineDomain::RayTracing)
        .addStorageWrite(outputB, PipelineDomain::RayTracing);
    graph.addPass("final write").addStorageWrite(target, PipelineDomain::Transfer);
    graph.addPass("late dependency producer").addStorageWrite(dependency, PipelineDomain::Transfer);
    graph.compile();

    std::array<uint32_t, 5> position{};
    position.fill(std::numeric_limits<uint32_t>::max());
    const auto& order = graph.compiledPassOrder();
    for (uint32_t i = 0; i < order.size(); ++i) {
        if (order[i] < position.size()) {
            position[order[i]] = i;
        }
    }

    bool ok = true;
    ok &= check(position[1] < position[3], "compute reader completes before the following writer", output);
    ok &= check(position[2] < position[3], "ray reader completes before the following writer", output);

    const auto barrierIt = std::find_if(
        graph.compiledBarriers().begin(),
        graph.compiledBarriers().end(),
        [&](const RenderGraphBarrier& barrier) {
            return barrier.resource.index == target.index && barrier.afterPass == 3u;
        });
    ok &= check(barrierIt != graph.compiledBarriers().end(), "reader-to-writer barrier exists", output);
    if (barrierIt != graph.compiledBarriers().end()) {
        ok &= check(
            (barrierIt->before.stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0,
            "reader barrier includes compute stage",
            output);
        ok &= check(
            (barrierIt->before.stage & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) != 0,
            "reader barrier includes ray-tracing stage",
            output);
        ok &= check(
            (barrierIt->before.access & VK_ACCESS_2_SHADER_READ_BIT) != 0,
            "reader barrier includes shader reads",
            output);
    }
    return ok;
}

bool testRestirGiHalfResolutionUpsampleContract(std::ostream& output) {
    RendererSettings settings{};
    settings.restirGiMode = RestirGiMode::Production;
    settings.restirGiSpatialRounds = 0u;

    bool ok = true;
    ok &= check(
        passes::RestirGIPass::requestsUpsample(settings, false, true),
        "half-resolution production GI upsamples without spatial reuse",
        output);
    ok &= check(
        !passes::RestirGIPass::requestsUpsample(settings, false, false),
        "full-resolution production GI does not request upsample",
        output);

    settings.restirGiMode = RestirGiMode::Off;
    ok &= check(
        !passes::RestirGIPass::requestsUpsample(settings, false, true),
        "disabled GI does not request upsample",
        output);
    return ok;
}

bool testRtxdiMediumRuntimeContract(std::ostream& output) {
    RtxdiRuntime runtime({
        .renderWidth = 1280u,
        .renderHeight = 720u,
        .qualityPreset = RtxdiQualityPreset::Medium,
        .checkerboard = false,
    });
    runtime.beginFrame(7u);

    const auto& di = runtime.context().GetReSTIRDIContext();
    const auto initial = di.GetInitialSamplingParameters();
    const auto temporal = di.GetTemporalResamplingParameters();
    const auto spatial = di.GetSpatialResamplingParameters();
    const auto& gi = runtime.context().GetReSTIRGIContext();
    const auto giTemporal = gi.GetTemporalResamplingParameters();
    const auto giSpatial = gi.GetSpatialResamplingParameters();
    const auto& pt = runtime.context().GetReSTIRPTContext();
    const auto ptInitial = pt.GetInitialSamplingParameters();
    const auto ptTemporal = pt.GetTemporalResamplingParameters();
    const auto ptSpatial = pt.GetSpatialResamplingParameters();
    const auto memory = runtime.memoryRequirements();

    bool ok = true;
    ok &= check(di.GetFrameIndex() == 7u, "RTXDI frame index advances", output);
    ok &= check(
        di.GetResamplingMode() == rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial,
        "RTXDI medium preset enables temporal and spatial DI",
        output);
    ok &= check(initial.numLocalLightSamples == 8u, "RTXDI medium preset uses eight local-light candidates", output);
    ok &= check(initial.numBrdfSamples == 1u, "RTXDI medium preset uses one BRDF candidate", output);
    ok &= check(
        temporal.biasCorrectionMode == ReSTIRDI_TemporalBiasCorrectionMode::Raytraced,
        "RTXDI medium preset uses ray-traced temporal correction",
        output);
    ok &= check(
        spatial.biasCorrectionMode == ReSTIRDI_SpatialBiasCorrectionMode::Basic,
        "RTXDI medium preset uses basic spatial correction",
        output);
    ok &= check(
        gi.GetResamplingMode() == rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial &&
            giTemporal.maxHistoryLength == 16u && giSpatial.numSamples == 2u,
        "RTXDI medium preset configures GI temporal and spatial reuse",
        output);
    ok &= check(
        pt.GetResamplingMode() == rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial &&
            ptInitial.maxBounceDepth == 5u && ptTemporal.maxHistoryLength == 16u &&
            ptSpatial.numSpatialSamples == 2u,
        "RTXDI medium preset configures ReSTIR PT reuse",
        output);
    ok &= check(memory.diReservoirBytes > 0u, "RTXDI DI memory is reported", output);
    ok &= check(memory.giReservoirBytes > 0u, "RTXDI GI memory is reported", output);
    ok &= check(memory.ptReservoirBytes > 0u, "RTXDI PT memory is reported", output);
    ok &= check(
        memory.totalReservoirBytes == memory.diReservoirBytes + memory.giReservoirBytes + memory.ptReservoirBytes,
        "RTXDI memory total matches its components",
        output);
    return ok;
}

bool testRtxdiSettingsContract(std::ostream& output) {
    bool ok = true;
    ok &= check(
        parseRendererPipelineMode("hybrid-rtxdi") == RendererPipelineMode::HybridRtxdi,
        "RTXDI hybrid pipeline name parses",
        output);
    ok &= check(
        parseRendererPipelineMode("ReSTIR PT") == RendererPipelineMode::PathTracerRtxdi,
        "RTXDI path tracer alias parses",
        output);
    ok &= check(
        parseRtxdiQualityPreset("reference") == RtxdiQualityPreset::Reference,
        "RTXDI reference quality parses",
        output);
    ok &= check(
        std::string_view(rendererPipelineModeName(RendererPipelineMode::PathTracerRtxdi)) == "path-tracer-rtxdi",
        "RTXDI path tracer name is stable",
        output);
    return ok;
}

} // namespace

int runRendererCoreRegressionTests(std::ostream& output) {
    const bool ok = testFreeListAllocator(output) &&
        testRenderGraphReaderOrdering(output) &&
        testRestirGiHalfResolutionUpsampleContract(output) &&
        testRtxdiMediumRuntimeContract(output) &&
        testRtxdiSettingsContract(output);
    output << (ok ? "Renderer core regression tests passed.\n" : "Renderer core regression tests failed.\n");
    return ok ? 0 : 1;
}

} // namespace rtv
