// The setup (setup.h), what this exe is when it is not in a game's folder. Its window installs or updates. Install: it asks for the .iso of
// Need for Speed: Hot Pursuit 2 (a disc of kKnownDiscs), or takes the disc itself from a DVD drive, and where to install; reads the image
// as ISO 9660 without mounting it (the disc in a drive: as the files Windows shows), checks the game's program by its SHA-1, makes the folder
// with all the game needs (install.cpp), puts this exe in it, and starts the game from there. Update: it asks for a folder the game is
// installed in and puts this exe and the port's own files there; the settings, the saves and data\replace are not touched. The folder is any the user names: typed, or browsed to (then with the game's own folder under it, as
// installers do). It is remembered (HKCU\Software\NFSHP2Port, InstallDir), so this exe started again elsewhere starts the installed game.
#include "setup.h"

#include "host.h"
#include "install.h"
#include "setup_iso.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#pragma comment(lib, "version.lib")

namespace fs = std::filesystem;

namespace {

const uint64_t kNeeded = 3600ull * 1024 * 1024;   // the folder: 3.0 GB of the disc's files, the PNGs, room to spare

std::wstring widen(const std::string &s)
{
    std::wstring w(s.size() + 1, L'\0');
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], (int)w.size());
    w.resize(n > 0 ? (size_t)n : 0);
    return w;
}

std::wstring exe_path()
{
    wchar_t p[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)(sizeof p / sizeof p[0]));
    return std::wstring(p, n);
}

bool is_game_folder(const fs::path &dir)
{
    std::error_code ec;
    return fs::is_regular_file(dir / L"data" / L"index.txt", ec) && fs::is_regular_file(dir / L"data" / L"SLUS_203.62", ec);
}

const wchar_t kGameFolder[] = L"Need for Speed Hot Pursuit 2";
const wchar_t kRegKey[] = L"Software\\NFSHP2Port", kRegValue[] = L"InstallDir";

// where the game was installed last, "" if nowhere or not known
std::wstring remembered_folder()
{
    wchar_t buf[MAX_PATH * 4] = {};
    DWORD bytes = sizeof buf - sizeof buf[0];
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, kRegValue, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) return L"";
    return buf;
}

void remember_folder(const std::wstring &dir)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, kRegValue, 0, REG_SZ, (const BYTE *)dir.c_str(), (DWORD)((dir.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

bool start_exe(const fs::path &exe)
{
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    if (!CreateProcessW(exe.wstring().c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().wstring().c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// a shortcut on the desktop to the installed game
void make_shortcut(const fs::path &exe)
{
    IShellLinkW *link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
    link->SetPath(exe.wstring().c_str());
    link->SetWorkingDirectory(exe.parent_path().wstring().c_str());
    link->SetDescription(L"Need for Speed: Hot Pursuit 2");
    IPersistFile *file = nullptr;
    PWSTR desktop = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) {
        file->Save((fs::path(desktop) / L"Need for Speed Hot Pursuit 2.lnk").wstring().c_str(), TRUE);
    }
    if (desktop) CoTaskMemFree(desktop);
    if (file) file->Release();
    link->Release();
}

// ---- the versions ----------------------------------------------------------------------------------
// an exe's version from its resource, major << 16 | minor; 0 when it has none or is not there
uint32_t exe_version(const fs::path &p)
{
    DWORD handle = 0;
    const DWORD n = GetFileVersionInfoSizeW(p.c_str(), &handle);
    if (!n) return 0;
    std::vector<uint8_t> b(n);
    VS_FIXEDFILEINFO *fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(p.c_str(), 0, n, b.data()) || !VerQueryValueW(b.data(), L"\\", (void **)&fi, &len) || !fi) return 0;
    return fi->dwFileVersionMS;
}

std::wstring version_text(uint32_t v) { return std::to_wstring(v >> 16) + L"." + std::to_wstring(v & 0xFFFF); }

// two files with the same bytes
bool same_file(const fs::path &a, const fs::path &b)
{
    std::error_code ec;
    if (fs::equivalent(a, b, ec)) return true;
    ec.clear();
    const uintmax_t na = fs::file_size(a, ec), nb = fs::file_size(b, ec);
    if (ec || na != nb) return false;
    FILE *fa = _wfopen(a.c_str(), L"rb"), *fb = _wfopen(b.c_str(), L"rb");
    bool same = fa && fb;
    std::vector<uint8_t> ba(1 << 20), bb(1 << 20);
    while (same) {
        const size_t ra = fread(ba.data(), 1, ba.size(), fa), rb = fread(bb.data(), 1, bb.size(), fb);
        same = ra == rb && !memcmp(ba.data(), bb.data(), ra);
        if (!ra) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return same;
}

// ---- the sources and the update --------------------------------------------------------------------
// what the game is installed from: a disc image, or a drive with the disc in it, named by its root, "E:\"
bool is_drive(const std::wstring &s) { return s.size() == 3 && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/'); }
bool open_source(Iso &iso, const std::wstring &src, std::string &why) { return is_drive(src) ? iso.open_drive(src, why) : iso.open(src, why); }

// the CD and DVD drives (a mounted image is one) that have the game's disc in them, by their roots
std::vector<std::wstring> game_drives()
{
    std::vector<std::wstring> out;
    const UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS);     // an empty drive is no error box
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask >> i & 1)) continue;
        const std::wstring root = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_CDROM) continue;
        std::error_code ec;
        if (fs::is_regular_file(root + L"SYSTEM.CNF", ec) && fs::is_regular_file(root + L"NFSHP2\\ZZDATA.BIN", ec)) out.push_back(root);
    }
    SetErrorMode(mode);
    return out;
}

