#include "vulkan_compositor.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <unistd.h>  // For close()
#include <sys/stat.h>  // For fstat()

// DRM format definitions
#ifndef DRM_FORMAT_ARGB8888
#define DRM_FORMAT_ARGB8888 0x34325241
#endif
#ifndef DRM_FORMAT_XRGB8888
#define DRM_FORMAT_XRGB8888 0x34325258
#endif

// Embedded SPIR-V shaders (compiled from GLSL)
static const unsigned char vert_spv[] = {
  0x03, 0x02, 0x23, 0x07, 0x00, 0x00, 0x01, 0x00, 0x0b, 0x00, 0x08, 0x00,
  0x2e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x02, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x47, 0x4c, 0x53, 0x4c, 0x2e, 0x73, 0x74, 0x64, 0x2e, 0x34, 0x35, 0x30,
  0x00, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x04, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x00, 0x00,
  0x0c, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x00, 0xc2, 0x01, 0x00, 0x00,
  0x05, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e,
  0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x03, 0x00, 0x09, 0x00, 0x00, 0x00,
  0x70, 0x6f, 0x73, 0x00, 0x05, 0x00, 0x06, 0x00, 0x0c, 0x00, 0x00, 0x00,
  0x67, 0x6c, 0x5f, 0x56, 0x65, 0x72, 0x74, 0x65, 0x78, 0x49, 0x6e, 0x64,
  0x65, 0x78, 0x00, 0x00, 0x05, 0x00, 0x05, 0x00, 0x18, 0x00, 0x00, 0x00,
  0x6f, 0x75, 0x74, 0x54, 0x65, 0x78, 0x43, 0x6f, 0x6f, 0x72, 0x64, 0x00,
  0x05, 0x00, 0x06, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x67, 0x6c, 0x5f, 0x50,
  0x65, 0x72, 0x56, 0x65, 0x72, 0x74, 0x65, 0x78, 0x00, 0x00, 0x00, 0x00,
  0x06, 0x00, 0x06, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x67, 0x6c, 0x5f, 0x50, 0x6f, 0x73, 0x69, 0x74, 0x69, 0x6f, 0x6e, 0x00,
  0x06, 0x00, 0x07, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x67, 0x6c, 0x5f, 0x50, 0x6f, 0x69, 0x6e, 0x74, 0x53, 0x69, 0x7a, 0x65,
  0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x07, 0x00, 0x1e, 0x00, 0x00, 0x00,
  0x02, 0x00, 0x00, 0x00, 0x67, 0x6c, 0x5f, 0x43, 0x6c, 0x69, 0x70, 0x44,
  0x69, 0x73, 0x74, 0x61, 0x6e, 0x63, 0x65, 0x00, 0x06, 0x00, 0x07, 0x00,
  0x1e, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x67, 0x6c, 0x5f, 0x43,
  0x75, 0x6c, 0x6c, 0x44, 0x69, 0x73, 0x74, 0x61, 0x6e, 0x63, 0x65, 0x00,
  0x05, 0x00, 0x03, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x47, 0x00, 0x04, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
  0x2a, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00, 0x18, 0x00, 0x00, 0x00,
  0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x03, 0x00,
  0x1e, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x48, 0x00, 0x05, 0x00,
  0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x05, 0x00, 0x1e, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x48, 0x00, 0x05, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x0b, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x48, 0x00, 0x05, 0x00,
  0x1e, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
  0x04, 0x00, 0x00, 0x00, 0x13, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x21, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x16, 0x00, 0x03, 0x00, 0x06, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x17, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
  0x02, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00,
  0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x15, 0x00, 0x04, 0x00,
  0x0a, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x20, 0x00, 0x04, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x0a, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00, 0x0b, 0x00, 0x00, 0x00,
  0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x04, 0x00,
  0x0a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x2b, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
  0x02, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x17, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00,
  0x17, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
  0x17, 0x00, 0x04, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
  0x04, 0x00, 0x00, 0x00, 0x15, 0x00, 0x04, 0x00, 0x1b, 0x00, 0x00, 0x00,
  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x04, 0x00,
  0x1b, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x1c, 0x00, 0x04, 0x00, 0x1d, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
  0x1c, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x06, 0x00, 0x1e, 0x00, 0x00, 0x00,
  0x1a, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00,
  0x1d, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x1f, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00,
  0x1f, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
  0x2b, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
  0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x2b, 0x00, 0x04, 0x00,
  0x06, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
  0x2b, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x2c, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x36, 0x00, 0x05, 0x00,
  0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0xf8, 0x00, 0x02, 0x00, 0x05, 0x00, 0x00, 0x00,
  0x3b, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
  0x07, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00,
  0x0d, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0xc4, 0x00, 0x05, 0x00,
  0x0a, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00,
  0x0e, 0x00, 0x00, 0x00, 0xc7, 0x00, 0x05, 0x00, 0x0a, 0x00, 0x00, 0x00,
  0x11, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
  0x6f, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
  0x11, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x04, 0x00, 0x0a, 0x00, 0x00, 0x00,
  0x13, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0xc7, 0x00, 0x05, 0x00,
  0x0a, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00,
  0x10, 0x00, 0x00, 0x00, 0x6f, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00,
  0x15, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x50, 0x00, 0x05, 0x00,
  0x07, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00,
  0x15, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x03, 0x00, 0x09, 0x00, 0x00, 0x00,
  0x16, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00,
  0x19, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x03, 0x00,
  0x18, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x04, 0x00,
  0x07, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
  0x8e, 0x00, 0x05, 0x00, 0x07, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
  0x22, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x50, 0x00, 0x05, 0x00,
  0x07, 0x00, 0x00, 0x00, 0x26, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00,
  0x25, 0x00, 0x00, 0x00, 0x83, 0x00, 0x05, 0x00, 0x07, 0x00, 0x00, 0x00,
  0x27, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x26, 0x00, 0x00, 0x00,
  0x51, 0x00, 0x05, 0x00, 0x06, 0x00, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00,
  0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x51, 0x00, 0x05, 0x00,
  0x06, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x50, 0x00, 0x07, 0x00, 0x1a, 0x00, 0x00, 0x00,
  0x2b, 0x00, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00,
  0x28, 0x00, 0x00, 0x00, 0x25, 0x00, 0x00, 0x00, 0x41, 0x00, 0x05, 0x00,
  0x2c, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x21, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x03, 0x00, 0x2d, 0x00, 0x00, 0x00,
  0x2b, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x01, 0x00, 0x38, 0x00, 0x01, 0x00
};

