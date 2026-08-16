/*
 * stamky / runtime_menu.cpp
 *
 * 运行菜单热路径与 owner-draw 实现。根/子组数据来自 memory-mapped runtime.bin；文件夹仅在
 * 用户主动展开时实时枚举。菜单根据工作区、DPI、字体与项目宽度选择多列行数，尽量保持
 * UI-06 的紧凑布局，同时把所有最终几何值限制在 Win32 int 范围内。
 *
 * 最关键约束是 TrackPopupMenuEx 的嵌套消息循环：showing_ 为 true 时，窗口过程仍可能收到
 * 第二次 SHOW、重建缓存、WM_INITMENUPOPUP、WM_MEASUREITEM、WM_DRAWITEM 或右键 Shell 菜单消息。
 * 因此“再次 show”只写 pendingShow_ 并 EndMenu；真正释放 DrawEntry/HBITMAP/HICON/HFONT、
 * 关闭/替换缓存必须等外层 TrackPopupMenuEx 返回。clearBitmapsPending_ 同理延迟清理。
 *
 * GDI/Shell 所有权：缓存像素创建的 HBITMAP 由 bitmaps_ 持有；DrawEntry 的 itemIcon 只接管
 * 文件夹临时枚举产生的 HICON；stock fallback 图标由 RuntimeMenu 长期持有。make_draw_entry
 * 完成所有权转移后会把 RuntimeItemView::hIcon 置空，失败路径再销毁未转移图标，防止双释放/泄漏。
 *
 * 子菜单延迟构建：PendingSubmenu 只保存稳定的组 ID 或文件夹路径。WM_INITMENUPOPUP 到来时才填充，
 * 并限制递归深度和文件夹项目数。文件夹枚举若中途失败，会显示“读取中断”而不是把部分结果
 * 冒充完整目录。
 */

#include "runtime_menu.h"
#include "lang.h"
#include "shell_utils.h"
#include <cmath>
#include <wingdi.h>

namespace sm {
namespace {

enum class TaskbarEdge { None, Left, Top, Right, Bottom };

TaskbarEdge edge_from_rect(const RECT& monitor, const RECT& taskbar) {
    const int width=taskbar.right-taskbar.left;
    const int height=taskbar.bottom-taskbar.top;
    if(width>=height){
        const int distTop=std::abs(taskbar.top-monitor.top);
        const int distBottom=std::abs(monitor.bottom-taskbar.bottom);
        return distTop<distBottom?TaskbarEdge::Top:TaskbarEdge::Bottom;
    }
    const int distLeft=std::abs(taskbar.left-monitor.left);
    const int distRight=std::abs(monitor.right-taskbar.right);
    return distLeft<distRight?TaskbarEdge::Left:TaskbarEdge::Right;
}

// 提取路径对应的 Shell 图标（32px 大图标）。.lnk/.url 会由 Shell 解析出目标图标，
// 这正是"显示子项自己的图标"所需；系统图标缓存使重复提取很快。
// 仅在用户主动展开文件夹时调用，根菜单弹出热路径不经过这里。
HICON icon_for_path(const std::wstring& path) {
    if (path.empty()) return nullptr;
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                        SHGFI_ICON | SHGFI_LARGEICON) || !sfi.hIcon)
        return nullptr;
    return sfi.hIcon;
}

bool taskbar_under_point(POINT pt, HMONITOR monitor, RECT& taskbarRect, TaskbarEdge& edge) {
    HWND h = WindowFromPoint(pt);
    HWND root = h ? GetAncestor(h, GA_ROOT) : nullptr;
    if (!root) return false;

    wchar_t cls[128]{};
    GetClassNameW(root, cls, static_cast<int>(_countof(cls)));
    if (_wcsicmp(cls, L"Shell_TrayWnd") != 0 && _wcsicmp(cls, L"Shell_SecondaryTrayWnd") != 0) return false;

    HMONITOR rootMon = MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST);
    if (rootMon != monitor || !GetWindowRect(root, &taskbarRect)) return false;

    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi)) return false;
    edge = edge_from_rect(mi.rcMonitor, taskbarRect);
    return true;
}

TaskbarEdge edge_from_work_area(const RECT& monitor, const RECT& work) {
    const int left = work.left - monitor.left;
    const int top = work.top - monitor.top;
    const int right = monitor.right - work.right;
    const int bottom = monitor.bottom - work.bottom;
    const int best = std::max({left, top, right, bottom});
    if (best <= 0) return TaskbarEdge::None;
    if (best == left) return TaskbarEdge::Left;
    if (best == top) return TaskbarEdge::Top;
    if (best == right) return TaskbarEdge::Right;
    return TaskbarEdge::Bottom;
}

