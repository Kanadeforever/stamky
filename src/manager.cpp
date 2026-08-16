/*
 * stamky / manager.cpp
 *
 * “内容管理”主编辑窗口：左侧 Group、右侧 Item 两个 ListView 共用 Model 作为唯一业务真相。
 * 界面行的 lParam 保存 Model 下标，任何 ListView_GetItem 失败都必须视为无选择，不能默认落到
 * 下标 0。子组 Item 的名称派生自目标 Group，不能通过 Item 行内编辑制造第二份独立名称。
 *
 * 持久化事务：persistedModel_ 是最近一次 groups.tsv 原子保存成功后的快照。UI 操作可以先修改
 * model_，但 commit() 一旦保存失败就恢复快照；只有配置文件真正提交成功才更新快照。runtime.bin
 * 是派生缓存，缓存重建失败会强提示，但不能把已经成功保存的用户配置反向伪装成“未保存”。
 *
 * Win32 重入：ItemEditor、IFileOpenDialog、TrackPopupMenu、MessageBox 都可能运行嵌套消息循环。
 * 跨这些边界时只保存 Group ID/Item ID 等稳定值，返回后重新 find_group，而不长期保存 vector
 * 元素指针。这样即使嵌套消息处理导致容器变化，也不会继续解引用已经搬迁的元素地址。
 *
 * 拖拽：dragList_ 明确记录当前拖动来自 groups_ 还是 items_；插入线使用独立 WS_POPUP 窗口，
 * 不画在 ListView 自身表面，避免滚动 BitBlt 把旧线条复制成残影。失去 capture、DPI 切换、
 * 鼠标释放和窗口销毁都必须结束拖拽状态并停止自动滚动定时器。
 *
 * DPI/布局：窗口和字体全部按当前 monitor/window DPI 计算，列宽会重新分配以避免 UI-06
 * 出现无意义水平滚动；本模块的发行整理不得擅自改变已确认的 UI-06 交互和视觉层级。
 */

#include "manager.h"
#include "lang.h"
#include "shell_utils.h"
#include "item_editor.h"
#include <uxtheme.h>
#include <set>

namespace sm {
namespace {
constexpr wchar_t kClass[]=L"stamky.Manager";
constexpr wchar_t kDragLineClass[]=L"stamky.DragLine";
constexpr UINT kBeginGroupRenameMsg=WM_APP+0x531;
constexpr UINT kSubgroupCommandBase=7000;   // “添加现有分组为子组”级联菜单的动态命令起点。

// 插入线窗口：2px 高灰色横线，独立顶层窗口（WS_EX_NOACTIVATE|TOOLWINDOW），
// 不参与列表的滚动/重绘，因此不会出现滚动后线条被 BitBlt 搬移的残留问题。
LRESULT CALLBACK DragLineProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_PAINT){
        PAINTSTRUCT ps{};
        HDC dc=BeginPaint(h,&ps);
        RECT rc{};GetClientRect(h,&rc);
        HPEN pen=CreatePen(PS_SOLID,2,GetSysColor(COLOR_3DSHADOW));
        if(pen){
            HPEN old=static_cast<HPEN>(SelectObject(dc,pen));
            MoveToEx(dc,0,rc.top,nullptr);
            LineTo(dc,rc.right,rc.top);
            if(old)SelectObject(dc,old);
            DeleteObject(pen);
        }
        EndPaint(h,&ps);
        return 0;
    }
    if(m==WM_ERASEBKGND)return 1;   // 自绘背景，避免闪烁
    return DefWindowProcW(h,m,w,l);
}
enum : int {
    IDC_GROUPS=100,IDC_ITEMS,IDC_STATUS,
    IDC_NEWGROUP,IDC_DELGROUP,IDC_RENAMEGROUP,IDC_PREVIEW,
    IDC_ADD,IDC_ADDFILE,IDC_CUSTOMITEM,IDC_ADDFOLDER,IDC_SCAN,IDC_ADDSUBGROUP,
    IDC_DELITEM,IDC_EDITITEM,IDC_UP,IDC_DOWN,IDC_SHORTCUT,IDC_SETTINGS
};
enum : int {
    IDM_ITEM_LAUNCH=51000, IDM_ITEM_EDIT, IDM_ITEM_OPENLOC, IDM_ITEM_REFRESHICON, IDM_ITEM_PROPERTIES, IDM_ITEM_DELETE,
    IDM_GROUP_PREVIEW, IDM_GROUP_RENAME, IDM_GROUP_SHORTCUT, IDM_GROUP_NEW, IDM_GROUP_DELETE
};
HMENU cid(int id){return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));}

HICON icon_from_pixels(const IconPixels& px) {
    if (px.size <= 0 || px.bgra.size() < static_cast<size_t>(px.size) * px.size * 4) return nullptr;
    BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=px.size; bi.bmiHeader.biHeight=-px.size;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr; HDC dc=GetDC(nullptr);
    if(!dc)return nullptr;
    HBITMAP color=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,nullptr,0); ReleaseDC(nullptr,dc);
    if(!color||!bits){if(color)DeleteObject(color);return nullptr;}
    memcpy(bits,px.bgra.data(),static_cast<size_t>(px.size)*px.size*4);
    HBITMAP mask=CreateBitmap(px.size,px.size,1,1,nullptr);
    ICONINFO ii{};ii.fIcon=TRUE;ii.hbmColor=color;ii.hbmMask=mask;
    HICON icon=CreateIconIndirect(&ii);
    DeleteObject(color);if(mask)DeleteObject(mask);return icon;
}

HICON stock_icon(SHSTOCKICONID id) {
    SHSTOCKICONINFO si{sizeof(si)};
    if(SUCCEEDED(SHGetStockIconInfo(id,SHGSI_ICON|SHGSI_SMALLICON,&si)))return si.hIcon;
    return nullptr;
}

// Windows 的 std::filesystem::path 对以分隔符结尾的目录可能给出空 filename()。
// 拖入 C:\Games\ 这类路径创建组时应得到“Games”，不能退化成根名“C:”。
std::wstring drop_folder_name(std::filesystem::path path) {
    while (!path.empty() && path.filename().empty()) {
        const auto parent=path.parent_path();
        if(parent==path)break;
        path=parent;
    }
    auto name=path.filename().wstring();
    if(name.empty())name=path.root_name().wstring();
    return name;
}
}

ManagerWindow::ManagerWindow(Model& m,Settings& st,std::function<bool()> c,std::function<void()> s,std::function<void(const std::wstring&)> p)
    :model_(m),persistedModel_(m),settings_(st),onChanged_(std::move(c)),openSettings_(std::move(s)),previewGroup_(std::move(p)){}
ManagerWindow::~ManagerWindow(){if(hwnd_)DestroyWindow(hwnd_);cleanup_visuals();}
void ManagerWindow::show(HWND owner){if(!hwnd_&&!create_window(owner))return;ShowWindow(hwnd_,SW_SHOWNORMAL);SetForegroundWindow(hwnd_);}

/* STAMKY_CN_DETAIL
 * Manager 是常驻对象、按需创建 HWND：关闭窗口时销毁 ListView/ImageList/字体以降低后台占用，
 * 下次 show 再创建。窗口类只注册一次；CreateWindowEx 失败必须停止后续控件构造，避免一组 null HWND
 * 被布局/消息代码当作有效窗口使用。
 */
bool ManagerWindow::create_window(HWND owner){
    (void)owner;
    WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=WndProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=kClass;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_STAMKY));
    wc.hbrBackground=reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW+1));
    if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
    // 内容管理使用独立的 AppUserModelID，并且不把隐藏 Host 设为 owner。
    // 因此它在任务栏上是一个独立窗口槽位，不会与任何 stamky.Group.<GUID>
    // 的“组快捷方式”合并；打开管理器后，固定的组入口仍然可以继续被点击弹菜单。
    hwnd_=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_CONTROLPARENT,kClass,tr(L"stamky - 内容管理").c_str(),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
                          CW_USEDEFAULT,CW_USEDEFAULT,1120,720,nullptr,nullptr,wc.hInstance,this);
    if(!hwnd_)return false;
    set_window_app_user_model_id(hwnd_,L"stamky.Manager");
    // 始终居中到鼠标所在显示器的工作区（避免窗口出现在随机位置）
    POINT cur{};GetCursorPos(&cur);
    HMONITOR mon=MonitorFromPoint(cur,MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(mon,&mi))mi.rcWork={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    const int ww=dip(hwnd_,1120),wh=dip(hwnd_,720);
    const int x=static_cast<int>(mi.rcWork.left)+(static_cast<int>(mi.rcWork.right-mi.rcWork.left)-ww)/2;
    const int y=static_cast<int>(mi.rcWork.top)+(static_cast<int>(mi.rcWork.bottom-mi.rcWork.top)-wh)/2;
    SetWindowPos(hwnd_,nullptr,x,y,ww,wh,SWP_NOZORDER|SWP_NOACTIVATE);
    DragAcceptFiles(hwnd_,TRUE);
    if(!create_controls()){DestroyWindow(hwnd_);return false;}
    refresh_groups(0);return true;
}

LRESULT CALLBACK ManagerWindow::WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    auto*self=reinterpret_cast<ManagerWindow*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_NCCREATE){self=static_cast<ManagerWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);if(self)self->hwnd_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    return self?self->proc(h,m,w,l):DefWindowProcW(h,m,w,l);
}

