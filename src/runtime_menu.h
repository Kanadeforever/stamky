/*
 * stamky / runtime_menu.h
 *
 * RuntimeMenu 只引用 RuntimeCache/Settings，不拥有它们。show() 是同步调用，但内部
 * TrackPopupMenuEx 会重入消息泵，所以公开的 is_showing() 是 App 判断能否替换缓存的硬约束。
 *
 * DrawEntry 与 HMENU owner-draw itemData 一一对应：只要 HMENU 仍可能触发 MEASURE/DRAW，
 * drawEntries_ 就必须存活。PendingShow 把重入请求复制成值对象，避免保存调用方临时字符串地址。
 * bitmaps_ 是跨多次菜单显示的尺寸化 GDI 缓存，并以 trim_bitmaps 限制资源数量。
 */

#pragma once
#include "common.h"
#include "runtime_cache.h"
#include "settings.h"
#include <memory>

namespace sm {
enum class MenuAnchorMode {
    Cursor,
    TaskbarAware
};

class RuntimeMenu {
public:
    RuntimeMenu(RuntimeCache& cache, Settings& settings):cache_(cache),settings_(settings){}
    ~RuntimeMenu();

    bool show(HWND owner,
              const std::wstring& groupId,
              MenuAnchorMode anchorMode = MenuAnchorMode::Cursor,
              std::optional<POINT> invocationPoint = std::nullopt);
    void clear_bitmaps();
    double last_prepare_ms() const { return lastPrepareMs_; }
    bool is_showing() const { return showing_; }

    bool handle_measure_item(MEASUREITEMSTRUCT* mi);
    bool handle_draw_item(DRAWITEMSTRUCT* di);
    bool handle_init_menu_popup(HMENU menu);
    bool handle_menu_rbutton_up(UINT itemIndex, HMENU menu, HWND owner);

private:
    struct BmpEntry {
        HBITMAP bmp=nullptr;
        int sourceSize=0;
        uint64_t tick=0;
    };
    struct DrawEntry {
        std::wstring text;
        std::wstring shellPath;
        ItemType type=ItemType::Program;
        HBITMAP bmp=nullptr;
        HICON itemIcon=nullptr;      // 运行时提取的专用图标（展开文件夹子项），菜单生命周期释放
        HICON fallbackIcon=nullptr;
        int sourceSize=0;
        bool submenu=false;
        bool disabled=false;
        bool columnStart=false;
        int width=0;
    };
    struct PendingSubmenu {
        std::wstring source;   // 子组 id 或文件夹路径
        int depth=0;
        bool isFolder=false;   // true=展开文件夹目录，false=展开子组
    };
    struct LaunchSpec {
        std::wstring path,arguments,workingDirectory;
        ItemType type=ItemType::Program;
    };
    struct PopupAnchor {
        POINT pt{};
        UINT flags=0;
    };
    struct PendingShow {
        HWND owner=nullptr;
        std::wstring groupId;
        MenuAnchorMode anchorMode=MenuAnchorMode::Cursor;
        std::optional<POINT> invocationPoint;
    };

    RuntimeCache& cache_;
    Settings& settings_;
    std::unordered_map<std::wstring,BmpEntry> bitmaps_;
    std::unordered_map<HMENU,PendingSubmenu> pendingSubmenus_;
    std::unordered_map<UINT,LaunchSpec> commands_;
    std::vector<std::unique_ptr<DrawEntry>> drawEntries_;
    uint64_t tick_=0;
    double lastPrepareMs_=0.0;
    HFONT menuFont_=nullptr;
    HICON genericAppIcon_=nullptr, folderIcon_=nullptr, fileIcon_=nullptr;
    UINT activeDpi_=96;
    int iconPx_=28,rowHeight_=34,hPad_=10,vPad_=4,arrowPx_=14;
    RECT workArea_{};
    RECT monitorArea_{};
    UINT nextCommand_=1000;
    bool showing_=false;
    bool clearBitmapsPending_=false;
    std::optional<PendingShow> pendingShow_;

    BmpEntry bitmap_for(const RuntimeItemView& item,int requested);
    void trim_bitmaps(size_t maxCount=128);
    DrawEntry* make_draw_entry(RuntimeItemView& item,bool columnStart,bool disabled=false);
    DrawEntry* make_text_entry(const std::wstring& text,bool disabled=false);
    int text_width(const std::wstring& text) const;
    int item_width(const RuntimeItemView& item) const;
    int choose_rows(const std::vector<RuntimeItemView>& items) const;
    int total_width_for_rows(const std::vector<RuntimeItemView>& items,int rows) const;
    bool build_group_menu(HMENU menu,const std::wstring&groupId,int depth);
    // 共享构建：把 items 按当前多列算法排布为 owner-draw 子菜单。
    bool build_items_menu(HMENU menu,std::vector<RuntimeItemView>& items,int depth);
    // 实时枚举文件夹：只在用户主动展开时读取目录并提取最多 200 个 Shell 图标；
    // 根菜单热路径不做目录枚举，因此不把这类 I/O 带入常规弹出延迟。
    bool build_folder_menu(HMENU menu,const std::wstring& path,int depth);
    void prepare_visuals(HWND owner,POINT pt);
    PopupAnchor popup_anchor(POINT invocationPoint,MenuAnchorMode mode) const;
    bool show_once(const PendingShow& request);
    void reset_active_state();
};
}
