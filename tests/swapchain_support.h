// SPDX-License-Identifier: LGPL-2.1-or-later
// See swapchain_support.mm.
#pragma once

#include <cstddef>
#include <cstdint>

// Makes swap chain creation for window handle 1 use a detached CAMetalLayer.
void test_install_layer_provider();
// Reads the pixel (x, y) of a PNG as R, G, B; false if the file cannot be read.
bool test_png_pixel(const char *path, size_t x, size_t y, uint8_t rgb[3]);
