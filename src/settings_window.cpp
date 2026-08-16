/*
 * stamky / settings_window.cpp
 *
 * 非模态设置窗口及内部“文件格式编辑器”。SettingsWindow 始终编辑 draft_，只有用户按确定且
 * settings.ini 原子保存成功后才把 draft_ 提交给真实 Settings；取消不会把预览期间的格式增删
 * 泄漏到当前运行态。这与内容管理的 persistedModel_ 事务理念一致。
 *
 * 格式编辑器使用独立模态消息循环：输入框按 Enter 才解释为“添加格式”，列表上的 Enter 不应
 * 隐式触发删除；Delete/“-”才是删除动作。LB_ERR 必须和有效选中状态区分，避免 Win32 的 -1
 * 返回值被 C++ bool 转换成 true。WM_QUIT 会重新投递给主消息泵。
 *
 * DPI/布局：控件位置按当前窗口 DPI 重算，字体对象在 DPI 变化时重建；右侧/预览等 UI-06
 * 已确认行为只做自适应尺寸和防截断，不在发行注释整理阶段改变交互。所有 CreateWindow/
 * 子类化关键步骤都有失败路径，避免窗口只创建一半仍继续处理消息。
 */

#include "settings_window.h"
#include "lang.h"
#include <uxtheme.h>

namespace sm {
namespace {
constexpr wchar_t kClass[]=L"stamky.Settings";
constexpr wchar_t kPreviewClass[]=L"stamky.SettingsPreview";
constexpr wchar_t kFormatEditorClass[]=L"stamky.FormatEditor";
enum:int{IDC_TITLE=200,IDC_PREVIEW,IDC_FONT_SLIDER,IDC_ICON_SLIDER,IDC_EXTS,IDC_LANG,IDC_SEP,IDC_SAVE,IDC_CANCEL};
enum:int{IDF_LIST=900,IDF_ADD,IDF_INPUT,IDF_OK,IDF_CANCEL};
HMENU cid(int id){return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));}

// 语言代码 → 界面显示名；未知代码直接显示代码本身。
std::wstring lang_display_name(const std::wstring& code){
    if(code==L"cn")return L"中文";
    if(code==L"en")return L"English";
    if(code==L"ja")return L"日本語";
    if(code==L"ko")return L"한국어";
    if(code==L"fr")return L"Français";
    if(code==L"de")return L"Deutsch";
    if(code==L"es")return L"Español";
    if(code==L"ru")return L"Русский";
    return code;
}

// 可启动文件格式编辑器：PATH 式列表（每行一个格式名），底部 + 新建（回车确认），
// 选中按 Delete 删除；确定才写回调用方（取消不残留）。
// 列表区按“资源管理器列表”风格自上而下、从左到右排布；项目过多时自动出现滚动条。
class FormatEditor {
public:
    static bool edit(HWND owner,std::vector<std::wstring>& out){
        FormatEditor ed(owner,out);
        return ed.run();
    }
private:
    HWND owner_=nullptr,hwnd_=nullptr,list_=nullptr,add_=nullptr,input_=nullptr;
    std::vector<std::wstring> work_;   // 本地副本：确定时写回 out_
    std::vector<std::wstring>& out_;
    HFONT font_=nullptr;
    WNDPROC origList_=nullptr;
    bool ok_=false;

