/*
 * stamky / settings_window.h
 *
 * settings_ 是 App 持有的已提交配置；draft_ 是当前窗口会话的编辑副本。两者不能在用户按 OK
 * 之前共享可变容器，否则 Cancel 无法提供真正事务语义。languageCodes_ 缓存 ComboBox 行与语言
 * 代码的稳定对应，避免重新枚举目录后索引错配。
 */

#pragma once
#include "common.h"
#include "settings.h"

namespace sm {
class SettingsWindow {
public:
    explicit SettingsWindow(Settings& settings):settings_(settings),draft_(settings){}
    ~SettingsWindow();
    void show(HWND owner=nullptr);
    bool pretranslate(MSG& msg);
private:
    Settings& settings_;
    Settings draft_;
    std::vector<std::wstring> languageCodes_;
    HWND hwnd_=nullptr;
    HWND preview_=nullptr;   // 实时预览区（模拟菜单项：项目图标+文字）
    HFONT font_=nullptr,titleFont_=nullptr;
    static LRESULT CALLBACK WndProc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK PreviewWndProc(HWND,UINT,WPARAM,LPARAM);
    LRESULT proc(HWND,UINT,WPARAM,LPARAM);
    void draw_preview(HDC dc,HWND hwnd) const;
    bool create(HWND owner);
    void apply_font();
    void layout();
    void load_controls();
    bool save_controls();
};
}
