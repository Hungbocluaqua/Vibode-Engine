<!-- open-code-summary-anchor -->
# UE5-Style Physically Based Sky System

**Implementation risk warning**: This system is architected for production scalability, not for incremental feature accumulation. The 4-subsystem design, DirtyDAG scheduling, render graph integration, temporal reprojection, sky classification, and quality policies are all designed for a renderer that already has a stable core. The Implementation Order below has been aggressively pruned to get a believable sky on screen before any infrastructure. Start with Stage A (analytical only, no LUTs) and do NOT skip to multi-scatter, aerial perspective, temporal, or CDF before single-scattering is validated across all conditions. Every stage is independently verifiable — if a stage's verification fails, stop and fix it before proceeding. The strict ordering is A → B (transmittance LUT) → C (sky-view LUT) → D (env sampling) → E (aerial perspective) → F (multi-scatter) → G (temporal, last).

## Architecture Overview

The atmosphere system is decomposed into four independent subsystems, each with a single responsibility, composed by a thin `SkySystem` orchestrator. This separation exists at runtime — each subsystem is independently constructable and testable — not just a documentation grouping.

```
PathTracerRenderer
  └── SkySystem (thin orchestrator, ~100 lines)
       ├── AtmosphereModel           (pure settings + physical math, no GPU resources)
       ├── AtmosphereLutSystem       (LUT generation + GPU resource ownership)
       ├── AtmosphereTemporalSystem  (history + reprojection + variance clamping)
       └── AtmosphereSamplingSystem  (CDF construction + MIS + environment sampling)
```

| Subsystem | Owns | Responsibility |
|-----------|------|---------------|
| `AtmosphereModel` | Settings structs, planet params, scattering coefficients, phase functions | Pure math — no GPU. Converts user settings into scattering coefficients. Provides `compute_transmittance()`, `rayleigh_phase()`, `mie_phase()` as const functions. |
| `AtmosphereLutSystem` | LUT images, compute pipelines, set 1 descriptor layout | GPU LUT generation + storage. Four compute passes (transmittance, multi-scatter, sky-view, aerial perspective). Writes into history resources consumed by temporal system. |
| `AtmosphereTemporalSystem` | History images, reprojection pipeline, variance params | Camera-motion-aware reprojection of sky-view LUT. Variance clipping, exposure rescaling, history rejection. Owns the DirtyDAG (see below) for LUT update scheduling. |
| `AtmosphereSamplingSystem` | CDF buffers, sky importance pipeline, sampling shaders | Sky importance CDF construction from sky-view LUT. MIS integration. Provides `environment_radiance()`, `environment_pdf()`, `sample_environment()` for the path tracer. |


**Descriptor strategy**: All atmosphere resources (LUTs, CDF buffers, uniforms, sampler) live in a single **set 1** descriptor set, owned by `AtmosphereLutSystem`. `AtmosphereSamplingSystem` writes its CDF buffers into this shared set. This keeps per-frame descriptor binds to 2 total (set 0 = scene, set 1 = atmosphere) and avoids fragmentation as clouds, weather textures, shadow LUTs, and volumetric resources are added later.

**Sun direction**: Editable in the editor UI (azimuth + elevation sliders).

**Sun/LUT separation**: The sun is **purely analytical** — removed from the sky-view LUT. The sky LUT stores only diffuse atmospheric scattering (sky dome color without the solar disk). The sun is handled entirely through:
- `analytical_sun_disk_radiance()` — for miss rays and analytic sun disk rendering
- `analytical_sun_center_radiance()` — for direct lighting, attenuated by `sun_transmittance()`

This avoids double-counting solar energy and keeps MIS clean — the sun is a delta light with no PDF contest with the sky LUT.

**Path tracer integration**: Atmosphere affects environment *radiance* (miss shader samples diffuse sky LUT + analytical sun disk), environment *sampling* (importance sampling from sky-view LUT luminance only — no sun in CDF), and direct lighting (sun attenuated by `sun_transmittance()`).

**World unit policy**: 1 world unit = 1 meter. This applies everywhere — atmosphere radii, camera near/far planes, BVH extents, exposure calculations. Without this the atmosphere breaks immediately.

**Planetary coordinate convention**: Planet center is at **(0, 0, 0)**. Sea level = radius 6,360,000 m. All atmosphere functions use absolute planetary coordinates. No planet-center offset hacks.

---

## Frame Pipeline

```
Sun Direction Change ──► Transmittance LUT update (if dirty)
      ↓
Sky-view LUT update (diffuse atmosphere only, no solar disk)
      ↓
Sky importance CDF rebuild (if sky-view LUT changed)
      ↓
Path Trace
  ├── environment_radiance(dir)   → sky LUT (diffuse) + analytical sun disk
  ├── environment_pdf(dir)        → CDF from sky LUT (no sun component)
  ├── sample_environment()        → importance-sampled sky (no sun)
  ├── direct lighting             → sun_transmittance(hitPos) × analytical_sun_center_radiance()
  └── aerial perspective          → chromatic inscatter + transmittance on hit
      ↓
Denoiser
      ↓
Physical Camera Exposure (EV100 → linear)
      ↓
Tone Map
      ↓
Present
```

(Transmittance LUT is the first LUT step. Multi-scatter LUT and temporal reprojection are added in Stages F and G — see Implementation Order below. Aerial perspective LUT is added in Stage E, replacing analytic fog.)

### Accumulation Reset Policy

Accumulation is reset when any of these change:
- Sun direction
- Atmosphere parameters (scattering, ozone, radii, albedo)
- Physical camera exposure (aperture, shutter, ISO)
- Quality level
- Debug view

This prevents history contamination from stale LUT data.

---

## Phase 0 — Foundation (Before Atmosphere)

### World Unit Policy

**1 world unit = 1 meter**. Enforced engine-wide:

| Component | Value in meters |
|-----------|-----------------|
| Planet center | (0, 0, 0) |
| Planet radius | 6,360,000 |
| Atmosphere radius | 6,420,000 |
| Camera near plane | 0.01 |
| Camera far plane | 100,000+ |
| Sun illuminance | 100,000 lux |
| Rayleigh scale height | 8,000 |
| Mie scale height | 1,200 |

### Planetary Coordinate System

**Planet center at (0, 0, 0)**, sea level = radius 6,360,000 m. All positions are absolute planetary coordinates.

- `length(position)` gives distance from planet center
- `length(position) - planetRadius` gives altitude above sea level
- The ground sphere is centered at origin with radius `planetRadius`
- The atmosphere shell is centered at origin with radius `atmosphereRadius`

This means:
- No planet-center offset hacks in any atmosphere function
- `ray_sphere_intersection()` uses `center = vec3(0.0)` directly
- `transmittance_lut()` and `sun_transmittance()` take unmodified world positions
- V1 uses single-precision with `double` discriminant in ray-sphere intersection

### Dual Coordinate Spaces

Planetary-scale coordinates conflict with real-time scene rendering. Scene geometry (meshes, BVH, transforms) uses float precision with a local origin. Atmosphere math needs absolute planetary coordinates.

The solution is a **dual-space transform**:

```
Space             Type       Purpose                              Origin
──────────────────────────────────────────────────────────────────────────
Planetary space   double     Atmosphere math, LUT sampling,       (0, 0, 0)
                             ray-sphere intersection,
                             sun transmittance

Scene space       float      Scene geometry, BVH,                 Arbitrary surface point
                             editor transforms, camera pose,
                             physics, gizmos
```

**Conversion**: The path tracer converts camera rays from scene space to planetary space before atmosphere evaluation:

```cpp
// Camera pose is in scene space. Convert to planetary space:
vec3 planetaryCameraPos = sceneToPlanetary(cameraPos);
// sceneToPlanetary = identity + planetary surface offset
// For v1 with planet at (0,0,0) and scene near sea level:
// planetaryCameraPos = cameraPos + vec3(0, planetRadius, 0)
// More generally, the scene-space origin is a tangent-plane point
// on the planet surface at the camera's latitude/longitude.
```

The floating origin rebase (see below) updates the scene-space origin as the camera moves, keeping scene geometry in well-conditioned float coordinates.

**Why this works**: Scene geometry exists within a few kilometers of the surface origin — well within float32 precision. The atmosphere exists at planetary scale (6,360,000 m) — requiring double precision. The dual-space transform bridges the two without compromising either.

### Large World Coordinates / Floating Origin Strategy (Long-Term)

At planetary scales (radius = 6,360,000 m), single-precision floats lose sub-millimeter precision at the camera origin. This breaks BVH traversal, ray-sphere intersection, and shadow rays.

**Long-term strategy** (UE5 LWC-inspired):

1. **Double-precision camera origin** — the camera position is stored as `double3`. All other scene geometry is relative `float` offsets.
2. **Floating origin rebase** — when the camera moves more than a threshold (e.g., 1 km) from the origin, subtract the delta from the camera and add it to all world transforms. This recenters precision.
3. **Atmosphere intersection** — already handled with double-precision discriminant.
4. **Transmittance LUT sampling** — heights and distances remain in single-precision float relative to the planetary surface (max value ~60,000 m = atmosphere height), which is safe for float32.

**Not required for v1** — the double-precision discriminant in ray-sphere intersection is sufficient for the editor's expected scale.

### Physical Camera / Exposure System

Atmosphere only produces correct output with physically based exposure.

```cpp
struct PhysicalCamera {
    float aperture = 16.0f;      // f-stop
    float shutterSpeed = 1.0f;   // seconds (1/100 = 0.01)
    float iso = 100.0f;
    float exposureCompensation = 0.0f;

    // Derived
    float ev100() const {
        return log2((aperture * aperture) / shutterSpeed * 100.0f / iso);
    }
    float linearExposure() const {
        return exposureCompensation > 0.0f
            ? (1.0f / (pow(2.0f, ev100()) * 1.2f)) * pow(2.0f, exposureCompensation)
            : 1.0f / (pow(2.0f, ev100()) * 1.2f);
    }
};
```

Exposure is passed to the tone mapper, which converts scene-referred HDR (in cd/m² / nits) to display-ready values.

### Tone Mapping Requirements

The physical exposure pipeline is incomplete without the tone mapper. The atmosphere produces high dynamic range (sun disk ~10⁶ cd/m², sky dome 10³–10⁴ cd/m²), and a naive Reinhard-style operator will wash out sky saturation and compress sun highlights into a dull grey.

