/*
 * stamky / common.cpp
 *
 * 跨模块基础设施：可执行目录解析、UTF-8/UTF-16 严格转换、GUID、TSV 转义、文件名清理、
 * DPI/字体换算以及启动诊断日志。这里的失败语义刻意偏保守：无法确认 exe 所在目录时返回
 * 空路径，调用方应停止持久化，而不是退化到“当前工作目录”写入用户数据。
 *
 * 字符编码：groups.tsv 与语言文件以 UTF-8 为外部文本边界；转换使用 MB_ERR_INVALID_CHARS /
 * WC_ERR_INVALID_CHARS，非法输入不得悄悄替换成问号后继续保存，否则会把一次读取错误固化为
 * 永久数据损坏。
 *
 * DPI：UI 尺寸以 96 DPI 的逻辑单位表达，窗口已有 Per-Monitor DPI 时优先 GetDpiForWindow；
 * 旧系统/失败路径再回退到设备上下文。返回 96 是最后的安全兜底，而不是把物理像素硬编码
 * 到各个窗口模块。
 */

#include "common.h"
#include "lang.h"
#include <atomic>
#include <fstream>
#include <sstream>

namespace sm {
/* STAMKY_CN_DETAIL
 * 可执行目录是全部便携数据路径的根。GetModuleFileNameW 在“缓冲区恰好不够”时并不会提供
 * 一个可安全继续拼接的绝对路径，所以这里把失败/疑似截断统一视为“没有根目录”。调用者随后
 * 必须显式失败，不能退化到当前工作目录，否则从快捷方式、计划任务或 Shell 启动时可能把 data/cache
 * 写进完全不同的位置。这里刻意不使用 current_path() 作为兜底。
 */
std::filesystem::path exe_dir() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) return {};
    buf.resize(n);
    return std::filesystem::path(buf).parent_path();
}
std::filesystem::path data_dir() { const auto root=exe_dir(); return root.empty()?std::filesystem::path{}:root/L"data"; }
std::filesystem::path cache_dir() { const auto root=exe_dir(); return root.empty()?std::filesystem::path{}:root/L"cache"; }
void ensure_directories() {
    const auto root = exe_dir();
    if (root.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(root / L"data", ec);
    ec.clear();
    std::filesystem::create_directories(root / L"cache" / L"icons", ec);
    ec.clear();
    std::filesystem::create_directories(root / L"GroupShortCuts", ec);
}
/* STAMKY_CN_DETAIL
 * UTF-8 -> UTF-16 使用 MB_ERR_INVALID_CHARS 做严格转换：非法字节序列不是“尽量替换后继续”，
 * 而是返回空结果让上层判定配置/缓存损坏。先查询所需 wchar 数，再按精确长度转换，可避免固定缓冲
 * 截断；空输入与转换失败由调用语境区分。groups.tsv/lang 等持久文件都依赖这条严格语义。
 */
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    if (s.size() > static_cast<size_t>(INT_MAX)) return {};
    const int input = static_cast<int>(s.size());
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), input, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), input, out.data(), n) != n) return {};
    return out;
}
std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return {};
    if (s.size() > static_cast<size_t>(INT_MAX)) return {};
    const int input = static_cast<int>(s.size());
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), input, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), input, out.data(), n, nullptr, nullptr) != n) return {};
    return out;
}
std::wstring guid_string() {
    GUID g{};
    if (SUCCEEDED(CoCreateGuid(&g))) {
        wchar_t buf[64]{};
        if (StringFromGUID2(g, buf, static_cast<int>(_countof(buf))) > 0) return buf;
    }

    static std::atomic<unsigned long long> seq{0};
    const ULONGLONG tick = GetTickCount64();
    const unsigned long long serial = ++seq;
    wchar_t tmp[96]{};
    swprintf_s(tmp, L"{%08lX-%08lX-%016llX-%016llX}",
               GetCurrentProcessId(), GetCurrentThreadId(), tick, serial);
    return tmp;
}
std::wstring file_display_name(const std::filesystem::path& p) {
    auto name = p.filename().wstring();
    if (p.has_extension()) name = p.stem().wstring();
    return name.empty() ? p.wstring() : name;
}
std::wstring lower_copy(std::wstring s) {
    if (!s.empty() && s.size() <= static_cast<size_t>(MAXDWORD))
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::wstring tsv_escape(const std::wstring& s) {
    std::wstring out; out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'\\': out += L"\\\\"; break;
        case L'\t': out += L"\\t"; break;
        case L'\r': out += L"\\r"; break;
        case L'\n': out += L"\\n"; break;
        default: out += c; break;
        }
    }
    return out;
}
std::wstring tsv_unescape(const std::wstring& s) {
    std::wstring out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            if (n == L't') out += L'\t';
            else if (n == L'r') out += L'\r';
            else if (n == L'n') out += L'\n';
            else if (n == L'\\') out += L'\\';
            else { out += L'\\'; out += n; }
        } else out += s[i];
    }
    return out;
}
std::vector<std::wstring> split_tab(const std::wstring& line) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true) {
        const size_t p = line.find(L'\t', start);
        parts.push_back(tsv_unescape(line.substr(start, p == std::wstring::npos ? p : p - start)));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    return parts;
}
bool path_exists(const std::wstring& p) {
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}
/* STAMKY_CN_DETAIL
 * 该函数只负责生成“可作为 Windows 文件名组件”的显示名称，不负责路径规范化。除了 Win32 禁止字符，
 * 还处理控制字符、末尾空格/句点以及 CON/PRN/AUX/NUL/COM1..9/LPT1..9 等设备名；最后限制长度，
 * 为 .lnk 后缀和目录前缀保留余量。这样 Group 名称再奇怪，也不会直接变成不可创建的快捷方式名。
 */
