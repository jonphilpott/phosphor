// stb_image_impl.cpp
// Single translation unit that compiles the stb_image implementation.
//
// stb_image is a "single-header" library: the full decoder is guarded by
// STB_IMAGE_IMPLEMENTATION so it only gets compiled once.  Every other file
// that needs stb_image just includes the header without the define and gets
// only the declarations.
//
// We keep this in its own file so the ~8000-line implementation doesn't
// bloat compile times for unrelated translation units.

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