    FormatEditor(HWND owner,std::vector<std::wstring>& out):owner_(owner),out_(out){work_=out;}
    ~FormatEditor(){if(hwnd_)DestroyWindow(hwnd_);if(font_)DeleteObject(font_);}

/* STAMKY_CN_DETAIL
 * FormatEditor::run 是真正的模态子循环。禁用 owner 后创建编辑窗口，循环同时处理 WM_QUIT/-1；
 * 收到 WM_QUIT 必须在退出子循环后重新 PostQuitMessage，把“应用正在退出”的语义传回外层消息循环。
 * 不能把 WM_QUIT 当普通窗口关闭吞掉。
 */
    bool run(){
        WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=WndProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=kFormatEditorClass;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(wc.hInstance,MAKEINTRESOURCEW(IDI_STAMKY));
        wc.hbrBackground=reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW+1));
        if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
        EnableWindow(owner_,FALSE);
        hwnd_=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_WINDOWEDGE,kFormatEditorClass,tr(L"可启动文件格式").c_str(),
                              WS_POPUP|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,420,500,
                              owner_,nullptr,wc.hInstance,this);
        if(!hwnd_){EnableWindow(owner_,TRUE);return false;}
        SetWindowPos(hwnd_,nullptr,0,0,dip(hwnd_,420),dip(hwnd_,500),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        if(!create_controls()){DestroyWindow(hwnd_);EnableWindow(owner_,TRUE);return false;}
        center();
        ShowWindow(hwnd_,SW_SHOWNORMAL);SetForegroundWindow(hwnd_);
        MSG msg{};
        int gm=0;
        while(hwnd_&&IsWindow(hwnd_)&&(gm=GetMessageW(&msg,nullptr,0,0))>0){
            if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE){DestroyWindow(hwnd_);continue;}
            if(msg.message==WM_KEYDOWN&&msg.wParam==VK_RETURN&&msg.hwnd==input_){add_from_input();continue;}
            if(!IsDialogMessageW(hwnd_,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        }
        if(gm==0) PostQuitMessage(static_cast<int>(msg.wParam));
        else if(gm<0) startup_log(L"FormatEditor GetMessageW 失败："+win32_error_text(GetLastError()));
        EnableWindow(owner_,TRUE);SetForegroundWindow(owner_);
        return ok_;
    }
    bool create_controls(){
        auto inst=GetModuleHandleW(nullptr);
        auto label=[&](const wchar_t*t){return CreateWindowW(L"STATIC",t,WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd_,nullptr,inst,nullptr);};
        label(tr(L"可启动文件格式").c_str());
        label(tr(L"每行一个格式名（如 lnk、jar）。点 + 新建（输入后回车）；选中后按钮变 -，点击或按 Delete 删除，支持 Ctrl/Shift 多选。").c_str());
        list_=CreateWindowExW(WS_EX_CLIENTEDGE,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_HSCROLL|LBS_EXTENDEDSEL|LBS_NOTIFY|LBS_MULTICOLUMN|LBS_NOINTEGRALHEIGHT,0,0,0,0,hwnd_,cid(IDF_LIST),inst,nullptr);
        // "+" 设为默认按钮：在输入框按回车即触发新建；有选中项时按钮文字变 "-" 并执行删除。
        add_=CreateWindowW(L"BUTTON",L"+",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON|BS_FLAT,0,0,0,0,hwnd_,cid(IDF_ADD),inst,nullptr);
        SetWindowTheme(add_,L"Explorer",nullptr);
        input_=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,hwnd_,cid(IDF_INPUT),inst,nullptr);
        CreateWindowW(L"BUTTON",tr(L"确定").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON|BS_FLAT,0,0,0,0,hwnd_,cid(IDF_OK),inst,nullptr);
        CreateWindowW(L"BUTTON",tr(L"取消").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON|BS_FLAT,0,0,0,0,hwnd_,cid(IDF_CANCEL),inst,nullptr);
        SetWindowTheme(list_,L"Explorer",nullptr);
        SetWindowTheme(GetDlgItem(hwnd_,IDF_OK),L"Explorer",nullptr);
        SetWindowTheme(GetDlgItem(hwnd_,IDF_CANCEL),L"Explorer",nullptr);
        // 子类化列表：拦截 Delete 键删除选中项。
        if(!list_||!add_||!input_||!GetDlgItem(hwnd_,IDF_OK)||!GetDlgItem(hwnd_,IDF_CANCEL))return false;
        int childCount=0;for(HWND c=GetWindow(hwnd_,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT))++childCount;
        if(childCount!=7)return false;
        SetLastError(ERROR_SUCCESS);
        origList_=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(list_,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ListProc)));
        if(!origList_&&GetLastError()!=ERROR_SUCCESS)return false;
        SetWindowLongPtrW(list_,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        apply_font();
        for(auto&e:work_){
            if(SendMessageW(list_,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(e.c_str()))<0)return false;
        }
        layout();
        update_add_button();
        return true;
    }
    void apply_font(){
        if(font_)DeleteObject(font_);
        font_=create_ui_font(dpi_for_window(hwnd_),9);
        for(HWND c=GetWindow(hwnd_,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT))SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
    }
    void layout(){
        // 布局（创建顺序：标题、说明、列表、+、输入框、确定、取消）
        RECT r{};GetClientRect(hwnd_,&r);
        const int m=dip(hwnd_,20),eh=dip(hwnd_,28),gap=dip(hwnd_,12);
        HWND c=GetWindow(hwnd_,GW_CHILD);
        int y=m;
        SetWindowPos(c,nullptr,m,y,r.right-m*2,dip(hwnd_,30),SWP_NOZORDER);
        c=GetWindow(c,GW_HWNDNEXT);y+=dip(hwnd_,38);

        const int noteW=r.right-m*2;
        int noteH=dip(hwnd_,40);
        {
            HWND note=c;
            wchar_t nbuf[1024]{};
            GetWindowTextW(note,nbuf,static_cast<int>(_countof(nbuf)));
            RECT nr{0,0,noteW,dip(hwnd_,800)};
            HDC dc=GetDC(hwnd_);
            if(dc){
                HFONT oldF=font_?static_cast<HFONT>(SelectObject(dc,font_)):nullptr;
                DrawTextW(dc,nbuf,-1,&nr,DT_CALCRECT|DT_WORDBREAK);
                if(oldF)SelectObject(dc,oldF);
                ReleaseDC(hwnd_,dc);
            }
            noteH=std::max(noteH,static_cast<int>(nr.bottom-nr.top)+dip(hwnd_,4));
            SetWindowPos(note,nullptr,m,y,noteW,noteH,SWP_NOZORDER);
        }
        // 利用原先列表下方的大块空白：列表及新增控件整体略向下移，列表本身小幅增高。
        const int listTopGap=dip(hwnd_,20);
        const int listH=dip(hwnd_,200);
        c=GetWindow(c,GW_HWNDNEXT);y+=noteH+listTopGap;

        HWND addBtn=GetWindow(c,GW_HWNDNEXT);
        HWND input=GetWindow(addBtn,GW_HWNDNEXT);
        HWND okBtn=GetWindow(input,GW_HWNDNEXT);
        HWND cancelBtn=GetWindow(okBtn,GW_HWNDNEXT);

        SetWindowPos(c,nullptr,m,y,r.right-m*2,listH,SWP_NOZORDER);
        const int rowY=y+listH+gap;
        const int addW=dip(hwnd_,72);
        SetWindowPos(addBtn,nullptr,m,rowY,addW,eh,SWP_NOZORDER);
        SetWindowPos(input,nullptr,m+addW+gap,rowY,r.right-m*2-addW-gap,eh,SWP_NOZORDER);

        const int by=r.bottom-m-dip(hwnd_,32);
        SetWindowPos(okBtn,nullptr,r.right-m-dip(hwnd_,164),by,dip(hwnd_,78),dip(hwnd_,32),SWP_NOZORDER);
        SetWindowPos(cancelBtn,nullptr,r.right-m-dip(hwnd_,78),by,dip(hwnd_,78),dip(hwnd_,32),SWP_NOZORDER);
        update_list_column_width();
    }
    void update_list_column_width(){
        if(!list_)return;
        int maxW=dip(hwnd_,90);
        HDC dc=GetDC(list_);
        if(dc){
            HFONT oldF=font_?static_cast<HFONT>(SelectObject(dc,font_)):nullptr;
            for(const auto& e:work_){
                SIZE sz{};
                if(!e.empty())GetTextExtentPoint32W(dc,e.c_str(),static_cast<int>(e.size()),&sz);
                maxW=std::max(maxW,static_cast<int>(sz.cx)+dip(hwnd_,24));
            }
            if(oldF)SelectObject(dc,oldF);
            ReleaseDC(list_,dc);
        }
        maxW=std::min(maxW,dip(hwnd_,220));
        SendMessageW(list_,LB_SETCOLUMNWIDTH,maxW,0);
        InvalidateRect(list_,nullptr,TRUE);
    }
    void center(){
        RECT wr{},orc{};GetWindowRect(hwnd_,&wr);GetWindowRect(owner_,&orc);
        const int ww=wr.right-wr.left,wh=wr.bottom-wr.top;
        int x=orc.left+((orc.right-orc.left)-ww)/2,y=orc.top+((orc.bottom-orc.top)-wh)/2;
        HMONITOR mon=MonitorFromRect(&orc,MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(mon,&mi)){mi.rcWork={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};}
        x=std::clamp(x,static_cast<int>(mi.rcWork.left),std::max(static_cast<int>(mi.rcWork.left),static_cast<int>(mi.rcWork.right)-ww));
        y=std::clamp(y,static_cast<int>(mi.rcWork.top),std::max(static_cast<int>(mi.rcWork.top),static_cast<int>(mi.rcWork.bottom)-wh));
        SetWindowPos(hwnd_,nullptr,x,y,ww,wh,SWP_NOZORDER|SWP_NOSIZE);
    }
    void add_from_input(){
        wchar_t buf[128]{};
        GetWindowTextW(input_,buf,static_cast<int>(_countof(buf)));
        std::wstring s=normalize_launch_extension(buf);
        if(s.empty())return;
        for(auto&e:work_) if(e==s)return;   // 已存在（work_ 均小写规范化）
        if(work_.size()>=kMaxLaunchExtensions)return;
        if(SendMessageW(list_,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(s.c_str()))<0)return;
        work_.push_back(s);
        update_list_column_width();
        SetWindowTextW(input_,L"");
        update_add_button();
        SetFocus(input_);
    }
    // 有选中项时按钮显示 "-"（删除），否则 "+"（新建）。
    void update_add_button(){
        SetWindowTextW(add_,SendMessageW(list_,LB_GETSELCOUNT,0,0)>0?L"-":L"+");
    }
