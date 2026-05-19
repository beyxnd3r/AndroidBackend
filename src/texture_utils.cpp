#include "../include/texture_utils.h"
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <map>
#include <thread>  // Добавлено для std::thread
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static std::mutex tile_cache_mutex;
static std::map<std::string, GLuint> tile_texture_cache;
static std::map<std::string, bool> tile_requested;

GLuint load_texture(const std::string& path)
{
    int w,h,ch;
    unsigned char* data = stbi_load(path.c_str(),&w,&h,&ch,4);
    if (!data) { std::cerr << "Failed to load texture: " << path << "\n"; return 0; }
    GLuint tex; glGenTextures(1,&tex);
    glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,data);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D,0);
    stbi_image_free(data);
    return tex;
}

std::string get_tile_path(int z, int x, int y) {
    return "tiles/"+std::to_string(z)+"/"+std::to_string(x)+"/"+std::to_string(y)+".png";
}

size_t write_data(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    return fwrite(ptr,size,nmemb,stream);
}

void download_tile_async(int z, int x, int y)
{
    std::string path = get_tile_path(z,x,y);
    if (std::filesystem::exists(path)) return;
    std::filesystem::create_directories("tiles/"+std::to_string(z)+"/"+std::to_string(x));
    const char* servers[]={"a","b","c"};
    std::string url = "https://"+std::string(servers[rand()%3])+".basemaps.cartocdn.com/rastertiles/voyager/"
                      +std::to_string(z)+"/"+std::to_string(x)+"/"+std::to_string(y)+".png";
    CURL* curl = curl_easy_init(); if (!curl) return;
    FILE* fp = fopen(path.c_str(),"wb"); if (!fp) { curl_easy_cleanup(curl); return; }
    curl_easy_setopt(curl,CURLOPT_URL,          url.c_str());
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_data);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,    fp);
    curl_easy_setopt(curl,CURLOPT_USERAGENT,    "Mozilla/5.0");
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,      10L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) std::cerr << "CURL ERROR: " << curl_easy_strerror(res) << "\n";
    curl_easy_cleanup(curl); fclose(fp);
}

GLuint get_or_request_tile(int z, int x, int y)
{
    std::string key  = std::to_string(z)+"/"+std::to_string(x)+"/"+std::to_string(y);
    std::string path = get_tile_path(z,x,y);
    std::lock_guard<std::mutex> lk(tile_cache_mutex);
    auto it = tile_texture_cache.find(key);
    if (it != tile_texture_cache.end() && it->second != 0) return it->second;
    if (std::filesystem::exists(path)) { GLuint tex=load_texture(path); tile_texture_cache[key]=tex; return tex; }
    if (!tile_requested[key]) {
        tile_requested[key]=true; tile_texture_cache[key]=0;
        std::thread([z,x,y](){ download_tile_async(z,x,y); }).detach();
    }
    return 0;
}