std::vector<uint8_t> resize_pbgra_bilinear(const uint8_t* src, int sw, int sh, int dw, int dh) {
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return {};
    const uint64_t bytes=static_cast<uint64_t>(dw)*static_cast<uint64_t>(dh)*4ull;
    if(bytes>static_cast<uint64_t>(std::numeric_limits<size_t>::max())||bytes>64ull*1024ull*1024ull)return {};
    std::vector<uint8_t> out(static_cast<size_t>(bytes));
    if (sw == dw && sh == dh) {
        std::memcpy(out.data(), src, out.size());
        return out;
    }

    for (int y = 0; y < dh; ++y) {
        const double sy = ((static_cast<double>(y) + 0.5) * sh / dh) - 0.5;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, sh - 1);
        const int y1 = std::min(y0 + 1, sh - 1);
        const double fy = std::clamp(sy - std::floor(sy), 0.0, 1.0);
        for (int x = 0; x < dw; ++x) {
            const double sx = ((static_cast<double>(x) + 0.5) * sw / dw) - 0.5;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, sw - 1);
            const int x1 = std::min(x0 + 1, sw - 1);
            const double fx = std::clamp(sx - std::floor(sx), 0.0, 1.0);
            const uint8_t* p00 = src + (static_cast<size_t>(y0) * sw + x0) * 4;
            const uint8_t* p10 = src + (static_cast<size_t>(y0) * sw + x1) * 4;
            const uint8_t* p01 = src + (static_cast<size_t>(y1) * sw + x0) * 4;
            const uint8_t* p11 = src + (static_cast<size_t>(y1) * sw + x1) * 4;
            uint8_t* d = out.data() + (static_cast<size_t>(y) * dw + x) * 4;
            for (int c = 0; c < 4; ++c) {
                const double top = p00[c] + (p10[c] - p00[c]) * fx;
                const double bottom = p01[c] + (p11[c] - p01[c]) * fx;
                d[c] = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(top + (bottom - top) * fy)), 0, 255));
            }
        }
    }
    return out;
}




}

RuntimeMenu::~RuntimeMenu(){reset_active_state();clear_bitmaps();}

void RuntimeMenu::clear_bitmaps(){
    if(showing_){clearBitmapsPending_=true;return;}
    for(auto&[k,v]:bitmaps_) if(v.bmp) DeleteObject(v.bmp);
    bitmaps_.clear();
    clearBitmapsPending_=false;
}

void RuntimeMenu::reset_active_state(){
    pendingSubmenus_.clear();
    commands_.clear();
    for(auto& d:drawEntries_) if(d->itemIcon){DestroyIcon(d->itemIcon);d->itemIcon=nullptr;}
    drawEntries_.clear();
    nextCommand_=1000;
    if(menuFont_){DeleteObject(menuFont_);menuFont_=nullptr;}
    if(genericAppIcon_){DestroyIcon(genericAppIcon_);genericAppIcon_=nullptr;}
    if(folderIcon_){DestroyIcon(folderIcon_);folderIcon_=nullptr;}
    if(fileIcon_){DestroyIcon(fileIcon_);fileIcon_=nullptr;}
}

RuntimeMenu::BmpEntry RuntimeMenu::bitmap_for(const RuntimeItemView& item,int requested){
    if(item.icons.empty()) return {};

    const RuntimeIconView* best=&item.icons[0];
    int distance=std::abs(static_cast<int>(best->size)-requested);
    for(const auto& ic:item.icons){
        const int nd=std::abs(static_cast<int>(ic.size)-requested);
        if(nd<distance){best=&ic;distance=nd;}
    }

    // 缓存键使用最终绘制尺寸，而不是源图尺寸。这样同一个源图被缩放到不同 DPI/设置时不会互相覆盖。
    const std::wstring key=item.id+L"@"+std::to_wstring(requested);
    auto it=bitmaps_.find(key);
    if(it!=bitmaps_.end()){
        it->second.tick=++tick_;
        return it->second;
    }

    const int sourceSize=static_cast<int>(best->size);
    const uint64_t requiredBytes=sourceSize>0?static_cast<uint64_t>(sourceSize)*static_cast<uint64_t>(sourceSize)*4ull:0ull;
    if(sourceSize<=0 || requiredBytes>std::numeric_limits<uint32_t>::max() || best->bytes<requiredBytes) return {};

    // IconCache 保证输入为 premultiplied BGRA。这里在内存中做一次很小的双线性缩放，
    // 生成与最终菜单图标完全相同尺寸的 PBGRA DIB，后续 AlphaBlend 只做 1:1 合成。
    // 避免 GDI AlphaBlend 自带缩放在圆形/斜边图标上形成毛刺或暗边。
    std::vector<uint8_t> pixels = resize_pbgra_bilinear(best->pixels,sourceSize,sourceSize,requested,requested);
    const uint64_t expectedPixels=static_cast<uint64_t>(requested)*static_cast<uint64_t>(requested)*4ull;
    if(expectedPixels>static_cast<uint64_t>(std::numeric_limits<size_t>::max())||pixels.size()!=static_cast<size_t>(expectedPixels))return {};

    BITMAPINFO bi{};
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=requested;
    bi.bmiHeader.biHeight=-requested;
    bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32;
    bi.bmiHeader.biCompression=BI_RGB;

    void* bits=nullptr;
    HDC dc=GetDC(nullptr);
    if(!dc)return {};
    HBITMAP hb=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    ReleaseDC(nullptr,dc);
    if(!hb||!bits){if(hb)DeleteObject(hb);return {};}
    std::memcpy(bits,pixels.data(),pixels.size());

    BmpEntry entry{hb,requested,++tick_};
    bitmaps_[key]=entry;
    return entry;
}