static const unsigned char frag_spv[] = {
  0x03, 0x02, 0x23, 0x07, 0x00, 0x00, 0x01, 0x00, 0x0b, 0x00, 0x08, 0x00,
  0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x02, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x47, 0x4c, 0x53, 0x4c, 0x2e, 0x73, 0x74, 0x64, 0x2e, 0x34, 0x35, 0x30,
  0x00, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x07, 0x00, 0x04, 0x00, 0x00, 0x00,
  0x04, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x00, 0x00,
  0x11, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x10, 0x00, 0x03, 0x00,
  0x04, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00,
  0x02, 0x00, 0x00, 0x00, 0xc2, 0x01, 0x00, 0x00, 0x05, 0x00, 0x04, 0x00,
  0x04, 0x00, 0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e, 0x00, 0x00, 0x00, 0x00,
  0x05, 0x00, 0x05, 0x00, 0x09, 0x00, 0x00, 0x00, 0x74, 0x65, 0x78, 0x43,
  0x6f, 0x6c, 0x6f, 0x72, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x05, 0x00,
  0x0d, 0x00, 0x00, 0x00, 0x6f, 0x76, 0x65, 0x72, 0x6c, 0x61, 0x79, 0x54,
  0x65, 0x78, 0x00, 0x00, 0x05, 0x00, 0x05, 0x00, 0x11, 0x00, 0x00, 0x00,
  0x69, 0x6e, 0x54, 0x65, 0x78, 0x43, 0x6f, 0x6f, 0x72, 0x64, 0x00, 0x00,
  0x05, 0x00, 0x05, 0x00, 0x15, 0x00, 0x00, 0x00, 0x6f, 0x75, 0x74, 0x43,
  0x6f, 0x6c, 0x6f, 0x72, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x06, 0x00,
  0x17, 0x00, 0x00, 0x00, 0x50, 0x75, 0x73, 0x68, 0x43, 0x6f, 0x6e, 0x73,
  0x74, 0x61, 0x6e, 0x74, 0x73, 0x00, 0x00, 0x00, 0x06, 0x00, 0x05, 0x00,
  0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0x6c, 0x70, 0x68,
  0x61, 0x00, 0x00, 0x00, 0x05, 0x00, 0x03, 0x00, 0x19, 0x00, 0x00, 0x00,
  0x70, 0x63, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00, 0x0d, 0x00, 0x00, 0x00,
  0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00,
  0x0d, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x47, 0x00, 0x04, 0x00, 0x11, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x04, 0x00, 0x15, 0x00, 0x00, 0x00,
  0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47, 0x00, 0x03, 0x00,
  0x17, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x48, 0x00, 0x05, 0x00,
  0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x13, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x21, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
  0x16, 0x00, 0x03, 0x00, 0x06, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x17, 0x00, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
  0x04, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00,
  0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x19, 0x00, 0x09, 0x00,
  0x0a, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x03, 0x00,
  0x0b, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00,
  0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00,
  0x3b, 0x00, 0x04, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x17, 0x00, 0x04, 0x00, 0x0f, 0x00, 0x00, 0x00,
  0x06, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00,
  0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
  0x3b, 0x00, 0x04, 0x00, 0x10, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x14, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00,
  0x14, 0x00, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
  0x1e, 0x00, 0x03, 0x00, 0x17, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
  0x20, 0x00, 0x04, 0x00, 0x18, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
  0x17, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x04, 0x00, 0x18, 0x00, 0x00, 0x00,
  0x19, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x15, 0x00, 0x04, 0x00,
  0x1a, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x2b, 0x00, 0x04, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x04, 0x00, 0x1c, 0x00, 0x00, 0x00,
  0x09, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x36, 0x00, 0x05, 0x00,
  0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x03, 0x00, 0x00, 0x00, 0xf8, 0x00, 0x02, 0x00, 0x05, 0x00, 0x00, 0x00,
  0x3b, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
  0x07, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x04, 0x00, 0x0b, 0x00, 0x00, 0x00,
  0x0e, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x04, 0x00,
  0x0f, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00,
  0x57, 0x00, 0x05, 0x00, 0x07, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00,
  0x0e, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x03, 0x00,
  0x09, 0x00, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x04, 0x00,
  0x07, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
  0x41, 0x00, 0x05, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00,
  0x19, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x3d, 0x00, 0x04, 0x00,
  0x06, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x1d, 0x00, 0x00, 0x00,
  0x8e, 0x00, 0x05, 0x00, 0x07, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00,
  0x16, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x03, 0x00,
  0x15, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x01, 0x00,
  0x38, 0x00, 0x01, 0x00
};

