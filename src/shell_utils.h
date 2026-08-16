/*
 * stamky / shell_utils.h
 *
 * Shell 边界函数全部在调用线程同步执行，调用期间可能出现 COM/菜单嵌套消息循环。返回 vector/
 * optional/string 都是拥有数据的值对象，不向外暴露 PIDL、IShellItem、IContextMenu 等 COM 指针。
 * ShellContextMenuResult 区分“构建失败 / 用户取消 / 已执行”，避免把取消误报成错误。
 */

#pragma once
#include "common.h"
#include "model.h"

namespace sm {
enum class ShellContextMenuResult {
    Failed,
    Dismissed,
    Invoked
};

// exts 为空时使用内置默认全集（default_launch_extensions）。
std::vector<std::wstring> pick_files(HWND owner, const std::vector<std::wstring>& exts = {});
std::optional<std::wstring> pick_folder(HWND owner);
bool create_app_shortcut(const std::wstring& linkPath, const std::wstring& arguments, const std::wstring& description, const std::wstring& appUserModelId = L"");
bool create_group_shortcut(const Group& group, std::wstring* outPath = nullptr);
size_t migrate_legacy_group_shortcuts(const Model& model);
void launch_path(const std::wstring& path);
void launch_item(const Item& item);
void open_containing_folder(const std::wstring& path);
void show_file_properties(HWND owner, const std::wstring& path);

// Runtime 菜单项右键时显示该 .lnk/.exe/文件的 Windows Shell 原生上下文菜单。
ShellContextMenuResult show_shell_context_menu(HWND owner, const std::wstring& path, POINT screenPoint);
// IContextMenu2/3 扩展需要 owner 转发若干菜单消息；有活动 Shell 菜单时返回 true。
bool handle_shell_context_menu_message(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* result);
// 为可见顶层窗口设置独立 AppUserModelID，避免内容管理窗口占用某个组的任务栏入口。
bool set_window_app_user_model_id(HWND hwnd, const std::wstring& appUserModelId);
}