/* STAMKY_CN_DETAIL
 * “+/-”共用一个按钮，但键盘 Enter 只在扩展名输入框内表示“提交新增”。当列表已有选中项、按钮文本
 * 变成“-”时，列表上的 Enter 不得隐式执行删除；删除仅来自显式点击“-”或 Delete 键。
 * 这是为了保持界面提示与实际 destructive action 一致。
 */
    void on_add_clicked(){
        if(SendMessageW(list_,LB_GETSELCOUNT,0,0)>0){delete_selected();return;}
        wchar_t buf[128]{};
        if(GetWindowTextW(input_,buf,static_cast<int>(_countof(buf)))>0)add_from_input();
        else SetFocus(input_);   // 点 + 后聚焦输入框，输入完按回车即新建
    }
    // 删除全部选中项（支持 Ctrl/Shift 多选），从高索引到低索引逐个删除。
    void delete_selected(){
        const int n=static_cast<int>(work_.size());
        bool any=false;
        for(int i=n-1;i>=0;--i){
            const LRESULT selected=SendMessageW(list_,LB_GETSEL,static_cast<WPARAM>(i),0);
            if(selected>0){
                if(SendMessageW(list_,LB_DELETESTRING,static_cast<WPARAM>(i),0)==LB_ERR)continue;
                work_.erase(work_.begin()+i);
                any=true;
            }
        }
        if(any){update_list_column_width();update_add_button();}
    }
    static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        auto*self=reinterpret_cast<FormatEditor*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<FormatEditor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);if(self)self->hwnd_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        return self?self->proc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }
    static LRESULT CALLBACK ListProc(HWND h,UINT m,WPARAM w,LPARAM l){
        auto*self=reinterpret_cast<FormatEditor*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(self&&m==WM_KEYDOWN&&w==VK_DELETE){self->delete_selected();return 0;}
        return self&&self->origList_?CallWindowProcW(self->origList_,h,m,w,l):DefWindowProcW(h,m,w,l);
    }
    LRESULT proc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_CTLCOLORSTATIC:{HDC dc=reinterpret_cast<HDC>(w);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));}
        case WM_DPICHANGED:{
            auto*r=reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
            apply_font();layout();return 0;
        }
        case WM_COMMAND:switch(LOWORD(w)){
            case IDF_LIST:if(HIWORD(w)==LBN_SELCHANGE){update_add_button();return 0;}break;
            case IDF_ADD:on_add_clicked();return 0;
            case IDF_OK:out_=work_.empty()?default_launch_extensions():work_;ok_=true;DestroyWindow(hwnd_);return 0;
            case IDF_CANCEL:DestroyWindow(hwnd_);return 0;
        }break;
        case WM_CLOSE:DestroyWindow(hwnd_);return 0;
        case WM_DESTROY:hwnd_=nullptr;return 0;
        case WM_NCDESTROY:SetWindowLongPtrW(h,GWLP_USERDATA,0);break;
        }
        return DefWindowProcW(h,m,w,l);
    }
};
}
SettingsWindow::~SettingsWindow(){if(hwnd_)DestroyWindow(hwnd_);if(font_)DeleteObject(font_);if(titleFont_)DeleteObject(titleFont_);}
void SettingsWindow::show(HWND owner){if(!hwnd_&&!create(owner))return;ShowWindow(hwnd_,SW_SHOWNORMAL);SetForegroundWindow(hwnd_);}
/* STAMKY_CN_DETAIL
 * SettingsWindow 打开时先 settings_ -> draft_。所有控件和 FormatEditor 只修改 draft_；只有“保存”
 * 且 draft_.save() 成功后才回写 settings_。Cancel/Esc/窗口 X 因此只销毁草稿，不会把临时格式列表
 * 泄漏到当前 Runtime 状态。
 */
