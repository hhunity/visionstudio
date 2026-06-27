#pragma once
#include <fstream>
#include <iterator>
#include <string>

// In-memory editor state for a text config file.
struct config_tab {
    std::string path;
    std::string text;
    bool        modified = false;

    void load() {
        std::ifstream f(path);
        if (!f.is_open()) return;
        text = std::string(std::istreambuf_iterator<char>(f), {});
        modified = false;
    }
    void save() {
        std::ofstream f(path);
        if (!f.is_open()) return;
        f << text;
        modified = false;
    }
};