bool ManagerWindow::create_controls(){
    auto inst=GetModuleHandleW(nullptr);
    groups_=CreateWindowExW(WS_EX_CLIENTEDGE,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|LVS_EDITLABELS,
                            0,0,0,0,hwnd_,cid(IDC_GROUPS),inst,nullptr);
    if(!groups_)return false;
    ListView_SetExtendedListViewStyle(groups_,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
    SetWindowTheme(groups_,L"Explorer",nullptr);
    const std::wstring gcName=tr(L"名称");
    LVCOLUMNW gc{};gc.mask=LVCF_TEXT|LVCF_WIDTH;gc.pszText=const_cast<LPWSTR>(gcName.c_str());gc.cx=210;ListView_InsertColumn(groups_,0,&gc);
    if(HWND header=ListView_GetHeader(groups_)){
        const LONG_PTR style=GetWindowLongPtrW(header,GWL_STYLE);
        SetWindowLongPtrW(header,GWL_STYLE,style|HDS_NOSIZING);
    }

    items_=CreateWindowExW(WS_EX_CLIENTEDGE,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|LVS_REPORT|LVS_SHOWSELALWAYS|LVS_EDITLABELS,
                           0,0,0,0,hwnd_,cid(IDC_ITEMS),inst,nullptr);
    if(!items_)return false;
    ListView_SetExtendedListViewStyle(items_,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
    SetWindowTheme(items_,L"Explorer",nullptr);
    const std::wstring heads[]={tr(L"名称"),tr(L"类型"),tr(L"路径 / 目标"),tr(L"来源 / 状态")};int widths[]={230,75,470,130};
    for(int i=0;i<4;++i){LVCOLUMNW c{};c.mask=LVCF_TEXT|LVCF_WIDTH;c.pszText=const_cast<LPWSTR>(heads[i].c_str());c.cx=widths[i];ListView_InsertColumn(items_,i,&c);}
    if(HWND header=ListView_GetHeader(items_)){
        const LONG_PTR style=GetWindowLongPtrW(header,GWL_STYLE);
        SetWindowLongPtrW(header,GWL_STYLE,style|HDS_NOSIZING);
    }

    auto btn=[&](int id,const wchar_t*t){return CreateWindowW(L"BUTTON",t,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON,0,0,0,0,hwnd_,cid(id),inst,nullptr);};
    btn(IDC_NEWGROUP,tr(L"＋ 新建").c_str());btn(IDC_RENAMEGROUP,tr(L"重命名").c_str());btn(IDC_DELGROUP,tr(L"删除").c_str());btn(IDC_PREVIEW,tr(L"预览").c_str());
    btn(IDC_ADD,tr(L"＋ 添加...").c_str());btn(IDC_SCAN,tr(L"扫描文件夹").c_str());btn(IDC_EDITITEM,tr(L"编辑").c_str());btn(IDC_DELITEM,tr(L"删除").c_str());btn(IDC_UP,L"↑");btn(IDC_DOWN,L"↓");
    btn(IDC_SHORTCUT,tr(L"创建组快捷方式").c_str());btn(IDC_SETTINGS,tr(L"设置").c_str());
    for(int id : {IDC_NEWGROUP,IDC_RENAMEGROUP,IDC_DELGROUP,IDC_PREVIEW,IDC_ADD,IDC_SCAN,IDC_EDITITEM,IDC_DELITEM,IDC_UP,IDC_DOWN,IDC_SHORTCUT,IDC_SETTINGS})
        if(!GetDlgItem(hwnd_,id))return false;
    // 复杂/低频添加动作放到“添加...”菜单，不再堆一排按钮。
    for(int id : {IDC_NEWGROUP,IDC_RENAMEGROUP,IDC_DELGROUP,IDC_PREVIEW,IDC_ADD,IDC_SCAN,IDC_EDITITEM,IDC_DELITEM,IDC_UP,IDC_DOWN,IDC_SHORTCUT,IDC_SETTINGS}){
        if(HWND b=GetDlgItem(hwnd_,id)){SendMessageW(b,BM_SETSTYLE,BS_PUSHBUTTON|BS_FLAT,TRUE);SetWindowTheme(b,L"Explorer",nullptr);}
    }
    status_=CreateWindowExW(0,STATUSCLASSNAMEW,L"",WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,0,0,0,0,hwnd_,cid(IDC_STATUS),inst,nullptr);
    if(!status_)return false;
    // 子类化项目列表：滚轮滚动后重算插入线（替代系统 SetInsertMark——其拖动中
    // 全量重绘导致卡死；插入线本体用独立窗口 dragLine_，见下）。
    SetLastError(ERROR_SUCCESS);
    origItems_=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(items_,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ListViewProc)));
    if(!origItems_&&GetLastError()!=ERROR_SUCCESS)return false;
    SetWindowLongPtrW(items_,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
    // 插入线独立窗口：不参与列表滚动/重绘，杜绝线条残留（列表滚动用 BitBlt
    // 搬移表面像素，直接把线画在列表表面会被搬移累积成多条残留）。
    WNDCLASSEXW dl{sizeof(dl)};dl.lpfnWndProc=DragLineProc;dl.hInstance=inst;dl.lpszClassName=kDragLineClass;dl.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassExW(&dl)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
    dragLine_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,kDragLineClass,L"",WS_POPUP,0,0,10,2,nullptr,nullptr,inst,nullptr);
    if(!dragLine_)return false;
    apply_fonts();layout();
    return true;
}

void ManagerWindow::apply_fonts(){
    if(font_)DeleteObject(font_);
    const UINT dpi=dpi_for_window(hwnd_);font_=create_ui_font(dpi,9);
    for(HWND c=GetWindow(hwnd_,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT))SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
}

void ManagerWindow::cleanup_visuals(){
    if(itemImages_){ImageList_Destroy(itemImages_);itemImages_=nullptr;}
    if(font_){DeleteObject(font_);font_=nullptr;}
}

// 管理器两个 Report ListView 永远按当前客户区宽度重排列，彻底消除底部横向滚动条。
// 这样列表仍保留垂直滚动；列头禁止手动拖宽，避免用户把总列宽再次拖出客户区。
void ManagerWindow::fit_list_columns_no_hscroll(){
    if(!groups_||!items_)return;

    RECT gr{};GetClientRect(groups_,&gr);
    const int groupW=std::max(0,static_cast<int>(gr.right-gr.left)-dip(hwnd_,2));
    if(groupW>0)ListView_SetColumnWidth(groups_,0,groupW);

    RECT ir{};GetClientRect(items_,&ir);
    const int totalW=std::max(0,static_cast<int>(ir.right-ir.left)-dip(hwnd_,2));
    if(totalW>0){
        // 与旧视觉权重大致一致，但总宽度严格等于客户区：名称 24%、类型 10%、路径 48%、状态吃剩余。
        const int nameW=std::max(1,totalW*24/100);
        const int typeW=std::max(1,totalW*10/100);
        const int pathW=std::max(1,totalW*48/100);
        const int stateW=std::max(1,totalW-nameW-typeW-pathW);
        ListView_SetColumnWidth(items_,0,nameW);
        ListView_SetColumnWidth(items_,1,typeW);
        ListView_SetColumnWidth(items_,2,pathW);
        ListView_SetColumnWidth(items_,3,stateW);
    }

    // Report ListView 没有“仅关闭水平滚动”的创建样式；列宽约束负责从根因消除 range，
    // ShowScrollBar 再做显式兜底，确保主题/重算期间不会留下可拖动的横向滚动条槽。
    ShowScrollBar(groups_,SB_HORZ,FALSE);
    ShowScrollBar(items_,SB_HORZ,FALSE);
}

/* STAMKY_CN_DETAIL
 * UI-06 布局只使用 DPI 换算后的逻辑尺寸，并在客户区变化时重新计算左右 ListView、按钮排和状态区。
 * 这里不引入“自适应重新设计”：按钮等宽/对齐、两列表和垂直留白是冻结的行为基线。列宽最后通过
 * fit_list_columns_no_hscroll() 收敛，目标是避免无意义水平滚动而不是改变用户已确认的几何关系。
 */
