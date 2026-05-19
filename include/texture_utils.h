#ifndef TEXTURE_UTILS_H
#define TEXTURE_UTILS_H

#include <string>
#include <GL/gl.h>

GLuint load_texture(const std::string& path);
std::string get_tile_path(int z, int x, int y);
size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream);
void download_tile_async(int z, int x, int y);
GLuint get_or_request_tile(int z, int x, int y);

#endif 