std::wstring safe_filename(std::wstring s) {
    static const wchar_t* bad = L"\\/:*?\"<>|";
    for(auto& c:s)if(c<32||wcschr(bad,c))c=L'_';
    while(!s.empty()&&(s.back()==L' '||s.back()==L'.'))s.pop_back();
    if(s.empty())s=tr(L"未命名");
    // Win32/DOS 设备名即使追加扩展名也可能被解析成设备；组名必须显式避开。
    const auto dot=s.find(L'.');
    const std::wstring stem=lower_copy(s.substr(0,dot));
    static const wchar_t* reserved[]={L"con",L"prn",L"aux",L"nul",
        L"com1",L"com2",L"com3",L"com4",L"com5",L"com6",L"com7",L"com8",L"com9",
        L"lpt1",L"lpt2",L"lpt3",L"lpt4",L"lpt5",L"lpt6",L"lpt7",L"lpt8",L"lpt9"};
    for(const wchar_t* name:reserved)if(stem==name){s.insert(s.begin(),L'_');break;}
    // 给“.lnk”与临时后缀留出余量，避免极长组名把快捷方式路径推到 ShellLink 边界。
    constexpr size_t kMaxSafeName=120;
    if(s.size()>kMaxSafeName)s.resize(kMaxSafeName);
    while(!s.empty()&&(s.back()==L' '||s.back()==L'.'))s.pop_back();
    return s.empty()?L"_":s;
}
int fit_text_width(HWND ctrl, HFONT font, int minW, int maxW) {
    if (!ctrl) return minW;
    if (maxW < minW) maxW = minW;
    const int textLen = GetWindowTextLengthW(ctrl);
    // 这个函数只用于给按钮/标签估算合适宽度。异常长的第三方翻译没有必要为了
    // “精确测量”分配巨大缓冲区：宽度最终也只会被钳制到 maxW。
    if (textLen > 32767) return maxW;
    std::wstring text(static_cast<size_t>(std::max(textLen, 0)) + 1, L'\0');
    const int n = textLen > 0 ? GetWindowTextW(ctrl, text.data(), static_cast<int>(text.size())) : 0;
    HDC dc = GetDC(ctrl);
    if (!dc) return minW;
    HFONT old = font ? static_cast<HFONT>(SelectObject(dc, font)) : nullptr;
    SIZE sz{};
    if (n > 0) GetTextExtentPoint32W(dc, text.data(), n, &sz);
    if (old) SelectObject(dc, old);
    ReleaseDC(ctrl, dc);
    return std::clamp(static_cast<int>(sz.cx) + MulDiv(16, static_cast<int>(dpi_for_window(ctrl)), 96), minW, maxW);
}

const std::vector<std::wstring>& default_launch_extensions() {
    static const std::vector<std::wstring> list = {L"lnk", L"url", L"exe", L"com", L"bat", L"cmd", L"appref-ms"};
    return list;
}
/* STAMKY_CN_DETAIL
 * DPI 查询按“现代 API -> 兼容回退”组织。GetDpiForWindow 只在运行系统提供时动态解析，避免链接期
 * 强依赖；失败后再使用窗口 DC/LOGPIXELSX，最终兜底 96 DPI。返回值永远至少为 96，供 dip()/字体
 * 创建和 UI-06 布局统一换算，避免局部代码各自猜测 DPI。
 */
UINT dpi_for_window(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn p = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (p && hwnd) {
        const UINT dpi = p(hwnd);
        if (dpi) return dpi;
    }
    HWND dcOwner = hwnd;
    HDC dc = GetDC(dcOwner);
    if (!dc) { dcOwner = nullptr; dc = GetDC(nullptr); }
    if (!dc) return 96;
    const int x = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(dcOwner, dc);
    return x > 0 ? static_cast<UINT>(x) : 96;
}
UINT dpi_for_monitor(HMONITOR monitor) {
    if (!monitor) return 0;
    using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static HMODULE shcore = []() -> HMODULE {
        HMODULE h = GetModuleHandleW(L"shcore.dll");
        return h ? h : LoadLibraryW(L"shcore.dll");
    }();
    static Fn p = shcore ? reinterpret_cast<Fn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
    UINT x = 96, y = 96;
    if (p && SUCCEEDED(p(monitor, 0, &x, &y)) && x) return x;
    return 0;
}
int dip(HWND hwnd, int logical) { return MulDiv(logical, static_cast<int>(dpi_for_window(hwnd)), 96); }
HFONT create_ui_font(UINT dpi, int points) {
    dpi = dpi ? dpi : 96;
    return CreateFontW(-MulDiv(points, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}
void reset_startup_log() {
    ensure_directories();
    const auto dir = data_dir();
    if (dir.empty()) return;
    std::ofstream f(dir / L"启动诊断.log", std::ios::binary | std::ios::trunc);
    if (f) {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        f.write(reinterpret_cast<const char*>(bom), 3);
    }
}
void startup_log(const std::wstring& text) {
    ensure_directories();
    const auto dir = data_dir();
    if (dir.empty()) return;
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::ofstream f(dir / L"启动诊断.log", std::ios::binary | std::ios::app);
    if (!f) return;
    const auto u = wide_to_utf8(std::wstring(prefix) + text + L"\r\n");
    if (!u.empty()) f.write(u.data(), static_cast<std::streamsize>(u.size()));
}
std::wstring win32_error_text(DWORD error) {
    if (!error) return L"0";
    wchar_t* raw = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    std::wstring msg = (n && raw) ? std::wstring(raw, n) : L"";
    if (raw) LocalFree(raw);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) msg.pop_back();
    return std::to_wstring(error) + (msg.empty() ? L"" : L" (" + msg + L")");
}

}