bool SettingsWindow::create(HWND owner){draft_=settings_;languageCodes_.clear();WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=WndProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=kClass;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_STAMKY));wc.hbrBackground=reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW+1));if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
    // 设置页使用普通对话框式标题栏，而不是 WS_EX_TOOLWINDOW 的工具窗标题栏。
    // 这样右上角关闭按钮按系统正常尺寸绘制；窗口仍然是 owner 的从属窗口，不单独占任务栏。
    hwnd_=CreateWindowExW(WS_EX_CONTROLPARENT,kClass,tr(L"stamky - 设置").c_str(),WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,520,500,owner,nullptr,wc.hInstance,this);if(!hwnd_)return false;
    // 始终居中到鼠标所在显示器的工作区
    POINT cur{};GetCursorPos(&cur);
    HMONITOR mon=MonitorFromPoint(cur,MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(mon,&mi))mi.rcWork={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    const int ww=dip(hwnd_,520),wh=dip(hwnd_,500);
    const int x=static_cast<int>(mi.rcWork.left)+(static_cast<int>(mi.rcWork.right-mi.rcWork.left)-ww)/2;
    const int y=static_cast<int>(mi.rcWork.top)+(static_cast<int>(mi.rcWork.bottom-mi.rcWork.top)-wh)/2;
    SetWindowPos(hwnd_,nullptr,x,y,ww,wh,SWP_NOZORDER|SWP_NOACTIVATE);
    auto inst=wc.hInstance;auto label=[&](const wchar_t*t){return CreateWindowW(L"STATIC",t,WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd_,nullptr,inst,nullptr);};
    CreateWindowW(L"STATIC",tr(L"外观与常规").c_str(),WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd_,cid(IDC_TITLE),inst,nullptr);
    label(tr(L"菜单文字大小（pt）").c_str());
    CreateWindowExW(0,TRACKBAR_CLASS,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|TBS_AUTOTICKS|TBS_HORZ,0,0,0,0,hwnd_,cid(IDC_FONT_SLIDER),inst,nullptr);
    label(tr(L"菜单图标大小（DIP）").c_str());
    CreateWindowExW(0,TRACKBAR_CLASS,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|TBS_AUTOTICKS|TBS_HORZ,0,0,0,0,hwnd_,cid(IDC_ICON_SLIDER),inst,nullptr);
    label(tr(L"可启动文件格式").c_str());
    CreateWindowW(L"BUTTON",tr(L"编辑格式…").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_PUSHBUTTON|BS_FLAT,0,0,0,0,hwnd_,cid(IDC_EXTS),inst,nullptr);
    SetWindowTheme(GetDlgItem(hwnd_,IDC_EXTS),L"Explorer",nullptr);
    label(tr(L"界面语言").c_str());
    CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,0,0,hwnd_,cid(IDC_LANG),inst,nullptr);
    CreateWindowW(L"BUTTON",tr(L"多列之间显示分隔线").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,0,0,0,0,hwnd_,cid(IDC_SEP),inst,nullptr);
    label(tr(L"菜单列数不再人为限制：程序会根据当前显示器工作区、文字和图标尺寸自动决定。极端超长列表仅在宽度也不足时启用原生滚动。").c_str());
    WNDCLASSEXW pw{sizeof(pw)};pw.lpfnWndProc=PreviewWndProc;pw.hInstance=wc.hInstance;pw.lpszClassName=kPreviewClass;pw.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassExW(&pw)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS){DestroyWindow(hwnd_);return false;}
    preview_=CreateWindowExW(0,kPreviewClass,L"",WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd_,cid(IDC_PREVIEW),wc.hInstance,this);
    if(!preview_){DestroyWindow(hwnd_);return false;}
    CreateWindowW(L"BUTTON",tr(L"保存").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,0,0,0,0,hwnd_,cid(IDC_SAVE),inst,nullptr);
    CreateWindowW(L"BUTTON",tr(L"取消").c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,hwnd_,cid(IDC_CANCEL),inst,nullptr);
    for(int id : {IDC_TITLE,IDC_FONT_SLIDER,IDC_ICON_SLIDER,IDC_EXTS,IDC_LANG,IDC_SEP,IDC_PREVIEW,IDC_SAVE,IDC_CANCEL})
        if(!GetDlgItem(hwnd_,id)){DestroyWindow(hwnd_);return false;}
    int childCount=0;for(HWND c=GetWindow(hwnd_,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT))++childCount;
    if(childCount!=14){DestroyWindow(hwnd_);return false;}
    for(int id : {IDC_SAVE,IDC_CANCEL}){if(HWND b=GetDlgItem(hwnd_,id)){SendMessageW(b,BM_SETSTYLE,BS_PUSHBUTTON|BS_FLAT|(id==IDC_SAVE?BS_DEFPUSHBUTTON:0),TRUE);SetWindowTheme(b,L"Explorer",nullptr);}}
    apply_font();layout();load_controls();return true;}