void ManagerWindow::layout(){
    if(!hwnd_)return;
    RECT r{};GetClientRect(hwnd_,&r);
    SendMessageW(status_,WM_SIZE,0,0);
    RECT sr{};GetWindowRect(status_,&sr);
    const int statusH=static_cast<int>(sr.bottom-sr.top);
    const int m=dip(hwnd_,14),gap=dip(hwnd_,10),leftW=dip(hwnd_,260),bh=dip(hwnd_,30);
    const int w=static_cast<int>(r.right);
    const int h=static_cast<int>(r.bottom)-statusH;
    const int buttonsH=bh*2+gap;
    const int listButtonGap=dip(hwnd_,16);
    const int listY=m;
    const int y1=h-m-buttonsH;
    const int y2=y1+bh+gap;
    const int listH=std::max(0,y1-listButtonGap-listY);

    // UI-06：列表与按钮区之间保留明确留白，避免控件边框几乎贴在一起。
    // 两个列表仍从客户区上边距开始，底边统一停在第一排按钮上方 listButtonGap。
    SetWindowPos(groups_,nullptr,m,listY,leftW,listH,SWP_NOZORDER);
    const int itemsX=m+leftW+gap;
    const int itemsW=std::max(0,w-itemsX-m);
    SetWindowPos(items_,nullptr,itemsX,listY,itemsW,listH,SWP_NOZORDER);
    fit_list_columns_no_hscroll();

    // 左侧按钮区严格按分组列表左右边界排版：
    // 第一排“新建 / 重命名 / 删除”完全等宽、两段间距完全相等；
    // 第二排“预览”保持同宽，“创建组快捷方式”从第二列起铺到列表右边界。
    // 这样两排的左/右外边界一致，不再出现 Delete 右侧或第二排按钮视觉错位。
    int groupActionW=std::max(1,(leftW-gap*2)/3);
    // 像素坐标无法出现半像素；必要时缩 1px，使剩余宽度能被两个间距整除。
    if(groupActionW>1 && ((leftW-3*groupActionW)&1))--groupActionW;
    const int groupButtonGap=std::max(0,(leftW-3*groupActionW)/2);
    const int groupRight=m+leftW;
    auto place_fixed=[&](int id,int xx,int yy,int bw){
        if(HWND b=GetDlgItem(hwnd_,id))SetWindowPos(b,nullptr,xx,yy,bw,bh,SWP_NOZORDER);
    };

    const int groupX1=m;
    const int groupX2=groupX1+groupActionW+groupButtonGap;
    const int groupX3=groupX2+groupActionW+groupButtonGap;
    place_fixed(IDC_NEWGROUP,groupX1,y1,groupActionW);
    place_fixed(IDC_RENAMEGROUP,groupX2,y1,groupActionW);
    place_fixed(IDC_DELGROUP,groupX3,y1,groupActionW);

    place_fixed(IDC_PREVIEW,groupX1,y2,groupActionW);
    if(HWND shortcut=GetDlgItem(hwnd_,IDC_SHORTCUT)){
        const int shortcutX=groupX2;
        const int shortcutW=std::max(1,groupRight-shortcutX);
        SetWindowPos(shortcut,nullptr,shortcutX,y2,shortcutW,bh,SWP_NOZORDER);
    }

    // 项目操作继续位于右侧列表下方第一排。
    int x=itemsX;
    auto place=[&](int id,int minW){
        HWND b=GetDlgItem(hwnd_,id);
        const int bw=fit_text_width(b,font_,dip(hwnd_,minW),dip(hwnd_,300));
        SetWindowPos(b,nullptr,x,y1,bw,bh,SWP_NOZORDER);
        x+=bw+gap;
    };
    place(IDC_ADD,92);place(IDC_SCAN,104);place(IDC_EDITITEM,62);place(IDC_DELITEM,62);place(IDC_UP,40);place(IDC_DOWN,40);

    // 设置独立放在窗口最右侧，不与“创建组快捷方式”组成按钮组。
    if(HWND settings=GetDlgItem(hwnd_,IDC_SETTINGS)){
        const int settingsW=fit_text_width(settings,font_,dip(hwnd_,62),dip(hwnd_,160));
        SetWindowPos(settings,nullptr,w-m-settingsW,y2,settingsW,bh,SWP_NOZORDER);
    }
}

int ManagerWindow::selected_group_index()const{int i=ListView_GetNextItem(groups_,-1,LVNI_SELECTED);if(i<0)return-1;LVITEMW it{};it.mask=LVIF_PARAM;it.iItem=i;if(!ListView_GetItem(groups_,&it))return-1;return static_cast<int>(it.lParam);}
Group*ManagerWindow::selected_group(){int i=selected_group_index();return i>=0&&i<static_cast<int>(model_.groups.size())?&model_.groups[i]:nullptr;}
const Group*ManagerWindow::selected_group()const{int i=selected_group_index();return i>=0&&i<static_cast<int>(model_.groups.size())?&model_.groups[i]:nullptr;}
int ManagerWindow::selected_item_actual_index()const{int i=ListView_GetNextItem(items_,-1,LVNI_SELECTED);if(i<0)return-1;LVITEMW it{};it.mask=LVIF_PARAM;it.iItem=i;if(!ListView_GetItem(items_,&it))return-1;return static_cast<int>(it.lParam);}
std::vector<int>ManagerWindow::selected_item_actual_indices()const{std::vector<int>v;for(int row=ListView_GetNextItem(items_,-1,LVNI_SELECTED);row>=0;row=ListView_GetNextItem(items_,row,LVNI_SELECTED)){LVITEMW it{};it.mask=LVIF_PARAM;it.iItem=row;if(ListView_GetItem(items_,&it))v.push_back(static_cast<int>(it.lParam));}std::sort(v.begin(),v.end());v.erase(std::unique(v.begin(),v.end()),v.end());return v;}


void ManagerWindow::refresh_groups(int select){
    int old=selected_group_index();ListView_DeleteAllItems(groups_);
    for(size_t i=0;i<model_.groups.size();++i){LVITEMW it{};it.mask=LVIF_TEXT|LVIF_PARAM;it.iItem=static_cast<int>(i);it.pszText=const_cast<LPWSTR>(model_.groups[i].name.c_str());it.lParam=static_cast<LPARAM>(i);ListView_InsertItem(groups_,&it);}
    if(select<0)select=old;if(select<0&&!model_.groups.empty())select=0;select=std::min<int>(select,static_cast<int>(model_.groups.size())-1);
    if(select>=0){ListView_SetItemState(groups_,select,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);ListView_EnsureVisible(groups_,select,FALSE);}
    fit_list_columns_no_hscroll();
    refresh_items();
}

void ManagerWindow::rebuild_item_image_list(){
    if(itemImages_){ListView_SetImageList(items_,nullptr,LVSIL_SMALL);ImageList_Destroy(itemImages_);itemImages_=nullptr;}
    const int px=std::clamp(dip(hwnd_,24),20,40);itemImages_=ImageList_Create(px,px,ILC_COLOR32|ILC_MASK,32,32);if(!itemImages_)return;
    ListView_SetImageList(items_,itemImages_,LVSIL_SMALL);
}

void ManagerWindow::refresh_items(int selectActual){
    ListView_DeleteAllItems(items_);rebuild_item_image_list();auto*g=selected_group();if(!g){refresh_status(0);update_action_states();return;}int row=0;
    for(size_t i=0;i<g->items.size();++i){auto&x=g->items[i];int image=-1;HICON hi=nullptr;
        if(x.type==ItemType::Group){hi=stock_icon(SIID_FOLDER);}else{auto px=icons_.load(x.id,32);if(px)hi=icon_from_pixels(*px);if(!hi)hi=stock_icon(x.type==ItemType::Folder?SIID_FOLDER:SIID_APPLICATION);}if(hi&&itemImages_){image=ImageList_AddIcon(itemImages_,hi);DestroyIcon(hi);}
        LVITEMW it{};it.mask=LVIF_TEXT|LVIF_PARAM|(image>=0?LVIF_IMAGE:0);it.iItem=row;it.pszText=const_cast<LPWSTR>(x.name.c_str());it.lParam=static_cast<LPARAM>(i);it.iImage=image;int idx=ListView_InsertItem(items_,&it);
        std::wstring type=item_type_name(x.type);ListView_SetItemText(items_,idx,1,type.data());
        std::wstring path=x.type==ItemType::Group?(model_.find_group(x.targetGroupId)?model_.find_group(x.targetGroupId)->name:tr(L"（分组不存在）")):x.path;ListView_SetItemText(items_,idx,2,path.data());
        std::wstring src=x.type==ItemType::Group?tr(L"内部"):(x.sourceFolder.empty()?tr(L"手动"):tr(L"扫描"));if(x.type!=ItemType::Group&&!path_exists(x.path)&&x.type!=ItemType::Url)src+=tr(L" · 缺失");ListView_SetItemText(items_,idx,3,src.data());
        if(static_cast<int>(i)==selectActual)ListView_SetItemState(items_,idx,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);++row;
    }
    fit_list_columns_no_hscroll();
    refresh_status(row);update_action_states();
}

void ManagerWindow::refresh_status(int visible){
    auto*g=selected_group();std::wstring s;if(!g)s=tr(L"没有分组");else{s=g->name+L"  ·  "+std::to_wstring(g->items.size())+tr(L" 个项目");if(visible>=0&&visible!=static_cast<int>(g->items.size()))s+=tr(L"  ·  当前显示 ")+std::to_wstring(visible)+tr(L" 个");s+=tr(L"     拖放：单文件→当前组；文件夹/多对象拖到左侧→新组；拖到右侧→加入当前组；双击启动；F2 编辑；Delete 删除；Alt+↑/↓ 调整顺序");}SendMessageW(status_,SB_SETTEXTW,0,reinterpret_cast<LPARAM>(s.c_str()));
}

void ManagerWindow::update_action_states(){
    const bool hasGroup=selected_group()!=nullptr;
    const int gi=selected_group_index();
    const int ii=selected_item_actual_index();
    const bool hasItem=hasGroup&&ii>=0;
    auto en=[&](int id,bool v){if(HWND h=GetDlgItem(hwnd_,id))EnableWindow(h,v?TRUE:FALSE);};
    en(IDC_RENAMEGROUP,hasGroup);en(IDC_DELGROUP,hasGroup&&model_.groups.size()>1);en(IDC_PREVIEW,hasGroup);
    en(IDC_ADD,hasGroup);en(IDC_SCAN,hasGroup);en(IDC_SHORTCUT,hasGroup);
    en(IDC_EDITITEM,hasItem);en(IDC_DELITEM,hasItem);
    en(IDC_UP,hasItem&&ii>0);
    en(IDC_DOWN,hasItem&&selected_group()&&ii+1<static_cast<int>(selected_group()->items.size()));
    (void)gi;
}

/* STAMKY_CN_DETAIL
 * commit() 是 UI 编辑与磁盘状态之间的事务边界。model_.save() 成功之前 persistedModel_ 不前移；失败时
 * 立即把 model_ 恢复为上次持久化快照并刷新调用方 UI。runtime cache 重建失败与 groups.tsv 保存失败
 * 语义不同：前者用户数据已经安全落盘，只是派生缓存需要重试，因此绝不能把成功保存的数据回滚掉。
 */