// The update of a folder the game is installed in: this exe as its speed.exe and the port's own files (the languages, the cursor, README).
// Nothing else there is written: not hp2.ini, not the saves and the replays, not data\replace, not the game's data.
bool update_folder(const std::wstring &root, std::wstring &why)
{
    std::error_code ec;
    const fs::path from = exe_path(), to = fs::path(root) / L"speed.exe";
    if (!fs::equivalent(from, to, ec)) {
        ec.clear();
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec) return why = L"speed.exe in that folder could not be replaced. If the game is running, close it and press Update again.", false;
    }
    if (!install_port_files(root)) return why = L"The port's own files could not be written into that folder.", false;
    return true;
}

// ---- the window ------------------------------------------------------------------------------------
// One window that does two things, chosen at its top: Install (the game's files from a disc image or from the disc in a drive, into a new
// folder) and Update (update_folder, on a folder where the game is installed already). The controls of the one not chosen are hidden.
enum {
    ID_BROWSE = 101, ID_INSTALL, ID_CANCEL, ID_FOLDER, ID_SHORTCUT, ID_TARGET, ID_DRIVE, ID_MODE_INSTALL, ID_MODE_UPDATE, ID_UPD_TARGET, ID_UPD_FOLDER,
    ID_DRIVE_FIRST = 200,
    WM_SETUP_PROGRESS = WM_APP + 1, WM_SETUP_DONE
};
enum { MODE_INSTALL, MODE_UPDATE };

struct Ui {
    HWND wnd = nullptr, path = nullptr, status = nullptr, target = nullptr, bar = nullptr, step = nullptr, install = nullptr, cancel = nullptr,
         browse = nullptr, folder = nullptr, shortcut = nullptr, drive = nullptr, mode_install = nullptr, mode_update = nullptr,
         upd_target = nullptr, upd_folder = nullptr, upd_status = nullptr;
    std::vector<HWND> install_page, update_page;
    HFONT font = nullptr, heading = nullptr;
    std::wstring iso, root, failure, disc;    // iso: the image, or the drive's root; disc: the name of its disc once it has been checked, "" until then
    std::wstring upd_root;
    int mode = MODE_INSTALL;
    bool upd_play = false;                    // the game in the folder is this exe already: the button is Play
    std::atomic<bool> busy{false}, stop{false};
    std::atomic<int> permille{0};
    std::wstring step_text;
    CRITICAL_SECTION lock;
    bool ok = false, finished = false, want_shortcut = true;
} g_ui;

void set_status(const std::wstring &s) { SetWindowTextW(g_ui.status, s.c_str()); }

