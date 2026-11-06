// The first start (setup.h): a window asks for the .iso of Need for Speed: Hot Pursuit 2 (a disc of kKnownDiscs) and where to install, reads the image
// as ISO 9660 without mounting it, checks the game's program by its SHA-1, makes the folder nfshp2port with all the game needs (install.cpp),
// puts this exe in it, and starts the game from there.
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

// ---- the window ------------------------------------------------------------------------------------
enum { ID_BROWSE = 101, ID_INSTALL, ID_CANCEL, ID_FOLDER, ID_SHORTCUT, WM_SETUP_PROGRESS = WM_APP + 1, WM_SETUP_DONE };

struct Ui {
    HWND wnd = nullptr, path = nullptr, status = nullptr, target = nullptr, bar = nullptr, step = nullptr, install = nullptr, cancel = nullptr,
         browse = nullptr, folder = nullptr, shortcut = nullptr;
    HFONT font = nullptr, heading = nullptr;
    std::wstring iso, root, failure;
    std::atomic<bool> busy{false}, stop{false};
    std::atomic<int> permille{0};
    std::wstring step_text;
    CRITICAL_SECTION lock;
    bool ok = false, finished = false, want_shortcut = true;
} g_ui;

void set_status(const std::wstring &s) { SetWindowTextW(g_ui.status, s.c_str()); }

