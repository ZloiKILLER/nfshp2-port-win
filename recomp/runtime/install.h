#pragma once
// What the first start puts in the game's folder (install.cpp): from the disc image, the archive as files in data\ (as tools/recomp/unpack_data.py
// did), every texture as a PNG and every text as UTF-8 (export_textures.py, export_text.py), and the port's own files from the exe's resources.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct Iso;

struct InstallSteps {
    std::function<void(int permille, const std::wstring &what)> progress;
    const std::atomic<bool> *stop = nullptr;
    std::string why;            // when it fails
};

bool install_game(Iso &iso, const std::wstring &root, InstallSteps &steps);
// the parts after the unpacking, on a data\ that has index.txt: for checking them against the Python tools' folders
bool install_exports(const std::wstring &data, InstallSteps &steps);
// the port's own files (the languages, the cursor, README) into a game's folder: what an update renews beside the exe
bool install_port_files(const std::wstring &root);
bool install_write_png(const std::wstring &path, int w, int h, const std::vector<uint8_t> &rgba);
