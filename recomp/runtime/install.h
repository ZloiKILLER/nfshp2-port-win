#pragma once
// What the first start puts in the game's folder (install.cpp): from the disc image, the archive as files in data\ (as tools/recomp/unpack_data.py
// did), and the port's own files from the exe's resources.
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
// every texture as a PNG (data\textures) and every text as UTF-8 (data\text), on a data\ that has index.txt, as export_textures.py and
// export_text.py make them. Not part of the install: HP2_EXPORTS=<data folder> alone, kept for a tool for the assets
bool install_exports(const std::wstring &data, InstallSteps &steps);
// the port's own files (the languages, the cursor, README) into a game's folder: what an update renews beside the exe
bool install_port_files(const std::wstring &root);
bool install_write_png(const std::wstring &path, int w, int h, const std::vector<uint8_t> &rgba);