// the image chosen: is it the game, and is there room
void check_iso()
{
    EnableWindow(g_ui.install, FALSE);
    Iso iso;
    std::string why;
    if (!iso.open(g_ui.iso, why)) return set_status(widen(why));
    std::string boot;
    const Iso::File *elf = iso.boot(boot);
    if (!elf || !iso.find("/NFSHP2/ZZDATA.BIN") || !iso.find("/NFSHP2/ZDIR.BIN") || !iso.find("/NFSHP2/IRX253/STREAM.IRX"))
        return set_status(L"This disc image is not Need for Speed: Hot Pursuit 2.");
    set_status(L"Checking the disc image...");
    UpdateWindow(g_ui.status);
    std::vector<uint8_t> buf((size_t)elf->size);
    if (!iso.read_file(*elf, 0, buf.data(), buf.size())) return set_status(L"The disc image cannot be read.");
    const std::string hex = sha1_hex(buf.data(), buf.size());
    std::string name;
    for (const KnownDisc &k : kKnownDiscs)
        if (hex == k.elf_sha1 && !_stricmp(boot.c_str(), k.boot)) name = k.name;
    if (name.empty())
        return set_status(L"This disc is not supported yet.");
    ULARGE_INTEGER free_bytes = {};
    const std::wstring drive = g_ui.root.substr(0, 3);
    if (GetDiskFreeSpaceExW(drive.c_str(), &free_bytes, nullptr, nullptr) && free_bytes.QuadPart < kNeeded)
        return set_status(widen(name) + L". Not enough free space: 3.6 GB are needed on " + drive + L".");
    set_status(widen(name) + L". Ready to install.");
    EnableWindow(g_ui.install, TRUE);
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

// where to install: the folder chosen, with nfshp2port in it (unless it is one already)
void choose_folder()
{
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(L"Choose where to install the game");
    d->SetOkButtonLabel(L"Install here");
    if (SUCCEEDED(d->Show(g_ui.wnd))) {
        IShellItem *it = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(d->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            std::wstring r = path;
            while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
            const std::wstring tail = L"nfshp2port";
            if (r.size() < tail.size() || _wcsicmp(r.c_str() + r.size() - tail.size(), tail.c_str())) r += L"\\nfshp2port";
            g_ui.root = r;
            SetWindowTextW(g_ui.target, r.c_str());
            CoTaskMemFree(path);
            if (!g_ui.iso.empty()) check_iso();
        }
        if (it) it->Release();
    }
    d->Release();
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
    bool ok = iso.open(g_ui.iso, why) && install_game(iso, g_ui.root, steps);
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
    for (HWND h : {g_ui.browse, g_ui.folder, g_ui.shortcut, g_ui.install}) EnableWindow(h, !locked);
    SetWindowTextW(g_ui.cancel, locked ? L"Cancel" : L"Exit");
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == ID_BROWSE && !g_ui.busy) browse();
        else if (LOWORD(w) == ID_FOLDER && !g_ui.busy) choose_folder();
        else if (LOWORD(w) == ID_INSTALL && !g_ui.busy) {
            g_ui.busy = true;
            g_ui.want_shortcut = SendMessageW(g_ui.shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED;
            lock_controls(true);
            ShowWindow(g_ui.bar, SW_SHOW);
            ShowWindow(g_ui.step, SW_SHOW);
            set_status(L"Installing. This takes about a minute.");
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

bool run_window(const fs::path &exe_dir)
{
    InitializeCriticalSection(&g_ui.lock);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g_ui.root = (exe_dir / L"nfshp2port").wstring();
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
    const int W = 600, H = 390;
    RECT r = {0, 0, W, H};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
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
    add(L"STATIC", L"The first start: the game's files come from your own copy.", 0, 84, 50, 490, 20);
    add(L"STATIC",
        L"Choose the disc image (.iso) of Need for Speed: Hot Pursuit 2 for PlayStation 2: USA (SLUS-20362) or Europe v1.00 (SLES-50731). "
        L"Its files are unpacked into the folder below; the disc image itself is only read.",
        0, 20, 90, W - 40, 60);
    add(L"STATIC", L"Disc image:", 0, 20, 158, 100, 20);
    g_ui.path = add(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, 20, 180, W - 40 - 110, 24);
    g_ui.browse = add(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, W - 20 - 100, 179, 100, 26, ID_BROWSE);
    g_ui.status = add(L"STATIC", L"No disc image chosen.", 0, 20, 212, W - 40, 20);
    add(L"STATIC", L"Install to:", 0, 20, 242, 100, 20);
    g_ui.target = add(L"STATIC", g_ui.root.c_str(), SS_PATHELLIPSIS, 100, 242, W - 120 - 110, 20);
    g_ui.folder = add(L"BUTTON", L"Change...", BS_PUSHBUTTON | WS_TABSTOP, W - 20 - 100, 238, 100, 26, ID_FOLDER);
    g_ui.bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD, 20, 278, W - 40, 18, g_ui.wnd, nullptr, inst, nullptr);
    SendMessageW(g_ui.bar, PBM_SETRANGE32, 0, 1000);
    g_ui.step = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_PATHELLIPSIS, 20, 300, W - 40, 20, g_ui.wnd, nullptr, inst, nullptr);
    SendMessageW(g_ui.step, WM_SETFONT, (WPARAM)g_ui.font, TRUE);
    g_ui.shortcut = add(L"BUTTON", L"Create a shortcut on the desktop", BS_AUTOCHECKBOX | WS_TABSTOP, 20, H - 42, 300, 22, ID_SHORTCUT);
    SendMessageW(g_ui.shortcut, BM_SETCHECK, BST_CHECKED, 0);
    g_ui.install = add(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_DISABLED, W - 20 - 210, H - 46, 100, 28, ID_INSTALL);
    g_ui.cancel = add(L"BUTTON", L"Exit", BS_PUSHBUTTON | WS_TABSTOP, W - 20 - 100, H - 46, 100, 28, ID_CANCEL);

    // HP2_SETUP_ISO: the image already chosen, HP2_SETUP_AUTO: and Install pressed (the tests' screenshots)
    if (const char *pre = getenv("HP2_SETUP_ISO")) {
        g_ui.iso = widen(pre);
        SetWindowTextW(g_ui.path, g_ui.iso.c_str());
    }
    ShowWindow(g_ui.wnd, SW_SHOW);
    UpdateWindow(g_ui.wnd);
    if (!g_ui.iso.empty()) check_iso();
    if (getenv("HP2_SETUP_AUTO") && IsWindowEnabled(g_ui.install)) PostMessageW(g_ui.wnd, WM_COMMAND, ID_INSTALL, 0);
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
        bool ok = bar != std::string::npos && iso.open(widen(s.substr(0, bar)), why) && install_game(iso, widen(s.substr(bar + 1)), steps);
        if (!why.empty() && steps.why.empty()) steps.why = why;
        exit(headless(steps, ok));
    }
    if (const char *data = getenv("HP2_EXPORTS")) exit(headless(steps, install_exports(widen(data), steps)));

    const fs::path exe = exe_path(), dir = exe.parent_path();
    if (is_game_folder(dir)) return SETUP_PLAY;
    // the game was installed beside this exe: it starts from its folder
    const fs::path beside = dir / L"nfshp2port";
    std::error_code ec;
    if (is_game_folder(beside) && fs::exists(beside / L"speed.exe", ec)) {
        if (!start_exe(beside / L"speed.exe")) MessageBoxW(nullptr, L"The game in nfshp2port could not be started.", L"Need for Speed: Hot Pursuit 2", MB_ICONERROR);
        return SETUP_EXIT;
    }
    if (!run_window(dir)) return SETUP_EXIT;
    const fs::path game = fs::path(g_ui.root) / L"speed.exe";
    if (g_ui.want_shortcut && !getenv("HP2_SETUP_NOSHORTCUT")) make_shortcut(game);
    if (!start_exe(game)) MessageBoxW(nullptr, L"The game is installed but could not be started. Start speed.exe in its folder.", L"Need for Speed: Hot Pursuit 2", MB_ICONERROR);
    return SETUP_EXIT;
}

#else
SetupResult setup_start() { return SETUP_PLAY; }
#endif
