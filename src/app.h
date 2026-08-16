/*
 * stamky / app.h
 *
 * App 拥有进程级长期对象，并规定它们的销毁顺序：Host HWND -> RuntimeMenu/Manager/SettingsWindow
 * 的交互入口 -> RuntimeCache 文件映射 -> Model/Settings。成员之间不是独立工具类；特别是
 * RuntimeMenu 持有 RuntimeCache/Settings 的引用，因此它们必须比 menu_ 活得更久。
 *
 * run() 是主消息泵入口；forward_to_existing() 是第二实例的轻量转发入口。所有 UI 操作都在
 * 创建这些窗口的同一 GUI 线程执行，代码没有把 HWND/HMENU/HBITMAP 跨线程共享。
 */

#pragma once
#include "common.h"
#include "model.h"
#include "settings.h"
#include "runtime_cache.h"
#include "runtime_menu.h"
#include "manager.h"
#include "settings_window.h"
#include <memory>
namespace sm {
class App {
public:
    int run(const std::wstring& initialCommand);
    static bool forward_to_existing(const std::wstring& command);
private:
    HWND hwnd_=nullptr; NOTIFYICONDATAW nid_{}; UINT taskbarCreatedMsg_=0; Model model_; Settings settings_; RuntimeCache cache_; RuntimeCacheBuilder builder_; std::unique_ptr<RuntimeMenu> menu_; std::unique_ptr<ManagerWindow> manager_; std::unique_ptr<SettingsWindow> settingsWindow_;
    static LRESULT CALLBACK WndProc(HWND,UINT,WPARAM,LPARAM); LRESULT proc(HWND,UINT,WPARAM,LPARAM); bool create_host(); void add_tray(); void remove_tray(); void show_tray_menu(std::optional<POINT> anchor=std::nullopt); void execute_command(const std::wstring&); void open_manager(); void open_settings(); bool rebuild_runtime(); void show_status();
};
}