bool ManagerWindow::commit(){
    if(!model_.save()){
        // 保存失败后恢复到最近一次成功落盘的快照，避免 UI 与 groups.tsv 长期分叉。
        model_=persistedModel_;
        MessageBoxW(hwnd_,tr(L"保存配置失败。请检查 data 目录是否可写。").c_str(),tr(L"stamky").c_str(),MB_OK|MB_ICONERROR);
        return false;
    }
    // groups.tsv 已经成功提交；即使后续运行缓存重建失败，也不能把模型回滚到旧版本。
    persistedModel_=model_;
    if(onChanged_&&!onChanged_()){
        MessageBoxW(hwnd_,tr(L"内容已经保存，但运行缓存更新失败。\n\n为了避免出现“管理器里有项目、弹出菜单却是旧内容”的情况，请不要继续使用旧菜单；可从托盘选择“重建图标与运行缓存”，并查看 data\\启动诊断.log。").c_str(),tr(L"stamky - 运行缓存更新失败").c_str(),MB_OK|MB_ICONERROR);
        // 配置本身已经原子提交成功；返回 true 表示调用方应继续按“已保存”状态刷新 UI。
        return true;
    }
    return true;
}

/* STAMKY_CN_DETAIL
 * “＋ 添加...”里的现有分组入口使用真正的 HMENU 级联子菜单，而不是点击父项后再 TrackPopupMenu
 * 第二次弹出菜单。MF_POPUP 交给 Win32 原生菜单状态机后，鼠标只要悬停父项就会自动展开，键盘右方向键
 * 也能进入子菜单，交互与 Explorer 等系统菜单一致。
 *
 * subgroupIds 与动态命令号只在这一次 TrackPopupMenu 生命周期内有效；TrackPopupMenu 返回以后先销毁整个
 * 菜单树，再根据命令号取得稳定的 Group ID，真正写入前由 add_subgroup() 重新 find_group 并再次做环检测。
 * 这样即使菜单的嵌套消息循环期间 Model 发生变化，也不会持有 vector 元素指针。子菜单一旦成功挂到父菜单，
 * 所有权就属于父 HMENU，DestroyMenu(m) 会递归销毁，失败分支只有“尚未挂接”时才单独 DestroyMenu(subgroupMenu)。
 */
void ManagerWindow::show_add_menu(){
    auto* parent=selected_group();
    if(!parent)return;
    const std::wstring parentId=parent->id;

    HMENU m=CreatePopupMenu();
    HMENU subgroupMenu=CreatePopupMenu();
    if(!m||!subgroupMenu){
        if(subgroupMenu)DestroyMenu(subgroupMenu);
        if(m)DestroyMenu(m);
        return;
    }

    std::vector<std::wstring> subgroupIds;
    for(const auto& group:model_.groups){
        if(model_.would_create_group_cycle(parentId,group.id))continue;
        const UINT command=kSubgroupCommandBase+static_cast<UINT>(subgroupIds.size());
        if(!AppendMenuW(subgroupMenu,MF_STRING,command,group.name.c_str())){
            DestroyMenu(subgroupMenu);
            DestroyMenu(m);
            return;
        }
        subgroupIds.push_back(group.id);
    }

    if(subgroupIds.empty()){
        if(!AppendMenuW(subgroupMenu,MF_STRING|MF_GRAYED,0,tr(L"没有可添加的分组").c_str())){
            DestroyMenu(subgroupMenu);
            DestroyMenu(m);
            return;
        }
    }

    const bool baseBuilt=
        AppendMenuW(m,MF_STRING,IDC_ADDFILE,tr(L"添加程序 / 快捷方式 / 文件...").c_str())&&
        AppendMenuW(m,MF_STRING,IDC_ADDFOLDER,tr(L"添加文件夹...").c_str())&&
        AppendMenuW(m,MF_STRING,IDC_CUSTOMITEM,tr(L"自定义项目...").c_str())&&
        AppendMenuW(m,MF_SEPARATOR,0,nullptr);
    if(!baseBuilt){
        DestroyMenu(subgroupMenu);
        DestroyMenu(m);
        return;
    }

    const UINT popupFlags=MF_POPUP|(subgroupIds.empty()?MF_GRAYED:0);
    if(!AppendMenuW(m,popupFlags,reinterpret_cast<UINT_PTR>(subgroupMenu),tr(L"添加现有分组为子组").c_str())){
        DestroyMenu(subgroupMenu);
        DestroyMenu(m);
        return;
    }
    // 从这里开始 subgroupMenu 的所有权已经转交给 m；不要再单独 DestroyMenu(subgroupMenu)。

    HWND addButton=GetDlgItem(hwnd_,IDC_ADD);
    RECT r{};
    if(!addButton||!GetWindowRect(addButton,&r)){DestroyMenu(m);return;}
    SetForegroundWindow(hwnd_);
    const int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,r.left,r.bottom,0,hwnd_,nullptr);
    DestroyMenu(m);
    PostMessageW(hwnd_,WM_NULL,0,0);

    switch(cmd){
    case IDC_ADDFILE:add_files();return;
    case IDC_ADDFOLDER:add_folder();return;
    case IDC_CUSTOMITEM:new_custom_item();return;
    default:break;
    }
    if(cmd>=static_cast<int>(kSubgroupCommandBase)&&
       cmd<static_cast<int>(kSubgroupCommandBase+subgroupIds.size())){
        add_subgroup(parentId,subgroupIds[static_cast<size_t>(cmd-kSubgroupCommandBase)]);
    }
}

void ManagerWindow::add_paths(const std::vector<std::wstring>&paths){auto*g=selected_group();if(!g||paths.empty())return;std::set<std::wstring>existing;for(auto&x:g->items)if(x.type!=ItemType::Group)existing.insert(lower_copy(x.path));int added=0;SetCursor(LoadCursorW(nullptr,IDC_WAIT));for(const auto&p:paths){if(p.empty())continue;auto key=lower_copy(p);if(existing.count(key))continue;Item it;it.id=guid_string();it.path=p;it.name=file_display_name(std::filesystem::path(p));it.type=classify_path(std::filesystem::path(p));icons_.ensure(it);g->items.push_back(std::move(it));existing.insert(key);++added;}SetCursor(LoadCursorW(nullptr,IDC_ARROW));if(added){const int select=static_cast<int>(g->items.size())-1;const bool saved=commit();refresh_items(saved?select:-1);}}

/* STAMKY_CN_DETAIL
 * Explorer 外部拖放到左侧 Group ListView 时，只有“单个文件夹”或“多对象”会建立新 Group。
 * 单个普通文件无论落在左/右列表都仍属于当前高亮 Group，避免用户只是把文件丢到左侧空白处就意外
 * 创建分组。右侧 Item ListView 则始终保持“把拖入对象作为当前组子内容”的语义。
 *
 * 单文件夹建立组时复用 Model::scan_folder：Group 名取文件夹自身名称，Item 包含该目录第一层中符合当前
 * launchExtensions 的可启动文件，以及该目录第一层的所有子文件夹。子文件夹以 Folder Item 保留，不递归
 * 把其内部文件摊平进当前组；这样既不会漏掉目录入口，也不会因为拖入一个大目录树瞬间生成成百上千个项目。
 * “拖到右侧”仍可把文件夹本身作为 Folder Item，二者语义明确分开。多对象建立一个 Group，内容就是这些对象本身；如果它们来自
 * 同一父目录则用父目录名作为组名，否则使用“新分组”。
 */
void ManagerWindow::add_drop_as_group(const std::vector<std::wstring>& paths, bool singleFolder){
    if(paths.empty())return;

    Group group;
    group.id=guid_string();

    if(singleFolder){
        const std::filesystem::path folder(paths.front());
        group.name=drop_folder_name(folder);
        if(group.name.empty())group.name=tr(L"新分组");

        auto found=model_.scan_folder(paths.front(),settings_.launchExtensions);
        if(!found){
            MessageBoxW(hwnd_,tr(L"扫描文件夹失败或在读取过程中中断。未导入任何项目。").c_str(),tr(L"扫描快捷方式").c_str(),MB_OK|MB_ICONERROR);
            return;
        }

        // scan_folder() 有意只收集“可启动文件”；而左侧拖放创建组还必须保留第一层子文件夹入口。
        // 这里单独枚举目录，并继续沿用 optional 扫描的完整性原则：目录枚举如果中途失败，则整次建组取消，
        // 不能把已经找到的一半文件/文件夹保存成看似成功的组。只加入第一层目录，不递归展开目录树。
        std::vector<Item> childFolders;
        std::error_code dirEc;
        std::filesystem::directory_iterator dirIt(folder,std::filesystem::directory_options::skip_permission_denied,dirEc),dirEnd;
        if(dirEc){
            MessageBoxW(hwnd_,tr(L"扫描文件夹失败或在读取过程中中断。未导入任何项目。").c_str(),tr(L"扫描快捷方式").c_str(),MB_OK|MB_ICONERROR);
            return;
        }
        for(;dirIt!=dirEnd;dirIt.increment(dirEc)){
            if(dirEc){
                MessageBoxW(hwnd_,tr(L"扫描文件夹失败或在读取过程中中断。未导入任何项目。").c_str(),tr(L"扫描快捷方式").c_str(),MB_OK|MB_ICONERROR);
                return;
            }
            const auto& entry=*dirIt;
            std::error_code statusEc;
            if(!entry.is_directory(statusEc)||statusEc)continue;
            Item item;
            item.id=guid_string();
            item.path=entry.path().wstring();
            item.name=file_display_name(entry.path());
            item.sourceFolder=folder.wstring();
            item.type=ItemType::Folder;
            childFolders.push_back(std::move(item));
        }
        if(dirEc){
            MessageBoxW(hwnd_,tr(L"扫描文件夹失败或在读取过程中中断。未导入任何项目。").c_str(),tr(L"扫描快捷方式").c_str(),MB_OK|MB_ICONERROR);
            return;
        }
        std::sort(childFolders.begin(),childFolders.end(),[](const Item& a,const Item& b){
            return CompareStringEx(LOCALE_NAME_USER_DEFAULT,NORM_IGNORECASE|SORT_DIGITSASNUMBERS,
                                   a.name.c_str(),-1,b.name.c_str(),-1,nullptr,nullptr,0)==CSTR_LESS_THAN;
        });

        SetCursor(LoadCursorW(nullptr,IDC_WAIT));
        for(auto& item:*found){icons_.ensure(item);group.items.push_back(std::move(item));}
        for(auto& item:childFolders){icons_.ensure(item);group.items.push_back(std::move(item));}
        SetCursor(LoadCursorW(nullptr,IDC_ARROW));
    }else{
        // 多选文件通常来自同一 Explorer 目录；同父目录时直接用目录名，可减少创建后的立即重命名。
        std::filesystem::path commonParent=std::filesystem::path(paths.front()).parent_path();
        bool sameParent=!commonParent.empty();
        const std::wstring commonKey=lower_copy(commonParent.wstring());
        std::set<std::wstring> existing;
        SetCursor(LoadCursorW(nullptr,IDC_WAIT));
        for(const auto& raw:paths){
            if(raw.empty())continue;
            const std::filesystem::path path(raw);
            if(sameParent&&lower_copy(path.parent_path().wstring())!=commonKey)sameParent=false;
            const auto key=lower_copy(raw);
            if(existing.count(key))continue;
            Item item;item.id=guid_string();item.path=raw;item.name=file_display_name(path);item.type=classify_path(path);
            icons_.ensure(item);group.items.push_back(std::move(item));existing.insert(key);
        }
        SetCursor(LoadCursorW(nullptr,IDC_ARROW));
        if(group.items.empty())return;
        if(sameParent)group.name=drop_folder_name(commonParent);
        if(group.name.empty())group.name=tr(L"新分组");
    }

    model_.groups.push_back(std::move(group));
    const int select=static_cast<int>(model_.groups.size())-1;
    const bool saved=commit();
    refresh_groups(saved?select:-1);
}
void ManagerWindow::add_files(){add_paths(pick_files(hwnd_,settings_.launchExtensions));}
void ManagerWindow::new_custom_item(){
    auto* g=selected_group();if(!g)return;
    const std::wstring groupId=g->id;
    Item it;it.id=guid_string();
    if(!ItemEditor::edit(hwnd_,it,true,&settings_.launchExtensions))return;
    g=model_.find_group(groupId);if(!g)return;
    icons_.ensure(it);g->items.push_back(std::move(it));
    const int select=static_cast<int>(g->items.size())-1;
    const bool saved=commit();
    refresh_items(saved?select:-1);
}

