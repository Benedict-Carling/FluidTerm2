#pragma once
#include <string>

const char* fileTail(const char* path);
bool showOpenFileDialog(std::string& fileName, const char* filter, const char* title);
bool showSaveFileDialog(std::string& fileName, const char* filter, const char* title); 