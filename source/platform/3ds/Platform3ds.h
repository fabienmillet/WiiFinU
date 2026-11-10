#pragma once
#include <string>

namespace Platform3ds {
/* First thing in main(): the GPU, both screens, the New 3DS's speed */
void init();
/* A line under the clock on the top screen ("" none) */
void setTopCaption(const std::string& s);
bool isNew3DS();
}