void ManagerWindow::add_folder(){auto p=pick_folder(hwnd_);if(p)add_paths({*p});}

void ManagerWindow::scan_folder(){
    auto* g=selected_group();if(!g)return;
    const std::wstring groupId=g->id;
    auto folder=pick_folder(hwnd_);if(!folder)return;
    g=model_.find_group(groupId);if(!g)return;
    auto found=model_.scan_folder(*folder,settings_.launchExtensions);
    if(!found){
        MessageBoxW(hwnd_,tr(L"扫描文件夹失败或在读取过程中中断。未导入任何项目。").c_str(),tr(L"扫描快捷方式").c_str(),MB_OK|MB_ICONERROR);
        return;
    }
    std::set<std::wstring> existing;
    for(const auto& x:g->items)if(x.type!=ItemType::Group)existing.insert(lower_copy(x.path));
    int added=0;SetCursor(LoadCursorW(nullptr,IDC_WAIT));
    for(auto& it:*found){auto key=lower_copy(it.path);if(existing.count(key))continue;icons_.ensure(it);existing.insert(key);g->items.push_back(std::move(it));++added;}
    SetCursor(LoadCursorW(nullptr,IDC_ARROW));
    if(added){
        const bool saved=commit();
        refresh_items();
        if(!saved)return;
    }
    std::wstring msg=tr(L"扫描完成：新增 ")+std::to_wstring(added)+tr(L" 个项目。\n\n已有项目顺序保持不变；新项目追加到列表末尾。");
    MessageBoxW(hwnd_,msg.c_str(),tr(L"扫描快捷方式").c_str(),MB_OK|MB_ICONINFORMATION);
}

/* STAMKY_CN_DETAIL
 * 级联菜单只负责“选择目标 Group ID”，真正修改 Model 集中在这里。调用方传入的是稳定字符串 ID，
 * 不能把 show_add_menu() 枚举时取得的 Group* 跨 TrackPopupMenu 的嵌套消息循环带进来。执行时重新取得
 * 打开“＋ 添加...”时的父 Group 与目标 Group，并再次调用 would_create_group_cycle()；因此即使菜单展开期间
 * 当前选择或组关系发生变化，也不会把命令误加到另一个刚被选中的组，更不会把已经失效的候选写入数据。
 * 子组名称仍完全派生于目标 Group，不产生第二套可编辑名称。
 */
void ManagerWindow::add_subgroup(const std::wstring& parentGroupId,const std::wstring& targetGroupId){
    auto* parent=model_.find_group(parentGroupId);
    auto* target=model_.find_group(targetGroupId);
    if(!parent||!target||model_.would_create_group_cycle(parent->id,target->id))return;
    Item it;it.id=guid_string();it.name=target->name;it.type=ItemType::Group;it.targetGroupId=target->id;
    parent->items.push_back(std::move(it));
    const int select=static_cast<int>(parent->items.size())-1;
    const bool saved=commit();
    refresh_items(saved?select:-1);
}

void ManagerWindow::delete_items(){auto*g=selected_group();auto idx=selected_item_actual_indices();if(!g||idx.empty())return;if(MessageBoxW(hwnd_,tr(idx.size()>1?L"删除选中的项目？\\n不会删除原始文件。":L"删除当前项目？\\n不会删除原始文件。").c_str(),tr(L"stamky").c_str(),MB_YESNO|MB_ICONWARNING)!=IDYES)return;for(auto it=idx.rbegin();it!=idx.rend();++it)if(*it>=0&&*it<static_cast<int>(g->items.size()))g->items.erase(g->items.begin()+*it);commit();refresh_items();}
void ManagerWindow::edit_item(){auto*g=selected_group();int i=selected_item_actual_index();if(!g||i<0||i>=static_cast<int>(g->items.size()))return;auto&it=g->items[i];if(it.type==ItemType::Group){MessageBoxW(hwnd_,tr(L"子组项目的名称来自目标分组。请在左侧重命名目标分组。").c_str(),tr(L"stamky").c_str(),MB_OK|MB_ICONINFORMATION);return;}const auto oldPath=it.path;const auto oldIcon=it.customIcon;if(ItemEditor::edit(hwnd_,it,false,&settings_.launchExtensions)){if(_wcsicmp(oldPath.c_str(),it.path.c_str())!=0||oldIcon!=it.customIcon){icons_.invalidate(it.id);icons_.ensure(it);}commit();refresh_items(i);}}
void ManagerWindow::refresh_item_icon(){auto*g=selected_group();int i=selected_item_actual_index();if(!g||i<0||i>=static_cast<int>(g->items.size()))return;auto&it=g->items[i];if(it.type==ItemType::Group)return;SetCursor(LoadCursorW(nullptr,IDC_WAIT));icons_.invalidate(it.id);bool ok=icons_.ensure(it);if(onChanged_&&!onChanged_())MessageBoxW(hwnd_,tr(L"图标已刷新，但运行缓存更新失败。").c_str(),tr(L"stamky").c_str(),MB_OK|MB_ICONERROR);SetCursor(LoadCursorW(nullptr,IDC_ARROW));refresh_items(i);if(!ok)MessageBoxW(hwnd_,tr(L"没有从该项目取得可用图标，将继续使用通用图标。").c_str(),tr(L"stamky").c_str(),MB_OK|MB_ICONINFORMATION);}
void ManagerWindow::launch_selected(){auto*g=selected_group();int i=selected_item_actual_index();if(!g||i<0||i>=static_cast<int>(g->items.size()))return;auto&it=g->items[i];if(it.type==ItemType::Group){if(previewGroup_)previewGroup_(it.targetGroupId);}else launch_item(it);}
void ManagerWindow::open_selected_location(){auto*g=selected_group();int i=selected_item_actual_index();if(!g||i<0||i>=static_cast<int>(g->items.size()))return;auto&it=g->items[i];if(it.type==ItemType::Group)return;if(it.type==ItemType::Url){launch_item(it);return;}open_containing_folder(it.path);}
void ManagerWindow::show_selected_shell_properties(){auto*g=selected_group();int i=selected_item_actual_index();if(!g||i<0||i>=static_cast<int>(g->items.size()))return;auto&it=g->items[i];if(it.type==ItemType::Group||it.type==ItemType::Url)return;show_file_properties(hwnd_,it.path);}