void RuntimeMenu::trim_bitmaps(size_t maxCount){
    while(bitmaps_.size()>maxCount){
        auto victim=bitmaps_.end();
        for(auto it=bitmaps_.begin();it!=bitmaps_.end();++it)
            if(victim==bitmaps_.end()||it->second.tick<victim->second.tick) victim=it;
        if(victim==bitmaps_.end()) break;
        if(victim->second.bmp) DeleteObject(victim->second.bmp);
        bitmaps_.erase(victim);
    }
}

void RuntimeMenu::prepare_visuals(HWND owner,POINT pt){
    HMONITOR mon=MonitorFromPoint(pt,MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if(GetMonitorInfoW(mon,&mi)){
        workArea_=mi.rcWork;
        monitorArea_=mi.rcMonitor;
    }else{
        RECT fallback{};
        if(!SystemParametersInfoW(SPI_GETWORKAREA,0,&fallback,0)) fallback={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
        workArea_=fallback;
        monitorArea_=fallback;
    }
    if(workArea_.right<=workArea_.left||workArea_.bottom<=workArea_.top){
        workArea_={0,0,std::max(1,GetSystemMetrics(SM_CXSCREEN)),std::max(1,GetSystemMetrics(SM_CYSCREEN))};
    }
    if(monitorArea_.right<=monitorArea_.left||monitorArea_.bottom<=monitorArea_.top)monitorArea_=workArea_;

    activeDpi_=dpi_for_monitor(mon);
    if(!activeDpi_){
        UINT wdpi=dpi_for_window(owner);
        activeDpi_=wdpi?wdpi:96;
    }
    menuFont_=create_ui_font(activeDpi_,settings_.menuFontPoints);
    iconPx_=std::clamp(MulDiv(settings_.iconLogicalSize,static_cast<int>(activeDpi_),96),16,128);
    hPad_=std::max(6,MulDiv(10,static_cast<int>(activeDpi_),96));
    vPad_=std::max(2,MulDiv(4,static_cast<int>(activeDpi_),96));
    arrowPx_=std::max(10,MulDiv(14,static_cast<int>(activeDpi_),96));

    TEXTMETRICW tm{};
    HDC dc=GetDC(owner);
    if(dc){
        HFONT old=menuFont_?static_cast<HFONT>(SelectObject(dc,menuFont_)):nullptr;
        GetTextMetricsW(dc,&tm);
        if(old)SelectObject(dc,old);
        ReleaseDC(owner,dc);
    }
    const int textHeightPx=tm.tmHeight>0?static_cast<int>(tm.tmHeight):std::max(1,MulDiv(settings_.menuFontPoints,static_cast<int>(activeDpi_),72));
    rowHeight_=std::max(iconPx_+vPad_*2,textHeightPx+vPad_*2);

    auto loadStock=[&](SHSTOCKICONID id)->HICON{
        SHSTOCKICONINFO si{sizeof(si)};
        return SUCCEEDED(SHGetStockIconInfo(id,SHGSI_ICON|SHGSI_LARGEICON,&si))?si.hIcon:nullptr;
    };
    genericAppIcon_=loadStock(SIID_APPLICATION);
    folderIcon_=loadStock(SIID_FOLDER);
    fileIcon_=loadStock(SIID_DOCNOASSOC);
}

RuntimeMenu::PopupAnchor RuntimeMenu::popup_anchor(POINT invocationPoint,MenuAnchorMode mode) const{
    PopupAnchor out{invocationPoint,0};
    if(mode!=MenuAnchorMode::TaskbarAware) return out;

    const HMONITOR mon=MonitorFromPoint(invocationPoint,MONITOR_DEFAULTTONEAREST);
    RECT taskbar{};
    TaskbarEdge edge=TaskbarEdge::None;
    const bool onTaskbar=taskbar_under_point(invocationPoint,mon,taskbar,edge);

    // --show 只用于“组快捷方式”入口，因此 TaskbarAware 模式不再退回普通鼠标 popup。
    // 优先读取真实 taskbar rect；读不到（自动隐藏/新版 Explorer/副屏差异）时，
    // 用工作区缺口推断任务栏边；如果工作区没有缺口，则选择离点击点最近的屏幕边。
    if(!onTaskbar){
        edge=edge_from_work_area(monitorArea_,workArea_);
        if(edge==TaskbarEdge::None){
            const int dl=std::abs(invocationPoint.x-monitorArea_.left);
            const int dt=std::abs(invocationPoint.y-monitorArea_.top);
            const int dr=std::abs(monitorArea_.right-1-invocationPoint.x);
            const int db=std::abs(monitorArea_.bottom-1-invocationPoint.y);
            const int best=std::min({dl,dt,dr,db});
            if(best==dl) edge=TaskbarEdge::Left;
            else if(best==dt) edge=TaskbarEdge::Top;
            else if(best==dr) edge=TaskbarEdge::Right;
            else edge=TaskbarEdge::Bottom;
        }
    }

    switch(edge){
    case TaskbarEdge::Bottom:
        out.pt.x=std::clamp(invocationPoint.x,workArea_.left,workArea_.right-1);
        out.pt.y=onTaskbar?taskbar.top:workArea_.bottom;
        out.flags=TPM_BOTTOMALIGN | TPM_CENTERALIGN;
        break;
    case TaskbarEdge::Top:
        out.pt.x=std::clamp(invocationPoint.x,workArea_.left,workArea_.right-1);
        out.pt.y=onTaskbar?taskbar.bottom:workArea_.top;
        out.flags=TPM_TOPALIGN | TPM_CENTERALIGN;
        break;
    case TaskbarEdge::Left:
        out.pt.x=onTaskbar?taskbar.right:workArea_.left;
        out.pt.y=std::clamp(invocationPoint.y,workArea_.top,workArea_.bottom-1);
        out.flags=TPM_LEFTALIGN | TPM_VCENTERALIGN;
        break;
    case TaskbarEdge::Right:
        out.pt.x=onTaskbar?taskbar.left:workArea_.right;
        out.pt.y=std::clamp(invocationPoint.y,workArea_.top,workArea_.bottom-1);
        out.flags=TPM_RIGHTALIGN | TPM_VCENTERALIGN;
        break;
    default: break;
    }
    return out;
}

int RuntimeMenu::text_width(const std::wstring&text)const{
    if(text.empty()) return 0;
    HDC dc=GetDC(nullptr);
    if(!dc)return static_cast<int>(text.size())*std::max(1,MulDiv(settings_.menuFontPoints,static_cast<int>(activeDpi_),144));
    HFONT old=menuFont_?static_cast<HFONT>(SelectObject(dc,menuFont_)):nullptr;
    SIZE sz{};
    GetTextExtentPoint32W(dc,text.c_str(),static_cast<int>(text.size()),&sz);
    if(old)SelectObject(dc,old);
    ReleaseDC(nullptr,dc);
    return sz.cx;
}

int RuntimeMenu::item_width(const RuntimeItemView&item)const{
    int w=hPad_+iconPx_+hPad_+text_width(item.name)+hPad_;
    if(item.type==ItemType::Group||item.type==ItemType::Folder) w+=arrowPx_+hPad_;
    const int workW=workArea_.right-workArea_.left;
    return std::min(w,std::max(160,workW-MulDiv(24,static_cast<int>(activeDpi_),96)));
}

int RuntimeMenu::total_width_for_rows(const std::vector<RuntimeItemView>&items,int rows)const{
    if(items.empty()) return 180;
    rows=std::max(1,rows);
    int64_t total=0;
    for(size_t begin=0;begin<items.size();begin+=static_cast<size_t>(rows)){
        int cw=0;
        const size_t end=std::min(items.size(),begin+static_cast<size_t>(rows));
        for(size_t i=begin;i<end;++i) cw=std::max(cw,item_width(items[i]));
        total+=cw;
        if(begin) total+=settings_.columnSeparator?MulDiv(7,static_cast<int>(activeDpi_),96):MulDiv(3,static_cast<int>(activeDpi_),96);
        if(total>=std::numeric_limits<int>::max())return std::numeric_limits<int>::max();
    }
    return static_cast<int>(total);
}

int RuntimeMenu::choose_rows(const std::vector<RuntimeItemView>&items)const{
    const int n=static_cast<int>(items.size());
    if(n<=0) return 1;
    const int workH=workArea_.bottom-workArea_.top;
    const int workW=workArea_.right-workArea_.left;
    const int safety=MulDiv(20,static_cast<int>(activeDpi_),96);
    const int capacity=std::max(1,(workH-safety)/std::max(1,rowHeight_));
    int rows=std::min(n,capacity);
    if(total_width_for_rows(items,rows)<=workW-safety) return rows;

    const int initialCols=static_cast<int>(std::ceil(n/static_cast<double>(rows)));
    for(int cols=initialCols-1;cols>=1;--cols){
        const int candidate=static_cast<int>(std::ceil(n/static_cast<double>(cols)));
        if(total_width_for_rows(items,candidate)<=workW-safety) return candidate;
    }
    return n;
}

RuntimeMenu::DrawEntry* RuntimeMenu::make_draw_entry(RuntimeItemView&item,bool columnStart,bool disabled){
    auto d=std::make_unique<DrawEntry>();
    d->text=item.name;
    d->shellPath=item.path;
    d->type=item.type;
    d->submenu=item.type==ItemType::Group||item.type==ItemType::Folder;
    d->disabled=disabled;
    d->columnStart=columnStart;
    d->width=item_width(item);
    auto b=bitmap_for(item,iconPx_);
    d->bmp=b.bmp;
    d->sourceSize=b.sourceSize;
    d->itemIcon=item.hIcon;
    item.hIcon=nullptr; // 临时 Shell 图标的所有权从枚举项转移给 DrawEntry。
    if(!d->bmp){
        if(item.type==ItemType::Group||item.type==ItemType::Folder)d->fallbackIcon=folderIcon_;
        else if(item.type==ItemType::Program||item.type==ItemType::Url)d->fallbackIcon=genericAppIcon_;
        else d->fallbackIcon=fileIcon_;
    }
    auto* p=d.get();
    drawEntries_.push_back(std::move(d));
    return p;
}

RuntimeMenu::DrawEntry* RuntimeMenu::make_text_entry(const std::wstring&text,bool disabled){
    auto d=std::make_unique<DrawEntry>();
    d->text=text;
    d->disabled=disabled;
    d->width=hPad_*2+text_width(text);
    auto* p=d.get();
    drawEntries_.push_back(std::move(d));
    return p;
}



bool RuntimeMenu::build_group_menu(HMENU menu,const std::wstring&groupId,int depth){
    if(depth>12){
        auto*d=make_text_entry(tr(L"（子组层级过深）"),true);
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask=MIIM_FTYPE|MIIM_STATE|MIIM_DATA|MIIM_STRING;
        mii.fType=MFT_OWNERDRAW;
        mii.fState=MFS_DISABLED;
        mii.dwItemData=reinterpret_cast<ULONG_PTR>(d);
        mii.dwTypeData=d->text.data();
        return InsertMenuItemW(menu,0,TRUE,&mii)!=FALSE;
    }

    auto g=cache_.group(groupId);
    if(!g) return false;

    MENUINFO info{sizeof(info)};
    info.fMask=MIM_MAXHEIGHT;
    const int workHeightPx=static_cast<int>(workArea_.bottom-workArea_.top);
    info.cyMax=static_cast<DWORD>(std::max(100,workHeightPx-MulDiv(12,static_cast<int>(activeDpi_),96)));
    SetMenuInfo(menu,&info);

    if(g->items.empty()){
        auto*d=make_text_entry(tr(L"（空）"),true);
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask=MIIM_FTYPE|MIIM_STATE|MIIM_DATA|MIIM_STRING;
        mii.fType=MFT_OWNERDRAW;
        mii.fState=MFS_DISABLED;
        mii.dwItemData=reinterpret_cast<ULONG_PTR>(d);
        mii.dwTypeData=d->text.data();
        return InsertMenuItemW(menu,0,TRUE,&mii)!=FALSE;
    }
    return build_items_menu(menu,g->items,depth);
}

bool RuntimeMenu::build_items_menu(HMENU menu,std::vector<RuntimeItemView>&items,int depth){
    if(!menu)return false;
    const int rows=choose_rows(items);
    for(size_t i=0;i<items.size();++i){
        auto&x=items[i];
        const bool colStart=i>0&&(i%static_cast<size_t>(rows)==0);
        auto*d=make_draw_entry(x,colStart,false);
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask=MIIM_FTYPE|MIIM_STATE|MIIM_DATA|MIIM_STRING;
        mii.fType=MFT_OWNERDRAW|(colStart?MFT_MENUBREAK:0);
        mii.fState=MFS_ENABLED;
        mii.dwItemData=reinterpret_cast<ULONG_PTR>(d);
        mii.dwTypeData=d->text.data();
        HMENU createdSub=nullptr;
        UINT createdCommand=0;
        if(x.type==ItemType::Group||x.type==ItemType::Folder){
            createdSub=CreatePopupMenu();
            if(!createdSub)return false;
            if(!AppendMenuW(createdSub,MF_STRING|MF_GRAYED,0,L"…")){DestroyMenu(createdSub);return false;}
            pendingSubmenus_[createdSub]={x.type==ItemType::Folder?x.path:x.targetGroupId,depth+1,x.type==ItemType::Folder};
            mii.fMask|=MIIM_SUBMENU;
            mii.hSubMenu=createdSub;
        }else{
            if(nextCommand_==0||nextCommand_==std::numeric_limits<UINT>::max())return false;
            createdCommand=nextCommand_++;
            mii.fMask|=MIIM_ID;
            mii.wID=createdCommand;
            commands_[createdCommand]={x.path,x.arguments,x.workingDirectory,x.type};
        }
        if(!InsertMenuItemW(menu,static_cast<UINT>(i),TRUE,&mii)){
            if(createdSub){pendingSubmenus_.erase(createdSub);DestroyMenu(createdSub);}
            if(createdCommand)commands_.erase(createdCommand);
            return false;
        }
    }
    return true;
}

bool RuntimeMenu::build_folder_menu(HMENU menu,const std::wstring&path,int depth){
    auto addDisabled=[&](const std::wstring& text){
        auto*d=make_text_entry(text,true);
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask=MIIM_FTYPE|MIIM_STATE|MIIM_DATA|MIIM_STRING;
        mii.fType=MFT_OWNERDRAW;
        mii.fState=MFS_DISABLED;
        mii.dwItemData=reinterpret_cast<ULONG_PTR>(d);
        mii.dwTypeData=d->text.data();
        return InsertMenuItemW(menu,0,TRUE,&mii)!=FALSE;
    };
    if(depth>12)return addDisabled(tr(L"（文件夹层级过深）"));
    if(path.empty())return addDisabled(tr(L"（文件夹路径为空）"));

    // 实时枚举：只读文件名与属性，不解析 .lnk、不抽 EXE 图标（性能底线）。
    // 过滤：跳过隐藏/系统项；文件必须属于设置的可启动格式。
    constexpr int kMaxItems=200;
    std::vector<RuntimeItemView> items;
    int visible=0;

    std::wstring pattern=path;
    if(pattern.back()!=L'\\') pattern+=L'\\';
    pattern+=L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE h=FindFirstFileW(pattern.c_str(),&fd);
    if(h==INVALID_HANDLE_VALUE){
        return addDisabled(GetLastError()==ERROR_FILE_NOT_FOUND?tr(L"（空文件夹）"):tr(L"（无法访问）"));
    }
    do{
        if(fd.dwFileAttributes&(FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM)) continue;
        if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){
            if(wcscmp(fd.cFileName,L".")==0||wcscmp(fd.cFileName,L"..")==0) continue;
            RuntimeItemView v;
            v.type=ItemType::Folder;
            v.name=fd.cFileName;
            v.path=path+L"\\"+fd.cFileName;
            ++visible;
            if(static_cast<int>(items.size())<kMaxItems){
                v.hIcon=icon_for_path(v.path);
                items.push_back(std::move(v));
            }
        }else{
            const std::filesystem::path p(path+L"\\"+fd.cFileName);
            const std::wstring ext=lower_copy(p.extension().wstring());
            if(!settings_.is_launchable_ext(ext)) continue;
            RuntimeItemView v;
            v.type=ext==L".url"?ItemType::Url:ItemType::Program;
            v.name=file_display_name(p);
            v.path=p.wstring();
            ++visible;
            if(static_cast<int>(items.size())<kMaxItems){
                v.hIcon=icon_for_path(v.path);
                items.push_back(std::move(v));
            }
        }
    }while(FindNextFileW(h,&fd));
    const DWORD findEndError=GetLastError();
    FindClose(h);
    if(findEndError!=ERROR_NO_MORE_FILES)return addDisabled(tr(L"（文件夹读取中断）"));

    if(items.empty()){
        return addDisabled(visible?tr(L"（该文件夹内没有可启动文件）"):tr(L"（空文件夹）"));
    }

    // 与管理端扫描一致的名称自然排序。
    std::sort(items.begin(),items.end(),[](const RuntimeItemView&a,const RuntimeItemView&b){
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT,NORM_IGNORECASE|SORT_DIGITSASNUMBERS,
                               a.name.c_str(),-1,b.name.c_str(),-1,nullptr,nullptr,0)==CSTR_LESS_THAN;
    });

    if(visible>kMaxItems){
        RuntimeItemView more;
        more.type=ItemType::Program;      // 点击打开整个文件夹（非展开）
        more.path=path;
        more.name=tr(L"（还有 ")+std::to_wstring(visible-kMaxItems)+tr(L" 项…，点击打开文件夹）");
        items.push_back(std::move(more));
    }
    const bool ok=build_items_menu(menu,items,depth);
    // build_items_menu() 会把已处理项的 hIcon 置空并转交 DrawEntry。若中途失败，
    // 尚未处理的临时 Shell HICON 仍留在 items 中，必须在此释放，避免资源泄漏。
    for(auto& item:items){
        if(item.hIcon){DestroyIcon(item.hIcon);item.hIcon=nullptr;}
    }
    return ok;
}

