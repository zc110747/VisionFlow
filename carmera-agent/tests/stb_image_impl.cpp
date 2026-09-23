// The single translation unit that instantiates the stb_image implementation.
//
// Extracted so that more than one test file can decode fixtures: stb_image.h
// emits non-static definitions when STB_IMAGE_IMPLEMENTATION is set, so defining
// it in two .cpp files of the same executable is a duplicate-symbol link error.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