void ManagerWindow::move_item(int d){
    auto*g=selected_group();if(!g)return;
    auto idxs=selected_item_actual_indices();
    if(idxs.empty())return;
    const int n=static_cast<int>(g->items.size());
    for(int i:idxs)if(i<0||i>=n)return;
    // 多选块移动：选中项按原顺序聚合为连续块，整体移动一格（与紧邻项交换位置）。
    if(d>0&&idxs.back()+1>=n)return;   // 下移：块后无可换项
    if(d<0&&idxs[0]<=0)return;         // 上移：块前无可换项
    std::vector<Item> block;
    block.reserve(idxs.size());
    for(int i:idxs)block.push_back(g->items[i]);   // idxs 升序 → 块内保持相对顺序
    for(auto it=idxs.rbegin();it!=idxs.rend();++it)g->items.erase(g->items.begin()+*it);
    const int insertAt=idxs[0]+(d>0?1:-1);
    g->items.insert(g->items.begin()+insertAt,block.begin(),block.end());
    const bool saved=commit();
    refresh_items();
    if(!saved)return;
    // 恢复整块选中（块内顺序即选中顺序）
    ListView_SetItemState(items_,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    for(int i=0;i<static_cast<int>(block.size());++i)
        ListView_SetItemState(items_,insertAt+i,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    ListView_SetItemState(items_,insertAt,LVIS_FOCUSED,LVIS_FOCUSED);
    update_action_states();
}
// 分组移动（Alt+↑/↓）：分组列表为单选，移动选中组一格并保持选中。
void ManagerWindow::move_group(int d){
    int i=selected_group_index(),j=i+d;
    if(i<0||j<0||j>=static_cast<int>(model_.groups.size()))return;
    std::swap(model_.groups[i],model_.groups[j]);
    const bool saved=commit();
    refresh_groups(saved?j:i);
}
void ManagerWindow::new_group(){model_.groups.push_back({guid_string(),tr(L"新分组"),{}});const int i=static_cast<int>(model_.groups.size())-1;if(!commit()){refresh_groups();return;}refresh_groups(i);rename_group();}
void ManagerWindow::rename_group(){
    const int row=ListView_GetNextItem(groups_,-1,LVNI_SELECTED);
    if(row<0)return;
    // 从按钮触发时焦点仍在 BUTTON 上，部分 comctl32/Explorer theme 组合会让
    // LVM_EDITLABEL 立即失败或瞬间结束。先恢复 ListView 的选择/焦点，再延后一条
    // 消息启动行内编辑，确保鼠标点击、F2、右键菜单三条入口行为一致。
    ListView_SetItemState(groups_,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    ListView_EnsureVisible(groups_,row,FALSE);
    SetFocus(groups_);
    PostMessageW(hwnd_,kBeginGroupRenameMsg,static_cast<WPARAM>(row),0);
}
void ManagerWindow::delete_group(){int i=selected_group_index();if(i<0)return;if(model_.groups.size()==1){MessageBoxW(hwnd_,tr(L"至少保留一个分组。").c_str(),tr(L"stamky").c_str(),MB_OK|MB_ICONINFORMATION);return;}if(MessageBoxW(hwnd_,tr(L"删除当前分组？\\n\\n不会删除任何原始文件；指向这个分组的子组项目会显示为失效。").c_str(),tr(L"stamky").c_str(),MB_YESNO|MB_ICONWARNING)!=IDYES)return;model_.groups.erase(model_.groups.begin()+i);commit();refresh_groups(std::min<int>(i,static_cast<int>(model_.groups.size())-1));}
void ManagerWindow::make_shortcut(){auto*g=selected_group();if(!g)return;std::wstring p;if(create_group_shortcut(*g,&p)){
    std::wstring msg=tr(L"组快捷方式已经创建并通过 ShellLink 回读校验：\\n\\n")+p+tr(L"\\n\\n请先双击确认能够弹出正确分组；确认后再固定到任务栏。旧版本生成的组快捷方式请删除后重新创建。");
    MessageBoxW(hwnd_,msg.c_str(),tr(L"stamky - 组快捷方式").c_str(),MB_OK|MB_ICONINFORMATION);
    std::wstring arg=L"/select,\""+p+L"\"";ShellExecuteW(hwnd_,L"open",L"explorer.exe",arg.c_str(),nullptr,SW_SHOWNORMAL);
}else MessageBoxW(hwnd_,tr(L"创建或校验组快捷方式失败。\\n\\n程序不会保留未通过 ShellLink 回读校验的 .lnk。").c_str(),tr(L"stamky").c_str(),MB_OK|MB_ICONERROR);}
void ManagerWindow::preview_group(){auto*g=selected_group();if(g&&previewGroup_)previewGroup_(g->id);}

void ManagerWindow::show_item_context_menu(POINT screen){
    // 右键未选中的行时先把该行变成当前项，避免上下文命令误作用到之前的选择。
    POINT client=screen;ScreenToClient(items_,&client);LVHITTESTINFO hit{};hit.pt=client;
    const int hitRow=ListView_HitTest(items_,&hit);
    if(hitRow>=0&&!(ListView_GetItemState(items_,hitRow,LVIS_SELECTED)&LVIS_SELECTED)){
        ListView_SetItemState(items_,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemState(items_,hitRow,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    }
    auto*g=selected_group();int i=selected_item_actual_index();if(!g||i<0||i>=static_cast<int>(g->items.size()))return;const auto&type=g->items[i].type;
    HMENU m=CreatePopupMenu();if(!m)return;
    const bool menuOk=
        AppendMenuW(m,MF_STRING,IDM_ITEM_LAUNCH,tr(L"启动\tEnter").c_str())&&
        AppendMenuW(m,MF_STRING,IDM_ITEM_EDIT,tr(L"编辑\tF2").c_str())&&
        AppendMenuW(m,MF_SEPARATOR,0,nullptr)&&
        AppendMenuW(m,MF_STRING,IDM_ITEM_OPENLOC,tr(type==ItemType::Url?L"打开链接":L"打开所在位置").c_str())&&
        AppendMenuW(m,(type==ItemType::Group?MF_GRAYED:MF_STRING),IDM_ITEM_REFRESHICON,tr(L"刷新图标").c_str())&&
        AppendMenuW(m,(type==ItemType::Group||type==ItemType::Url?MF_GRAYED:MF_STRING),IDM_ITEM_PROPERTIES,tr(L"Windows 属性").c_str())&&
        AppendMenuW(m,MF_SEPARATOR,0,nullptr)&&
        AppendMenuW(m,MF_STRING,IDM_ITEM_DELETE,tr(L"删除\tDelete").c_str());
    if(!menuOk){DestroyMenu(m);return;}
    SetForegroundWindow(hwnd_);
    int c=TrackPopupMenu(m,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,screen.x,screen.y,0,hwnd_,nullptr);
    DestroyMenu(m);
    // TrackPopupMenu 的 owner 是普通顶层窗口时，补一个 WM_NULL 可以可靠解除菜单 modal 状态，
    // 避免用户选中命令后右键菜单残影/不自动收起。
    PostMessageW(hwnd_,WM_NULL,0,0);
    switch(c){case IDM_ITEM_LAUNCH:launch_selected();break;case IDM_ITEM_EDIT:edit_item();break;case IDM_ITEM_OPENLOC:open_selected_location();break;case IDM_ITEM_REFRESHICON:refresh_item_icon();break;case IDM_ITEM_PROPERTIES:show_selected_shell_properties();break;case IDM_ITEM_DELETE:delete_items();break;}
}
void ManagerWindow::show_group_context_menu(POINT screen){
    POINT client=screen;ScreenToClient(groups_,&client);LVHITTESTINFO hit{};hit.pt=client;
    const int hitRow=ListView_HitTest(groups_,&hit);
    if(hitRow>=0){
        ListView_SetItemState(groups_,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemState(groups_,hitRow,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    }
    HMENU m=CreatePopupMenu();if(!m)return;
    const bool menuOk=
        AppendMenuW(m,MF_STRING,IDM_GROUP_PREVIEW,tr(L"预览菜单").c_str())&&
        AppendMenuW(m,MF_STRING,IDM_GROUP_RENAME,tr(L"重命名\tF2").c_str())&&
        AppendMenuW(m,MF_STRING,IDM_GROUP_SHORTCUT,tr(L"创建组快捷方式").c_str())&&
        AppendMenuW(m,MF_SEPARATOR,0,nullptr)&&
        AppendMenuW(m,MF_STRING,IDM_GROUP_NEW,tr(L"新建分组").c_str())&&
        AppendMenuW(m,MF_STRING,IDM_GROUP_DELETE,tr(L"删除分组").c_str());
    if(!menuOk){DestroyMenu(m);return;}
    SetForegroundWindow(hwnd_);
    const int c=TrackPopupMenu(m,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,screen.x,screen.y,0,hwnd_,nullptr);
    DestroyMenu(m);PostMessageW(hwnd_,WM_NULL,0,0);
    switch(c){case IDM_GROUP_PREVIEW:preview_group();break;case IDM_GROUP_RENAME:rename_group();break;case IDM_GROUP_SHORTCUT:make_shortcut();break;case IDM_GROUP_NEW:new_group();break;case IDM_GROUP_DELETE:delete_group();break;}
}

/* STAMKY_CN_DETAIL
 * pretranslate 统一处理 F2/Delete/Alt+↑↓/拖拽等管理器快捷键，并把其余键交给 IsDialogMessageW，
 * 让 Tab/默认按钮等 Win32 对话框语义仍然成立。对子组的 F2 特判是数据模型约束：应重命名目标 Group，
 * 而不是修改引用 Item.name。
 */
bool ManagerWindow::pretranslate(MSG& msg){
    if(!hwnd_||!IsWindow(hwnd_))return false;
    if(msg.hwnd!=hwnd_&&!IsChild(hwnd_,msg.hwnd))return false;
    // Alt+方向键是系统组合键：到达的是 WM_SYSKEYDOWN 而不是 WM_KEYDOWN，
    // 必须在 SYSKEYDOWN 分支拦截，否则会被 IsDialogMessageW/系统菜单吞掉。
    if(msg.message==WM_SYSKEYDOWN){
        const WPARAM key=msg.wParam;HWND focus=GetFocus();
        const bool itemFocus=focus==items_||IsChild(items_,focus);
        const bool groupFocus=focus==groups_||IsChild(groups_,focus);
        if((GetKeyState(VK_MENU)&0x8000)){
            if(itemFocus){
                if(key==VK_UP){move_item(-1);return true;}
                if(key==VK_DOWN){move_item(1);return true;}
            }else if(groupFocus){
                if(key==VK_UP){move_group(-1);return true;}
                if(key==VK_DOWN){move_group(1);return true;}
            }
        }
    }
    if(msg.message==WM_KEYDOWN){
        const WPARAM key=msg.wParam;HWND focus=GetFocus();const bool itemFocus=focus==items_||IsChild(items_,focus);const bool groupFocus=focus==groups_||IsChild(groups_,focus);
        if((GetKeyState(VK_CONTROL)&0x8000)&&key=='O'){add_files();return true;}
        if((GetKeyState(VK_CONTROL)&0x8000)&&key=='N'){new_group();return true;}
        if(key==VK_ESCAPE){
            if(ListView_GetEditControl(items_)||ListView_GetEditControl(groups_))return false;     // 行内编辑中：交给编辑框取消
            DestroyWindow(hwnd_);                                                                  // 否则关闭内容管理
            return true;
        }
        if(key==VK_F2){if(itemFocus)edit_item();else if(groupFocus)rename_group();else return false;return true;}
        if(key==VK_DELETE){if(itemFocus)delete_items();else if(groupFocus)delete_group();else return false;return true;}
        if(key==VK_RETURN&&itemFocus){launch_selected();return true;}
    }
    return IsDialogMessageW(hwnd_,&msg)!=FALSE;
}

LRESULT ManagerWindow::proc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case kBeginGroupRenameMsg:{
        const int row=static_cast<int>(w);
        if(!groups_||row<0||row>=ListView_GetItemCount(groups_))return 0;
        // 若已处于编辑状态，不重复创建 EDIT 子控件。
        if(ListView_GetEditControl(groups_))return 0;
        ListView_SetItemState(groups_,-1,0,LVIS_FOCUSED);
        ListView_SetItemState(groups_,row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        SetFocus(groups_);
        if(HWND edit=ListView_EditLabel(groups_,row)){
            SetFocus(edit);
            SendMessageW(edit,EM_SETSEL,0,-1);
        }
        return 0;
    }
    case WM_SIZE:layout();return 0;
    case WM_CTLCOLORSTATIC:{HDC dc=reinterpret_cast<HDC>(w);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));}
    case WM_DPICHANGED:{dragRowHeight_=0;auto*r=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);apply_fonts();layout();refresh_items(selected_item_actual_index());return 0;}
    case WM_DROPFILES:{
        HDROP drop=reinterpret_cast<HDROP>(w);
        POINT client{};
        const bool hasPoint=DragQueryPoint(drop,&client)!=FALSE;
        POINT screen=client;
        if(hasPoint)ClientToScreen(hwnd_,&screen);
        RECT groupRect{},itemRect{};
        const bool overGroups=hasPoint&&groups_&&GetWindowRect(groups_,&groupRect)&&PtInRect(&groupRect,screen);
        const bool overItems=hasPoint&&items_&&GetWindowRect(items_,&itemRect)&&PtInRect(&itemRect,screen);

        const UINT n=DragQueryFileW(drop,0xFFFFFFFF,nullptr,0);
        std::vector<std::wstring> p;
        p.reserve(n);
        for(UINT i=0;i<n;++i){
            const UINT len=DragQueryFileW(drop,i,nullptr,0);
            if(!len)continue;
            std::wstring s(static_cast<size_t>(len)+1,L'\0');
            const UINT got=DragQueryFileW(drop,i,s.data(),len+1);
            if(!got)continue;
            s.resize(got);
            p.push_back(std::move(s));
        }
        DragFinish(drop);
        if(p.empty())return 0;

        std::error_code ec;
        const bool singleFolder=p.size()==1&&std::filesystem::is_directory(std::filesystem::path(p.front()),ec)&&!ec;
        // 单文件始终加入当前高亮组；单文件夹/多对象则必须明确落在左/右列表：
        // 左侧建立新组，右侧作为当前组子内容。落在按钮区/状态栏等位置时不做破坏性猜测。
        if(!singleFolder&&p.size()==1)add_paths(p);
        else if(overGroups)add_drop_as_group(p,singleFolder);
        else if(overItems)add_paths(p);
        return 0;
    }
    case WM_CLOSE:DestroyWindow(hwnd_);return 0;
    case WM_DESTROY:DragAcceptFiles(hwnd_,FALSE);if(dragLine_){DestroyWindow(dragLine_);dragLine_=nullptr;}cleanup_visuals();hwnd_=nullptr;groups_=items_=status_=nullptr;return 0;
    case WM_NCDESTROY:SetWindowLongPtrW(h,GWLP_USERDATA,0);return DefWindowProcW(h,m,w,l);
    case WM_COMMAND:{int id=LOWORD(w);switch(id){case IDC_NEWGROUP:new_group();break;case IDC_RENAMEGROUP:rename_group();break;case IDC_DELGROUP:delete_group();break;case IDC_PREVIEW:preview_group();break;case IDC_ADD:show_add_menu();break;case IDC_ADDFILE:add_files();break;case IDC_CUSTOMITEM:new_custom_item();break;case IDC_ADDFOLDER:add_folder();break;case IDC_SCAN:scan_folder();break;case IDC_EDITITEM:edit_item();break;case IDC_DELITEM:delete_items();break;case IDC_UP:move_item(-1);break;case IDC_DOWN:move_item(1);break;case IDC_SHORTCUT:make_shortcut();break;case IDC_SETTINGS:if(openSettings_)openSettings_();break;}return 0;}
    case WM_KEYDOWN:{HWND focus=GetFocus();const bool itemFocus=focus==items_||IsChild(items_,focus);const bool groupFocus=focus==groups_||IsChild(groups_,focus);if((GetKeyState(VK_CONTROL)&0x8000)&&w=='O'){add_files();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='N'){new_group();return 0;}if(w==VK_F2){if(itemFocus)edit_item();else if(groupFocus)rename_group();return 0;}if(w==VK_DELETE){if(itemFocus)delete_items();else if(groupFocus)delete_group();return 0;}if(w==VK_RETURN&&itemFocus){launch_selected();return 0;}break;}
    case WM_CONTEXTMENU:{
        // ListView 鼠标右键统一由 NM_RCLICK 处理。Windows 还可能随后把同一次右键转换成
        // WM_CONTEXTMENU；如果这里再次弹菜单，就会形成“执行后还有一层菜单留在屏幕上”的假残影。
        // 因此 WM_CONTEXTMENU 这里只处理键盘菜单键 / Shift+F10（坐标为 -1,-1）。
        HWND src=reinterpret_cast<HWND>(w);POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        if(src!=items_&&src!=groups_)return 0;
        if(p.x!=-1||p.y!=-1)return 0;
        RECT rr{};GetWindowRect(src,&rr);p={rr.left+20,rr.top+20};
        if(src==items_)show_item_context_menu(p);else show_group_context_menu(p);return 0;}
    case WM_NOTIFY:{auto*hdr=reinterpret_cast<NMHDR*>(l);if(hdr->hwndFrom==groups_){if(hdr->code==LVN_ITEMCHANGED){auto*n=reinterpret_cast<NMLISTVIEW*>(l);if((n->uNewState&LVIS_SELECTED)&&!(n->uOldState&LVIS_SELECTED))refresh_items();}else if(hdr->code==LVN_ENDLABELEDITW){auto*n=reinterpret_cast<NMLVDISPINFOW*>(l);if(n->item.pszText&&n->item.pszText[0]){LVITEMW cur{};cur.mask=LVIF_PARAM;cur.iItem=n->item.iItem;if(!ListView_GetItem(groups_,&cur))return FALSE;int i=static_cast<int>(cur.lParam);if(i>=0&&i<static_cast<int>(model_.groups.size())){model_.groups[i].name=n->item.pszText;for(auto&g:model_.groups)for(auto&it:g.items)if(it.type==ItemType::Group&&_wcsicmp(it.targetGroupId.c_str(),model_.groups[i].id.c_str())==0)it.name=model_.groups[i].name;if(!commit()){refresh_groups(i);return FALSE;}refresh_items();return TRUE;}}}else if(hdr->code==NM_DBLCLK){preview_group();}else if(hdr->code==NM_RCLICK){POINT p{};if(GetCursorPos(&p))show_group_context_menu(p);return 0;}else if(hdr->code==LVN_BEGINDRAG){dragging_=true;dragFrom_=selected_group_index();dragList_=groups_;dragRowHeight_=0;SetCapture(hwnd_);if(GetCapture()!=hwnd_){dragging_=false;dragFrom_=-1;dragList_=nullptr;}return 0;}else if(hdr->code==LVN_KEYDOWN){auto*k=reinterpret_cast<NMLVKEYDOWN*>(l);if(k->wVKey==VK_DELETE){delete_group();return 0;}if(k->wVKey==VK_F2){rename_group();return 0;}}}
        else if(hdr->hwndFrom==items_){if(hdr->code==LVN_ITEMCHANGED){update_action_states();}else if(hdr->code==NM_DBLCLK){launch_selected();return 0;}if(hdr->code==NM_RCLICK){POINT p{};if(GetCursorPos(&p))show_item_context_menu(p);return 0;}if(hdr->code==LVN_KEYDOWN){auto*k=reinterpret_cast<NMLVKEYDOWN*>(l);if(k->wVKey==VK_DELETE){delete_items();return 0;}if(k->wVKey==VK_F2){edit_item();return 0;}if(k->wVKey==VK_RETURN){launch_selected();return 0;}if((GetKeyState(VK_MENU)&0x8000)&&k->wVKey==VK_UP){move_item(-1);return 0;}if((GetKeyState(VK_MENU)&0x8000)&&k->wVKey==VK_DOWN){move_item(1);return 0;}}if(hdr->code==LVN_BEGINLABELEDITW){auto*n=reinterpret_cast<NMLVDISPINFOW*>(l);auto*g=selected_group();LVITEMW cur{};cur.mask=LVIF_PARAM;cur.iItem=n->item.iItem;if(g&&ListView_GetItem(items_,&cur)){const int ai=static_cast<int>(cur.lParam);if(ai>=0&&ai<static_cast<int>(g->items.size())&&g->items[ai].type==ItemType::Group)return TRUE;}}
        if(hdr->code==LVN_ENDLABELEDITW){auto*n=reinterpret_cast<NMLVDISPINFOW*>(l);if(n->item.pszText&&n->item.pszText[0]){auto*g=selected_group();LVITEMW cur{};cur.mask=LVIF_PARAM;cur.iItem=n->item.iItem;if(!ListView_GetItem(items_,&cur))return FALSE;int ai=static_cast<int>(cur.lParam);if(g&&ai>=0&&ai<static_cast<int>(g->items.size())&&g->items[ai].type!=ItemType::Group){g->items[ai].name=n->item.pszText;if(!commit()){refresh_items(ai);return FALSE;}return TRUE;}}}else if(hdr->code==LVN_BEGINDRAG){if(selected_item_actual_indices().size()==1){dragging_=true;dragFrom_=selected_item_actual_index();dragList_=items_;dragRowHeight_=0;SetCapture(hwnd_);if(GetCapture()!=hwnd_){dragging_=false;dragFrom_=-1;dragList_=nullptr;}}}}return 0;}
    case WM_MOUSEMOVE:if(dragging_){
        SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));
        HWND dl=dragList_?dragList_:items_;
        POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};MapWindowPoints(hwnd_,dl,&pt,1);
        update_drag_insert(pt);
        check_drag_scroll(pt);
        return 0;
    }break;
    case WM_TIMER:if(w==kDragScrollTimerId&&dragging_&&dragScrollDir_){
        HWND dl=dragList_?dragList_:items_;
        if(dragRowHeight_>0)
            SendMessageW(dl,LVM_SCROLL,0,static_cast<LPARAM>(dragScrollDir_*dragRowHeight_));
        POINT p{};GetCursorPos(&p);ScreenToClient(dl,&p);
        update_drag_insert(p);
        check_drag_scroll(p);
        return 0;
    }break;
    case WM_LBUTTONUP:if(dragging_){dragging_=false;ReleaseCapture();if(dragLine_)ShowWindow(dragLine_,SW_HIDE);dragInsertY_=-1;if(dragScrollDir_){KillTimer(hwnd_,kDragScrollTimerId);dragScrollDir_=0;}
        const HWND dl=dragList_?dragList_:items_;
        POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};MapWindowPoints(hwnd_,dl,&pt,1);
        const int count=ListView_GetItemCount(dl);
        int target=-1;
        LVHITTESTINFO ht{};ht.pt=pt;const int row=ListView_HitTest(dl,&ht);
        if(row>=0){
            RECT rr{};if(ListView_GetItemRect(dl,row,&rr,LVIR_BOUNDS))target=row+(pt.y>=(rr.top+rr.bottom)/2?1:0);
        }else if(count>0){
            RECT first{},last{};
            if(ListView_GetItemRect(dl,0,&first,LVIR_BOUNDS)&&ListView_GetItemRect(dl,count-1,&last,LVIR_BOUNDS)){
                if(pt.y<first.top)target=0;
                else if(pt.y>=last.bottom)target=count;
            }
        }else{
            target=0;
        }
        auto*g=selected_group();
        if(dragList_==groups_){
            const int n=static_cast<int>(model_.groups.size());
            if(dragFrom_>=0&&dragFrom_<n&&target>=0){
                target=std::clamp(target,0,n);
                if(target!=dragFrom_&&target!=dragFrom_+1){
                    Group moved=std::move(model_.groups[dragFrom_]);
                    model_.groups.erase(model_.groups.begin()+dragFrom_);
                    if(target>dragFrom_)--target;
                    model_.groups.insert(model_.groups.begin()+target,std::move(moved));
                    const bool saved=commit();refresh_groups(saved?target:-1);
                }
            }
            dragFrom_=-1;dragList_=nullptr;dragRowHeight_=0;return 0;
        }
        if(g&&dragFrom_>=0&&dragFrom_<static_cast<int>(g->items.size())&&target>=0){
            const int n=static_cast<int>(g->items.size());
            target=std::clamp(target,0,n);
            if(target!=dragFrom_&&target!=dragFrom_+1){
                Item moved=std::move(g->items[dragFrom_]);
                g->items.erase(g->items.begin()+dragFrom_);
                if(target>dragFrom_)--target;
                g->items.insert(g->items.begin()+target,std::move(moved));
                const bool saved=commit();refresh_items(saved?target:-1);
            }
        }
        dragFrom_=-1;dragList_=nullptr;dragRowHeight_=0;return 0;
    }break;
    case WM_CAPTURECHANGED:if(dragging_){dragging_=false;dragFrom_=-1;dragList_=nullptr;dragRowHeight_=0;if(dragScrollDir_){KillTimer(hwnd_,kDragScrollTimerId);dragScrollDir_=0;}if(dragLine_)ShowWindow(dragLine_,SW_HIDE);dragInsertY_=-1;}break;
    }
    return DefWindowProcW(h,m,w,l);
}

