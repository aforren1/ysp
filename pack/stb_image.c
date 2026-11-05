/* stb_image.h compiled once for the pack tool (pack/import.c), JPEG only,
 * with upstream's warnings off (pack/CMakeLists.txt). No stdio: the tool
 * hands it bytes. 65536 on a side is the pack's texture limit. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 65536
#include "stb_image.h"