VulkanCompositor::VulkanCompositor() = default;

VulkanCompositor::~VulkanCompositor() {
    cleanup();
}

bool VulkanCompositor::init(VulkanContext* vk, uint32_t width, uint32_t height) {
    vk_ = vk;
    width_ = width;
    height_ = height;

    if (!createOverlayResources()) return false;
    if (!createPipeline()) return false;
    if (!createDescriptorSets()) return false;

    return true;
}

bool VulkanCompositor::createOverlayResources() {
    VkDevice device = vk_->device();

    // Create overlay image
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_B8G8R8A8_UNORM;  // CEF uses BGRA
    image_info.extent = {width_, height_, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &image_info, nullptr, &overlay_image_) != VK_SUCCESS) {
        std::cerr << "Failed to create overlay image" << std::endl;
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, overlay_image_, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = vk_->findMemoryType(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device, &alloc_info, nullptr, &overlay_memory_) != VK_SUCCESS) {
        std::cerr << "Failed to allocate overlay image memory" << std::endl;
        return false;
    }
    vkBindImageMemory(device, overlay_image_, overlay_memory_, 0);

    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = overlay_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device, &view_info, nullptr, &overlay_view_) != VK_SUCCESS) {
        std::cerr << "Failed to create overlay image view" << std::endl;
        return false;
    }

    // Create sampler
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(device, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
        std::cerr << "Failed to create sampler" << std::endl;
        return false;
    }

    // Create staging buffer
    VkDeviceSize buffer_size = width_ * height_ * 4;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &buffer_info, nullptr, &staging_buffer_) != VK_SUCCESS) {
        std::cerr << "Failed to create staging buffer" << std::endl;
        return false;
    }

    vkGetBufferMemoryRequirements(device, staging_buffer_, &mem_reqs);

    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = vk_->findMemoryType(mem_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device, &alloc_info, nullptr, &staging_memory_) != VK_SUCCESS) {
        std::cerr << "Failed to allocate staging memory" << std::endl;
        return false;
    }
    vkBindBufferMemory(device, staging_buffer_, staging_memory_, 0);
    vkMapMemory(device, staging_memory_, 0, buffer_size, 0, &staging_mapped_);

    // Transition overlay image to shader read layout
    VkCommandBuffer cmd = vk_->beginSingleTimeCommands();
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = overlay_image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    vk_->endSingleTimeCommands(cmd);

    return true;
}

