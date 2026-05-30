#ifndef RTV_ATMOSPHERE_PHASE_GLSL
#define RTV_ATMOSPHERE_PHASE_GLSL

#define ATMOSPHERE_RAY_QUALITY_FULL    0u
#define ATMOSPHERE_RAY_QUALITY_REDUCED 1u
#define ATMOSPHERE_RAY_QUALITY_FAST    2u
#define ATMOSPHERE_RAY_QUALITY_MINIMAL 3u

const float ATMOSPHERE_PHASE_PI = 3.14159265358979323846;
const float ATMOSPHERE_PLANET_RADIUS = 6360000.0;
const float ATMOSPHERE_TOP_RADIUS = 6420000.0;
const vec3 ATMOSPHERE_RAYLEIGH_SCATTERING = vec3(5.802e-6, 13.558e-6, 33.100e-6);
const vec3 ATMOSPHERE_MIE_SCATTERING = vec3(3.996e-6);
const vec3 ATMOSPHERE_OZONE_ABSORPTION = vec3(0.650e-6, 1.881e-6, 0.085e-6);

float atmosphere_rayleigh_phase(float cosTheta) {
    float mu2 = cosTheta * cosTheta;
    return 3.0 * (1.0 + mu2) / (16.0 * ATMOSPHERE_PHASE_PI);
}

float atmosphere_mie_phase(float cosTheta, float anisotropy) {
    float g = clamp(anisotropy, -0.95, 0.95);
    float g2 = g * g;
    float denom = max(pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5), 1.0e-4);
    return (1.0 - g2) / (4.0 * ATMOSPHERE_PHASE_PI * denom);
}

float atmosphere_air_mass(float cosZenith) {
    float y = clamp(cosZenith, -0.08, 1.0);
    return 1.0 / max(y + 0.15 * pow(max(1.08 - y, 0.0), -1.253), 0.06);
}

vec3 atmosphere_scene_to_planetary(vec3 scenePos) {
    return scenePos + vec3(0.0, ATMOSPHERE_PLANET_RADIUS + 1.7, 0.0);
}

bool atmosphere_ray_sphere_intersection(vec3 origin, vec3 dir, float radius, out float tNear, out float tFar) {
    vec3 d = normalize(dir);
    double a = double(dot(d, d));
    double b = 2.0 * double(dot(origin, d));
    double c = double(dot(origin, origin)) - double(radius) * double(radius);
    double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) {
        tNear = 0.0;
        tFar = 0.0;
        return false;
    }
    double root = sqrt(disc);
    double invDenom = 0.5 / a;
    tNear = max(float((-b - root) * invDenom), 0.0);
    tFar = float((-b + root) * invDenom);
    return tFar >= tNear && tFar >= 0.0;
}

vec3 atmosphere_extinction_at_height(float heightMeters) {
    float rayleighDensity = exp(-heightMeters / 8000.0);
    float mieDensity = exp(-heightMeters / 1200.0);
    float ozoneDensity = max(1.0 - abs(heightMeters - 25000.0) / 15000.0, 0.0);
    return ATMOSPHERE_RAYLEIGH_SCATTERING * rayleighDensity +
        ATMOSPHERE_MIE_SCATTERING * mieDensity +
        ATMOSPHERE_OZONE_ABSORPTION * ozoneDensity;
}

vec3 atmosphere_segment_transmittance(vec3 sceneOrigin, vec3 dir, float distanceMeters, int sampleCount) {
    vec3 planetaryOrigin = atmosphere_scene_to_planetary(sceneOrigin);
    vec3 d = normalize(dir);
    float maxDistance = max(distanceMeters, 0.0);
    int samples = max(sampleCount, 1);
    float segmentLength = maxDistance / float(samples);
    vec3 opticalDepth = vec3(0.0);
    for (int i = 0; i < 32; ++i) {
        if (i >= samples) {
            break;
        }
        float t = (float(i) + 0.5) * segmentLength;
        vec3 samplePos = planetaryOrigin + d * t;
        float height = max(length(samplePos) - ATMOSPHERE_PLANET_RADIUS, 0.0);
        opticalDepth += atmosphere_extinction_at_height(height) * segmentLength;
    }
    return exp(-opticalDepth);
}

vec3 atmosphere_sun_transmittance(vec3 scenePos, vec3 sunDir) {
    vec3 planetaryPos = atmosphere_scene_to_planetary(scenePos);
    float tNear;
    float tFar;
    if (!atmosphere_ray_sphere_intersection(planetaryPos, sunDir, ATMOSPHERE_TOP_RADIUS, tNear, tFar)) {
        return vec3(1.0);
    }
    return atmosphere_segment_transmittance(scenePos, sunDir, tFar, 16);
}

#endif
