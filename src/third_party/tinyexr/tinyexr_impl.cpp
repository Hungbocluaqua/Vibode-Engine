// Single translation unit that compiles the vendored tinyexr OpenEXR decoder.
//
// tinyexr is header-only; defining TINYEXR_IMPLEMENTATION here emits the
// decoder/encoder code exactly once. ZIP-compressed EXR scanlines are decoded
// through the bundled miniz, so no external zlib dependency is required.
#include <miniz.h>

#define TINYEXR_IMPLEMENTATION
#include <tinyexr.h>