`tone_map.comp` implements a **UE5-style filmic curve** (Krzysztof Narkowicz's ACES filmic approximation) with explicit white point and display transform:

```glsl
// Exposure happens BEFORE tone mapping:
// linearColor *= physicalCamera.linearExposure();
// vec3 color = applyFilmicTonemap(linearColor);

vec3 applyFilmicTonemap(vec3 color) {
    // Pre-desaturate highlights to prevent white-grey sun disk
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    vec3 desaturated = vec3(luminance);
    float highlightFactor = smoothstep(0.5, 1.0, luminance);
    color = mix(color, desaturated, highlightFactor * 0.3);

    // UE5/ACES filmic S-curve: smooth shoulder rolloff
    // x * (6.2*x + 0.5) / (x * (6.2*x + 1.7) + 0.06)
    color = color * (6.2 * color + 0.5) / (color * (6.2 * color + 1.7) + 0.06);

    // White point: D65 at 100 nits output (display-referred)
    // No further white balance adjustment — exposure handles that
    return color;
}

// Display transform: linear → sRGB gamma
vec3 linearToSrgb(vec3 color) {
    return mix(
        1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055,
        12.92 * color,
        lessThan(color, vec3(0.0031308))
    );
}
```

**White point**: D65 (6500K). No chromatic adaptation in the tone mapper — white balance is a pre-exposure operation on the scene-referred linear data.

**Highlight desaturation**: The `smoothstep(0.5, 1.0, luminance)` desaturation prevents the sun disk from washing to white-grey. The 0.3 mix factor ensures the shoulder remains warm. This is the single most important quality factor for atmosphere tone mapping — without it, the physically correct orange sunset turns grey.

**Mid-tones**: The filmic S-curve has a linear segment near 0.18 (18% grey). No mid-tone color shift. Sky blues, sunset oranges, and horizon gradients pass through with neutral saturation.

**Minimum viable alternative** (if full filmic curve is too heavy for initial bring-up):
- Simple Reinhard: `color / (color + 1.0)` — acceptable for preliminary calibration only
- Then replace with filmic once sky validation begins
- Never ship Reinhard for atmosphere — sky will look flat and washed out

The tone mapper choice directly affects how the physically calibrated atmosphere *looks* — a weak operator will undo the physical calibration.

**Files to create/modify:**
- `include/rtv/PhysicalCamera.h` / `src/rtv/PhysicalCamera.cpp` (new)
- `include/rtv/PathTracerRenderer.h` — add PhysicalCamera to RendererSettings
- `src/rtv/RenderSettingsPanel.cpp` — add Exposure section
- `shaders/tone_map.comp` — accept exposure in EV100 or linear form, implement filmic curve

### Spectral Calibration: Lux → Radiance

Atmosphere rendering operates in spectral radiance (W/sr/m²), but user-facing controls use photometric units (lux). The conversion chain:

```
Sun illuminance (lux) = 100,000 lm/m² (D65 reference)
                         │
                         ▼ divide by π × solidAngle
Sun radiance (W/sr/m²)  = illuminance / (π × Ω_sun)
                         where Ω_sun = 2π(1 - cos(sunAngularRadius))
                                       ≈ π × sunAngularRadius² (small-angle)
                         ≈ 100,000 / (π × 6.8×10⁻⁵) ≈ 4.68×10⁸ cd/m²
                         │
                         ▼ RGB approximation of 5778K blackbody
GPU sunCenterRadiance    ≈ vec3(1.0, 0.95, 0.80) × 16.0
                         // Scaled such that sampled LUT values produce
                         // physically plausible cd/m² after atmosphere scattering
```

The `sunIlluminance` slider (1,000–200,000 lux) scales `sunCenterRadiance` linearly. The scattering coefficients in `AtmosphereParams` are calibrated to this baseline — changing the illuminance without rescaling coefficients produces incorrect atmospheric luminance.

This calibration ensures:
- LUT samples produce scene-referenced cd/m² values
- Exposure system maps cd/m² → display-ready values correctly
- Sun disk and sky dome have physically consistent luminance ratios

---

## Phase 1 — Atmosphere System

### Files to Create

| File | Purpose |
|------|---------|
| `include/rtv/AtmosphereParams.h` | AtmosphereParams struct (canonical CPU-side — only physical parameter model) |
| `include/rtv/AtmosphereModel.h` | AtmosphereModel (pure math) |
| `include/rtv/AtmosphereLutSystem.h` | AtmosphereLutSystem (GPU LUT gen, owns set 1) |
| `include/rtv/AtmosphereTemporalSystem.h` | AtmosphereTemporalSystem (DirtyDAG, reprojection) |
| `include/rtv/AtmosphereSamplingSystem.h` | AtmosphereSamplingSystem (CDF, MIS) |
| `include/rtv/SkySystem.h` | SkySystem (thin orchestrator) |
| `src/rtv/` | 4 subsystem .cpp files + SkySystem.cpp |
| `shaders/atmosphere_phase.glsl` | Rayleigh + Mie + Henyey-Greenstein phase functions (~60 lines) |
| `shaders/atmosphere_lut_sampling.glsl` | LUT sampling functions (transmittance, multi-scatter, sky-view, aerial perspective) (~150 lines) |
| `shaders/atmosphere_lighting.glsl` | Sun transmittance, aerial perspective composition, sun disk (~120 lines) |
| `shaders/environment_sampling.glsl` | Importance sampling from sky CDF, MIS with BSDF (~120 lines) |
| `shaders/transmittance_lut.comp` | Transmittance LUT precomputation (~80 lines) |
| `shaders/multi_scatter_lut.comp` | Multi-scattering LUT precomputation (~60 lines) |
| `shaders/sky_view_lut.comp` | Sky-view LUT rendering (diffuse atmosphere only, no solar disk) (~120 lines) |
| `shaders/aerial_perspective_lut.comp` | 3D aerial perspective LUT (~100 lines) |
| `shaders/sky_importance_build.comp` | Importance sampling CDF from sky LUT (~80 lines) |
| `shaders/sky_reproject.comp` | Temporal reprojection + variance clamping for sky-view LUT (~100 lines) |
| `shaders/sky_debug_views.comp` | Atmosphere debug view renderer (~60 lines) |

### Files to Modify

| File | Changes |
|------|---------|
| `CMakeLists.txt` | Add `AtmosphereModel.cpp`, `AtmosphereLutSystem.cpp`, `AtmosphereTemporalSystem.cpp`, `AtmosphereSamplingSystem.cpp`, `SkySystem.cpp`, `PhysicalCamera.cpp` |
| `include/rtv/PathTracerRenderer.h` | Add SkySystem member (single orchestrator), atmosphere + camera fields to RendererSettings, new private methods |
| `src/rtv/PathTracerRenderer.cpp` | Constructor init, `recordPathTrace()` integration, `createResolutionResources()`, `updateCamera()` |
| `src/rtv/RenderSettingsPanel.cpp` | Add atmosphere section, exposure section |
| `src/rtv/PathTracerRenderer.cpp` | Constructor init, `recordPathTrace()` integration, `createResolutionResources()`, `updateCamera()` |
| `src/rtv/RenderSettingsPanel.cpp` | Add atmosphere section, exposure section |
| `include/rtv/RendererDebug.h` | Add atmosphere debug view enum entries |
| `shaders/rt_common.glsl` | Remove atmosphere defines; include split glsl files; add set 1 bindings |
| `shaders/pathtrace.rgen` | Add set 1 bindings, aerial perspective on hit, sun_transmittance in direct lighting |
| `shaders/pathtrace.rmiss` | Sample diffuse sky LUT + analytical sun disk for miss radiance |
| `shaders/pathtrace_shadow.rmiss` | Add set 1 bindings |
| `shaders/tone_map.comp` | Physical camera EV100 exposure |

---

## Data Structures

### AtmosphereParams (CPU-side — canonical model)

The CPU-side physical model uses the single `AtmosphereParams` struct defined in the [`AtmosphereModel`](#atmospheremodel-pure-math) section above. This is the ONE canonical parameter structure:

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `planetRadius` | float | 6,360,000 m | Sea level radius |
| `atmosphereRadius` | float | 6,420,000 m | Top of atmosphere |
| `groundAlbedo` | float | 0.3 | Lambertian ground reflectance |
| `rayleighScaleHeight` | float | 8,000 m | |
| `rayleighScattering` | vec3 | (5.8e-6, 1.355e-5, 3.309e-5) | RGB scattering coefficients |
| `mieScaleHeight` | float | 1,200 m | |
| `mieScattering` | float | 2e-5 | |
| `mieExtinction` | float | 3e-5 | Includes absorption |
| `miePhaseAnisotropy` | float | 0.8 | |
| `ozoneScaleHeight` | float | 25,000 m | |
| `ozonePeakHeight` | float | 23,000 m | |
| `ozoneAbsorption` | vec3 | (0.65, 1.0, 0.05) | RGB absorption |
| `sunAngularRadius` | float | 0.004675 rad | ~0.5° |
| `sunIlluminance` | vec3 | (1.474, 1.484, 1.374) W/m² | RGB solar illuminance |

**Everything else** in the system is derived, cached, or transcoded from this struct. There is no second parameter model.

**Calibration policy**: Physically inspired, not spectrally absolute radiometry. All solar values (illuminance, radiance, scattering coefficients) are calibrated to a D65-equivalent solar spectral baseline using RGB approximation of a 5778K blackbody. This ensures the analytical sun disk, direct lighting, and atmospheric scattering produce consistent luminance levels. The result is:
- LUT samples produce scene-referenced cd/m² values
- Exposure system maps cd/m² → display-ready values correctly
- Sun disk and sky dome have physically consistent luminance ratios

Not a spectrally calibrated radiometric pipeline. If your use case requires absolute spectral accuracy (e.g., scientific remote sensing), the RGB approximation of Chappuis ozone absorption and Rayleigh/Mie scattering coefficients will be a limiting factor.

**Ozone note**: Simplified layer model. Wavelength-based absorption (Chappuis band) may be needed for accurate sunset coloration later.

### AtmosphereUniform (GPU-side, std140)

```glsl
layout(set = 1, binding = 0, std140) uniform Atmosphere {
    vec4 planetRadius_atmosphereRadius;
    vec4 rayleighScattering_scaleHeight;
    vec4 mieScattering_scaleHeight_anisotropy;
    vec4 ozoneAbsorption_layerCenter_layerHalfWidth;
    vec4 groundAlbedo_padding;
    vec4 sunDirection_sunIlluminance;
    vec4 sunAngularRadius_skyIntensity_pad_pad;
};
```

### Set 1 Bindings (single unified atmosphere descriptor set)

All atmosphere resources — LUTs, uniforms, CDF buffers, sampler, sky classification — live in one set. This avoids fragmentation as clouds, weather textures, shadow LUTs, and volumetric resources are added later.

```glsl
layout(set = 1, binding = 0, std140) uniform Atmosphere { ... };
layout(set = 1, binding = 1) uniform texture2D transmittance_lut;
layout(set = 1, binding = 2) uniform texture2D multi_scatter_lut;
layout(set = 1, binding = 3) uniform texture2D sky_view_lut;           // diffuse only, no sun
layout(set = 1, binding = 4) uniform texture3D aerial_perspective_lut;
layout(set = 1, binding = 5) uniform sampler atmosphere_sampler;
layout(set = 1, binding = 6, std430) readonly buffer SkyCdfRows { float sky_cdf_rows[]; };
layout(set = 1, binding = 7, std430) readonly buffer SkyCdfCols { float sky_cdf_cols[]; };
layout(set = 1, binding = 8, std140) uniform SkyLutParams {
    uint width;
    uint height;
    float inv_total_lum;
    float pad;
};
// Binding 9 reserved for future SkyClassification buffer (not allocated in v1)
```

Owned by `AtmosphereLutSystem`:
- `AtmosphereLutSystem` creates and updates bindings 0–5 (uniform, LUTs, sampler)
- `AtmosphereSamplingSystem` writes into bindings 6–8 (CDF buffers, params)
- All subsystems share the same `VkDescriptorSet`, written via `vkUpdateDescriptorSets`

---

## LUT Specifications

| LUT | Format | Resolution | VRAM | Update Frequency |
|-----|--------|------------|------|------------------|
| Transmittance | `R16G16B16A16_SFLOAT` | 256×64 | 128 KB | On parameter change |
| Multi-scattering | `R16G16B16A16_SFLOAT` | 32×32 | 8 KB | On parameter change |
| Sky-view | `R16G16B16A16_SFLOAT` | 256×144 (configurable) | 288 KB | Every frame (temporal reproject) |
| Sky-view (previous) | `R16G16B16A16_SFLOAT` | 256×144 | 288 KB | History for reprojection |
| Aerial perspective | `R16G16B16A16_SFLOAT` | 96×96×48 (3D) | 2.6 MB | On parameter change |
| Total atmosphere VRAM | | | ~3.3 MB | |

**Quality scaling** for sky-view LUT:

```cpp
enum class AtmosphereQuality { Low, Medium, High, Cinematic };
// Low:       128×72   (72 KB)
// Medium:    192×108  (162 KB)
// High:     256×144   (288 KB)   [default]
// Cinematic: 512×288  (1.1 MB)
```

**Long-term: directional cubemap sky cache** — The lat-long sky-view LUT (256×144) is acceptable initially, but horizon gradients, sunset detail, Mie forward scattering, and cloud coupling will eventually expose resolution limits, especially pole-distortion artifacts near zenith during camera rotation. Future direction: store the sky cache as a cubemap face array (e.g., 6 × 128²) — better angular distribution, no pole distortion, reflection-friendly, simpler reprojection in 3D direction space.

**⚠️ API abstraction boundary — do NOT expose lat-long externally**: All sky-view LUT sampling must go through a single function that takes a direction vector and returns a color. Consumers (miss shader, reflection evaluation, environment sampling, aerial perspective composition) must NEVER know the underlying parameterization. This allows replacing lat-long with cubemap sky cache later without touching any shader outside of the LUT sampling file.

```glsl
// CORRECT — abstracted interface in atmosphere_lut_sampling.glsl:
vec3 sample_sky_view_lut(vec3 direction);   // direction → LUT lookup (lat-long or cubemap)
vec3 sample_previous_sky_view_lut(vec3 direction);

// INCORRECT — lat-long UV dependency leaks to consumers:
// vec3 sample_sky_view_lut(vec2 uv);       // WRONG — ties all consumers to lat-long
```

If `sample_sky_view_lut(vec2 uv)` exists anywhere outside the LUT sampling file, the migration to cubemap requires changes in the miss shader, reflection code, environment sampling, debug views, and temporal reprojection — each with their own lat-long→cubemap conversion bug surface. Keep it abstracted.

---

## Shader Architecture

The monolithic `rt_common.glsl` is split into separate files for maintainability and compile time.

### Shader Compile Iteration Strategy

The atmosphere system introduces multiple compute shaders (transmittance LUT, sky-view LUT, multi-scatter LUT, aerial perspective LUT, CDF build, reprojection) with shared GLSL includes. A clear iteration workflow is essential — without it, shader development becomes a painful edit-recompile-launch-inspect loop.

**Hot reload not available in v1**: Vulkan pipeline compilation is expensive and Vulkan does not provide a standard hot-reload mechanism. The iteration workflow relies on fast turnaround rather than runtime recompilation:

1. **Dedicated shader compilation script**: `tools/compile_atmosphere_shaders.bat` that invokes `glslc` on every atmosphere shader and reports errors per-file. This catches syntax and type errors without launching the engine.

2. **Include dependency graph**: The split shader files (`atmosphere_phase.glsl`, `atmosphere_lut_sampling.glsl`, `atmosphere_lighting.glsl`, `environment_sampling.glsl`) are independent compilation units that #include from `atmosphere_common.glsl`. Changing `atmosphere_common.glsl` invalidates all atmosphere shaders — the build script must detect this and recompile everything.

3. **Error surfacing during engine startup**: Pipeline creation errors (from invalid SPIR-V) are logged to the console with the shader name and line number. The engine should NOT crash on shader compile failure — instead, it should:
   - Log the full glslc error output
   - Disable the atmosphere system gracefully (sky defaults to flat color)
   - Continue rendering the scene

4. **Shader specialization constants**: Parameterize LUT resolution, step counts, and quality tiers via specialization constants rather than preprocessor defines where possible. This avoids recompilation when only the constant values change.

5. **Precompiled SPIR-V cache**: Store compiled SPIR-V blobs in a build directory alongside the source shaders. The build script only recompiles when source modification times change. This shaves ~2 seconds off each iteration for the full atmosphere shader set.

6. **Shader include path policy**: All atmosphere shaders use a consistent base include path (`-I src/shaders/atmosphere/`) to avoid relative-path confusion between shader files in different directories.

### `atmosphere_phase.glsl`

Pure phase functions — no scattering math, no LUTs:

```glsl
float rayleigh_phase(float cosTheta);
float mie_phase(float cosTheta, float g);
vec3 henyey_greenstein_phase(float cosTheta, float g);
```

### `atmosphere_lut_sampling.glsl`

LUT accessors — no scattering computation:

```glsl
vec3 sample_transmittance_lut(vec3 origin, vec3 dir);         // planet at (0,0,0)
vec3 sample_multi_scatter_lut(vec2 uv);
vec3 sample_sky_view_lut(vec3 dir);                           // diffuse only
void sample_aerial_perspective(vec3 worldPos, vec3 viewDir,
                                out vec3 inscatter, out float transmittance);
```

**Transmittance LUT coordinate mapping** (256×64):

```
UV.x = sqrt(max(0, altitude) / (atmosphereHeight))            // sqrt compresses UV near surface
UV.y = 0.5 + 0.5 * cos(zenithAngle)                           // cos zenith → [0,1]
```

- Altitude = `length(origin) - planetRadius`, clamped to [0, atmosphereHeight]
- Zenith angle = angle between `origin` direction and `-dir` direction
- The sqrt in U gives more LUT resolution near the surface (where Rayleigh density is highest)
- V spans the full [-1, 1] cosine range for both upward and downward-looking rays

### `atmosphere_lighting.glsl`

Lighting integration:

```glsl
vec3 sun_transmittance(vec3 worldPos, vec3 sunDir);           // LUT-sampled
vec3 sun_disk_radiance(vec3 dir);                             // analytical sun + limb darkening
vec3 fast_sky_radiance(vec3 dir);                             // single-scattering preview
void compose_aerial_perspective(inout vec3 radiance, vec3 worldPos, vec3 viewDir);
```

### `environment_sampling.glsl`

Importance sampling + MIS:

```glsl
vec3 environment_radiance_atmosphere(vec3 dir);               // sky LUT + analytical sun disk
float environment_pdf_atmosphere(vec3 dir);                    // CDF from sky LUT only (no sun)
vec3 sample_environment_direction_atmosphere(inout uint state,
                                              out vec3 dir, out float pdf);
```

### Ray-Sphere Intersection

In `atmosphere_lut_sampling.glsl`:

```glsl
// Returns intersection distances (tNear, tFar) for ray against sphere centered at origin.
// Handles: inside atmosphere, outside atmosphere, grazing angles.
// Uses double-precision intermediates for discriminant stability at
// planetary scale (R = 6,360,000 m).
bool ray_sphere_intersection(vec3 origin, vec3 dir, float radius,
                             out float tNear, out float tFar) {
    // Planet center is at (0, 0, 0) — no offset needed
    double a = double(dot(dir, dir));
    double b = 2.0 * double(dot(origin, dir));
    double c = double(dot(origin, origin)) - double(radius) * double(radius);
    double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return false;
    double sqrtDisc = sqrt(disc);
    double t0 = (-b - sqrtDisc) / (2.0 * a);
    double t1 = (-b + sqrtDisc) / (2.0 * a);
    tNear = max(float(t0), 0.0f);
    tFar = float(t1);
    return true;
}
```

Three cases handled:
- **Camera inside atmosphere**: tNear = 0, integration from camera to tFar
- **Camera outside atmosphere**: integration from tNear (entry) to tFar (exit)
- **Grazing rays**: large tFar, double-precision discriminant prevents misses

### Multi-Scattering Strategy

**⚠️ Deferred to Stage F**: Multi-scatter is the last major scattering feature. It introduces a multiplicative correction that obscures every other bug. Single-scattering (Stages A–E in Implementation Order) must be validated across all conditions before adding multi-scatter. Do NOT implement this section early — the Hillaire approximation is forgiving, but debugging optical depth, transmittance, and phase functions is already hard enough without an extra unknown.

Uses **Hillaire's single-approximation infinite scattering**:

1. Precompute a 32×32 multi-scattering LUT mapping (cos(view zenith), cos(sun zenith)) → multi-scattering contribution.
2. At runtime, sample LUT and multiply first-order scattering by the scale factor.
3. No iterative scattering passes needed.

This provides energy-conserving multiple scattering without the cost of iterative accumulation.

**Known limits**: The Hillaire approximation is a single-scale factor applied to first-order scattering. It models the *average* energy redistribution but does not capture directional detail. Differences from reference are most noticeable in:

- **Sunsets** — the reddening from multiple long-path scattering is weaker than a full Monte Carlo integration would produce
- **Heavy haze / turbid atmospheres** — thick aerosols change the multi-scattering distribution in ways a single-scale factor cannot represent
- **Cloud coupling** — clouds redirect scattered energy differently than the clear-sky assumption baked into the LUT

For v1 these limits are acceptable — the approximation produces visually plausible results across the standard clear-sky to sunset range. The multi-scattering LUT can be replaced with an iterative scattering pass later if fidelity requires it.

### Sun Transmittance for Direct Lighting

```glsl
vec3 sun_transmittance(vec3 worldPos, vec3 sunDir) {
    // Compute transmittance from worldPos to sun through atmosphere.
    // worldPos is in absolute planetary coordinates (planet center at origin).
    // Uses the transmittance LUT sampled from worldPos toward sun direction.
    return sample_transmittance_lut(worldPos, sunDir);
}
```

Applied in `pathtrace.rgen`:

```glsl
if (camera.sunlight_enabled != 0u) {
    vec3 sunDir = analytical_sun_direction();
    float ndl = max(dot(lightingNormal, sunDir), 0.0);
    if (ndl > 0.0 && !trace_shadow(...)) {
        vec3 sunAttenuation = sun_transmittance(hit.world_pos, sunDir);
        vec3 sunColor = analytical_sun_center_radiance() * sunAttenuation;
        sunContribution = bsdf * sunColor * ndl;
    }
}
```

### Sun/LUT Separation Policy

The sky-view LUT explicitly **excludes** the solar disk. It stores only the diffuse atmospheric scattering component.

**Miss shader path** (`environment_radiance(dir)`):
```glsl
vec3 environment_radiance(vec3 dir) {
    if (atmosphere_enabled) {
        vec3 sky = sample_sky_view_lut(dir);          // diffuse atmosphere
        vec3 sun = sun_disk_radiance(dir);             // analytical solar disk
        return sky + sun;
    }
    // fallback to HDR map or gradient as before
    ...
}
```

**Environment sampling** (`environment_pdf` / `sample_environment_direction`):
The importance CDF is built **only from the diffuse sky LUT**. The sun is not represented in the CDF because it's a delta light. MIS is performed between:
- BSDF sampling (cosine-weighted hemisphere)
- Diffuse sky importance sampling (from CDF)
- Sun is evaluated separately as a delta light (no PDF weight)

This avoids double-counting, ensures clean convergence, and keeps the importance CDF representing only the smooth diffuse sky dome.

### Sun Disk Angular Sampling Policy

The sun is currently modeled as a perfect delta light for direct lighting:

```glsl
sunContribution = bsdf * analytical_sun_center_radiance() * ndl;  // no sampling needed
```

This is correct for v1 — the sun disk is small (0.0093 rad ≈ 0.53°) and the delta approximation converges well with many samples.

**Future finite-area sun sampling** (`sample_sun_cone()`):

When soft shadows, volumetric shafts, or cloud scattering are introduced, the sun must be sampled as a finite angular emitter:

```glsl
vec3 sample_sun_cone(inout uint rngState, vec3 sunDir, out vec3 sampleDir, out float pdf) {
    // Sample a direction within the sun's angular disk
    float r1 = rand(rngState);
    float r2 = rand(rngState);
    float theta = sunAngularRadius * sqrt(r1);
    float phi = 2.0 * PI * r2;
    // Build local basis around sunDir, rotate by (theta, phi)
    sampleDir = sunDir + localU * sin(theta) * cos(phi) + localV * sin(theta) * sin(phi);
    sampleDir = normalize(sampleDir);
    pdf = 1.0 / (PI * sunAngularRadius * sunAngularRadius);  // uniform disk PDF
}
```

The `analytical_sun_center_radiance()` is replaced by evaluating the sun radiance times the BSDF at the sampled direction, divided by the PDF. This produces physically correct soft penumbrae and enables volumetric light shafts. Deferred until soft-shadow quality requires it.

### Transmittance Caching (Future)

For frequently sampled directions, consider caching:

- **Horizon transmittance** — precompute a 1D LUT of transmittance at the horizon for standard camera heights
- **Lookup optimization** — cache the most recent transmittance lookup by direction in shared memory

Not needed for v1, but will matter when atmosphere sampling becomes a bottleneck.

---

## LUT Update Scheduling (DirtyDAG)

The current manual-dirty-flag approach (`paramsDirty_`, `sunMovedThisFrame_`) becomes fragile as atmosphere gains independent dirty domains, partial updates, cached LUT reuse, and quality-dependent invalidation. Replace it with a dependency DAG.

### Dirty Bits

```cpp
enum class AtmosphereDirtyBits : uint32_t {
    None            = 0,
    Rayleigh        = 1 << 0,  // Rayleigh scattering coefficient
    Mie             = 1 << 1,  // Mie scattering coefficient
    Ozone           = 1 << 2,  // Ozone absorption
    SunDirection    = 1 << 3,  // Sun azimuth/elevation
    Exposure        = 1 << 4,  // EV100 change (tone mapper only)
    Quality         = 1 << 5,  // LUT resolution / quality tier
};
```

### LUT Dependency Declarations

Each LUT node declares which dirty bits invalidate it. The DAG is built once and traversed on each dirty set:

```cpp
struct LutNode {
    const char* name;
    AtmosphereDirtyBits invalidatedBy;  // bits that force recompute
    AtmosphereDirtyBits dependsOn;      // other LUTs this reads from
    void (*record)(VkCommandBuffer cmd);
};

// Dependency graph:
// Transmittance LUT  → invalidatedBy: Rayleigh | Mie | Ozone
// MultiScatter LUT    → invalidatedBy: Rayleigh | Mie | Ozone
//                      → dependsOn: Transmittance
// SkyView LUT         → invalidatedBy: SunDirection
//                      → dependsOn: Transmittance | MultiScatter
// AerialPerspective   → invalidatedBy: Rayleigh | Mie | Ozone
//                      → dependsOn: Transmittance
// CDF                 → invalidatedBy: (none — derives from SkyView)
//                      → dependsOn: SkyView
```

### Traversal

```cpp
class DirtyDAG {
public:
    void setDirty(AtmosphereDirtyBits bits);    // mark bits, traverse DAG
    bool needsUpdate(const char* lutName) const; // query per LUT
    void markUpdated(const char* lutName);       // after recording pass

    // Debug
    const char* lastDirtySource() const;
    uint64_t updateCount(const char* lutName) const;
private:
    AtmosphereDirtyBits dirtyBits_{~0u};  // start fully dirty
    // DAG edges built once in constructor
    std::vector<LutNode> nodes_;
};
```

This replaces all manual `if (dirty)` checks in the render loop with a single `DirtyDAG::setDirty()` call on parameter change. The DAG transitively marks dependent LUTs, ensuring nothing is missed when, e.g., Rayleigh changes invalidate transmittance → multi-scatter → sky-view → CDF in a single traversal.

Owned by `AtmosphereTemporalSystem`.

The sky-view LUT must be stable across frames to avoid flickering in both the rendered sky and the importance sampling CDF.

**v1 approach** — Camera-motion reprojection only, simple EMA blend. No variance clipping, no exposure rescaling, no rejection heuristics. These are deferred to v2 because temporal debugging before LUT correctness, lighting correctness, and exposure correctness is extremely painful. A basic `lerp(current, previous, 0.9)` with reprojection absorbs most visible flicker and is trivially debuggable.

### Sun-Based Trigger

```
if (length(sunDirection - previousSunDirection) > threshold) {
    // Full recompute of sky-view LUT
    // Reset importance CDF
} else {
    // Use temporal reprojection
}
```

### Camera Motion Reprojection (v1)

Camera rotation changes the sky-view mapping. When the sun is stationary but the camera rotates, the previous frame's sky LUT is reprojected:

```
// In sky_reproject.comp (v1 — simple):
// 1. For each texel in current frame's sky LUT
// 2. Convert texel UV → sky direction using current camera matrix
// 3. Compute previous-frame UV for that sky direction using previous camera matrix
// 4. Sample previous sky LUT at previous UV
// 5. Blend: result = lerp(current, previous, 0.9)
```

No neighborhood clamping, no standard deviation, no dynamic blend factor. Just camera reprojection + fixed-weight EMA.

### v2 Extensions (deferred)

These are correct and necessary for production quality but must not be implemented until Stage E is reached with the basic blend working:

- **Variance clamping**: Clamp history sample to `[μ - σ, μ + σ]` of 3×3 neighborhood before blending. Prevents ghosting during fast camera movement.
- **Dynamic blend factor**: Reduce history weight near motion boundaries (sky-surface edges, sun disk transition) to avoid streaking.
- **Exposure rescaling**: When EV100 changes, rescale history by `currentLinearExposure / previousLinearExposure` before blending. Prevents transient brightness flicker during exposure edits.
- **History rejection**: Discard history samples where the clamped value diverges from the current sample by more than a confidence interval. Handles occlusion changes and disocclusion events.

### Lat-Long Reprojection Distortion

The sky-view LUT uses a latitude-longitude parameterization. Reprojection via `UV → direction → previous UV` works correctly in most directions, but lat-long projections distort heavily near the poles:

- **Zenith compression**: Small motion near the zenith maps to large UV displacement, causing oversampling or hole-filling
- **Horizon stretching**: The opposite — directions near the horizon are oversampled in UV space, causing undersampling in the reprojection lookup

These manifest as subtle wobble near the zenith during rapid rotation. For v1, the simple EMA blend absorbs most of the artifact. For future improvement:

- **Octahedral mapping**: Projects the sphere onto an octahedron, halving maximum angular distortion
- **Cubemap reprojection**: Store sky-view as cubemap face array; reproject in 3D direction space with no UV singularity

### Implementation (v1)

- Store `previousSkyViewLut_` Image + `previousSunDirection_` + `previousViewProj_`
- `sky_reproject.comp` compute shader: UV reprojection + `lerp(current, previous, 0.9)`
- `recordTemporalReproject()` replaces full `recordSkyViewLut()` when the sky change is small
- Full recompute on parameter change, large sun movement, or accumulation reset
- `VarianceClampingParams` struct is defined for future use but `setVarianceClamping()` is a no-op in v1

---

## Environment Sampling — Sky Importance Sampling Subsystem

This is a critical subsystem, not an afterthought. Without proper importance sampling, the diffuse sky dome (especially near the horizon) will not converge efficiently in the path tracer.

**Note**: The sun is NOT in the sky LUT or the importance CDF. The sun is handled separately as an analytical delta light. This simplifies MIS and avoids double-counting.

### Compute Shader: `sky_importance_build.comp`

After the sky-view LUT is updated, this compute shader:

1. Extracts luminance from each texel of the sky-view LUT (diffuse only)
2. Builds marginal row CDF (height dimension)
3. Builds conditional column CDF (width dimension) per row
4. Writes to GPU buffers

### GPU Buffers (set 1, bindings 6-8)

```glsl
layout(set = 1, binding = 6, std430) readonly buffer SkyCdfRows { float sky_cdf_rows[]; };
layout(set = 1, binding = 7, std430) readonly buffer SkyCdfCols { float sky_cdf_cols[]; };
layout(set = 1, binding = 8, std140) uniform SkyLutParams {
    uint width;
    uint height;
    float inv_total_lum;
    float pad;
};
```

### MIS Strategy

Multiple Importance Sampling between:

| Technique | Samples | Distribution |
|-----------|---------|--------------|
| BSDF sampling | 1 sample | Cosine-weighted hemisphere |
| Environment importance | 1 sample | Sky LUT CDF (diffuse only) |
| Sun direct | Separate delta | Analytical (no sampling needed) |

Power heuristic is applied for BSDF vs environment MIS. The sun is evaluated unconditionally as a delta light with probability 1 (no PDF division needed).

### Shader Sampling API (in `environment_sampling.glsl`)

```glsl
vec3 environment_radiance_atmosphere(vec3 dir);           // sky LUT + analytical sun disk
float environment_pdf_atmosphere(vec3 dir);                // PDF from sky CDF (no sun)
vec3 sample_environment_direction_atmosphere(inout uint state,
                                              out vec3 dir, out float pdf);  // importance-sampled
```

### CDF Rebuild Frequency

- Only on sky-view LUT update (not every frame when temporal reprojection is active)
- The marginal/conditional buffers are double-buffered to avoid read-before-write hazards

### Blue Noise Sampling Integration

Atmosphere importance sampling is variance-sensitive — the sky dome contains high-dynamic-range features near the sun and horizon that amplify structured noise artifacts.

**Sequence**: Owen-scrambled Sobol (the `sobol` and `owen_scramble` intrinsics in GLSL, or a precomputed blue noise texture). The scrambling decorrelates samples across pixels, breaking the structured aliasing of naive random sampling.

**Per-pixel state**:

```glsl
uint sampleIndex = pixelIndex * frameCount + bounceIndex;
uvec2 sobolTerm = sobol(sampleIndex);
uint scrambled = owen_scramble(sobolTerm.x, scrambleSeed);
float u1 = float(scrambled) / 4294967295.0;
float u2 = float(sobolTerm.y) / 4294967295.0;
```

**Frame scrambling**: `scrambleSeed` is a random per-frame seed (e.g., hash of `frameCount`). This temporalizes the noise pattern so the denoiser can filter it.

**Spatial tiling** (alternative): If `sobol` intrinsics are unavailable, use a precomputed 128×128 blue noise texture tiled across the render target, with a per-frame texture rotation.

This applies to:
- `sample_environment_direction_atmosphere()` — sky CDF sampling
- `sun_disk_radiance()` — solar disk jitter for finite-area sampling (see note below)
- Cloud raymarch jitter (future)

---

## Aerial Perspective LUT

**⚠️ Deferred to Stage E**: The aerial perspective LUT is one of the highest-risk components. Its 3D parameterization, logarithmic slicing, coordinate mapping, transmittance coupling, and world-space lookup all fail in subtle and visually indistinguishable ways. Do NOT implement until atmosphere itself (Stages A–D) is producing correct, stable output. Start with analytic fog (Stage E.1) to validate the composition pipeline before tackling the full 3D LUT.

The aerial perspective LUT uses a **96×96×48** 3D texture with **logarithmic depth distribution**. (v1 start with 64×64×32 if VRAM is constrained — the 96×96×48 target prevents altitude banding, distant fog stepping, and horizon quantization in cinematic sun angles.)

### Storage Format

### Storage Format

**RGBA16F single texture** — RGB = chromatic inscatter, A = transmittance luminance scalar.

```
sampled.rgb = inscatter   (chromatic, wavelength-dependent)
sampled.a   = transmittance_luminance  (scalar approximation)
```

Atmospheric inscatter is chromatic (red at sunset, blue at noon). Packing it into RGB while using alpha for transmittance scalar preserves the correct fog coloration while keeping a single 3D texture lookup. The transmittance scalar is sufficient for the product `surface_color * transmittance` — the remaining color shift comes from the inscatter term.

### Depth Slicing

```cpp
// 48 depth slices distributed logarithmically
for (int i = 0; i < 48; ++i) {
    float t = pow(float(i) / 47.0f, 3.0f);  // cubic distribution
    float distance = lerp(1.0f, maxViewDistance, t);
}
```

This avoids banding at both close and far distances.

### Lookup

```glsl
void sample_aerial_perspective(vec3 worldPos, vec3 viewDir,
                                out vec3 inscatter, out float transmittance) {
    float height = length(worldPos) - planetRadius;
    float cosZenith = dot(normalize(worldPos), -normalize(viewDir));
    float distance = ...;  // distance from viewpoint to worldPos
    float depthSlice = log_slice(distance);
    vec3 uvw = vec3(cosZenith * 0.5 + 0.5, height / atmosphereHeight, depthSlice);
    vec4 sampled = texture(aerial_perspective_lut, uvw);
    inscatter = sampled.rgb;       // chromatic inscatter
    transmittance = sampled.a;     // luminance transmittance scalar
}
```

### Usage in Path Tracing

```glsl
vec3 surfaceRadiance = ...;  // computed by path tracer
vec3 inscatter;
float transmittance;
sample_aerial_perspective(hitPosition, viewDir, inscatter, transmittance);
vec3 finalRadiance = surfaceRadiance * transmittance + inscatter;
```

### Analytic Fog — Recommended Intermediate Step

The 96×96×48 aerial perspective LUT is architecturally correct, but 3D LUT parameterization (height, cosZenith, log-depth) is a substantial debugging surface. An analytic fog approximation provides immediate visual feedback and validates transmittance composition logic before committing to the full LUT:

```glsl
vec3 analyticFog(vec3 worldPos, vec3 viewDir, vec3 sunDir, vec3 surfaceColor) {
    float height = max(length(worldPos) - planetRadius, 0.0);
    float distance = length(worldPos - cameraPos);
    float density = exp(-height / 8000.0) * (1.0 - exp(-distance / 50000.0));
    vec3 inscatter = density * vec3(0.47, 0.52, 0.65);  // atmospheric blue
    float transmittance = exp(-density);
    return surfaceColor * transmittance + inscatter;
}
```

This is a temporary debugging tool, not a production path. Use it during Stage A implementation to verify that aerial perspective composition (inscatter × transmittance × surface color) works correctly in the path tracer. Replace with the LUT in Stage C.

**Recommendation**: Implement analytic fog first (Stage A), then the full LUT (Stage C). The analytic version costs ~5 shader lines and immediately validates the composition pipeline — before investing in 3D LUT lookups, depth slicing, and parameterization debugging.

---

---

## Fast Sky Evaluation Path

For reflections, shadow rays, preview mode, and editor thumbnails:

```glsl
vec3 fast_sky_radiance(vec3 dir) {
    // Simplified analytical approximation with optical depth.
    // Uses precomputed scattering coefficients + single-scattering only.
    // No LUT lookups, no multi-scattering.
    // Suitable for low-quality preview or non-primary rays.
    float cosTheta = dot(dir, sunDirection);
    float height = max(cameraAltitude, 0.0);

    // Approximate optical depth through atmosphere:
    //   opticalDepth ≈ atmosphereHeight * exp(-height / scaleHeight) / cos(sunZenith)
    // This prevents overly bright horizontal sky in fast mode.
    float cosSunZenith = max(dot(normalize(cameraPos), sunDirection), 0.001);
    float opticalDepthRayleigh = atmosphereHeight * exp(-height / rayleighScaleHeight) / cosSunZenith;
    float opticalDepthMie = atmosphereHeight * exp(-height / mieScaleHeight) / cosSunZenith;

    vec3 transmittance = exp(-(rayleigh_coeff * opticalDepthRayleigh + mie_coeff * opticalDepthMie));

    vec3 rayleigh = rayleigh_coeff * rayleigh_phase(cosTheta);
    vec3 mie = mie_coeff * mie_phase(cosTheta, mieAnisotropy);
    return (rayleigh + mie) * sunIlluminance * transmittance;
}
```

**Known divergence**: This path lacks multi-scattering and precise ozone absorption, so sunset coloration and horizon brightness may differ noticeably from the full LUT path. Acceptable for preview and indirect bounce use.

### Energy Matching

Without normalization, `fast_sky_radiance()` may diverge visibly from the LUT-based `environment_radiance()` in total energy. If indirect GI brightness shifts between ray types (e.g., a diffuse bounce appears dimmer than the primary ray's sky), the scene loses visual coherence.

Apply a normalization scalar to `fast_sky_radiance()` derived from the LUT system:

```glsl
// Option A — Average LUT luminance ratio (computed once after sky-view LUT update)
float fastNormalization = computeFastPathNormalization();
// Where: fastNormalization = avgLutLuminance / avgFastLuminance
// Sampled over a uniform direction distribution (~64 directions)

// Option B — Transmittance-integrated irradiance
float fastNormalization = integratedTransmittanceRatio;
// Where: integratedTransmittanceRatio = lut_total_irradiance / fast_total_irradiance
```

The normalization scalar is updated whenever the sky-view LUT is recomputed. This is not an energy correction (the fast path is intentionally approximate) but a perceptual matching term so the scene does not visibly shift brightness when switching between ray quality tiers.

This is exposed in the shader and used based on the ray quality policy (see below).

### Atmosphere Ray Quality Policy

Different ray types use different atmosphere evaluation quality:

```cpp
enum class AtmosphereRayQuality {
    Full,     // Full LUT lookups + analytical sun disk (primary rays, specular)
    Reduced,  // LUT-based atmosphere, no multi-scatter, no sun disk (glossy reflections)
    Fast,     // Single-scattering analytical (indirect diffuse, preview)
    Minimal,  // Transmittance-only (shadow rays, subsurface)
};
```

| Ray Type | Quality | Details |
|----------|---------|---------|
| Primary camera | Full | `environment_radiance()` with LUT + analytical sun disk |
| Specular reflection | Full | Full LUT + analytical sun disk for mirror/glass reflections |
| Glossy reflection | Reduced | LUT-based sky radiance, no multi-scatter, no sun disk |
| Diffuse GI bounce | Fast | `fast_sky_radiance()` — single-scattering only |
| Shadow ray | Minimal | `sun_transmittance()` only |
| Preview mode | Fast | `fast_sky_radiance()` — editor responsiveness |

The quality is passed via a uniform or push constant and evaluated in the shader.

---

## Numerical Stability in LUT Generation

LUT generation involves several numerically sensitive operations that must be handled with explicit guard policies. These matter most at the horizon (grazing rays at sunset) where atmosphere systems commonly produce NaN, Inf, or visual artifacts.

### Optical Depth Integration

The transmittance LUT integrates optical depth along ray segments. Integration must be stable for both thin (overhead) and extremely long (grazing horizon) paths:

```glsl
// Policy — optical depth sampling:
const int TRANSMITTANCE_STEPS = 200;   // primary integration
const int TRANSMITTANCE_STEPS_SUNSET = 500;  // when cos(viewZenith) < 0.05
// Switching to higher step count near horizon avoids banding artifacts
// where the optical depth integral becomes path-length-sensitive.
```

**Adaptive step selection**: When the ray segment spans a large altitude range (ground near horizon), uniform stepping may miss the dense lower atmosphere. Use the ratio of segment endpoints to detect this:

```glsl
bool needsSunsetSteps(float height0, float height1, float cosZenith) {
    // Horizon rays span the widest altitude range per unit step
    return abs(cosZenith) < 0.05f && abs(height1 - height0) > 50000.0f;
}
```

### Exponential Underflow Safety

Atmospheric density at high altitude is exponential. Multiplying very small floats produces denormals or zero:

```
// At 100 km altitude (top of atmosphere):
density ≈ exp(-100000.0 / 8400.0) = exp(-11.9) ≈ 6.7e-6   // fine
density² ≈ 4.5e-11   // still representable in f32
density^3 ≈ 3.0e-16  // near f32 denormal boundary (~1.17e-38)

// Danger zone: above ~120 km, cubing density gives denormals
// Policy: clamp density to minimum representable for multi-scatter contributions
const float MIN_OPTICAL_DEPTH = 1e-8f;  // below this, contribution is negligible
```

**Safety floors** for all atmospheric quantities:

| Quantity | Floor | Rationale |
|----------|-------|-----------|
| Optical depth | `1e-8f` | Below this, transmittance ≈ 1.0 (no attenuation) |
| Phase function | `1e-6f` | Prevents fireflies at back-scatter nulls |
| Multi-scatter factor | `1.0f` | Never darken the sky below single-scattering |
| Sun radiance | `1e-4f` | Prevents log(0) in exposure |

### Phase Function Stability Near g ≈ 1

The Henyey–Greenstein phase function becomes extremely sharp as asymmetry parameter g approaches 1 (Mie forward peak):

```glsl
float hgPhase(float cosTheta, float g) {
    float g2 = g * g;
    // Clamp denominator to prevent division by zero near g=1, cosTheta=1
    float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    denom = max(denom, 1e-4f);  // safety floor
    return (1.0f - g2) / (PI * denom * sqrt(denom));
}
```

Without the `denom` floor, `g = 0.99` and `cosTheta > 0.999` produces a denominator of ~1e-6 and a phase value exceeding 1e4 — causing fireflies in path tracing. The floor caps this to a physically implausible but numerically safe value.

**g clamping policy**:

| Context | Max g | Note |
|---------|-------|------|
| Clear sky Mie | 0.76 | Standard value |
| Heavy haze | 0.85 | Forward-scattering dominant |
| Analytical fast path | 0.90 | Acceptable for preview |
| LUT build | 0.85 | Must match runtime — clamp to prevent CDF explosion |

### Horizon Integration Instability

At the horizon (cosZenith ≈ 0), the ray passes through the maximum atmospheric path length. Two stability measures are required:

1. **Segment count scaling**: Double the integration steps when the ray is within 3° of the horizon (cosZenith < 0.05). This directly addresses the most common source of visible banding.

2. **Pre-computed transmittance floor**: The transmittance at the horizon should never fall below `exp(-MAX_OPTICAL_DEPTH)`. If the integrated depth exceeds ~30 (which would give transmittance ≈ 1e-13), clamp to `1e-6f` to prevent floating-point underflow interacting with the tone mapper.

### Transmittance Precision Loss

The transmittance LUT stores values in R16G16B16A16_UNORM. For clear skies, transmittance along short paths is ≈ 1.0 — quantized to 16-bit UNORM, this is exact. For sunset horizon paths, transmittance can drop to 1e-4 or lower — quantized to 16-bit, this is approximately 6 decimal digits of precision, which is sufficient for v1.

**If banding is visible in horizon transmittance**: switch to R16G16B16A16_SFLOAT (half-float), which provides better precision near zero at the cost of slightly more VRAM (+0 bytes for same format, but drivers may use different internal representation).

### Transmittance Caching (Future)

Not relevant for v1 — every lookup samples the LUT directly. Future optimization may cache frequently sampled UV coordinates for reflected rays.

---

## Atmospheric Ground Bounce

The multi-scattering approximation currently models atmosphere-to-atmosphere scattering only. It ignores ground-to-atmosphere bounce — sunlight that hits the terrain and scatters back into the atmosphere.

### Why It Matters

At sunset, terrain color strongly affects lower-atmosphere tint:
- Snow cover → cooler, brighter horizon
- Desert sand → warmer, more orange lower atmosphere
- Ocean → darker, slightly blue horizon tint
- Dense forest → muted, green-leaning horizon light

Without ground bounce, the lower atmosphere always appears with the same base hue regardless of surface albedo, which is physically incorrect and visibly flattens sunset scenes.

### Cost-Benefit

This is NOT full GI coupling (which requires path tracing from atmosphere samples into the scene). It is a single Lambertian bounce approximation added to the multi-scatter LUT generation:

```glsl
// Inside multi-scatter LUT compute shader:
vec3 groundAlbedo = atmosphere.groundAlbedo;  // single uniform, not texture
vec3 groundIrradiance = integrate_sky_dome_irradiance(sunDir);  // precomputed or approximated
vec3 groundBounce = groundAlbedo * groundIrradiance * groundReflectancePhase(cosZenith);
totalScattering += groundBounce * transmittanceToGround;
```

Where `groundReflectancePhase` is a Lambertian term: `1.0 / PI` cos-weighted by the view zenith angle.

**Critical**: Ground bounce evaluates against direct solar irradiance only, not recursively against atmosphere multi-scatter irradiance. The bounce feeds into the multi-scatter approximation as one additional scattering event, not as an iterative feedback loop. This prevents runaway energy accumulation. If ground bounce were evaluated against the multi-scattered sky dome, each iteration would add more energy until the lower atmosphere overbrihtens — the multi-scatter LUT would converge to an incorrect equilibrium rather than a single-bounce correction.

### Integration

- Added to the multi-scatter LUT compute shader
- `groundAlbedo` field already exists in `AtmosphereParams.groundAlbedo`
- No additional passes, no additional textures
- Total shader cost: ~10 lines

**Defer after multi-scatter validation**: Implement the multi-scatter LUT first without ground bounce. Validate horizon colors, sunset appearance, and overall sky dome energy independently. Add ground bounce only after the base multi-scatter LUT passes its acceptance criteria. This reduces unknowns during bring-up — when sunsets look wrong, the cause is optical depth or phase functions, not ground bounce coupling.

---

Add to `RendererDebugView`:

```cpp
AtmosphereTransmittance = 28,
AtmosphereMultiScatter = 29,
AtmosphereSkyView = 30,
AtmosphereAerialPerspective = 31,
AtmosphereOpticalDepth = 32,
AtmosphereScatteringCoeff = 33,
```

These are rendered by `sky_debug_views.comp` which samples the LUTs and outputs to a visualization image.

---

## AtmosphereModel (pure math)

```cpp
struct AtmosphereParams {
    // Planet
    float planetRadius = 6360000.0f;
    float atmosphereRadius = 6420000.0f;
    float groundAlbedo = 0.3f;

    // Rayleigh
    float rayleighScaleHeight = 8000.0f;
    glm::vec3 rayleighScattering = {5.8e-6f, 1.355e-5f, 3.309e-5f};  // RGB

    // Mie
    float mieScaleHeight = 1200.0f;
    float mieScattering = 2e-5f;
    float mieExtinction = 3e-5f;            // absorption included
    float miePhaseAnisotropy = 0.8f;

    // Ozone
    float ozoneScaleHeight = 25000.0f;
    float ozonePeakHeight = 23000.0f;
    glm::vec3 ozoneAbsorption = {0.65f, 1.0f, 0.05f};

    // Sun
    float sunAngularRadius = 0.004675f;     // radians
    glm::vec3 sunIlluminance = {1.474f, 1.484f, 1.374f};  // W/m²
};

class AtmosphereModel {
public:
    explicit AtmosphereModel(const AtmosphereParams& params = {});

    // Pure math — called from CPU and GPU via uniform buffer
    float rayleighPhase(float cosTheta) const;
    float miePhase(float cosTheta) const;
    glm::vec3 computeTransmittance(float height, float cosZenith) const;
    glm::vec3 computeInscatter(float height, float cosZenith,
                               float distance, glm::vec3 sunDir) const;

    const AtmosphereParams& params() const { return params_; }
    void setParams(const AtmosphereParams& p);
    AtmosphereParams& params() { return params_; }

private:
    AtmosphereParams params_;
};
```

**Ownership**: Stateless value object. Instantiated by `SkySystem`, referenced by all subsystems. Copied to GPU as uniform buffer via `AtmosphereLutSystem`.

---

## AtmosphereLutSystem (GPU resources + LUT generation, owns unified set 1)

```cpp
class AtmosphereLutSystem {
public:
    AtmosphereLutSystem(VulkanContext& context, ResourceAllocator& allocator,
                        DescriptorLayoutCache& layoutCache, PipelineCache& pipelineCache,
                        AtmosphereModel& model,
                        const std::filesystem::path& shaderDir,
                        const std::filesystem::path& shaderOutDir);
    ~AtmosphereLutSystem();

    // Resize on quality change
    void resize(VkExtent2D renderExtent, AtmosphereQuality quality);

    // Record compute passes (each bails if DirtyDAG says not needed)
    void recordTransmittanceLut(FrameGraphContext& ctx);
    void recordMultiScatterLut(FrameGraphContext& ctx);
    void recordSkyViewLut(FrameGraphContext& ctx, const CameraUniform& camera);
    void recordAerialPerspectiveLut(FrameGraphContext& ctx, const CameraUniform& camera);

    // Unified descriptor set — all atmosphere resources live here
    VkDescriptorSetLayout descriptorSetLayout() const;
    VkDescriptorSet descriptorSet() const;

    // Write CDF binding into the shared set (called by AtmosphereSamplingSystem)
    void updateCdfBinding(VkBuffer cdfRows, VkBuffer cdfCols, VkBuffer cdfParams);

    Image& transmittanceLut();
    Image& multiScatterLut();
    Image& skyViewLut();
    Image& aerialPerspectiveLut();

    VkSampler sampler() const;

    // Uniform buffer — populated from AtmosphereModel
    Buffer& atmosphereBuffer();
    void uploadUniforms(VkCommandBuffer cmd);

private:
    VulkanContext& context_;
    AtmosphereModel& model_;

    // LUT images (ownership)
    Image transmittanceLut_;
    Image multiScatterLut_;
    Image skyViewLut_;
    Image aerialPerspectiveLut_;

    // Sampler + unified descriptor set
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;  // bindings 0–8 (9 reserved for future sky classification)

    // Buffers
    Buffer atmosphereBuffer_;
    Buffer lutParamsBuffer_;

    // Pipelines
    std::unique_ptr<ComputePipeline> transmittancePipeline_;
    std::unique_ptr<ComputePipeline> multiScatterPipeline_;
    std::unique_ptr<ComputePipeline> skyViewPipeline_;
    std::unique_ptr<ComputePipeline> aerialPerspectivePipeline_;

    // Shaders
    std::unique_ptr<ShaderModule> transmittanceShader_;
    std::unique_ptr<ShaderModule> multiScatterShader_;
    std::unique_ptr<ShaderModule> skyViewShader_;
    std::unique_ptr<ShaderModule> aerialPerspectiveShader_;
};
```

---

## AtmosphereTemporalSystem (history + reprojection + DirtyDAG)

**⚠️ Deferred to Stage G**: Temporal reprojection must only be implemented AFTER sky (Stage A), LUTs (Stages B–C), environment sampling (Stage D), aerial perspective (Stage E), and multi-scatter (Stage F) are all producing correct, stable output. Temporal debugging before correctness is a trap — variance artifacts and reprojection errors are visually indistinguishable from scattering bugs. v1 uses simple EMA only (no variance clamping, no rejection heuristics).

```cpp
// VarianceClampingParams defined for future use.
// v1: setVarianceClamping() is a no-op — only camera reprojection + lerp(0.9) is active.
struct VarianceClampingParams {
    float historyWeight = 0.9f;
    float varianceGamma = 1.0f;
    float minHistoryFrames = 4.0f;
    float sunMovementThreshold = 0.001f;  // radians
};

class AtmosphereTemporalSystem {
public:
    AtmosphereTemporalSystem(VulkanContext& context, ResourceAllocator& allocator,
                             DescriptorLayoutCache& layoutCache, PipelineCache& pipelineCache,
                             const std::filesystem::path& shaderDir,
                             const std::filesystem::path& shaderOutDir);
    ~AtmosphereTemporalSystem();

    // DirtyDAG — called when any atmosphere parameter changes
    void markDirty(AtmosphereDirtyBits bits);

    // Per-frame scheduling — queries DirtyDAG
    bool needsTransmittanceUpdate() const;
    bool needsMultiScatterUpdate() const;
    bool needsSkyViewUpdate() const;
    bool needsAerialPerspectiveUpdate() const;
    bool sunMoved() const;

    void onTransmittanceUpdated();
    void onMultiScatterUpdated();
    void onSkyViewUpdated();
    void onAerialPerspectiveUpdated();

    // Sky-view reprojection
    // v1: simple camera reprojection + lerp(current, previous, 0.9). No variance clamping.
    void recordTemporalReproject(FrameGraphContext& ctx,
                                  const CameraUniform& camera);

    // Camera tracking
    void setCurrentCamera(const CameraUniform& camera);
    glm::vec3 sunDirection() const;
    void setSunDirection(glm::vec3 dir);
    void setSunAzimuthElevation(float azimuth, float elevation);

    // Quality
    void setQuality(AtmosphereQuality quality);
    // v1: no-op — variance clamping deferred to v2
    void setVarianceClamping(const VarianceClampingParams& params);
    void setRayQuality(AtmosphereRayQuality quality);

    // Access for LUT system
    bool shouldReplaceSkyView() const;  // false → use temporal reproject

private:
    // DirtyDAG (dependency graph for LUT invalidation)
    DirtyDAG dirtyDag_;

    // History
    Image previousSkyViewLut_;

    // Pipelines
    std::unique_ptr<ComputePipeline> reprojectPipeline_;

    // Shaders
    std::unique_ptr<ShaderModule> reprojectShader_;

    // State
    VarianceClampingParams varianceClamping_;
    glm::vec3 previousSunDirection_{};
    glm::mat4 previousViewProj_{1.0f};
    AtmosphereQuality quality_ = AtmosphereQuality::High;
    AtmosphereRayQuality rayQuality_ = AtmosphereRayQuality::Full;
};
```

---

## AtmosphereSamplingSystem (CDF + MIS — no descriptor set ownership)

```cpp
class AtmosphereSamplingSystem {
public:
    AtmosphereSamplingSystem(VulkanContext& context, ResourceAllocator& allocator,
                             DescriptorLayoutCache& layoutCache, PipelineCache& pipelineCache,
                             AtmosphereLutSystem& lutSystem,  // gets shared descriptor set
                             const std::filesystem::path& shaderDir,
                             const std::filesystem::path& shaderOutDir);
    ~AtmosphereSamplingSystem();

    // CDF build — called after sky-view LUT is updated
    void recordCdfBuild(FrameGraphContext& ctx);

    // Access for path tracer (owned by us, bound via LutSystem::updateCdfBinding)
    Buffer& cdfRows();
    Buffer& cdfCols();
    Buffer& cdfParamsBuffer();

private:
    VulkanContext& context_;
    AtmosphereLutSystem& lutSystem_;  // writes CDF buffers into shared descriptor set

    // Buffers
    Buffer cdfRows_;
    Buffer cdfCols_;
    Buffer cdfParamsBuffer_;

    // Pipeline
    std::unique_ptr<ComputePipeline> cdfPipeline_;

    // Shaders
    std::unique_ptr<ShaderModule> cdfShader_;
};
```

---

## SkySystem (thin orchestrator)

```cpp
class SkySystem {
public:
    SkySystem(VulkanContext& context, ResourceAllocator& allocator,
              DescriptorLayoutCache& layoutCache, PipelineCache& pipelineCache,
              const std::filesystem::path& shaderDir,
              const std::filesystem::path& shaderOutDir);
    ~SkySystem();

    // High-level per-frame record — queries DirtyDAG, dispatches subsystems
    void recordAtmosphere(FrameGraphContext& ctx, const CameraUniform& camera);

    // Settings passthrough
    void updateSettings(const AtmosphereParams& params);
    void setSunDirection(glm::vec3 dir);
    void setSunAzimuthElevation(float azimuth, float elevation);
    void setQuality(AtmosphereQuality quality);
    void setVarianceClamping(const VarianceClampingParams& params);
    void setRayQuality(AtmosphereRayQuality quality);

    // Single unified descriptor set (set 1) — all atmosphere resources
    VkDescriptorSet descriptorSet() const;
    VkDescriptorSetLayout descriptorSetLayout() const;

    AtmosphereLutSystem& lutSystem() { return *lutSystem_; }
    AtmosphereTemporalSystem& temporalSystem() { return *temporalSystem_; }
    AtmosphereSamplingSystem& samplingSystem() { return *samplingSystem_; }

private:
    std::unique_ptr<AtmosphereModel> model_;
    std::unique_ptr<AtmosphereLutSystem> lutSystem_;
    std::unique_ptr<AtmosphereTemporalSystem> temporalSystem_;
    std::unique_ptr<AtmosphereSamplingSystem> samplingSystem_;
};
```

---

## Path Tracer Integration

### Environment Radiance (Miss Shader + `environment_sampling.glsl`)

`environment_radiance(dir)` combines the diffuse sky LUT with the analytical sun disk:

```glsl
vec3 environment_radiance(vec3 dir) {
    if (atmosphere_enabled) {
        vec3 sky = sample_sky_view_lut(dir);          // diffuse atmosphere only
        vec3 sun = sun_disk_radiance(dir);             // analytical solar disk + limb darkening
        return sky + sun;
    }
    // fallback to HDR map or gradient as before
    ...
}
```

The sky LUT excludes the solar disk entirely. The sun is added analytically with proper angular falloff and limb darkening.

### Environment Sampling

`sample_environment_direction()` and `environment_pdf()` use the sky importance CDF **without the sun component**. The sun is a delta light evaluated separately.

```glsl
float environment_pdf(vec3 dir) {
    if (atmosphere_enabled) {
        // PDF from diffuse sky CDF only — no sun contribution
        return environment_pdf_atmosphere(dir);
    }
    ...
}
```

### MIS Strategy

Multiple Importance Sampling between:

| Technique | Samples | Handles |
|-----------|---------|---------|
| BSDF sampling | 1 sample | Cosine hemisphere / GGX |
| Environment importance | 1 sample | Sky CDF (diffuse dome only) |
| Sun direct | Separate | Analytical delta light |

Power heuristic is applied:
```glsl
// BSDF vs environment MIS
float weight = power_heuristic(bsdfPdf, envPdf);
environmentContribution = bsdf * envRadiance * weight / max(envPdf, 1e-6);

// Sun is a delta light — no PDF contest
sunContribution = bsdf * sunColor * ndl;  // no MIS weight
```

The sun is not in the environment's PDF or sampling, so there is no double-counting risk.

### Sun Transmittance in Direct Lighting

```glsl
if (camera.sunlight_enabled != 0u) {
    vec3 sunDir = analytical_sun_direction();
    float ndl = max(dot(lightingNormal, sunDir), 0.0);
    if (ndl > 0.0 && !trace_shadow(shadow_origin(hit, sunDir), sunDir, 10000.0)) {
        vec3 sunAttenuation = sun_transmittance(hit.world_pos, sunDir);
        vec3 bsdf = eval_brdf(material, wo, sunDir, lightingNormal);
        sunContribution = bsdf * analytical_sun_center_radiance() * sunAttenuation * ndl;
    }
}
```

### Reflection Quality Policy

The atmosphere quality for secondary rays follows the `AtmosphereRayQuality` enum:

```glsl
// In indirect ray evaluation:
if (bounce > 0 && rayQuality <= AtmosphereRayQuality_Fast) {
    vec3 sky = fast_sky_radiance(ray.direction);  // single-scattering only
} else {
    vec3 sky = environment_radiance(ray.direction);  // full LUT + sun
}
```

**Quality transition note**: The step between quality tiers (Full → Reduced → Fast) can produce visible discontinuities on highly reflective surfaces (water, chrome, polished metal) where a single surface reflects both primary-quality and reduced-quality rays depending on bounce depth. These transitions are generally subtle — the difference is diffuse sky color, not sun disk presence — and may not require explicit smoothing in v1. If artifacts appear, a temporal blend or hierarchical quality fallback can be added.

### Aerial Perspective in Ray Generation (`pathtrace.rgen`)

```glsl
if (atmosphere_enabled && did_hit) {
    vec3 viewDir = normalize(ray.origin - hit_position);
    vec3 inscatter;
    float transmittance;
    sample_aerial_perspective(hit_position, -ray.direction, inscatter, transmittance);
    sample_color = sample_color * transmittance + inscatter;
}
```

### Descriptor Binding

Both `recordHardwarePathTrace()` and `recordComputePathTrace()` bind set 0 (scene) + set 1 (atmosphere):

```cpp
VkDescriptorSet sets[] = { sceneSet.handle(), skySystem_->descriptorSet() };
vkCmdBindDescriptorSets(cmd, bindPoint, layout, 0, 2, sets, 0, nullptr);
```

---

## Render Loop Integration

In `recordPathTrace()`, the atmosphere passes are registered as render graph nodes. The render graph tracks all LUT dependencies and inserts barriers automatically:

```cpp
void PathTracerRenderer::recordAtmosphere(FrameGraphContext& ctx,
                                           const CameraUniform& camera) {
    if (!settings_.atmosphereEnabled) return;

    skySystem_->recordAtmosphere(ctx, camera);
}
```

Inside `SkySystem::recordAtmosphere()`:

```cpp
void SkySystem::recordAtmosphere(FrameGraphContext& ctx,
                                  const CameraUniform& camera) {
    temporalSystem_->setCurrentCamera(camera);

    // Each tick advances the temporal system's frame counter for history validation.
    // At v1 start, the DirtyDAG is fully dirty — all LUTs generate on first frame.

    // Transmittance LUT (if DirtyDAG says needed)
    if (temporalSystem_->needsTransmittanceUpdate()) {
        lutSystem_->recordTransmittanceLut(ctx);
        temporalSystem_->onTransmittanceUpdated();
    }
    // Multi-scatter LUT (if DirtyDAG says needed)
    if (temporalSystem_->needsMultiScatterUpdate()) {
        lutSystem_->recordMultiScatterLut(ctx);
        temporalSystem_->onMultiScatterUpdated();
    }
    // Aerial Perspective LUT (if DirtyDAG says needed)
    if (temporalSystem_->needsAerialPerspectiveUpdate()) {
        lutSystem_->recordAerialPerspectiveLut(ctx, camera);
        temporalSystem_->onAerialPerspectiveUpdated();
    }
    // Sky-view LUT: temporal reproject (preferred) or full recompute
    if (temporalSystem_->needsSkyViewUpdate()) {
        if (temporalSystem_->shouldReplaceSkyView()) {
            lutSystem_->recordSkyViewLut(ctx, camera);
        } else {
            temporalSystem_->recordTemporalReproject(ctx, camera);
        }
        temporalSystem_->onSkyViewUpdated();
    }
    // CDF build follows sky-view LUT
    if (!temporalSystem_->needsSkyViewUpdate()) {
        // CDF rebuilds every frame (cheap compared to path tracing)
        samplingSystem_->recordCdfBuild(ctx);
    }
}
```

### Render Graph Node Declarations

**⚠️ Deferred — explicit sequencing preferred through Stage D.** The render graph formalization below is correct for the final architecture, but during early atmosphere bring-up (Stages A–D), explicit per-pass recording with manual barriers is preferred. A generic render graph node abstraction adds ownership ambiguity, barrier debugging complexity, and pass-ordering indirection that slows iteration when the underlying LUT math is still changing. The node table is the *target architecture* — implement it when the atmosphere compute pipeline is stable (Stage E or later). Until then, sequence passes explicitly:

```cpp
// Preferred for Stages A–D: explicit sequencing.
// NOT a render graph node abstraction.
recordTransmittanceLut(cmd);
barrier(cmd);
recordSkyViewLut(cmd);
barrier(cmd);
recordCdfBuild(cmd);
```

Each LUT pass is registered as a render graph node. The render graph uses these declarations to build the automatic dependency graph:

```
Node                          │ Writes                    │ Reads                        │ Stage
──────────────────────────────┼───────────────────────────┼──────────────────────────────┼─────────────────────
RecordTransmittanceLut        │ transmittance_lut         │ (none — pure compute)        │ COMPUTE
RecordMultiScatterLut         │ multi_scatter_lut         │ transmittance_lut            │ COMPUTE
RecordSkyViewLut              │ sky_view_lut              │ transmittance_lut,           │ COMPUTE
                              │                           │  multi_scatter_lut           │
RecordTemporalReproject       │ sky_view_lut              │ previous_sky_view_lut,       │ COMPUTE
                              │                           │  velocity                    │
RecordAerialPerspectiveLut    │ aerial_perspective_lut    │ transmittance_lut            │ COMPUTE
RecordCdfBuild                │ cdf_rows, cdf_cols        │ sky_view_lut                 │ COMPUTE
PathTrace                     │ output_image              │ all LUTs, cdf_rows/cdf_cols  │ RAYTRACE
```

The render graph automatically:

- Inserts `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT → VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` barriers between LUT passes
- Inserts `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT → VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR` before path tracing
- Skips barrier edges for unchanged (dirty=false) nodes
- Handles `previous_sky_view_lut` as a history resource — rendered to on even frames, read from on odd frames (ping-pong)

### Descriptor Binding

Both `recordHardwarePathTrace()` and `recordComputePathTrace()` bind set 0 (scene) + set 1 (unified atmosphere):

```cpp
VkDescriptorSet sets[] = {
    sceneSet.handle(),
    skySystem_->descriptorSet()
};
vkCmdBindDescriptorSets(cmd, bindPoint, layout, 0, 2, sets, 0, nullptr);
```

The single atmosphere descriptor set (owned by `AtmosphereLutSystem`) contains all LUTs, CDF buffers, uniforms, and sampler. `AtmosphereSamplingSystem` writes its CDF buffers into it via `AtmosphereLutSystem::updateCdfBinding()`.

---

## SkyClassification Buffer

Atmosphere content (sky, sun disk, horizon, aerial perspective haze) has very different statistics from surface content. Generic denoisers smear horizons, blur solar disks, and ghost aerial perspective without per-pixel classification.

### Buffer (definition only — no allocation in v1)

```glsl
// Per-pixel classification, 1 byte per pixel
// Binding 9 in set 1 (reserved, not allocated in v1)
#define SKY_PURE          0  // No surface in this pixel — pure sky/atmosphere
#define SKY_HORIZON       1  // Sky-surface boundary — denoise carefully
#define SKY_SUN           2  // Sun disk pixel — do not denoise
#define SKY_AERIAL_FAR    3  // Far distance aerial perspective — high transmittance weight
#define SKY_AERIAL_NEAR   4  // Near distance aerial perspective — blend
#define SKY_SURFACE       5  // Pure surface (may still have aerial perspective overlay, but surface-dominant)
```

### Status

**Architecture placeholder only — not implemented in v1.**

The enum definitions above are the full extent of v1. No buffer is allocated, no descriptor binding is created, no image is written in the ray generation shader. Even reserving binding/layout complexity early has cost — this section exists solely to document the long-term intent.

When implementation begins:
- **Generation**: Written by `pathtrace.rgen` as a side channel during ray dispatch — classify each pixel based on hit state, sun disk overlap, distance, and horizon proximity
- **Denoiser**: Reads classification to adjust variance thresholds, disable temporal accumulation on sun pixels, and apply horizon-aware filtering kernels
- **Tone mapper**: Applies different bloom thresholds for sun pixels vs sky pixels

### Ownership

Allocated as a transient render graph resource matching the output resolution. Not persisted across frames — regenerated every frame by the ray generation shader.

---

## Atmospheric Shadowing (Future)

Atmospheric shadowing — the soft, wavelength-dependent shadow cast by atmospheric scattering on distant objects — is a known gap in v1.

### What v1 handles

- **Sun transmittance**: `sun_transmittance(worldPos, sunDir)` evaluates atmosphere transmittance along the sun ray from `worldPos` to the top of atmosphere. This handles direct sunlight attenuation for any shaded point.
- **Single-scattered sky light**: The environment sampling + path tracer already captures sky dome illumination at the shaded point.

### What v1 does NOT handle

- **Distant object shadowing**: A tall mountain 50 km away should cast a blue tinted shadow on the ground behind it because the atmosphere scatters shorter wavelengths into the shadow volume. v1 ignores this — shadows are binary (sun disk occluded or not) with Lambertian attenuation.
- **Atmospheric penumbra**: The sun disk is 0.5° wide. A shadow at 10 km has a penumbral region ~87 m wide due to the angular extent of the sun. v1 treats the sun as a directional light — no penumbra.
- **Volumetric shadow**: Smoke, haze, and cloud shadows that vary with atmospheric density are not modeled.

### Future approach

Atmospheric shadowing is deferred until after the core atmosphere system is stable. When addressed, the likely strategy is:

1. **Angular sun disk sampling** — Replace the binary shadow ray with 4-8 shadow rays distributed across the sun disk, each evaluating sun transmittance. This gives physically correct penumbra and chromatic softening.
2. **Screened Poisson or moment-based filtering** — For performance, sample 1 shadow ray per pixel + spatial/temporal filter to reconstruct penumbra.
3. **Atmospheric shadow map (distant occluders, long-term)** — For objects > 1 km, precompute atmosphere-aware transmittance through a coarse volumetric shadow map. This is an optimization, not a correctness fix.

No action needed in v1. This section exists to prevent architectural surprises — specifically, the `sun_transmittance()` function signature should support angular disk sampling when the time comes.

## Editor UI

Add an "Atmosphere" section to `RenderSettingsPanel.cpp`:

```
Atmosphere
  ├── Enable Atmosphere            [checkbox]
  ├── Quality                      [combo: Low/Medium/High/Cinematic]
  ├── Debug View                   [combo: Off/Transmittance/MultiScatter/SkyView/...]
  │
  ├── Sun
  │   ├── Elevation               [slider -90..90 degrees]
  │   ├── Azimuth                 [slider 0..360 degrees]
  │   ├── Illuminance             [slider 1000..200000 lux]
  │   └── Angular Radius          [slider 0.0..0.05 rad]
  │
  ├── Scattering
  │   ├── Sky Intensity           [slider 0..5]
  │   ├── Rayleigh Scattering     [color picker RGB]
  │   ├── Rayleigh Scale Height   [slider 1000..20000 m]
  │   ├── Mie Scattering          [color picker RGB]
  │   ├── Mie Scale Height        [slider 200..5000 m]
  │   └── Mie Anisotropy          [slider -0.999..0.999]
  │
  ├── Ozone
  │   ├── Layer Center            [slider 10000..40000 m]
  │   └── Layer Half Width        [slider 5000..30000 m]
  │
  └── Ground Albedo               [slider 0..1]
```

Add a "Physical Camera" section:

```
Physical Camera
  ├── Aperture (f-stop)            [slider 1.0..32.0]
  ├── Shutter Speed                [slider 0.001..30.0 s]
  ├── ISO                          [slider 50..12800]
  └── Exposure Compensation        [slider -5..+5 EV]
```

---

## Dynamic Exposure Adaptation (Future)

A physically correct atmosphere spans enormous luminance ranges — noon sun (~100,000 cd/m²), sunset (~1,000 cd/m²), twilight (~1 cd/m²), moonlit night (~0.01 cd/m²). No single fixed exposure can capture this range without either clipping highlights or crushing shadows.

### Current State

The PhysicalCamera system provides manual EV100 control (aperture, shutter, ISO). This works for static shots and editor use, but the noon→sunset→night transition will be unusable without adaptation — the sky either clips to white or the ground falls into black.

### Future Approach

- **Histogram-based metering**: Compute a luminance histogram from the tone mapper input each frame. Derive target EV100 from the histogram's average or median luminance, weighted by a zone-system style importance mask.
- **Temporal adaptation**: Smooth the EV100 transition with a time constant (0.1–2.0 s) to prevent abrupt brightness changes. Faster adaptation for large scene changes (doorway transitions, sun direction changes), slower for continuous adjustment.
- **Local exposure (zone system)**: For high-contrast scenes (sun in frame with dark interior), apply a spatial exposure map (e.g., Gaussian-filtered luminance at half resolution) to prevent simultaneous highlight clip and shadow crush. UE5's eye adaptation uses a similar per-pixel exposure blend.
- **User control**: Adaptation speed, target luminance range, manual EV offset for artistic override.

### Integration

- Reads the same luminance buffer as the tone mapper
- Writes target EV100 into the PhysicalCamera uniform buffer
- Temporal reprojection v2 extension already rescales history on EV100 changes

Not needed for v1 — manual exposure is sufficient for static lighting scenes. Implement when the renderer supports dynamic lighting (day/night cycles, scene transitions, editor flythrough with realistic skies).

---

## Atmosphere for Reflections (Future)

Currently, the sky system is integrated into miss rays, diffuse GI, and direct lighting. Glossy and mirror surfaces will eventually need specialized atmosphere sampling to avoid noise, banding, and energy inconsistency in reflections.

### What Works Already

- Miss shader: `environment_radiance(dir)` with full LUT + analytical sun disk — correct for primary rays
- Specular reflections: `environment_radiance()` for mirror surfaces — correct because the reflection is a single direction
- Diffuse GI: `fast_sky_radiance()` for bounce rays — approximate but acceptable

### What Will Need Work

- **Roughness-filtered environment**: A glossy reflection samples a cone of directions. Sampling the sky LUT at the reflection direction only (single-sample) ignores roughness, producing a mirror-sharp reflection even on rough glossy surfaces. Future: prefiltered sky cache (cubemap mip-chain or dual-paraboloid at multiple roughness levels).
- **Importance for reflections**: The environment CDF is optimized for the full dome. Glossy reflections only sample a lobe-shaped region. Future: a secondary, roughness-parameterized importance map or on-the-fly rejection sampling within the lobe.
- **Energy consistency**: The fast path (`fast_sky_radiance()`) is used for diffuse GI but not for glossy reflections. When glossy and diffuse GI sample different sky paths at the same bounce depth, the energy mismatch can produce visible seams. The normalization scalar (see Energy Matching above) mitigates this but does not fully solve it for multi-bounce paths.

### Recommendation

Add a roughness parameter to `environment_radiance()` that triggers:
- Roughness < 0.05: single-direction LUT lookup (current behavior)
- Roughness 0.05–0.3: trilinear-filtered LUT (blurs the sky-view LUT in UV space by roughness-scaled offset)
- Roughness > 0.3: `fast_sky_radiance()` (approximate, acceptable for diffuse)

This is not required for v1. Implement when glossy reflections show visible noise or banding on rough metal/water surfaces against clear skies.

---

### Scope Note

Volumetric clouds are a full rendering subsystem, equal in scope and complexity to the atmosphere system. UE5-quality clouds typically require multiple months of engineering. The cloud system will likely exceed the atmosphere in shader complexity and runtime cost.

### Integration Strategy

Clouds integrate directly into `environment_radiance()`, not as a compositing overlay. This means:

- Clouds are visible in reflections (indirect rays sample `environment_radiance()`)
- Clouds affect atmosphere transmittance (cloud shadow on the ground)
- Clouds contribute to environment lighting for the path tracer

```glsl
vec3 environment_radiance(vec3 dir) {
    if (atmosphere_enabled) {
        vec3 sky = sample_sky_view_lut(dir);
        vec3 cloud = sample_cloud_radiance(dir);  // raymarch cloud density
        // Clouds scatter and transmit sky behind them
        return sky * cloud_transmittance + cloud_inscatter;
    }
    ...
}
```

### CDF Integration (Future)

When clouds are added, the environment importance CDF must be extended. Clouds contribute directional luminance that differs significantly from the clear-sky distribution. Two strategies:

1. **Separate cloud CDF** — Precompute a 2D CDF from the cloud luminance field and MIS between sky + cloud CDFs
2. **Combined CDF** — Recompute the sky-view LUT with cloud contribution baked in, rebuild a single combined CDF

Strategy 1 is recommended for modularity — clouds update at a different rate from atmosphere parameters, so separate CDFs can be rebuilt independently. The MIS power heuristic extends to three techniques: BSDF, sky CDF, cloud CDF.

This is noted here for architectural awareness; implementation is deferred to the cloud system phase.

### What Cloud Rendering Involves

- **Noise generation**: 3D Perlin-Worley base shape + detail erosion + curl noise
- **Weather system**: Coverage maps, cloud type maps, precipitation data
- **Density evaluation**: Base shape × detail × height gradient × weather coverage
- **Raymarching**: Intersection with cloud shell (bottom/top height), fixed-step or adaptive raymarch
- **Light scattering**: Beer-Lambert extinction + Henyey-Greenstein phase + multi-scattering approximation
- **Shadow integration**: Cone marching toward sun, or precomputed shadow LUT
- **Temporal reprojection**: Half-resolution render, reproject using velocity, blue noise jitter
- **Atmosphere coupling**: Clouds participate in aerial perspective; sky behind clouds is atmospherically attenuated

### Files (future)

- `include/rtv/CloudSystem.h` / `src/rtv/CloudSystem.cpp` (~1500+ lines)
- `shaders/cloud_noise.glsl` (Perlin-Worley utilities)
- `shaders/cloud_raymarch.comp`
- `shaders/cloud_density.comp`
- `shaders/cloud_temporal.comp`
- `shaders/cloud_shadow.comp`

### Key Techniques (for reference when implementation begins)

- 3D Perlin-Worley noise for base shape + detail
- Weather map driven coverage + cloud type
- Beer-Lambert extinction + Henyey-Greenstein phase
- Temporal reprojection with blue noise jitter
- Shadow cone marching (not per-sample sun rays)
- Weather system (coverage, precipitation, cloud type blending)
- Multi-scattering approximation for cloud self-shadowing
- Silver lining effect (energy redistribution toward sun-facing edges)

---

## Atmosphere Validation Strategy

A physically based atmosphere system is uniquely difficult to debug because errors in scattering physics, exposure, tone mapping, LUT coordinate mapping, and phase functions are visually indistinguishable. A systematic validation methodology — not visual inspection alone — is essential.

### Reference Methodology

| Reference | Type | What it provides |
|-----------|------|------------------|
| Bruneton 2017 ("Precomputed Atmospheric Scattering") | C++ reference implementation, source available | Ground-truth transmittance, inscatter, sky dome for standard Earth atmosphere parameters |
| Eric Bruneton's online demos | WebGL interactive | Visual reference for noon, sunset, twilight at multiple altitudes |
| UE5 SkyAtmosphere | Real-time engine reference | Comparison for quality tier behavior, LUT resolution impact, temporal behavior |
| Hosek-Wilkie 2012 | Analytic sky model | Sparse but useful cross-reference for clear sky luminance distribution |
| Mitsuba 3 (path tracer) | Spectral reference renderer | Ground-truth for single-scattering validation via `scene-atmosphere.xml` examples |

**Not a reference**: A random screenshot from Google Images. Atmosphere appearance is exposure-dependent, tone-mapper-dependent, and display-dependent. Two correct implementations with different exposures or tone mappers will look different. Validation must use the same physical parameters, same exposure, and same tone mapper.

### Validation Scenes

Each validation scene is a documented test case with a known-correct output expectation:

#### 1. Noon Clear Sky (Zenith Validation)

| Parameter | Value |
|-----------|-------|
| Sun elevation | 90° (overhead) |
| Turbidity | Clear (default Rayleigh/Mie coefficients) |
| Altitude | Sea level (0 m) |
| Ground albedo | 0.3 |
| Exposure | EV100 = 15 (sunny 16 rule: f/16, 1/100s, ISO 100) |

**Expected**: Deep blue zenith (approximately RGB ~(0.3, 0.5, 0.9) after tone mapping). Horizon is lighter blue but not white. Sun disk is >10× brighter than sky dome. No color banding in the zenith-to-horizon gradient.

**Common failures**: Zenith too cyan (ozone missing or wrong coefficients), horizon too dark (transmittance error), sun disk too dim (exposure calibration wrong).

#### 2. Sunset Horizon (Energy Validation)

| Parameter | Value |
|-----------|-------|
| Sun elevation | 2° (near horizon) |
| Turbidity | Clear |
| Altitude | Sea level |
| Ground albedo | 0.3 |
| Exposure | EV100 = 12 (compensate for lower light) |

**Expected**: Strong orange-to-red tint near the sun. Sky dome opposite the sun shows purple-pink (Bishop's ring). Rayleigh scattering dominates — blue light is scattered out of the direct path, leaving only red/orange. Horizon glow is smooth, no abrupt cutoff.

**Common failures**: No warm tint (sun transmittance not wavelength-dependent), horizon abrupt (LUT resolution too low or coordinate mapping wrong), sky too bright overall (exposure not compensated).

#### 3. Space View (Transmittance Validation)

| Parameter | Value |
|-----------|-------|
| Camera altitude | 100,000 m (top of atmosphere + some margin) |
| Sun elevation | 45° |
| Turbidity | Clear |
| Ground albedo | 0.3 |
| Exposure | EV100 = 15 |

**Expected**: Thin blue atmospheric limb at the horizon. Space above is black. Ground is visible but slightly blue-tinted by atmospheric transmittance. The limb is a narrow gradient (~2° angular width).

**Common failures**: Atmosphere visible above the horizon (atmosphere shell radius wrong or transmittance not zero above atmosphere), limb too wide (LUT resolution or scattering coefficient error), ground too blue (transmittance over-attenuates red).

#### 4. Sun Transmittance Curves (Numerical Validation)

Sample `sun_transmittance(hitPos, sunDir)` at multiple altitudes with the sun at varying angles. Plot transmittance per RGB channel:

| Test case | Altitude | Sun zenith angle | Expected transmittance (R, G, B) |
|-----------|----------|-----------------|----------------------------------|
| Overhead, sea level | 0 m | 0° | ~(0.3, 0.4, 0.5) |
| Overhead, mountain | 3000 m | 0° | ~(0.6, 0.7, 0.8) |
| Sunset, sea level | 0 m | 88° | ~(0.01, 0.001, 0.0001) |
| Sunset, mountain | 3000 m | 88° | ~(0.05, 0.01, 0.001) |

**Expected**: Red channel attenuates least (longest wavelength), blue most. The ratio R/B increases as the path through the atmosphere lengthens (sunset). Values should match Bruneton reference to within 5%.

**Common failures**: Transmittance too high (optical depth integration error), no wavelength separation (all channels equal), transmittance too low (over-integration or step count too low).

#### 5. Altitude Scattering Test

Render the same view at multiple altitudes (sea level, 2000 m, 5000 m, 10000 m). The sky should:
- Become darker (less scattering above)
- Become cooler in color temperature (less Mie scattering at high altitude)
- The horizon gradient becomes sharper

This validates that scale heights and altitude-dependent density are correct.

#### 6. White Furnace Sky Test (Energy Conservation)

Replace the sun with a uniform white emitter covering the entire sky dome. The total energy arriving at the camera (integrated over all directions) should equal the total emitted energy minus absorption. This validates that scattering does not create or destroy energy.

**Expected**: The integrated radiance across the sky dome is within 1% of the emitted radiance (minus ground absorption). Any significant deviation indicates an energy leak in the scattering model.

### Diagnostic Workflow

When the sky looks wrong, do NOT tweak parameters. Follow this diagnostic tree:

```
Sky looks wrong
├── Is exposure correct?
│   ├── NO → Fix exposure calibration first (sun disk should be ~10⁶ cd/m² before tone mapping)
│   └── YES → Is tone mapping correct?
│       ├── NO → Verify filmic curve, white point, highlight desaturation
│       └── YES → Is scattering correct?
│           ├── Check transmittance LUT (validate against analytical Bruneton reference)
│           ├── Check sky-view LUT (compare against Stage A analytical for same parameters)
│           ├── Check sun transmittance (curves at multiple altitudes/angles)
│           ├── Debug view: transmittance overlay (is it smooth?)
│           ├── Debug view: optical depth (is the integration correct?)
│           └── Debug view: scattering coefficient visualization
```

**Golden rule for atmosphere debugging**: Before blaming multi-scattering, aerial perspective, temporal, or LUT resolution, validate single-scattering analytical output (Stage A) against a known reference. Every production atmosphere bug I have seen traces back to incorrect optical depth integration, wrong phase function normalization, or exposure miscalibration — not the fancy features.

### Per-Stage Verification Checklist

Each implementation stage has a pass/fail gate. Do NOT advance to the next stage until the current stage passes:

**Stage A** (analytical sky):
- [ ] [A0] Noon sky works with single-file prototype (no C++ infrastructure yet)
- [ ] [A0] Noon sky shows deep blue zenith, lighter horizon
- [ ] [A0] Sunset shows warm orange/red tint near sun direction
- [ ] [A0] Analytical sun disk is sharp and >10× brighter than sky dome
- [ ] [A0] No double-counting between sky radiance and sun disk
- [ ] [A0] Exposure calibration produces reasonable luminance across noon/sunset/night
- [ ] Zenith blue matches expected RGB range
- [ ] Sunset shows warm tint (not grey or cyan)
- [ ] Sun disk is sharp, >10× brighter than sky
- [ ] Exposure calibration produces reasonable luminance across noon/sunset/night
- [ ] Tone mapper preserves sky colors (no clipping, no washout)
- [ ] No double-counting between analytical sun and sky

**Stage B** (transmittance LUT):
- [ ] LUT samples match analytical `compute_transmittance()` within 1%
- [ ] No banding or quantization artifacts in transmittance overlay
- [ ] Optical depth integration at grazing angles is smooth (no step artifacts)

**Stage C** (sky-view LUT + direct lighting):
- [ ] Sky-view LUT matches Stage A analytical reference (same parameters)
- [ ] Horizon is stable under camera rotation (no flicker)
- [ ] Sun correctly attenuated through atmosphere at sunset
- [ ] Shadow rays show orange cast at low sun angles
- [ ] No energy discontinuity between sky LUT and analytical sun

**Stage D** (environment sampling):
- [ ] CDF-based sampling converges 2×–5× faster than uniform sampling
- [ ] No sun in PDF (sun remains delta light)
- [ ] MIS weights are correct — no energy loss at BSDF/environment boundary
- [ ] CDF rebuilds produce smooth, artifact-free distributions
- [ ] No visible correlation between sampling pattern and sky features

**Stage E** (aerial perspective):
- [ ] Analytic fog produces plausible inscatter before LUT is built
- [ ] 3D LUT matches analytic fog output (validates parameterization)
- [ ] Distant objects show physically correct chromatic inscatter
- [ ] Fog density increases with distance, decreases with altitude
- [ ] Horizon fade is smooth — no abrupt cutoff at any altitude
- [ ] Aerial perspective transmittance matches direct transmittance LUT lookup

**Stage F** (multi-scatter):
- [ ] Multi-scatter brightens dark areas of sky dome (opposite sun and near horizon)
- [ ] No double-counting between sky-view and multi-scatter LUTs
- [ ] Ground bounce does not over-brighten lower atmosphere
- [ ] Multi-scatter LUT convergence: single iteration is stable per Hillaire
- [ ] White furnace test passes (energy within 1% of emitted)

**Stage G** (temporal):
- [ ] Sky-view LUT is stable under camera rotation — no flicker or ghosting
- [ ] DirtyDAG correctly tracks transitive LUT invalidation (change Rayleigh → all downstream LUTs regenerate)
- [ ] Sun movement triggers full recompute, not reprojection
- [ ] Accumulation resets trigger on correct parameter changes (and only those)
- [ ] No temporal lag when changing sun direction while camera is stationary

---

> **Over-Architected Warning**: The following sections document the full production architecture, but are NOT v1 implementation targets. Do NOT implement any of these in the first pass — they are included because the architecture would be incomplete without them, but implementing them before the foundation is stable will stall development:
> - SkyClassification buffer (G.6) — no binding, no image, no writes until denoiser requires it
> - Cubemap sky cache (G.7) — reserved for future
> - Variance clamping / rejection heuristics (Stage G — deferred hooks)
> - Future transmittance caching (section deferred)
> - Atmospheric shadowing (Future section)
> - Finite-area sun sampling (section deferred)
> - Exposure adaptation (Future section)
> - Atmosphere for reflections (Future section)
>
> The 4-subsystem design, DirtyDAG scheduling, render graph integration, and quality policies are all scaled for a renderer that already has a stable core. The stages below are aggressively pruned to get a believable sky on screen before any infrastructure.

### What To Implement First (Quick Reference)

| When | What | Why stop here |
|------|------|---------------|
| 1st | **Single-file prototype in rmiss (Stage A0)** — no architecture, just scattering math + sun disk in one function | First sky pixel before ANY infrastructure |
| 2nd | **Analytical Rayleigh+Mie, sun disk, exposure, tonemap** — refactor A0 into AtmosphereModel + uniform buffer | Teaches calibration + phase behavior before LUT infrastructure |
| 3rd | **Transmittance LUT only** | Validate optical depth integration before coordinate mapping |
| 4th | **Sky-View LUT + direct lighting integration** | Validates LUT pipeline, sunset energy, horizon behavior |
| 5th | **Environment importance sampling (CDF, MIS)** | Converges faster; needed before temporal to avoid compounding artifacts |
| 6th | **Aerial perspective (analytic fog → full 3D LUT)** | Atmosphere must be trustworthy before inscatter over distance |
| 7th | **Multi-scatter (Hillaire + ground bounce)** | Every visual problem has one fewer unknown source |
| 8th | **Temporal stability (EMA reprojection)** | Only after sky, exposure, lighting, multi-scatter are all verified stable |
| 9th | **Future systems (quality policies, blue noise, classification)** | Production polish, not correctness |

**Critical**: The stages below are STRICTLY ordered. Do NOT implement multiple stages simultaneously. Each stage must be stable and producing correct output before the next begins. Adding temporal, CDF, or quality-path work before the core atmosphere is stable will stall development.

### Stage 0 — Foundation (before anything atmospheric)

| Task | Description | Lines |
|------|-------------|-------|
| 0.1 | World unit + planet coordinate policy (1 unit = 1 m, planet center at origin) | 10 |
| 0.2 | PhysicalCamera + EV100 exposure system | 150 |
| 0.3 | Double-precision ray-sphere intersection | 20 |
| | **Total** | **180** |

### Stage A — Minimal Physically Based Sky (DO FIRST, do not skip — analytical only, NO LUTs)

Goal: A physically correct sky visible in the viewport using ONLY analytical scattering. No GPU LUTs, no temporal, no CDF, no reprojection, no aerial perspective, no multi-scatter.

**Bootstrap path — Stage A0: single-file prototype before any architecture.**

Before creating `AtmosphereModel.h`, `SkySystem.h`, LUT pipelines, render graph nodes, or descriptor set abstractions, write the entire sky evaluation as a single GLSL function inside `pathtrace.rmiss`:

```
// Stage A0 — minimal bootstrap. Discard after Stage A1.
// File: pathtrace.rmiss (single function, no architecture)
vec3 fast_sky_radiance(vec3 dir, vec3 sunDir) {
    // Analytical Rayleigh single scattering
    // Analytical Mie single scattering
    // Analytical sun disk
    // No LUTs, no temporal, no CDF, no aerial perspective
    // No AtmosphereModel class, no SkySystem, no descriptor sets
}
```

The *only* external file is `atmosphere_phase.glsl` (Rayleigh + Mie phase functions, ~60 lines). Everything else — scattering coefficients, optical depth integration, transmittance, sun disk — lives directly in the miss shader. No `AtmosphereParams` struct on the CPU, no uniform buffer upload, no pipeline creation. Just a function that takes a direction and returns a color.

**Why this matters**: The first sky pixel must work before any infrastructure exists. If you create `AtmosphereModel`, `AtmosphereLutSystem`, descriptor sets, and render graph nodes before the scattering integral produces correct output, you have 4+ potential failure sources for every test frame. Stage A0 eliminates all infrastructure risk — it's just shader math. Once `fast_sky_radiance()` produces a believable noon sky and sunset, refactor the constants into `AtmosphereParams` and the math into `AtmosphereModel` (tasks A.1–A.8 below). This refactor is ~100 lines of C++.

**Stage A0 verification** (before any C++ code is written):
- [ ] Noon sky shows deep blue zenith, lighter horizon
- [ ] Sunset shows warm orange/red tint near sun direction
- [ ] Analytical sun disk is sharp and >10× brighter than sky dome
- [ ] No double-counting between sky radiance and sun disk
- [ ] Exposure calibration produces reasonable luminance across noon/sunset/night

| Task | Description | Lines |
|------|-------------|-------|
| A0 | Single-file prototype in `pathtrace.rmiss` (discard after A.1–A.8) | 80 |
| A.1 | AtmosphereParams struct + AtmosphereModel (pure math) | 80 |
| A.2 | AtmosphereUniform (GPU layout, std140) + SkyLutParams | 30 |
| A.3 | Split shader library — `atmosphere_phase.glsl` + `atmosphere_lighting.glsl` (NO env sampling, NO LUT sampling yet) | 200 |
| A.4 | `fast_sky_radiance()` analytical Rayleigh + Mie (refactor from A0 prototype) | 100 |
| A.5 | Analytical sun disk shader (`sun_disk_radiance` with limb darkening) | 40 |
| A.6 | PhysicalCamera + EV100 exposure system (if not from Stage 0) | 150 |
| A.7 | Tone mapper validation (UE5/ACES filmic — verify sky at noon, sunset, night) | 60 |
| A.8 | Editor UI — enable/disable, sun direction, color temperature | 50 |
| | **Total (A0 + A.1–A.8)** | **~790** |

**Verification**: Stage A0 single-file prototype produces a believable noon sky and sunset before any C++ infrastructure exists. After refactoring into `AtmosphereModel` and `AtmosphereUniform`, the output must match the prototype pixel-for-pixel at the same parameters. Analytical sun disk is sharp, no double-counting. Exposure produces reasonable luminance values across noon/sunset/night. Tone mapper preserves sky colors without clipping.

### Stage B — Transmittance LUT Only

| Task | Description | Lines |
|------|-------------|-------|
| B.1 | Transmittance LUT compute shader | 80 |
| B.2 | AtmosphereLutSystem C++ (LUT images, pipelines, unified descriptor set) | 350 |
| B.3 | Editor UI — sun illuminance slider, ground albedo | 40 |
| | **Total** | **~470** |

**Verification**: Transmittance LUT produces correct optical depth values. Validate against analytical `compute_transmittance()` from Stage A at multiple heights and angles. LUT sampling matches direct calculation within 1% across the full parameter space.

### Stage C — Sky-View LUT + Direct Lighting

| Task | Description | Lines |
|------|-------------|-------|
| C.1 | Sky-view LUT compute shader (diffuse sky only, no sun, no multi-scatter) | 100 |
| C.2 | Miss shader integration (`pathtrace.rmiss`: sky LUT + analytical sun) | 30 |
| C.3 | Sun transmittance shader (`sun_transmittance` from LUT) | 30 |
| C.4 | Analytical sun direct lighting in `pathtrace.rgen` (delta light, no MIS) | 40 |
| C.5 | Atmosphere-aware shadow ray (transmittance along sun ray) | 20 |
| C.6 | Debug views — per-LUT overlay | 50 |
| | **Total** | **~270** |

**Verification**: Sky dome from LUT matches analytical reference from Stage A. Horizon is stable under camera rotation. Sun correctly attenuated through atmosphere at sunset. Shadow rays show orange cast at low sun angles.

### Stage D — Environment Importance Sampling

| Task | Description | Lines |
|------|-------------|-------|
| D.1 | Sky importance CDF build compute shader | 80 |
| D.2 | Sampling buffers (CdfRows, CdfCols, params) + AtmosphereSamplingSystem | 120 |
| D.3 | Environment PDF from CDF (no sun in PDF) | 40 |
| D.4 | Environment sampling from CDF | 40 |
| D.5 | MIS power heuristic (BSDF vs environment CDF) | 30 |
| | **Total** | **~310** |

**Verification**: Environment importance sampling converges faster than uniform. No sun in PDF (sun is delta-light, no double counting). MIS weight is correct — no energy loss at BSDF/environment boundary.

### Stage G — Temporal Stability

| Task | Description | Lines |
|------|-------------|-------|
| G.1 | Previous sky-view LUT + ping-pong history | 20 |
| G.2 | Temporal reprojection shader (camera-motion UV reproject + lerp(current, previous, 0.9)) | 60 |
| G.3 | AtmosphereTemporalSystem C++ (DirtyDAG, camera tracking) | 120 |
| G.4 | Accumulation reset policy (trigger on atmosphere parameter change) | 20 |
| | **Total** | **~220** |

**v1 only**: Simple EMA blend. No variance clamping, no exposure rescaling, no rejection heuristics. These are deferred because temporal debugging before LUT correctness is extremely painful. Implement only AFTER sky, exposure, lighting, and multi-scatter are all verified stable (Stages A–F).

**Verification**: Sky-view LUT is stable under camera motion — no flicker, no ghosting. DirtyDAG correctly tracks transitive LUT invalidation. Sun movement triggers full recompute, not reprojection.

### Stage E — Aerial Perspective

| Task | Description | Lines |
|------|-------------|-------|
| E.1 | Analytic fog approximation (~5 shader lines, validates composition pipeline) | 10 |
| E.2 | Aerial perspective LUT compute shader (96×96×48, logarithmic depth) | 100 |
| E.3 | Aerial perspective composition in `pathtrace.rgen` (inscatter + transmittance) | 30 |
| E.4 | Replace analytic fog with LUT lookup | 10 |
| E.5 | Debug views — per-LUT overlay | 50 |
| | **Total** | **~200** |

**⚠️ High risk**: 3D LUT parameterization, logarithmic slicing, coordinate mapping, transmittance coupling, chromatic inscatter, and world-space lookup all fail in subtle and visually indistinguishable ways. Do NOT attempt until atmosphere itself (Stages A–D) is producing correct, stable output.

**Verification**: Distant objects show atmospheric inscatter (blue tint at range). Fog density increases with distance, decreases with altitude. Horizon fade is smooth. Analytic fog and LUT produce visually similar results (validates LUT parameterization).

### Stage F — Multi-Scattering

| Task | Description | Lines |
|------|-------------|-------|
| F.1 | Multi-scatter LUT compute shader (Hillaire single-approximation) | 70 |
| F.2 | MultiScatterLut C++ integration into AtmosphereLutSystem | 50 |
| F.3 | Ground bounce in multi-scatter LUT (after base multi-scatter is validated) | 10 |
| F.4 | Tone mapper improvements (highlight desaturation, shoulder shaping) | 60 |
| F.5 | Sunset energy validation — verify Rayleigh/Mie ratios produce correct horizon | 20 |
| | **Total** | **~210** |

**⚠️ Late stage — do NOT rush**: Multi-scatter is introduced late because it introduces a multiplicative correction that obscures every other bug. Single-scattering (Stages A–E) must be validated across ALL conditions (noon, sunset, night, turbid, clear) before adding multi-scatter. The Hillaire approximation is forgiving, but debugging optical depth, transmittance, and phase functions is already hard enough without an extra unknown.

**Verification**: Multi-scatter brightens the sky dome correctly. Sunset horizon shows warm tint. No double-counting between sky-view and multi-scatter LUTs.

### Stage H — Future Systems (deferred until core is stable)

| Task | Description | Lines |
|------|-------------|-------|
| G.1 | Ray quality policy (AtmosphereRayQuality — Full/Reduced/Fast/Minimal per ray class) | 80 |
| G.2 | Fast sky path + energy matching normalization | 50 |
| G.3 | Ground bounce (if not done in F.3 — deferred until multi-scatter is validated) | 10 |
| G.4 | Blue noise sample distribution | 80 |
| G.5 | Debug views — quality overlay | 30 |
| G.6 | SkyClassification buffer (placeholder only — no allocation or writes) | — |
| G.7 | Cubemap sky cache migration (reserved for future) | — |
| | **Total (implemented)** | **~250** |

SkyClassification and cubemap cache are architecture placeholders only. No binding, no image, no implementation in this stage.

### Future — Volumetric Clouds

| Task | Description | Lines |
|------|-------------|-------|
| H.1 | Volumetric cloud system (entire subsystem — deferred) | 2000+ |

**Estimated total (A–H)**: ~2520 lines (C++ + GLSL). Stage A0 (single-file prototype, ~80 lines) is discarded after refactoring into A.1–A.8. Stage A total including A0 bootstrap: ~790 lines.

**Why strict ordering matters**:
- Stage A0 missing → first sky pixel arrives after infrastructure, creating 4+ failure sources per test frame
- Stage A missing → you can't calibrate exposure or validate scattering behavior
- Stage B missing → no transmittance LUT, no optical depth validation
- Stage C missing → no sky-view LUT, no direct lighting integration
- Stage D missing → environment sampling converges slowly, MIS is wrong
- Stage E missing → distant objects look flat
- Stage F missing → sky is too dark (no multi-scatter) — but single-scattering must be correct first
- Stage G missing → sky flickers under camera motion; temporal debugging before all upstream layers are correct is a trap
- Stage H missing → no production impact — these are polish and future systems

Every stage is independently verifiable. If a stage's verification fails, stop and fix it before proceeding.

---

## Performance Budgeting

Atmosphere system complexity must be bounded by explicit GPU timing targets. Without budgeting, LUT resolutions, quality tiers, and update frequencies expand to fill all available frame time.

### Per-Pass GPU Timing Targets

| Pass | Cinematic (< 100 ms frame) | High (< 33 ms) | Medium (< 16 ms) | Low (< 8 ms) |
|------|---------------------------|----------------|-------------------|---------------|
| Transmittance LUT | 1.0 ms | 0.5 ms | 0.2 ms | 0.1 ms |
| Sky-view LUT | 2.0 ms | 0.8 ms | 0.3 ms | 0.15 ms |
| CDF build | 0.5 ms | 0.3 ms | 0.15 ms | 0.1 ms |
| Aerial perspective LUT | 1.5 ms | 0.5 ms | — | — |
| Multi-scatter LUT | 0.3 ms | 0.15 ms | 0.1 ms | — |
| Temporal reproject | 0.2 ms | 0.15 ms | 0.1 ms | 0.05 ms |
| **Total atmosphere compute** | **~5.5 ms** | **~2.4 ms** | **~0.85 ms** | **~0.3 ms** |
| Atmosphere sampling in path trace | Included in path trace budget | | | |

**Target frame times** (total GPU, including path tracing, denoising, compositing):
- Cinematic: 100 ms (10 FPS — interactive preview)
- High: 33 ms (30 FPS)
- Medium: 16 ms (60 FPS)
- Low: 8 ms (120 FPS — editor viewport navigation)

### Update Frequency Policy

| Trigger | LUTs affected | Update strategy |
|---------|---------------|-----------------|
| Sun direction change | Transmittance, sky-view, CDF | Recompute all in next frame |
| Atmosphere parameter change | All | Recompute all over 1–2 frames |
| Camera rotation | Sky-view | Temporal reproject (no recompute) |
| Camera translation | Sky-view, aerial perspective | Temporal reproject + LUT lookup only |
| Quality level change | All | Recompute all at new resolution |
| Initial load | All | Recompute all (frame 0 may be incomplete) |

**Key design rule**: Sun direction and atmosphere parameter changes should be infrequent (user-initiated only). Camera motion is the common case. The temporal reprojection path must be fast enough that it adds no perceptible latency to viewport navigation.

### Async Compute Scheduling

All atmosphere compute passes are candidates for the async compute queue (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT on a separate queue). However, the path tracer's ray tracing work also benefits from async compute overlap. The scheduling policy:

- **v1**: All atmosphere compute runs synchronously on the graphics queue before path tracing. Simple, correct, no queue synchronization complexity.
- **v2 candidate**: Transmittance LUT and CDF build move to async compute queue, overlapping with path tracing. Requires timeline semaphores and careful barrier management.

**Why NOT async compute in v1**: The atmosphere compute passes are small (0.1–2 ms each). The overhead of queue submission, timeline semaphore signaling, and cross-queue barrier insertion dwarfs the compute time. Async compute becomes worthwhile only when total atmosphere compute exceeds ~3 ms (Medium quality or higher, with all LUTs active).

### Resolution vs Frequency Tuning

For interactive parameter scrubbing (sun slider, scattering coefficient adjustment), LUT recomputation latency is directly visible. Mitigations:

| Technique | Latency hiding | Quality impact | Complexity |
|-----------|---------------|----------------|------------|
| Full-res recompute | None | None | Baseline (v1) |
| Half-res during interaction | 4× faster LUT gen | Temporary blur | Low |
| Progressive refinement | Instant at low res, refine over N frames | Temporal artifacts at start | Medium |
| Dirty flag debounce | Merge parameter changes within N ms | One-frame lag on final value | Low |

**v1 approach**: Full-res recompute. The editor sun slider already has a debounce (typically 50–100 ms before the value is committed). This is sufficient for initial implementation. Defer half-res and progressive refinement until user testing shows perceptible latency.

### Budget Enforcement

- Each LUT compute shader reports execution time via Vulkan timestamp queries.
- A debug overlay shows frame time breakdown for all atmosphere passes.
- If total atmosphere compute exceeds budget for the current quality level, the system should:
  1. Log a warning with per-pass breakdown
  2. Suggest lowering a quality tier via notification
  3. NOT silently drop passes or reduce quality automatically — deterministic behavior is more important than budget compliance during development

---

## Design Decisions Summary

| Decision | Choice | Rationale |
|----------|--------|-----------|
| World unit | 1 unit = 1 meter | Atmosphere, camera, BVH all consistent |
| Planet coordinate system | Origin at planet center (0,0,0) | No offset hacks in any atmosphere function |
| Large world precision | double discriminant + future LWC | Planetary-scale float precision failure |
| Descriptor set | Set 1 exclusively | Clean separation from scene set 0 |
| Sun/LUT separation | Purely analytical sun; LUT = diffuse sky only | Avoids double-counting, clean MIS, stable convergence |
| MIS strategy | BSDF vs sky CDF (power heuristic); sun = delta light | No sun-in-CDF conflict; correct weighting |
| Multi-scattering | Hillaire single-approximation | Fast, energy-conserving, no iteration |
| Sky-view update | Camera-motion-aware temporal reprojection + variance clamping | Prevents flicker, stabilizes CDF, avoids ghosting |
| Aerial perspective storage | RGBA16F: RGB=inscatter, A=transmittance | Chromatic fog, single lookup, no banding |
| Aerial perspective depth | 96×96×48 log-depth | Prevents altitude banding, distant fog stepping, and horizon quantization in cinematic sun angles |
| Environment sampling | CDF from diffuse sky LUT luminance (no sun) | Critical for HDR sky convergence |
| Sun disk | Analytic with limb darkening | Sharp sun without LUT undersampling |
| Sun transmittance | LUT-sampled attenuation on direct light | Required for physically correct sunset |
| Shader organization | Split into 4 focused files | Maintainability, compile time, specialization |
| Exposure | Physical EV100 (aperture/shutter/ISO) | Required for physically plausible atmosphere |
| Tone mapping | UE5/ACES filmic curve + pre-desaturation of highlights | Reinhard insufficient for atmosphere HDR; highlight desaturation prevents grey sun disk |
| Validation strategy | Per-stage reference methodology with 6 validation scenes + diagnostic tree | Systematic debugging prevents parameter tweaking without understanding root cause |
| Performance budgeting | Explicit GPU timing targets per pass per quality tier, update frequency policy, async compute scheduling rules | Prevents unbounded atmosphere compute growth |
| Sky LUT parameterization | Lat-long with abstracted sampling API (direction in, color out) | Cubemap migration without touching consumers; no lat-long UV leak to external shaders |
| Ray quality | AtmosphereRayQuality enum (Full/Reduced/Fast/Minimal) | Performance scaling for secondary rays |
| Ozone | Simplified layer model | Acceptable for v1; Chappuis band model later |
| Clouds | Full environment_radiance integration | Reflects in all rays, not compositing hack |
| Async compute | v1: synchronous graphics queue. v2: transmittance LUT + CDF build on async compute queue (when total atmosphere compute > 3 ms) | See Performance Budgeting for scheduling policy and threshold |
| Denoiser coupling | None yet | Atmosphere-aware sky masks / variance moments deferred |
| Color pipeline | RGB approximation | Not true spectral rendering; sunset/ozone limits |
| LUT cost | Inline generation | Progressive refinement may be needed for editor latency |

---

## Known Limitations (v1)

- Ozone uses simplified absorption rather than wavelength-based Chappuis band — sunset may not match reference
- Fast sky path is single-scattering only — accurate enough for preview, not for final pixel
- No space-to-atmosphere transition for orbital cameras (grazing precision is handled but no exoatmospheric rendering)
- Single precision for scene geometry — floating origin rebase not implemented; large-world scenes may drift
- No transmittance caching — every lookup samples the LUT; optimization deferred
- Cloud system will require separate hardware requirements (3D noise texture generation)
- Aerial perspective uses scalar transmittance (alpha channel) rather than full RGB — slight color shift in transmittance at extreme distances but acceptable for real-time
- Scalar transmittance from aerial perspective LUT alpha is a luminance approximation — wavelength-dependent extinction at extreme distances is slightly lossy but acceptable for real-time
- No atmospheric coupling for glossy/diffuse indirect rays beyond `fast_sky_radiance()` — true volumetric GI is not targeted
- Environment sampling CDF excludes the sun — the sun must be sampled as a separate delta light; this is by design but worth noting
- **LUT generation cost** — Multiple LUT passes at high resolutions may impact editor responsiveness during parameter scrubbing. See Performance Budgeting section for timing targets and mitigation strategies.
- **Quality transition artifacts** — Ray quality tiers (Full → Reduced → Fast → Minimal) may produce visible seams on reflective surfaces (water, chrome) at quality boundaries. Not a correctness bug, but may require smoothing heuristics later.
- **Denoiser unaware of atmosphere** — The denoiser currently treats sky pixels identically to surface pixels. This risks smearing the sun disk, blurring cloud edges, and destabilizing the horizon. The SkyClassification buffer (see above) is designed to address this — the denoiser reads it to adjust variance thresholds and disable accumulation on sun pixels. Implementation deferred to the denoiser phase.
- **Four-subsystem architectural coupling** — AtmosphereLutSystem reads transient resources (FrameGraphContext) that must outlive the enclosing FrameGraph pass. If the render graph ever defers execution (e.g., async recording), the LUT system's direct resource access may break. The current synchronous-execution render graph avoids this, but architectural coupling exists. Future: AtmosphereLutSystem should declare all resource lifetimes via the render graph API, not through direct member pointers.
- **RGB color pipeline** — The entire atmosphere model operates in RGB, not true spectral rendering. This limits accuracy of sunsets, ozone coloration, and extraterrestrial transitions. Acceptable for real-time; spectral rendering is a separate research track.
- **sun_transmittance() abstraction** — The function wraps LUT sampling keyed on (height, cosZenith), but the API takes (worldPos, sunDir). The coordinate transform is hidden inside. Fine for v1, but may cause confusion if LUT parameterization changes. Future: separate `compute_height_mu()` from `sample_transmittance()`.
