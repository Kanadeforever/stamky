/*
 * stamky / manager.h
 *
 * ManagerWindow 是非模态顶层窗口，生命周期由 App 持有。它引用外部 Model/Settings，
 * 通过 onChanged_ 请求 App 重建派生运行缓存；因此 Manager 不拥有 RuntimeCache，也不自行
 * 替换进程级菜单对象。persistedModel_ 则是本窗口私有的“最后成功落盘”事务快照。
 *
 * HWND/HFONT/HIMAGELIST/WNDPROC 都属于 GUI 线程资源：创建成功后记录句柄，WM_DESTROY/析构
 * 统一清理。dragList_ 是拖拽上下文的一部分，任何拖拽结束路径都必须清零，不能默认 items_。
 */

#pragma once
#include "common.h"
#include "model.h"
#include "icon_cache.h"
#include "settings.h"
#include <functional>

namespace sm {
class ManagerWindow {
public:
    ManagerWindow(Model& model,
                  Settings& settings,
                  std::function<bool()> onChanged,
                  std::function<void()> openSettings,
                  std::function<void(const std::wstring&)> previewGroup);
    ~ManagerWindow();
    void show(HWND owner = nullptr);
    HWND hwnd() const { return hwnd_; }
    bool pretranslate(MSG& msg);
private:
    Model& model_;
    Model persistedModel_;
    Settings& settings_;
    IconCache icons_;
    std::function<bool()> onChanged_;
    std::function<void()> openSettings_;
    std::function<void(const std::wstring&)> previewGroup_;
    HWND hwnd_=nullptr, groups_=nullptr, items_=nullptr, status_=nullptr;
    HFONT font_=nullptr;
    HIMAGELIST itemImages_=nullptr;
    bool dragging_=false;
    int dragFrom_=-1;
    HWND dragList_=nullptr;    // 当前拖拽的列表（items_ 或 groups_）
    int dragInsertY_=-1;       // 插入线 Y（dragList_ 客户区坐标，-1=隐藏）
    int dragScrollDir_=0;      // 拖动自动滚动方向（-1 上 / 0 停 / 1 下）
    int dragRowHeight_=0;      // 列表行高（边缘滚动触发区计算）
    HWND dragLine_=nullptr;    // 独立插入线窗口（不参与列表滚动/重绘，杜绝残留）
    WNDPROC origItems_=nullptr; // items_ 原始窗口过程（子类化：滚轮滚动后重算插入线）

    static constexpr UINT kDragScrollTimerId=0x5201;
    static constexpr UINT kDragScrollIntervalMs=120;   // 慢速滚动：约每秒 8 行

    static LRESULT CALLBACK ListViewProc(HWND,UINT,WPARAM,LPARAM);
    void update_drag_insert(const POINT& pt);   // pt 为 items_ 客户区坐标：移动插入线窗口
    void check_drag_scroll(const POINT& pt);    // 边缘触发区检测：启动/停止拖动自动滚动

    static LRESULT CALLBACK WndProc(HWND,UINT,WPARAM,LPARAM);
    LRESULT proc(HWND,UINT,WPARAM,LPARAM);
    bool create_window(HWND owner);
    bool create_controls();
    void layout();
    void fit_list_columns_no_hscroll();
    void apply_fonts();
    void cleanup_visuals();
    int selected_group_index() const;
    Group* selected_group();
    const Group* selected_group() const;
    int selected_item_actual_index() const;
    std::vector<int> selected_item_actual_indices() const;
    void refresh_groups(int select=-1);
    void refresh_items(int selectActual=-1);
    void refresh_status(int visible=-1);
    void rebuild_item_image_list();
    void add_paths(const std::vector<std::wstring>& paths);
    void add_drop_as_group(const std::vector<std::wstring>& paths, bool singleFolder);
    void add_files();
    void new_custom_item();
    void add_folder();
    void scan_folder();
    void add_subgroup(const std::wstring& parentGroupId, const std::wstring& targetGroupId);
    void delete_items();
    void edit_item();
    void refresh_item_icon();
    void launch_selected();
    void open_selected_location();
    void show_selected_shell_properties();
    void move_item(int delta);
    void move_group(int delta);
    void new_group();
    void rename_group();
    void delete_group();
    void make_shortcut();
    void preview_group();
    void show_item_context_menu(POINT screen);
    void show_group_context_menu(POINT screen);
    bool commit();
    void update_action_states();
    void show_add_menu();
};
}
