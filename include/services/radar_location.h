#pragma once

#include <cstddef>

namespace services {
namespace location {

void init();
double lat();
double lon();
const char* name();
size_t currentIndex();
size_t count();

void setIndex(size_t index);
void next();
void set(double latitude, double longitude, const char* location_name);
bool saveFromStrings(const char* lat_str, const char* lon_str);
void clear();

}  // namespace location
}  // namespace services