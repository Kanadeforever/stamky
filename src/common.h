/*
 * stamky / common.h
 *
 * Win32 公共边界与无状态辅助函数声明。所有模块经由本头文件共享一致的 Windows 头文件宏、
 * Unicode 数据类型和 DPI/路径/编码约定，避免不同翻译单元各自定义一套不兼容的规则。
 *
 * 正式支持范围以 Windows 10+ 为构建/测试目标，所以 _WIN32_WINNT/_WIN32_IE 使用现代接口级别；
 * 这不等于程序逻辑“硬性只能”运行在 Windows 10。旧 Windows 的非官方移植需要自行调整目标宏、
 * API 回退和工具链，并重新测试，不能仅修改 manifest 就宣称兼容。
 */

#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// _WIN32_IE 与 _WIN32_WINNT=0x0A00 对齐：避免 commctrl 等头文件裁剪现代接口
// （如 SHCreateItemFromParsingName 需要 >= 0x0700）。
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <utility>

constexpr int IDI_STAMKY = 101;

namespace sm {
std::filesystem::path exe_dir();
std::filesystem::path data_dir();
std::filesystem::path cache_dir();
std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& s);
std::wstring guid_string();
std::wstring file_display_name(const std::filesystem::path& p);
std::wstring lower_copy(std::wstring s);
std::wstring tsv_escape(const std::wstring& s);
std::wstring tsv_unescape(const std::wstring& s);
std::vector<std::wstring> split_tab(const std::wstring& line);
bool path_exists(const std::wstring& p);
std::wstring safe_filename(std::wstring s);
// 按控件当前文本测量所需宽度（不同语言文本长度差异自适应，防截断/超行）。
// 返回 clamp(文本宽 + 内边距, minW, maxW)；font 为空时用控件默认字体。
int fit_text_width(HWND ctrl, HFONT font, int minW, int maxW = 400);
// 内置默认可启动扩展名全集（小写无点）：供 Settings 默认值、
// 文件选择器过滤、扫描导入过滤、主菜单文件夹展开过滤共用。
const std::vector<std::wstring>& default_launch_extensions();
UINT dpi_for_window(HWND hwnd);
UINT dpi_for_monitor(HMONITOR monitor);
int dip(HWND hwnd, int logical);
HFONT create_ui_font(UINT dpi, int points = 9);
void ensure_directories();
void reset_startup_log();
void startup_log(const std::wstring& text);
std::wstring win32_error_text(DWORD error);
}