bool VulkanCompositor::createPipeline() {
    VkDevice device = vk_->device();

    // Create render pass (load existing, store result)
    VkAttachmentDescription color_attachment{};
    color_attachment.format = vk_->swapchainFormat();
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // Keep mpv's output
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;

    if (vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass_) != VK_SUCCESS) {
        std::cerr << "Failed to create render pass" << std::endl;
        return false;
    }

    // Descriptor set layout
    VkDescriptorSetLayoutBinding sampler_binding{};
    sampler_binding.binding = 0;
    sampler_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler_binding.descriptorCount = 1;
    sampler_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &sampler_binding;

    if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &descriptor_layout_) != VK_SUCCESS) {
        std::cerr << "Failed to create descriptor set layout" << std::endl;
        return false;
    }

    // Push constant range
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(PushConstants);

    // Pipeline layout
    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descriptor_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;

    if (vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout" << std::endl;
        return false;
    }

    // Shader modules
    VkShaderModuleCreateInfo vert_info{};
    vert_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    vert_info.codeSize = sizeof(vert_spv);
    vert_info.pCode = reinterpret_cast<const uint32_t*>(vert_spv);

    VkShaderModule vert_module;
    if (vkCreateShaderModule(device, &vert_info, nullptr, &vert_module) != VK_SUCCESS) {
        std::cerr << "Failed to create vertex shader module" << std::endl;
        return false;
    }

    VkShaderModuleCreateInfo frag_info{};
    frag_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    frag_info.codeSize = sizeof(frag_spv);
    frag_info.pCode = reinterpret_cast<const uint32_t*>(frag_spv);

    VkShaderModule frag_module;
    if (vkCreateShaderModule(device, &frag_info, nullptr, &frag_module) != VK_SUCCESS) {
        std::cerr << "Failed to create fragment shader module" << std::endl;
        vkDestroyShaderModule(device, vert_module, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo shader_stages[2] = {};
    shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shader_stages[0].module = vert_module;
    shader_stages[0].pName = "main";
    shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shader_stages[1].module = frag_module;
    shader_stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width_);
    viewport.height = static_cast<float>(height_);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {width_, height_};

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;  // CEF uses premultiplied alpha
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &blend_attachment;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = shader_stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass_;
    pipeline_info.subpass = 0;

    VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);

    vkDestroyShaderModule(device, vert_module, nullptr);
    vkDestroyShaderModule(device, frag_module, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create graphics pipeline" << std::endl;
        return false;
    }

    return true;
}