// 项目列表子类化：滚轮滚动后按当前光标重算插入线，避免线条残留在旧位置。
// 插入线本体由独立窗口 dragLine_ 呈现（不参与列表重绘/滚动）。
LRESULT CALLBACK ManagerWindow::ListViewProc(HWND h,UINT m,WPARAM w,LPARAM l){
    auto*self=reinterpret_cast<ManagerWindow*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_MOUSEWHEEL){
        LRESULT r=self&&self->origItems_?CallWindowProcW(self->origItems_,h,m,w,l):DefWindowProcW(h,m,w,l);
        if(self&&self->dragging_){
            POINT p{};GetCursorPos(&p);ScreenToClient(h,&p);
            self->update_drag_insert(p);
        }
        return r;
    }
    return self&&self->origItems_?CallWindowProcW(self->origItems_,h,m,w,l):DefWindowProcW(h,m,w,l);
}

// 按光标位置重算插入线（pt 为 dragList_ 客户区坐标）：位置变化时移动独立
// 插入线窗口到新位置（屏幕坐标），列表滚动/重绘与插入线互不干扰。
/* STAMKY_CN_DETAIL
 * 拖拽重排只允许 items_ ListView。dragList_ 显式记录本次拖拽来源，mouse capture 失败则立即终止；
 * WM_LBUTTONUP/CancelMode/失焦/DPI 等所有结束路径都会清空拖拽上下文和插入线。这样旧 dragIndex
 * 不会在下一次完全无关的鼠标操作中被复用，造成“看似随机移动项目”。
 */