bool RuntimeMenu::handle_init_menu_popup(HMENU menu){
    auto it=pendingSubmenus_.find(menu);
    if(it==pendingSubmenus_.end()) return false;
    int existingCount=GetMenuItemCount(menu);
    if(existingCount<0)return false;
    while(existingCount-->0){
        if(!DeleteMenu(menu,0,MF_BYPOSITION))return false;
    }
    const auto pending=it->second;
    pendingSubmenus_.erase(it);
    const bool ok=pending.isFolder?build_folder_menu(menu,pending.source,pending.depth)
                                  :build_group_menu(menu,pending.source,pending.depth);
    if(!ok){
        auto*d=make_text_entry(pending.isFolder?tr(L"（文件夹不可访问）"):tr(L"（分组不存在）"),true);
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask=MIIM_FTYPE|MIIM_STATE|MIIM_DATA|MIIM_STRING;
        mii.fType=MFT_OWNERDRAW;
        mii.fState=MFS_DISABLED;
        mii.dwItemData=reinterpret_cast<ULONG_PTR>(d);
        mii.dwTypeData=d->text.data();
        if(!InsertMenuItemW(menu,0,TRUE,&mii))return false;
    }
    return true;
}

bool RuntimeMenu::handle_menu_rbutton_up(UINT itemIndex,HMENU menu,HWND owner){
    if(!menu) return false;
    MENUITEMINFOW mii{sizeof(mii)};
    mii.fMask=MIIM_DATA|MIIM_FTYPE|MIIM_STATE;
    if(!GetMenuItemInfoW(menu,itemIndex,TRUE,&mii) || !(mii.fType&MFT_OWNERDRAW) || !mii.dwItemData) return false;

    auto* d=reinterpret_cast<DrawEntry*>(mii.dwItemData);
    if(d->disabled || d->submenu || d->shellPath.empty()) return false;

    POINT pt{};
    if(!GetCursorPos(&pt))return false;
    const ShellContextMenuResult result=show_shell_context_menu(owner,d->shellPath,pt);
    if(result==ShellContextMenuResult::Invoked||pendingShow_) EndMenu();
    return result!=ShellContextMenuResult::Failed;
}