bool VulkanCompositor::createDescriptorSets() {
    VkDevice device = vk_->device();

    // Descriptor pool
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    pool_info.maxSets = 1;

    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        std::cerr << "Failed to create descriptor pool" << std::endl;
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = descriptor_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_layout_;

    if (vkAllocateDescriptorSets(device, &alloc_info, &descriptor_set_) != VK_SUCCESS) {
        std::cerr << "Failed to allocate descriptor set" << std::endl;
        return false;
    }

    // Update descriptor set
    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = overlay_view_;
    image_info.sampler = sampler_;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set_;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    return true;
}

void VulkanCompositor::updateOverlay(const void* data, int width, int height) {
    if (!staging_mapped_ || width != static_cast<int>(width_) || height != static_cast<int>(height_)) {
        return;
    }

    // Copy data to staging buffer
    std::memcpy(staging_mapped_, data, width * height * 4);

    // Transfer to GPU image
    VkCommandBuffer cmd = vk_->beginSingleTimeCommands();

    // Transition to transfer dst
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = overlay_image_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy buffer to image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width_, height_, 1};

    vkCmdCopyBufferToImage(cmd, staging_buffer_, overlay_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition back to shader read
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vk_->endSingleTimeCommands(cmd);
}

void VulkanCompositor::composite(VkCommandBuffer cmd, VkImage target, VkImageView targetView,
                                  uint32_t width, uint32_t height, float alpha) {
    // Memory barrier to ensure DMA-BUF content is visible before sampling
    if (using_dmabuf_ && dmabuf_[dmabuf_current_].image) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = dmabuf_[dmabuf_current_].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;  // External writes (CEF)
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Create temporary framebuffer for this target
    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass_;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &targetView;
    fb_info.width = width;
    fb_info.height = height;
    fb_info.layers = 1;

    VkFramebuffer framebuffer;
    vkCreateFramebuffer(vk_->device(), &fb_info, nullptr, &framebuffer);

    VkRenderPassBeginInfo render_pass_begin{};
    render_pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin.renderPass = render_pass_;
    render_pass_begin.framebuffer = framebuffer;
    render_pass_begin.renderArea.offset = {0, 0};
    render_pass_begin.renderArea.extent = {width, height};

    vkCmdBeginRenderPass(cmd, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {width, height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    PushConstants pc{};
    pc.alpha = alpha;
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    // Cleanup framebuffer after command buffer is submitted (need to defer this)
    // For now, we'll destroy it immediately which is not ideal but works
    // In production, this should be deferred until after the command buffer completes
    vkDestroyFramebuffer(vk_->device(), framebuffer, nullptr);
}

void VulkanCompositor::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) return;

    vkDeviceWaitIdle(vk_->device());

    // Destroy all resources
    destroyDmaBufImage();
    destroyOverlayResources();

    // Destroy pipeline resources (swapchain format might have changed)
    VkDevice device = vk_->device();
    if (pipeline_) {
        vkDestroyPipeline(device, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_) {
        vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_) {
        vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    if (descriptor_layout_) {
        vkDestroyDescriptorSetLayout(device, descriptor_layout_, nullptr);
        descriptor_layout_ = VK_NULL_HANDLE;
    }
    if (render_pass_) {
        vkDestroyRenderPass(device, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }

    width_ = width;
    height_ = height;

    // Recreate everything
    createOverlayResources();
    createPipeline();
    createDescriptorSets();
}

void VulkanCompositor::destroyOverlayResources() {
    VkDevice device = vk_->device();

    if (staging_mapped_) {
        vkUnmapMemory(device, staging_memory_);
        staging_mapped_ = nullptr;
    }
    if (staging_buffer_) {
        vkDestroyBuffer(device, staging_buffer_, nullptr);
        staging_buffer_ = VK_NULL_HANDLE;
    }
    if (staging_memory_) {
        vkFreeMemory(device, staging_memory_, nullptr);
        staging_memory_ = VK_NULL_HANDLE;
    }
    if (sampler_) {
        vkDestroySampler(device, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (overlay_view_) {
        vkDestroyImageView(device, overlay_view_, nullptr);
        overlay_view_ = VK_NULL_HANDLE;
    }
    if (overlay_image_) {
        vkDestroyImage(device, overlay_image_, nullptr);
        overlay_image_ = VK_NULL_HANDLE;
    }
    if (overlay_memory_) {
        vkFreeMemory(device, overlay_memory_, nullptr);
        overlay_memory_ = VK_NULL_HANDLE;
    }
}

void VulkanCompositor::destroyDmaBufImage() {
    VkDevice device = vk_->device();
    for (int i = 0; i < DMABUF_BUFFER_COUNT; i++) {
        auto& buf = dmabuf_[i];
        if (buf.view) {
            vkDestroyImageView(device, buf.view, nullptr);
            buf.view = VK_NULL_HANDLE;
        }
        if (buf.image) {
            vkDestroyImage(device, buf.image, nullptr);
            buf.image = VK_NULL_HANDLE;
        }
        if (buf.memory) {
            vkFreeMemory(device, buf.memory, nullptr);
            buf.memory = VK_NULL_HANDLE;
        }
        buf.width = 0;
        buf.height = 0;
        buf.buffer_id = 0;
    }
    dmabuf_current_ = 0;
    using_dmabuf_ = false;
    has_software_content_ = false;
}

bool VulkanCompositor::updateOverlayFromDmaBuf(const AcceleratedPaintInfo& info) {
    if (info.planes.empty() || info.planes[0].fd < 0) {
        return false;
    }

    // Skip if we already know DMA-BUF doesn't work
    if (!dmabuf_supported_) {
        close(info.planes[0].fd);
        return false;
    }

    VkDevice device = vk_->device();

    // Get unique identifier for this DMA-BUF using fstat
    struct stat st;
    if (fstat(info.planes[0].fd, &st) != 0) {
        close(info.planes[0].fd);
        return false;
    }
    uint64_t buffer_id = (static_cast<uint64_t>(st.st_dev) << 32) | st.st_ino;

    // Check if we already have this buffer imported in any slot
    for (int i = 0; i < DMABUF_BUFFER_COUNT; i++) {
        if (dmabuf_[i].buffer_id == buffer_id && dmabuf_[i].image) {
            // Found existing import - reuse it
            close(info.planes[0].fd);
            dmabuf_current_ = i;
            using_dmabuf_ = true;

            // Update descriptor to point to this buffer
            VkDescriptorImageInfo img_info{};
            img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            img_info.imageView = dmabuf_[i].view;
            img_info.sampler = sampler_;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptor_set_;
            write.dstBinding = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &img_info;

            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            return true;
        }
    }

    // New buffer - find an empty slot
    int next = -1;
    for (int i = 0; i < DMABUF_BUFFER_COUNT; i++) {
        if (!dmabuf_[i].image) {
            next = i;
            break;
        }
    }
    if (next < 0) {
        // All slots full - skip frame rather than risk GPU hang
        close(info.planes[0].fd);
        return using_dmabuf_;
    }
    auto& buf = dmabuf_[next];

    // Create and import new DMA-BUF image
    VkExternalMemoryImageCreateInfo ext_info{};
    ext_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkImageDrmFormatModifierExplicitCreateInfoEXT drm_info{};
    drm_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    drm_info.drmFormatModifier = info.modifier;

    VkSubresourceLayout plane_layout{};
    plane_layout.offset = info.planes[0].offset;
    plane_layout.rowPitch = info.planes[0].stride;
    plane_layout.size = 0;
    plane_layout.depthPitch = 0;
    plane_layout.arrayPitch = 0;
    drm_info.drmFormatModifierPlaneCount = 1;
    drm_info.pPlaneLayouts = &plane_layout;

    ext_info.pNext = &drm_info;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.pNext = &ext_info;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    image_info.extent = {static_cast<uint32_t>(info.width),
                        static_cast<uint32_t>(info.height), 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vkCreateImage(device, &image_info, nullptr, &buf.image);
    if (result != VK_SUCCESS) {
        std::cerr << "DMA-BUF: vkCreateImage failed: " << result << std::endl;
        dmabuf_supported_ = false;
        close(info.planes[0].fd);
        return false;
    }

    // Get memory fd properties
    VkMemoryFdPropertiesKHR fd_props{};
    fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;

    auto vkGetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");

    if (!vkGetMemoryFdPropertiesKHR ||
        vkGetMemoryFdPropertiesKHR(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                    info.planes[0].fd, &fd_props) != VK_SUCCESS) {
        std::cerr << "DMA-BUF: vkGetMemoryFdPropertiesKHR failed" << std::endl;
        dmabuf_supported_ = false;
        close(info.planes[0].fd);
        vkDestroyImage(device, buf.image, nullptr);
        buf.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, buf.image, &mem_reqs);

    uint32_t mem_type_bits = mem_reqs.memoryTypeBits & fd_props.memoryTypeBits;
    if (mem_type_bits == 0) {
        std::cerr << "DMA-BUF: no compatible memory type" << std::endl;
        dmabuf_supported_ = false;
        close(info.planes[0].fd);
        vkDestroyImage(device, buf.image, nullptr);
        buf.image = VK_NULL_HANDLE;
        return false;
    }

    VkImportMemoryFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = info.planes[0].fd;

    VkMemoryDedicatedAllocateInfo dedicated_info{};
    dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated_info.pNext = &import_info;
    dedicated_info.image = buf.image;

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.pNext = &dedicated_info;
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = vk_->findMemoryType(mem_type_bits, 0);

    result = vkAllocateMemory(device, &alloc_info, nullptr, &buf.memory);
    if (result != VK_SUCCESS) {
        std::cerr << "DMA-BUF: vkAllocateMemory failed: " << result << std::endl;
        dmabuf_supported_ = false;
        close(info.planes[0].fd);
        vkDestroyImage(device, buf.image, nullptr);
        buf.image = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindImageMemory(device, buf.image, buf.memory, 0);
    if (result != VK_SUCCESS) {
        std::cerr << "DMA-BUF: vkBindImageMemory failed: " << result << std::endl;
        dmabuf_supported_ = false;
        vkFreeMemory(device, buf.memory, nullptr);
        buf.memory = VK_NULL_HANDLE;
        vkDestroyImage(device, buf.image, nullptr);
        buf.image = VK_NULL_HANDLE;
        return false;
    }

    // Note: For DMA-BUF imports with DRM modifiers, explicit layout transition
    // is not needed - the image can be used directly in UNDEFINED layout with
    // VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT. The driver handles it.

    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = buf.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_B8G8R8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device, &view_info, nullptr, &buf.view) != VK_SUCCESS) {
        std::cerr << "DMA-BUF: vkCreateImageView failed" << std::endl;
        dmabuf_supported_ = false;
        vkFreeMemory(device, buf.memory, nullptr);
        buf.memory = VK_NULL_HANDLE;
        vkDestroyImage(device, buf.image, nullptr);
        buf.image = VK_NULL_HANDLE;
        return false;
    }

    buf.width = info.width;
    buf.height = info.height;
    buf.buffer_id = buffer_id;
    dmabuf_current_ = next;

    // Update descriptor set
    VkDescriptorImageInfo img_info{};
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img_info.imageView = buf.view;
    img_info.sampler = sampler_;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &img_info;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    using_dmabuf_ = true;
    return true;
}

void VulkanCompositor::cleanup() {
    if (!vk_) return;
    VkDevice device = vk_->device();
    if (!device) {
        vk_ = nullptr;
        return;
    }

    vkDeviceWaitIdle(device);

    destroyDmaBufImage();
    destroyOverlayResources();

    if (pipeline_) {
        vkDestroyPipeline(device, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_) {
        vkDestroyPipelineLayout(device, pipeline_layout_, nullptr);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_) {
        vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    if (descriptor_layout_) {
        vkDestroyDescriptorSetLayout(device, descriptor_layout_, nullptr);
        descriptor_layout_ = VK_NULL_HANDLE;
    }
    if (render_pass_) {
        vkDestroyRenderPass(device, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }

    vk_ = nullptr;
}