void ManagerWindow::update_drag_insert(const POINT& pt){
    const HWND dl=dragList_?dragList_:items_;
    int newY=-1;
    LVHITTESTINFO ht{};ht.pt=pt;
    const int row=ListView_HitTest(dl,&ht);
    const int n=ListView_GetItemCount(dl);
    if(row>=0){
        RECT rr{};if(ListView_GetItemRect(dl,row,&rr,LVIR_BOUNDS))newY=(pt.y>=(rr.top+rr.bottom)/2)?rr.bottom:rr.top;
    }else if(n>0){
        // 空白区域只在首行上方/末行下方显示插入线；与鼠标释放时的落点算法保持一致。
        RECT first{},last{};
        if(ListView_GetItemRect(dl,0,&first,LVIR_BOUNDS)&&ListView_GetItemRect(dl,n-1,&last,LVIR_BOUNDS)){
            if(pt.y<first.top)newY=first.top;
            else if(pt.y>=last.bottom)newY=last.bottom;
        }
    }
    if(newY!=dragInsertY_){
        dragInsertY_=newY;
        if(dragLine_){
            if(newY>=0){
                RECT rc{};GetClientRect(dl,&rc);
                POINT org{rc.left,newY};
                ClientToScreen(dl,&org);
                SetWindowPos(dragLine_,HWND_TOPMOST,org.x,org.y,rc.right-rc.left,2,SWP_NOACTIVATE|SWP_SHOWWINDOW);
            }else{
                ShowWindow(dragLine_,SW_HIDE);
            }
        }
    }
}

// 拖动自动滚动：光标进入列表上/下边缘（约两行高）或客户区外时启动慢速滚动，
// 移出触发区停止。滚动由 WM_TIMER 驱动（每 tick 滚一行），避免过度灵敏。
void ManagerWindow::check_drag_scroll(const POINT& pt){
    const HWND dl=dragList_?dragList_:items_;
    RECT rc{};GetClientRect(dl,&rc);
    const int n=ListView_GetItemCount(dl);
    if(n>0&&dragRowHeight_<=0){
        RECT rr0{};ListView_GetItemRect(dl,0,&rr0,LVIR_BOUNDS);
        dragRowHeight_=rr0.bottom-rr0.top;
    }
    int dir=0;
    if(pt.y<rc.top)dir=-1;                       // 上边界外
    else if(pt.y>=rc.bottom)dir=1;               // 下边界外
    else if(dragRowHeight_>0){
        const int zone=dragRowHeight_*2;         // 边缘两行高内触发
        if(pt.y<rc.top+zone)dir=-1;
        else if(pt.y>rc.bottom-zone)dir=1;
    }
    if(dir!=dragScrollDir_){
        if(dir){
            if(!SetTimer(hwnd_,kDragScrollTimerId,kDragScrollIntervalMs,nullptr))dir=0;
        }else KillTimer(hwnd_,kDragScrollTimerId);
        dragScrollDir_=dir;
    }
}
}