bool RuntimeMenu::handle_measure_item(MEASUREITEMSTRUCT*mi){
    if(!mi||mi->CtlType!=ODT_MENU||!mi->itemData) return false;
    auto*d=reinterpret_cast<DrawEntry*>(mi->itemData);
    mi->itemHeight=static_cast<UINT>(rowHeight_);
    mi->itemWidth=static_cast<UINT>(std::max(d->width,MulDiv(120,static_cast<int>(activeDpi_),96)));
    return true;
}

bool RuntimeMenu::handle_draw_item(DRAWITEMSTRUCT*di){
    if(!di||di->CtlType!=ODT_MENU||!di->itemData) return false;
    auto*d=reinterpret_cast<DrawEntry*>(di->itemData);
    RECT rc=di->rcItem;
    const bool selected=(di->itemState&ODS_SELECTED)!=0&&!d->disabled;
    const COLORREF bg=GetSysColor(selected?COLOR_MENUHILIGHT:COLOR_WINDOW);
    const COLORREF fg=GetSysColor(d->disabled?COLOR_GRAYTEXT:(selected?COLOR_HIGHLIGHTTEXT:COLOR_MENUTEXT));
    HBRUSH br=CreateSolidBrush(bg);
    if(br){FillRect(di->hDC,&rc,br);DeleteObject(br);}
    else FillRect(di->hDC,&rc,GetSysColorBrush(selected?COLOR_HIGHLIGHT:COLOR_WINDOW));

    if(d->columnStart&&settings_.columnSeparator){
        HPEN pen=CreatePen(PS_SOLID,1,GetSysColor(COLOR_3DSHADOW));
        if(pen){
            HPEN old=static_cast<HPEN>(SelectObject(di->hDC,pen));
            MoveToEx(di->hDC,rc.left+1,rc.top,nullptr);
            LineTo(di->hDC,rc.left+1,rc.bottom);
            if(old)SelectObject(di->hDC,old);
            DeleteObject(pen);
        }
    }

    int x=rc.left+hPad_;
    const int cy=(rc.top+rc.bottom)/2;
    if(d->bmp&&d->sourceSize>0){
        HDC mem=CreateCompatibleDC(di->hDC);
        if(mem){
            HGDIOBJ old=SelectObject(mem,d->bmp);
            BLENDFUNCTION bf{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            const int iy=cy-iconPx_/2;
            // bitmap_for 已经生成最终尺寸的 PBGRA，因此这里严格 1:1 AlphaBlend，不再让 GDI 做缩放。
            if(!AlphaBlend(di->hDC,x,iy,iconPx_,iconPx_,mem,0,0,d->sourceSize,d->sourceSize,bf)){
                BitBlt(di->hDC,x,iy,iconPx_,iconPx_,mem,0,0,SRCCOPY);
            }
            if(old)SelectObject(mem,old);
            DeleteDC(mem);
        }
    }else if(d->itemIcon){
        DrawIconEx(di->hDC,x,cy-iconPx_/2,d->itemIcon,iconPx_,iconPx_,0,nullptr,DI_NORMAL);
    }else if(d->fallbackIcon){
        DrawIconEx(di->hDC,x,cy-iconPx_/2,d->fallbackIcon,iconPx_,iconPx_,0,nullptr,DI_NORMAL);
    }
    x+=iconPx_+hPad_;

    RECT tr{x,rc.top,rc.right-hPad_-(d->submenu?arrowPx_+hPad_:0),rc.bottom};
    HFONT oldFont=menuFont_?static_cast<HFONT>(SelectObject(di->hDC,menuFont_)):nullptr;
    SetBkMode(di->hDC,TRANSPARENT);
    SetTextColor(di->hDC,fg);
    // 方案 A：不绘制任何自绘箭头，子菜单指示完全使用系统自带的实心三角
    // （对 hSubMenu 项由系统绘制在项右缘）；上方 tr 仍预留 arrowPx_ 宽度避免文字被压住。
    DrawTextW(di->hDC,d->text.c_str(),static_cast<int>(d->text.size()),&tr,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
    if(oldFont) SelectObject(di->hDC,oldFont);

    // 原生菜单的鼠标高亮本身已经提供清晰焦点。这里不额外 DrawFocusRect，
    // 避免 owner-draw 菜单出现一圈不必要的虚线/毛刺感。
    return true;
}

bool RuntimeMenu::show_once(const PendingShow& request){
    LARGE_INTEGER fq{},t0{},t1{};
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);

    if(!cache_.group(request.groupId)) return false;
    reset_active_state();
    POINT pt{};
    if(request.invocationPoint) pt=*request.invocationPoint;
    else if(!GetCursorPos(&pt)){
        RECT ownerRect{};
        if(request.owner&&GetWindowRect(request.owner,&ownerRect))
            pt={(ownerRect.left+ownerRect.right)/2,(ownerRect.top+ownerRect.bottom)/2};
        else
            pt={GetSystemMetrics(SM_CXSCREEN)/2,GetSystemMetrics(SM_CYSCREEN)/2};
    }
    prepare_visuals(request.owner,pt);

    HMENU menu=CreatePopupMenu();
    if(!menu){reset_active_state();return false;}
    if(!build_group_menu(menu,request.groupId,0)){
        DestroyMenu(menu);
        reset_active_state();
        return false;
    }

    QueryPerformanceCounter(&t1);
    lastPrepareMs_=fq.QuadPart?1000.0*static_cast<double>(t1.QuadPart-t0.QuadPart)/static_cast<double>(fq.QuadPart):0.0;
    const PopupAnchor anchor=popup_anchor(pt,request.anchorMode);
    SetForegroundWindow(request.owner);
    const UINT flags=TPM_RETURNCMD|TPM_LEFTBUTTON|anchor.flags;
    const int cmd=TrackPopupMenuEx(menu,flags,anchor.pt.x,anchor.pt.y,request.owner,nullptr);
    DestroyMenu(menu);
    PostMessageW(request.owner,WM_NULL,0,0);

    auto it=commands_.find(static_cast<UINT>(cmd));
    if(it!=commands_.end()&&!pendingShow_){
        Item temp;
        temp.type=it->second.type;
        temp.path=it->second.path;
        temp.arguments=it->second.arguments;
        temp.workingDirectory=it->second.workingDirectory;
        launch_item(temp);
    }
    reset_active_state();
    trim_bitmaps();
    return true;
}

bool RuntimeMenu::show(HWND owner,const std::wstring& groupId,MenuAnchorMode anchorMode,std::optional<POINT> invocationPoint){
    PendingShow request{owner,groupId,anchorMode,invocationPoint};
    if(showing_){
        pendingShow_=std::move(request);
        EndMenu();
        return true;
    }

    showing_=true;
    bool result=false;
    std::optional<PendingShow> current=std::move(request);
    while(current){
        pendingShow_.reset();
        result=show_once(*current)||result;
        current=std::move(pendingShow_);
    }
    showing_=false;
    if(clearBitmapsPending_) clear_bitmaps();
    return result;
}
}