LRESULT CALLBACK SettingsWindow::WndProc(HWND h,UINT m,WPARAM w,LPARAM l){auto*self=reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);if(self)self->hwnd_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}return self?self->proc(h,m,w,l):DefWindowProcW(h,m,w,l);}

LRESULT CALLBACK SettingsWindow::PreviewWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    auto*self=reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if(m==WM_NCCREATE){
        self=static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        if(self)SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    }
    if(m==WM_PAINT){
        PAINTSTRUCT ps{};
        HDC dc=BeginPaint(h,&ps);
        if(self)self->draw_preview(dc,h);
        EndPaint(h,&ps);
        return 0;
    }
    if(m==WM_ERASEBKGND) return 1;   // 自绘背景，避免闪烁
    return DefWindowProcW(h,m,w,l);
}

void SettingsWindow::draw_preview(HDC dc,HWND hwnd) const {
    RECT rc{};GetClientRect(hwnd,&rc);
    // 模拟菜单项：白底 + 边框，项目图标 + 文字，尺寸实时跟随设置。
    FillRect(dc,&rc,reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW+1)));
    HPEN pen=CreatePen(PS_SOLID,1,GetSysColor(COLOR_3DSHADOW));
    if(pen){
        HPEN oldPen=static_cast<HPEN>(SelectObject(dc,pen));
        HGDIOBJ oldBr=SelectObject(dc,GetStockObject(NULL_BRUSH));
        Rectangle(dc,rc.left,rc.top,rc.right,rc.bottom);
        SelectObject(dc,oldBr);
        SelectObject(dc,oldPen);
        DeleteObject(pen);
    }

    UINT dpi=dpi_for_window(hwnd_);
    // 预览直接读取两条滑块的实时位置。数值输入框已删除，尺寸只保留一套交互来源，
    // 避免“滑块 + 手填数字”重复表达同一设置。
    int pt=static_cast<int>(SendMessageW(GetDlgItem(hwnd_,IDC_FONT_SLIDER),TBM_GETPOS,0,0));
    if(pt<8||pt>20) pt=draft_.menuFontPoints;
    int iconLogical=static_cast<int>(SendMessageW(GetDlgItem(hwnd_,IDC_ICON_SLIDER),TBM_GETPOS,0,0));
    if(iconLogical<16||iconLogical>64) iconLogical=draft_.iconLogicalSize;
    const int pad=MulDiv(12,static_cast<int>(dpi),96);
    const int iconPx=std::clamp(MulDiv(iconLogical,static_cast<int>(dpi),96),16,128);
    HICON icon=LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_STAMKY));
    HFONT f=create_ui_font(dpi,pt);
    HGDIOBJ oldFont=f?SelectObject(dc,f):nullptr;
    const int cy=(rc.top+rc.bottom)/2;
    int x=rc.left+pad;
    if(icon)DrawIconEx(dc,x,cy-iconPx/2,icon,iconPx,iconPx,0,nullptr,DI_NORMAL);
    x+=iconPx+pad;
    RECT tr{x,rc.top,rc.right-pad,rc.bottom};
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
    DrawTextW(dc,L"stamky",-1,&tr,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
    if(oldFont)SelectObject(dc,oldFont);
    if(f)DeleteObject(f);
}
void SettingsWindow::apply_font(){if(font_)DeleteObject(font_);if(titleFont_)DeleteObject(titleFont_);UINT dpi=dpi_for_window(hwnd_);font_=create_ui_font(dpi,9);titleFont_=CreateFontW(-MulDiv(14,static_cast<int>(dpi),72),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");for(HWND c=GetWindow(hwnd_,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT))SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);SendMessageW(GetDlgItem(hwnd_,IDC_TITLE),WM_SETFONT,reinterpret_cast<WPARAM>(titleFont_),TRUE);}
/* STAMKY_CN_DETAIL
 * 设置页布局继续遵守 UI-06：字体/图标 Trackbar、格式按钮和紧凑预览的位置关系被冻结。
 * WM_DPICHANGED 会先采用系统建议的新窗口 RECT，再重建字体并按新 DPI 重新布局；不沿用旧像素坐标，
 * 以免跨 100%/125%/150% 显示器后控件逐步漂移或裁切。
 */
void SettingsWindow::layout(){
    RECT r{};GetClientRect(hwnd_,&r);
    // Win32 RECT 成员类型是 LONG；布局计算统一先收敛为 int，避免 MSVC 的
    // std::min/std::max 模板在 LONG 与 int 混用时产生 C2672/C2737 级联错误。
    const int clientW=static_cast<int>(r.right-r.left);
    const int clientH=static_cast<int>(r.bottom-r.top);
    const int m=dip(hwnd_,20),lh=dip(hwnd_,24),eh=dip(hwnd_,28),gap=dip(hwnd_,12);
    int y=m;
    SetWindowPos(GetDlgItem(hwnd_,IDC_TITLE),nullptr,m,y,dip(hwnd_,300),dip(hwnd_,34),SWP_NOZORDER);
    y+=dip(hwnd_,42);

    // 顶部区域使用“紧凑预览 + 两组尺寸设置”并排布局。
    // 左侧预览保证 64 DIP 图标 + 20pt 文字仍能完整显示；右侧两条滑块直接占满设置区。
    const int previewH=dip(hwnd_,96);
    const int sideW=dip(hwnd_,190);
    const int innerGap=dip(hwnd_,12);
    const int previewW=std::max(dip(hwnd_,220),clientW-m*2-innerGap-sideW);
    const int sideX=m+previewW+innerGap;
    SetWindowPos(preview_,nullptr,m,y,previewW,previewH,SWP_NOZORDER);

    HWND c=GetWindow(GetDlgItem(hwnd_,IDC_TITLE),GW_HWNDNEXT);
    // UI-04：删除两只重复的数字输入框，让两条 Trackbar 直接占满右侧设置区。
    // 这样视觉上更干净，也把原先被 48 DIP 数值框和间距占掉的宽度全部还给滑块。
    const int sliderW=sideW;
    for(int row=0;row<2;++row){
        HWND lab=c;c=GetWindow(c,GW_HWNDNEXT);
        HWND sl=c;c=GetWindow(c,GW_HWNDNEXT);
        const int rowY=y+row*dip(hwnd_,48);
        SetWindowPos(lab,nullptr,sideX,rowY,sideW,lh,SWP_NOZORDER);
        SetWindowPos(sl,nullptr,sideX,rowY+dip(hwnd_,20),sliderW,eh,SWP_NOZORDER);
    }
    y+=previewH+gap;

    {HWND lab=c;c=GetWindow(c,GW_HWNDNEXT);HWND btn=c;c=GetWindow(c,GW_HWNDNEXT);
        SetWindowPos(lab,nullptr,m,y,dip(hwnd_,210),lh,SWP_NOZORDER);
        const int bw=fit_text_width(btn,font_,dip(hwnd_,100),dip(hwnd_,240));
        const int controlX=m+dip(hwnd_,240);
        const int availableW=std::max(dip(hwnd_,100),clientW-m-controlX);
        SetWindowPos(btn,nullptr,controlX,y,std::min(bw,availableW),eh,SWP_NOZORDER);
        y+=eh+gap;}
    {HWND lab=c;c=GetWindow(c,GW_HWNDNEXT);HWND combo=c;c=GetWindow(c,GW_HWNDNEXT);
        SetWindowPos(lab,nullptr,m,y,dip(hwnd_,210),lh,SWP_NOZORDER);
        const int controlX=m+dip(hwnd_,240);
        const int comboW=std::max(dip(hwnd_,120),clientW-m-controlX);
        SetWindowPos(combo,nullptr,controlX,y,comboW,eh,SWP_NOZORDER);
        y+=eh+gap;}
    SetWindowPos(c,nullptr,m,y,std::max(dip(hwnd_,220),clientW-m*2),lh,SWP_NOZORDER);
    c=GetWindow(c,GW_HWNDNEXT);y+=lh+gap;

    // 说明文字高度按实际换行测量（不同语言长度不同，固定高度会裁掉超行）
    {
        HWND note=c;
        wchar_t nbuf[1024]{};
        GetWindowTextW(note,nbuf,static_cast<int>(_countof(nbuf)));
        const int noteW=clientW-m*2;
        RECT nr{0,0,noteW,dip(hwnd_,600)};
        HDC dc=GetDC(hwnd_);
        if(dc){
            HFONT oldF=font_?static_cast<HFONT>(SelectObject(dc,font_)):nullptr;
            DrawTextW(dc,nbuf,-1,&nr,DT_CALCRECT|DT_WORDBREAK);
            if(oldF)SelectObject(dc,oldF);
            ReleaseDC(hwnd_,dc);
        }
        SetWindowPos(note,nullptr,m,y,noteW,std::max(dip(hwnd_,48),static_cast<int>(nr.bottom-nr.top)+dip(hwnd_,4)),SWP_NOZORDER);
    }
    const int by=clientH-m-dip(hwnd_,32);
    SetWindowPos(GetDlgItem(hwnd_,IDC_SAVE),nullptr,clientW-m-dip(hwnd_,164),by,dip(hwnd_,78),dip(hwnd_,32),SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd_,IDC_CANCEL),nullptr,clientW-m-dip(hwnd_,78),by,dip(hwnd_,78),dip(hwnd_,32),SWP_NOZORDER);
}
void SettingsWindow::load_controls(){
    SendMessageW(GetDlgItem(hwnd_,IDC_FONT_SLIDER),TBM_SETRANGE,TRUE,MAKELONG(8,20));
    SendMessageW(GetDlgItem(hwnd_,IDC_FONT_SLIDER),TBM_SETPAGESIZE,0,4);
    SendMessageW(GetDlgItem(hwnd_,IDC_ICON_SLIDER),TBM_SETRANGE,TRUE,MAKELONG(16,64));
    SendMessageW(GetDlgItem(hwnd_,IDC_ICON_SLIDER),TBM_SETPAGESIZE,0,8);
    SendMessageW(GetDlgItem(hwnd_,IDC_ICON_SLIDER),TBM_SETTICFREQ,4,0);
    SendMessageW(GetDlgItem(hwnd_,IDC_FONT_SLIDER),TBM_SETPOS,TRUE,draft_.menuFontPoints);
    SendMessageW(GetDlgItem(hwnd_,IDC_ICON_SLIDER),TBM_SETPOS,TRUE,draft_.iconLogicalSize);
    SendDlgItemMessageW(hwnd_,IDC_SEP,BM_SETCHECK,draft_.columnSeparator?BST_CHECKED:BST_UNCHECKED,0);
    // 语言下拉：枚举 data\lang\*.ini
    HWND langCombo=GetDlgItem(hwnd_,IDC_LANG);
    SendMessageW(langCombo,CB_RESETCONTENT,0,0);
    int sel=0;
    languageCodes_=lang().available_languages();
    for(size_t i=0;i<languageCodes_.size();++i){
        const std::wstring name=lang_display_name(languageCodes_[i]);
        SendMessageW(langCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
        if(_wcsicmp(languageCodes_[i].c_str(),draft_.language.c_str())==0)sel=static_cast<int>(i);
    }
    if(!languageCodes_.empty()) SendMessageW(langCombo,CB_SETCURSEL,sel,0);
    InvalidateRect(preview_,nullptr,TRUE);
}
/* STAMKY_CN_DETAIL
 * 控件值先集中收集到 draft_ 并统一验证，再调用 Settings::save 原子持久化；只有成功才更新 settings_。
 * 这让“磁盘写失败”与“用户按 Cancel”具有同样的隔离效果：全局运行设置仍是上一次成功保存的版本。
 */
bool SettingsWindow::save_controls(){
    const int font=static_cast<int>(SendMessageW(GetDlgItem(hwnd_,IDC_FONT_SLIDER),TBM_GETPOS,0,0));
    draft_.menuFontPoints=std::clamp(font,8,20);
    const int icon=static_cast<int>(SendMessageW(GetDlgItem(hwnd_,IDC_ICON_SLIDER),TBM_GETPOS,0,0));
    draft_.iconLogicalSize=std::clamp(icon,16,64);
    draft_.columnSeparator=SendDlgItemMessageW(hwnd_,IDC_SEP,BM_GETCHECK,0,0)==BST_CHECKED;
    const int li=static_cast<int>(SendMessageW(GetDlgItem(hwnd_,IDC_LANG),CB_GETCURSEL,0,0));
    if(li>=0&&li<static_cast<int>(languageCodes_.size())) draft_.language=languageCodes_[li];
    if(!draft_.save()){
        MessageBoxW(hwnd_,tr(L"保存设置失败。请检查 data 目录是否可写。").c_str(),tr(L"stamky").c_str(),MB_OK|MB_ICONERROR);
        return false;
    }
    const bool languageChanged=_wcsicmp(settings_.language.c_str(),draft_.language.c_str())!=0;
    settings_=draft_;
    if(languageChanged) lang().load(settings_.language);
    return true;
}
bool SettingsWindow::pretranslate(MSG& msg){if(!hwnd_||!IsWindow(hwnd_))return false;if(msg.hwnd!=hwnd_&&!IsChild(hwnd_,msg.hwnd))return false;if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE){DestroyWindow(hwnd_);return true;}return IsDialogMessageW(hwnd_,&msg)!=FALSE;}
LRESULT SettingsWindow::proc(HWND h,UINT m,WPARAM w,LPARAM l){switch(m){case WM_CTLCOLORSTATIC:{HDC dc=reinterpret_cast<HDC>(w);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));}case WM_DPICHANGED:{auto*r=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);apply_font();layout();InvalidateRect(preview_,nullptr,TRUE);return 0;}case WM_HSCROLL:{HWND sl=reinterpret_cast<HWND>(l);if(sl==GetDlgItem(hwnd_,IDC_FONT_SLIDER)||sl==GetDlgItem(hwnd_,IDC_ICON_SLIDER))InvalidateRect(preview_,nullptr,TRUE);return 0;}case WM_COMMAND:switch(LOWORD(w)){case IDC_EXTS:FormatEditor::edit(hwnd_,draft_.launchExtensions);return 0;case IDC_SAVE:if(save_controls())DestroyWindow(hwnd_);return 0;case IDC_CANCEL:DestroyWindow(hwnd_);return 0;}break;case WM_CLOSE:DestroyWindow(hwnd_);return 0;case WM_DESTROY:hwnd_=nullptr;if(font_){DeleteObject(font_);font_=nullptr;}if(titleFont_){DeleteObject(titleFont_);titleFont_=nullptr;}return 0;case WM_NCDESTROY:SetWindowLongPtrW(h,GWLP_USERDATA,0);break;}return DefWindowProcW(h,m,w,l);}
}