// the folder in a box, as it is typed: no quotes, no spaces around it, no slash at its end
std::wstring typed_folder(HWND box)
{
    wchar_t buf[MAX_PATH * 4] = {};
    GetWindowTextW(box, buf, (int)(sizeof buf / sizeof buf[0]));
    std::wstring r = buf;
    while (!r.empty() && (r.back() == L' ' || r.back() == L'"' || r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    size_t at = 0;
    while (at < r.size() && (r[at] == L' ' || r[at] == L'"')) at++;
    r = r.substr(at);
    if (r.size() == 2 && r[1] == L':') r += L'\\';     // a drive alone is its root
    return r;
}

// The source is the game's (g_ui.disc): is the folder one to install into, and is there room. Install is there to press only then.
void check_folder()
{
    if (g_ui.mode != MODE_INSTALL) return;
    EnableWindow(g_ui.install, FALSE);
    if (g_ui.disc.empty()) return;
    g_ui.root = typed_folder(g_ui.target);
    const fs::path root = g_ui.root;
    if (g_ui.root.empty() || !root.is_absolute()) return set_status(g_ui.disc + L". Give the folder with its drive, like D:\\Games\\" + kGameFolder + L".");
    ULARGE_INTEGER free_bytes = {};
    const std::wstring drive = root.root_path().wstring();
    if (!GetDiskFreeSpaceExW(drive.c_str(), &free_bytes, nullptr, nullptr)) return set_status(g_ui.disc + L". There is no drive " + drive + L".");
    if (free_bytes.QuadPart < kNeeded) return set_status(g_ui.disc + L". Not enough free space: 3.6 GB are needed on " + drive + L".");
    set_status(g_ui.disc + L". Ready to install.");
    EnableWindow(g_ui.install, TRUE);
}

// the image or the drive chosen: is it the game
void check_iso()
{
    EnableWindow(g_ui.install, FALSE);
    g_ui.disc.clear();
    const std::wstring what = is_drive(g_ui.iso) ? L"disc" : L"disc image";
    Iso iso;
    std::string why;
    if (!open_source(iso, g_ui.iso, why)) return set_status(widen(why));
    std::string boot;
    const Iso::File *elf = iso.boot(boot);
    if (!elf || !iso.find("/NFSHP2/ZZDATA.BIN") || !iso.find("/NFSHP2/ZDIR.BIN") || !iso.find("/NFSHP2/IRX253/STREAM.IRX"))
        return set_status(L"This " + what + L" is not Need for Speed: Hot Pursuit 2.");
    set_status(L"Checking the " + what + L"...");
    UpdateWindow(g_ui.status);
    std::vector<uint8_t> buf((size_t)elf->size);
    if (!iso.read_file(*elf, 0, buf.data(), buf.size())) return set_status(L"The " + what + L" cannot be read.");
    const std::string hex = sha1_hex(buf.data(), buf.size());
    std::string name;
    for (const KnownDisc &k : kKnownDiscs)
        if (hex == k.elf_sha1 && !_stricmp(boot.c_str(), k.boot)) name = k.name;
    if (name.empty())
        return set_status(L"This disc is not supported yet.");
    g_ui.disc = widen(name);
    check_folder();
}

void browse()
{
    wchar_t file[MAX_PATH * 4] = {};
    OPENFILENAMEW of = {sizeof of};
    of.hwndOwner = g_ui.wnd;
    of.lpstrFilter = L"Disc images (*.iso)\0*.iso\0All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    of.lpstrTitle = L"Choose the disc image of Need for Speed: Hot Pursuit 2";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return;
    g_ui.iso = file;
    SetWindowTextW(g_ui.path, file);
    check_iso();
}

// From drive: the drive that has the game's disc in it; of several, the one picked from a menu under the button
void from_drive()
{
    const std::vector<std::wstring> drives = game_drives();
    if (drives.empty()) {
        g_ui.iso.clear();
        g_ui.disc.clear();
        SetWindowTextW(g_ui.path, L"");
        EnableWindow(g_ui.install, FALSE);
        return set_status(L"No disc of the game in a DVD drive. Put the disc in and press From drive again.");
    }
    std::wstring pick = drives[0];
    if (drives.size() > 1) {
        HMENU menu = CreatePopupMenu();
        for (size_t i = 0; i < drives.size(); i++) AppendMenuW(menu, MF_STRING, ID_DRIVE_FIRST + i, (L"Drive " + drives[i].substr(0, 2)).c_str());
        RECT r;
        GetWindowRect(g_ui.drive, &r);
        const int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_ui.wnd, nullptr);
        DestroyMenu(menu);
        if (id < ID_DRIVE_FIRST) return;
        pick = drives[(size_t)(id - ID_DRIVE_FIRST)];
    }
    g_ui.iso = pick;
    SetWindowTextW(g_ui.path, (L"The disc in drive " + pick.substr(0, 2)).c_str());
    check_iso();
}

// The folder to update: is the game in it, and which version. The button is Update, or Play when the game there is this exe already.
void check_update()
{
    if (g_ui.mode != MODE_UPDATE) return;
    g_ui.upd_play = false;
    SetWindowTextW(g_ui.install, L"Update");
    EnableWindow(g_ui.install, FALSE);
    g_ui.upd_root = typed_folder(g_ui.upd_target);
    const fs::path root = g_ui.upd_root;
    auto say = [](const std::wstring &s) { SetWindowTextW(g_ui.upd_status, s.c_str()); };
    if (g_ui.upd_root.empty()) return say(L"Choose the folder the game is installed in.");
    if (!root.is_absolute() || !is_game_folder(root)) return say(L"There is no installed game in that folder (it has no data\\index.txt).");
    const fs::path exe = root / L"speed.exe";
    const uint32_t have = exe_version(exe), mine = exe_version(exe_path());
    const std::wstring to = L"version " + version_text(mine);
    std::error_code ec;
    EnableWindow(g_ui.install, TRUE);
    if (!fs::exists(exe, ec)) return say(L"The game's data is in that folder, its program is not. Update puts " + to + L" there.");
    if (same_file(exe, exe_path())) {
        g_ui.upd_play = true;
        SetWindowTextW(g_ui.install, L"Play");
        return say(L"The game in that folder is this " + to + L" already. Nothing to update.");
    }
    if (have > mine) return say(L"The game in that folder is version " + version_text(have) + L", newer than this one. Update would put the older " + to + L" there.");
    if (have == mine) return say(L"Installed: version " + version_text(have) + L", another build of it. Update puts this build there.");
    say(L"Installed: version " + (have ? version_text(have) : std::wstring(L"unknown")) + L". Update puts " + to + L" there.");
}

// Where to install, browsed to: the folder chosen with the game's own folder under it, unless it is that already (by its name, or by the game
// being in it). What is in the box after that is the user's to change, and is taken as it stands. For an update: the folder chosen, or the
// game's folder under it when the game is there and not in the one chosen.
void choose_folder(bool update)
{
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(update ? L"Choose the folder the game is installed in" : L"Choose where to install the game");
    d->SetOkButtonLabel(update ? L"This folder" : L"Install here");
    if (SUCCEEDED(d->Show(g_ui.wnd))) {
        IShellItem *it = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            std::wstring r = path;
            while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
            const fs::path picked = r;
            if (update) {
                if (!is_game_folder(picked) && is_game_folder(picked / kGameFolder)) r = (picked / kGameFolder).wstring();
                SetWindowTextW(g_ui.upd_target, r.c_str());     // EN_CHANGE checks it
            } else {
                if (_wcsicmp(picked.filename().c_str(), kGameFolder) && !is_game_folder(picked)) r = (picked / kGameFolder).wstring();
                g_ui.root = r;
                SetWindowTextW(g_ui.target, r.c_str());         // EN_CHANGE checks it
            }
            CoTaskMemFree(path);
        }
        if (it) it->Release();
    }
    d->Release();
}

