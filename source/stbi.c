// stb_image v2.30 (public domain, https://github.com/nothings/stb): decodes
// album art for the music player. JPEG and PNG from memory only.
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
