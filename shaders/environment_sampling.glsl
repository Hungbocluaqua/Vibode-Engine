#ifndef RTV_ENVIRONMENT_SAMPLING_GLSL
#define RTV_ENVIRONMENT_SAMPLING_GLSL

#include "atmosphere_lighting.glsl"

float environment_pdf_atmosphere(vec3 dir) {
    vec3 radiance = atmosphere_sky_radiance(dir, ATMOSPHERE_RAY_QUALITY_FULL);
    float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
    if (lum <= 1.0e-5) {
        return 1.0 / (4.0 * PI);
    }
    float lat = asin(clamp(normalize(dir).y, -1.0, 1.0));
    float sinTheta = max(cos(lat), 0.001);
    return lum / (2.0 * PI * PI * max(sinTheta, 0.001));
}

vec3 sample_environment_direction_atmosphere(inout uint state, out vec3 out_dir, out float out_pdf) {
    float z = 1.0 - 2.0 * rand_f32(state);
    float phi = 2.0 * PI * rand_f32(state);
    float r = sqrt(max(1.0 - z * z, 0.0));
    out_dir = vec3(r * cos(phi), z, r * sin(phi));
    out_pdf = environment_pdf_atmosphere(out_dir);
    return atmosphere_sky_radiance(out_dir, ATMOSPHERE_RAY_QUALITY_FULL);
}

float environment_pdf_atmosphere_cdf(vec3 dir) {
    vec3 radiance = atmosphere_sky_radiance(dir, ATMOSPHERE_RAY_QUALITY_FULL);
    float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
    float invTotalLum = env_params.inv_total_lum;
    return lum * invTotalLum / (4.0 * PI);
}

vec3 sample_environment_direction_atmosphere_cdf(inout uint state, out vec3 out_dir, out float out_pdf, uint cdfWidth, uint cdfHeight) {
    float u = rand_f32(state);
    uint totalPixels = cdfWidth * cdfHeight;
    uint low = 0;
    uint high = totalPixels - 1u;
    while (low < high) {
        uint mid = (low + high) / 2u;
        if (sky_cdf_cols[mid] < u) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    uint x = low % cdfWidth;
    uint y = low / cdfWidth;
    vec2 uv = (vec2(x, y) + vec2(rand_f32(state), rand_f32(state))) / vec2(cdfWidth, cdfHeight);
    float phi2 = (uv.x - 0.5) * 2.0 * PI;
    float lat = (uv.y - 0.5) * PI;
    float cosLat = cos(lat);
    out_dir = vec3(cosLat * cos(phi2), sin(lat), cosLat * sin(phi2));
    out_pdf = environment_pdf_atmosphere_cdf(out_dir);
    return atmosphere_sky_radiance(out_dir, ATMOSPHERE_RAY_QUALITY_FULL);
}

#endif