void set_mode(int mode)
{
    g_ui.mode = mode;
    for (HWND h : g_ui.install_page) ShowWindow(h, mode == MODE_INSTALL ? SW_SHOW : SW_HIDE);
    for (HWND h : g_ui.update_page) ShowWindow(h, mode == MODE_UPDATE ? SW_SHOW : SW_HIDE);
    SendMessageW(g_ui.mode_install, BM_SETCHECK, mode == MODE_INSTALL ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_ui.mode_update, BM_SETCHECK, mode == MODE_UPDATE ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g_ui.install, mode == MODE_INSTALL ? L"Install" : L"Update");
    EnableWindow(g_ui.install, FALSE);
    if (mode == MODE_INSTALL) check_folder();
    else check_update();
    // the two pages lie over each other: all of the window drawn again after one is hidden and the other shown
    RedrawWindow(g_ui.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void progress(int permille, const std::wstring &text)
{
    EnterCriticalSection(&g_ui.lock);
    g_ui.step_text = text;
    LeaveCriticalSection(&g_ui.lock);
    g_ui.permille = permille;
    PostMessageW(g_ui.wnd, WM_SETUP_PROGRESS, 0, 0);
}

void install_thread()
{
    Iso iso;
    std::string why;
    InstallSteps steps;
    steps.progress = progress;
    steps.stop = &g_ui.stop;
    bool ok = open_source(iso, g_ui.iso, why) && install_game(iso, g_ui.root, steps);
    if (ok) {
        // this exe into the game's folder: the game is started from there from now on
        std::error_code ec;
        const fs::path from = exe_path(), to = fs::path(g_ui.root) / L"speed.exe";
        if (!fs::equivalent(from, to, ec)) {
            ec.clear();
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        }
        ok = fs::exists(to);
        if (!ok) steps.why = "speed.exe could not be copied into the folder.";
    }
    if (!ok) {
        EnterCriticalSection(&g_ui.lock);
        g_ui.failure = widen(steps.why.empty() ? why : steps.why);
        LeaveCriticalSection(&g_ui.lock);
    }
    g_ui.ok = ok && !g_ui.stop;
    PostMessageW(g_ui.wnd, WM_SETUP_DONE, 0, 0);
}

void lock_controls(bool locked)
{
    for (HWND h : {g_ui.browse, g_ui.drive, g_ui.folder, g_ui.shortcut, g_ui.install, g_ui.target, g_ui.mode_install, g_ui.mode_update})
        EnableWindow(h, !locked);
    SetWindowTextW(g_ui.cancel, locked ? L"Cancel" : L"Exit");
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        if (g_ui.busy && LOWORD(w) != ID_CANCEL) return 0;
        if (LOWORD(w) == ID_BROWSE) browse();
        else if (LOWORD(w) == ID_DRIVE) from_drive();
        else if (LOWORD(w) == ID_FOLDER) choose_folder(false);
        else if (LOWORD(w) == ID_UPD_FOLDER) choose_folder(true);
        else if (LOWORD(w) == ID_TARGET && HIWORD(w) == EN_CHANGE) check_folder();
        else if (LOWORD(w) == ID_UPD_TARGET && HIWORD(w) == EN_CHANGE) check_update();
        else if (LOWORD(w) == ID_MODE_INSTALL && g_ui.mode != MODE_INSTALL) set_mode(MODE_INSTALL);
        else if (LOWORD(w) == ID_MODE_UPDATE && g_ui.mode != MODE_UPDATE) set_mode(MODE_UPDATE);
        else if (LOWORD(w) == ID_INSTALL && g_ui.mode == MODE_UPDATE) {
            check_update();
            if (!IsWindowEnabled(g_ui.install)) return 0;
            if (!g_ui.upd_play) {
                const HCURSOR old = SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_WAIT));
                std::wstring why;
                const bool ok = update_folder(g_ui.upd_root, why);
                SetCursor(old);
                if (!ok) {
                    SetWindowTextW(g_ui.upd_status, why.c_str());
                    return 0;
                }
            }
            g_ui.root = g_ui.upd_root;
            g_ui.want_shortcut = false;
            g_ui.finished = true;
            DestroyWindow(h);
        } else if (LOWORD(w) == ID_INSTALL) {
            g_ui.root = typed_folder(g_ui.target);
            std::error_code ec;
            const fs::path root = g_ui.root;
            if (fs::exists(root, ec) && !fs::is_directory(root, ec)) {
                MessageBoxW(h, L"That is a file, not a folder.", L"Setup", MB_ICONWARNING);
                return 0;
            }
            if (fs::is_directory(root, ec) && !fs::is_empty(root, ec) && !is_game_folder(root) &&
                MessageBoxW(h, (L"The folder\n" + g_ui.root + L"\nalready has files in it. The game's files go straight into it.\n\nInstall there anyway?").c_str(),
                            L"Setup", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                return 0;
            g_ui.busy = true;
            g_ui.want_shortcut = SendMessageW(g_ui.shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED;
            lock_controls(true);
            ShowWindow(g_ui.bar, SW_SHOW);
            ShowWindow(g_ui.step, SW_SHOW);
            set_status(is_drive(g_ui.iso) ? L"Installing from the disc. This takes several minutes." : L"Installing. This takes about a minute.");
            std::thread(install_thread).detach();
        } else if (LOWORD(w) == ID_CANCEL) {
            if (g_ui.busy) g_ui.stop = true;
            else DestroyWindow(h);
        }
        return 0;
    case WM_SETUP_PROGRESS: {
        SendMessageW(g_ui.bar, PBM_SETPOS, (WPARAM)g_ui.permille.load(), 0);
        EnterCriticalSection(&g_ui.lock);
        std::wstring t = g_ui.step_text;
        LeaveCriticalSection(&g_ui.lock);
        SetWindowTextW(g_ui.step, t.c_str());
        return 0;
    }
    case WM_SETUP_DONE:
        g_ui.busy = false;
        if (g_ui.ok) {
            g_ui.finished = true;
            DestroyWindow(h);
        } else {
            EnterCriticalSection(&g_ui.lock);
            std::wstring f = g_ui.failure;
            LeaveCriticalSection(&g_ui.lock);
            set_status(g_ui.stop ? L"Installation cancelled." : L"Installation failed: " + f);
            lock_controls(false);
            SetWindowTextW(g_ui.step, L"");
        }
        g_ui.stop = false;
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        if (g_ui.busy) g_ui.stop = true;
        else DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND add(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id = 0, HFONT f = nullptr)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_ui.wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_ui.font), TRUE);
    return c;
}

// the window as it is drawn, to a PNG (HP2_SETUP_SHOT: the tests look at it)
void save_shot(const std::wstring &path)
{
    RECT r;
    GetWindowRect(g_ui.wnd, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    HDC wdc = GetWindowDC(g_ui.wnd), mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(g_ui.wnd, mdc, getenv("HP2_SETUP_SHOT_DWM") ? 2 : 0);     // 0: the window draws itself into the picture, nothing of the screen
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    const uint8_t *s = (const uint8_t *)bits;
    for (size_t i = 0; i < (size_t)w * h; i++) rgba[i * 4] = s[i * 4 + 2], rgba[i * 4 + 1] = s[i * 4 + 1], rgba[i * 4 + 2] = s[i * 4], rgba[i * 4 + 3] = 255;
    install_write_png(path, w, h, rgba);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(g_ui.wnd, wdc);
}

// found: a folder where the game is installed already, or empty
bool run_window(const fs::path &exe_dir, const fs::path &found)
{
    InitializeCriticalSection(&g_ui.lock);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_ui.root = (exe_dir / kGameFolder).wstring();
    NONCLIENTMETRICSW ncm = {sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_ui.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW big = ncm.lfMessageFont;
    big.lfHeight = big.lfHeight * 16 / 9;
    big.lfWeight = FW_SEMIBOLD;
    g_ui.heading = CreateFontIndirectW(&big);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"HP2Setup";
    RegisterClassW(&wc);
    const int W = 600, H = 486;
    RECT r = {0, 0, W, H};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    g_ui.wnd = CreateWindowExW(0, wc.lpszClassName, L"Need for Speed: Hot Pursuit 2 - Setup", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                               r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    RECT wr;
    GetWindowRect(g_ui.wnd, &wr);
    SetWindowPos(g_ui.wnd, nullptr, (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2, (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    HWND icon = add(L"STATIC", nullptr, SS_ICON, 20, 18, 48, 48);
    SendMessageW(icon, STM_SETICON, (WPARAM)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 48, 48, 0), 0);
    add(L"STATIC", L"Need for Speed: Hot Pursuit 2", 0, 84, 14, 490, 34, 0, g_ui.heading);
    add(L"STATIC", (L"Setup of version " + version_text(exe_version(exe_path())) + L": the game's files come from your own copy.").c_str(), 0, 84, 50, 490, 20);
    g_ui.mode_install = add(L"BUTTON", L"Install the game", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 20, 84, 200, 22, ID_MODE_INSTALL);
    g_ui.mode_update = add(L"BUTTON", L"Update an installed game", BS_AUTORADIOBUTTON, 240, 84, 300, 22, ID_MODE_UPDATE);

    // ---- Install
    auto in = [](HWND c) { return g_ui.install_page.push_back(c), c; };
    in(add(L"STATIC",
           L"Choose the disc image (.iso) of your copy of Need for Speed: Hot Pursuit 2 for PlayStation 2, or put the disc itself "
           L"into a DVD drive and press From drive. Its files are unpacked into the folder below; the disc is only read.",
           WS_GROUP, 20, 118, W - 40, 58));
    // the discs by their numbers, as they are printed on the case and shown by an emulator
    in(add(L"STATIC", L"Supported discs:  USA (SLUS-20362),  Europe v1.00 and v2.00 (SLES-50731),\nAsia (SLPM-65181),  Korea (SLPM-67527).", 0, 20, 180,
           W - 40, 40));
    in(add(L"STATIC", L"Disc image or drive:", 0, 20, 228, 200, 20));
    g_ui.path = in(add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, 20, 250, W - 40 - 220, 24));
    g_ui.browse = in(add(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, W - 20 - 210, 249, 100, 26, ID_BROWSE));
    g_ui.drive = in(add(L"BUTTON", L"From drive", BS_PUSHBUTTON | WS_TABSTOP, W - 20 - 100, 249, 100, 26, ID_DRIVE));
    g_ui.status = in(add(L"STATIC", L"No disc image chosen.", 0, 20, 282, W - 40, 20));
    in(add(L"STATIC", L"Install to:", 0, 20, 310, 100, 20));
    g_ui.target = in(add(L"EDIT", g_ui.root.c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 20, 332, W - 40 - 110, 24, ID_TARGET));
    g_ui.folder = in(add(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, W - 20 - 100, 331, 100, 26, ID_FOLDER));
    g_ui.shortcut = in(add(L"BUTTON", L"Create a shortcut on the desktop", BS_AUTOCHECKBOX | WS_TABSTOP, 20, H - 42, 300, 22, ID_SHORTCUT));
    SendMessageW(g_ui.shortcut, BM_SETCHECK, BST_CHECKED, 0);

    // ---- Update
    auto up = [](HWND c) { return g_ui.update_page.push_back(c), ShowWindow(c, SW_HIDE), c; };
    up(add(L"STATIC",
           L"Choose the folder where the game is installed. Its program (speed.exe) and the port's own files (the languages, the cursor, "
           L"README) are replaced by this version's. Your settings, saves, replays and the files in data\\replace stay as they are, "
           L"and no disc is needed.",
           WS_GROUP, 20, 118, W - 40, 78));
    up(add(L"STATIC", L"Game folder:", 0, 20, 228, 200, 20));
    g_ui.upd_target = up(add(L"EDIT", found.wstring().c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 20, 250, W - 40 - 110, 24, ID_UPD_TARGET));
    g_ui.upd_folder = up(add(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, W - 20 - 100, 249, 100, 26, ID_UPD_FOLDER));
    g_ui.upd_status = up(add(L"STATIC", L"", 0, 20, 282, W - 40, 40));

    g_ui.bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD, 20, 372, W - 40, 18, g_ui.wnd, nullptr, inst, nullptr);
    SendMessageW(g_ui.bar, PBM_SETRANGE32, 0, 1000);
    g_ui.step = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_PATHELLIPSIS, 20, 394, W - 40, 20, g_ui.wnd, nullptr, inst, nullptr);
    SendMessageW(g_ui.step, WM_SETFONT, (WPARAM)g_ui.font, TRUE);
    g_ui.install = add(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_DISABLED | WS_GROUP, W - 20 - 210, H - 46, 100, 28, ID_INSTALL);
    g_ui.cancel = add(L"BUTTON", L"Exit", BS_PUSHBUTTON | WS_TABSTOP, W - 20 - 100, H - 46, 100, 28, ID_CANCEL);

    // What the window opens with: Update when the game is installed already, else Install. For the tests' pictures: HP2_SETUP_MODE, the image
    // (or the drive) already chosen HP2_SETUP_ISO, the folders HP2_SETUP_DIR and HP2_SETUP_UPDIR, the button pressed HP2_SETUP_AUTO.
    if (const char *dir = getenv("HP2_SETUP_DIR")) SetWindowTextW(g_ui.target, widen(dir).c_str());
    if (const char *dir = getenv("HP2_SETUP_UPDIR")) SetWindowTextW(g_ui.upd_target, widen(dir).c_str());
    if (const char *pre = getenv("HP2_SETUP_ISO")) {
        g_ui.iso = widen(pre);
        SetWindowTextW(g_ui.path, is_drive(g_ui.iso) ? (L"The disc in drive " + g_ui.iso.substr(0, 2)).c_str() : g_ui.iso.c_str());
    }
    int mode = found.empty() ? MODE_INSTALL : MODE_UPDATE;
    if (const char *m = getenv("HP2_SETUP_MODE")) mode = !strcmp(m, "update") ? MODE_UPDATE : MODE_INSTALL;
    ShowWindow(g_ui.wnd, SW_SHOW);
    UpdateWindow(g_ui.wnd);
    set_mode(mode);
    if (!g_ui.iso.empty()) check_iso();
    if (mode == MODE_UPDATE) check_update();
    if (const char *shot = getenv("HP2_SETUP_SHOT")) {
        // HP2_SETUP_SWITCH: the other of Install and Update chosen first, as a click on it does. Then time for the window to be drawn
        // and shown by the system: the picture is of what is on the screen.
        if (getenv("HP2_SETUP_SWITCH")) PostMessageW(g_ui.wnd, WM_COMMAND, mode == MODE_UPDATE ? ID_MODE_INSTALL : ID_MODE_UPDATE, 0);
        if (getenv("HP2_SETUP_DRIVE")) PostMessageW(g_ui.wnd, WM_COMMAND, ID_DRIVE, 0);        // From drive pressed
        for (int pass = 0; pass < 3; pass++) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (IsDialogMessageW(g_ui.wnd, &msg)) continue;
                DispatchMessageW(&msg);
            }
            Sleep(200);
        }
        save_shot(widen(shot));
        DestroyWindow(g_ui.wnd);
    } else if (getenv("HP2_SETUP_AUTO") && IsWindowEnabled(g_ui.install)) PostMessageW(g_ui.wnd, WM_COMMAND, ID_INSTALL, 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_ui.wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return g_ui.finished;
}

int headless(InstallSteps &steps, bool ok)
{
    fprintf(stderr, "%s%s\n", ok ? "install: done" : "install: failed: ", ok ? "" : steps.why.c_str());
    return ok ? 0 : 1;
}

}   // namespace

SetupResult setup_start()
{
    if (getenv("HP2_DATA")) return SETUP_PLAY;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // the tests: an install or the exports with no window, a line of progress now and then
    InstallSteps steps;
    int last = -1;
    steps.progress = [&](int pm, const std::wstring &) {
        if (pm / 50 != last) last = pm / 50, fprintf(stderr, "install: %d%%\n", pm / 10);
    };
    if (const char *spec = getenv("HP2_INSTALL")) {
        std::string s = spec;
        size_t bar = s.find('|');
        Iso iso;
        std::string why;
        bool ok = bar != std::string::npos && open_source(iso, widen(s.substr(0, bar)), why) && install_game(iso, widen(s.substr(bar + 1)), steps);
        if (!why.empty() && steps.why.empty()) steps.why = why;
        exit(headless(steps, ok));
    }
    if (const char *data = getenv("HP2_EXPORTS")) exit(headless(steps, install_exports(widen(data), steps)));
    if (const char *up = getenv("HP2_UPDATE")) {        // the update with no window (the tests)
        std::wstring why;
        const bool ok = is_game_folder(widen(up)) && update_folder(widen(up), why);
        fprintf(stderr, "update: %s\n", ok ? "done" : "failed");
        exit(ok ? 0 : 1);
    }

    const fs::path exe = exe_path(), dir = exe.parent_path();
    if (is_game_folder(dir)) return SETUP_PLAY;
    // Is the game installed already: looked for where it was installed last (the folder remembered), then beside this exe under the folder's
    // usual name, and under nfshp2port, the name the first releases made. The window opens either way; with a game found it opens on
    // Update with that folder in the box (and Play in place of Update when the game there is this very exe).
    std::vector<fs::path> known;
    const std::wstring remembered = remembered_folder();
    if (!remembered.empty()) known.push_back(remembered);
    known.push_back(dir / kGameFolder);
    known.push_back(dir / L"nfshp2port");
    fs::path found;
    for (const fs::path &at : known)
        if (found.empty() && is_game_folder(at)) found = at;
    if (!run_window(dir, found)) return SETUP_EXIT;
    remember_folder(g_ui.root);
    const fs::path game = fs::path(g_ui.root) / L"speed.exe";
    if (g_ui.want_shortcut && !getenv("HP2_SETUP_NOSHORTCUT")) make_shortcut(game);
    if (!start_exe(game)) MessageBoxW(nullptr, L"The game is installed but could not be started. Start speed.exe in its folder.", L"Need for Speed: Hot Pursuit 2", MB_ICONERROR);
    return SETUP_EXIT;
}

#else
SetupResult setup_start() { return SETUP_PLAY; }
#endif
