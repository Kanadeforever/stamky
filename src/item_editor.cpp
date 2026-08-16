/*
 * stamky / item_editor.cpp
 *
 * 单个项目的模态编辑器。编辑时先复制 Item 到局部工作副本，只有用户明确按“确定”且字段
 * 验证通过后才覆盖调用方对象；取消、窗口关闭、控件创建失败都保持原 Item 不变。
 *
 * 模态实现仍使用当前 GUI 线程的 GetMessage/IsDialogMessage 循环，因此 Shell 文件选择器、
 * MessageBox 等可能形成嵌套消息循环。代码不在这些调用前后长期持有 Model vector 元素地址；
 * WM_QUIT 会重新投递给外层主消息泵，避免关闭应用的意图被内部编辑器吞掉。
 *
 * Shell/COM：IFileOpenDialog 的 SetOptions/SetFileTypes/Show/GetResult 每一步均检查 HRESULT；
 * COM 接口按成功取得的所有权逐一 Release。文件过滤规则与 Settings 的启动扩展名共用同一语义。
 */

#include "item_editor.h"
#include "lang.h"
#include "shell_utils.h"
#include <uxtheme.h>

namespace sm {
namespace {
constexpr wchar_t kClass[] = L"stamky.ItemEditor";
// 注意：不能命名 IDC_ICON——与 winuser.h 的 #define IDC_ICON 宏冲突（RC1 历史教训），
// 会导致枚举被预处理器替换成数字、级联语法错误。
enum : int { IDC_NAME = 600, IDC_PATH, IDC_ARGS, IDC_WORKDIR, IDC_ICONPATH, IDC_BROWSE, IDC_BROWSEDIR, IDC_BROWSEICON, IDC_CLEARICON, IDC_OK, IDC_CANCEL };
HMENU cid(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

class EditorImpl {
public:
    EditorImpl(HWND owner, Item& item, bool isNew, const std::vector<std::wstring>* launchExts)
        : owner_(owner), item_(item), isNew_(isNew), launchExts_(launchExts) {}
/* STAMKY_CN_DETAIL
 * ItemEditor 采用“编辑副本、确认再提交”：run() 的窗口/模态循环只操作 work_，并在 owner 禁用期间
 * 正确转发 WM_QUIT。任何 CreateWindow/GetMessage 错误都使本次编辑失败，调用方原 Item 保持不变。
 */
    bool run() {
        if(!register_class())return false;
        EnableWindow(owner_, FALSE);
        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE, kClass,
                                tr(isNew_ ? L"添加项目" : L"项目属性").c_str(),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 330,
                                owner_, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) { EnableWindow(owner_, TRUE); return false; }
        SetWindowPos(hwnd_, nullptr, 0, 0, dip(hwnd_, 640), dip(hwnd_, 408), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        if(!create_controls()){DestroyWindow(hwnd_);EnableWindow(owner_,TRUE);return false;}
        center_to_owner();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        MSG msg{};
        int gm=0;
        while (hwnd_ && IsWindow(hwnd_) && (gm=GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { DestroyWindow(hwnd_); continue; }
            if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if(gm==0) PostQuitMessage(static_cast<int>(msg.wParam));
        else if(gm<0) startup_log(L"ItemEditor GetMessageW 失败："+win32_error_text(GetLastError()));
        EnableWindow(owner_, TRUE);
        SetForegroundWindow(owner_);
        return accepted_;
    }
private:
    HWND owner_ = nullptr, hwnd_ = nullptr;
    Item& item_;
    bool isNew_ = false, accepted_ = false;
    const std::vector<std::wstring>* launchExts_ = nullptr;
    HFONT font_ = nullptr;

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<EditorImpl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            self = static_cast<EditorImpl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd_ = h;
        }
        return self ? self->proc(h, m, w, l) : DefWindowProcW(h, m, w, l);
    }
    bool register_class() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
        if(RegisterClassExW(&wc))return true;
        return GetLastError()==ERROR_CLASS_ALREADY_EXISTS;
    }
    bool create_controls() {
        const auto inst = GetModuleHandleW(nullptr);
        auto label = [&](const wchar_t* t) { return CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd_, nullptr, inst, nullptr); };
        label(tr(L"显示名称").c_str());
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", item_.name.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        0,0,0,0, hwnd_, cid(IDC_NAME), inst, nullptr);
        label(tr(L"路径 / URL").c_str());
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", item_.path.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        0,0,0,0, hwnd_, cid(IDC_PATH), inst, nullptr);
        CreateWindowW(L"BUTTON", tr(L"浏览…").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,0,0, hwnd_, cid(IDC_BROWSE), inst, nullptr);
        label(tr(L"启动参数（可选）").c_str());
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", item_.arguments.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        0,0,0,0, hwnd_, cid(IDC_ARGS), inst, nullptr);
        label(tr(L"工作目录（可选）").c_str());
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", item_.workingDirectory.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        0,0,0,0, hwnd_, cid(IDC_WORKDIR), inst, nullptr);
        CreateWindowW(L"BUTTON", tr(L"浏览…").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,0,0, hwnd_, cid(IDC_BROWSEDIR), inst, nullptr);
        label(tr(L"自定义图标（可选）").c_str());
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", item_.customIcon.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        0,0,0,0, hwnd_, cid(IDC_ICONPATH), inst, nullptr);
        CreateWindowW(L"BUTTON", tr(L"浏览…").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,0,0, hwnd_, cid(IDC_BROWSEICON), inst, nullptr);
        CreateWindowW(L"BUTTON", tr(L"清除").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,0,0, hwnd_, cid(IDC_CLEARICON), inst, nullptr);
        CreateWindowW(L"BUTTON", tr(L"保存").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0,0,0,0, hwnd_, cid(IDC_OK), inst, nullptr);
        CreateWindowW(L"BUTTON", tr(L"取消").c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,0,0,0, hwnd_, cid(IDC_CANCEL), inst, nullptr);
        for(int id : {IDC_NAME,IDC_PATH,IDC_ARGS,IDC_WORKDIR,IDC_ICONPATH,IDC_BROWSE,IDC_BROWSEDIR,IDC_BROWSEICON,IDC_CLEARICON,IDC_OK,IDC_CANCEL})
            if(!GetDlgItem(hwnd_,id))return false;
        int childCount=0;for(HWND c=GetWindow(hwnd_,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT))++childCount;
        if(childCount!=16)return false;
        for(int id : {IDC_BROWSE,IDC_BROWSEDIR,IDC_BROWSEICON,IDC_CLEARICON,IDC_OK,IDC_CANCEL}){if(HWND b=GetDlgItem(hwnd_,id)){SendMessageW(b,BM_SETSTYLE,BS_PUSHBUTTON|BS_FLAT|(id==IDC_OK?BS_DEFPUSHBUTTON:0),TRUE);SetWindowTheme(b,L"Explorer",nullptr);}}
        font_ = create_ui_font(dpi_for_window(hwnd_), 9);
        for (HWND c = GetWindow(hwnd_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        layout();
        SetFocus(GetDlgItem(hwnd_, item_.name.empty() ? IDC_NAME : IDC_PATH));
        return true;
    }
    void layout() {
        const int m = dip(hwnd_, 18), lh = dip(hwnd_, 22), eh = dip(hwnd_, 28), gap = dip(hwnd_, 8), bw = dip(hwnd_, 76);
        RECT rc{}; GetClientRect(hwnd_, &rc); const int w = rc.right - rc.left;
        HWND c = GetWindow(hwnd_, GW_CHILD);
        int y = m;
        for (int row = 0; row < 5; ++row) {
            HWND lab = c; c = GetWindow(c, GW_HWNDNEXT);
            HWND edit = c; c = GetWindow(c, GW_HWNDNEXT);
            // 标签宽度按文本自适应（不同语言长度不同防截断/换行）
            SetWindowPos(lab, nullptr, m, y, fit_text_width(lab, font_, dip(hwnd_, 150), dip(hwnd_, 320)), lh, SWP_NOZORDER);
            y += lh;
            if (row == 4) {
                // 图标行：编辑 + 浏览… + 清除
                HWND b1 = c; c = GetWindow(c, GW_HWNDNEXT);
                HWND b2 = c; c = GetWindow(c, GW_HWNDNEXT);
                const int bw2 = dip(hwnd_, 64);
                const int ew = w - m * 2 - (bw2 + gap) * 2;
                SetWindowPos(edit, nullptr, m, y, ew, eh, SWP_NOZORDER);
                SetWindowPos(b1, nullptr, m + ew + gap, y, bw2, eh, SWP_NOZORDER);
                SetWindowPos(b2, nullptr, m + ew + gap * 2 + bw2, y, bw2, eh, SWP_NOZORDER);
            } else {
                const bool hasBrowse = (row == 1 || row == 3);
                const int ew = w - m * 2 - (hasBrowse ? bw + gap : 0);
                SetWindowPos(edit, nullptr, m, y, ew, eh, SWP_NOZORDER);
                if (hasBrowse) {
                    HWND b = c; c = GetWindow(c, GW_HWNDNEXT);
                    SetWindowPos(b, nullptr, m + ew + gap, y, bw, eh, SWP_NOZORDER);
                }
            }
            y += eh + gap;
        }
        const int by = rc.bottom - m - dip(hwnd_, 32);
        SetWindowPos(GetDlgItem(hwnd_, IDC_OK), nullptr, w - m - dip(hwnd_, 164), by, dip(hwnd_, 78), dip(hwnd_, 32), SWP_NOZORDER);
        SetWindowPos(GetDlgItem(hwnd_, IDC_CANCEL), nullptr, w - m - dip(hwnd_, 78), by, dip(hwnd_, 78), dip(hwnd_, 32), SWP_NOZORDER);
    }
    void center_to_owner() {
        RECT wr{}, orc{}; GetWindowRect(hwnd_, &wr); GetWindowRect(owner_, &orc);
        const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        int x = orc.left + ((orc.right - orc.left) - ww) / 2;
        int y = orc.top + ((orc.bottom - orc.top) - wh) / 2;
        HMONITOR mon = MonitorFromRect(&orc, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(mon,&mi))mi.rcWork={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
        const int workLeft = static_cast<int>(mi.rcWork.left);
        const int workTop = static_cast<int>(mi.rcWork.top);
        const int workRight = static_cast<int>(mi.rcWork.right);
        const int workBottom = static_cast<int>(mi.rcWork.bottom);
        x = std::clamp(x, workLeft, std::max(workLeft, workRight - ww));
        y = std::clamp(y, workTop, std::max(workTop, workBottom - wh));
        SetWindowPos(hwnd_, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOSIZE);
    }
    std::wstring text(int id) const {
        const int n = GetWindowTextLengthW(GetDlgItem(hwnd_, id));
        std::wstring s(static_cast<size_t>(n) + 1, L'\0');
        if (n) GetDlgItemTextW(hwnd_, id, s.data(), n + 1);
        s.resize(n); return s;
    }
/* STAMKY_CN_DETAIL
 * 文件选择委托给 Shell 工具层，并沿用 Settings 中已经规范化的 launchExtensions；返回模态对话框后
 * 只把最终路径写入编辑副本。COM 对话框期间外层 Manager 可能继续收到系统消息，因此编辑器不持有
 * 外层 vector 元素的裸指针。
 */
    void browse_file() {
        auto p = pick_files(hwnd_, launchExts_ ? *launchExts_ : std::vector<std::wstring>{});
        if (!p.empty()) {
            SetDlgItemTextW(hwnd_, IDC_PATH, p.front().c_str());
            if (GetWindowTextLengthW(GetDlgItem(hwnd_, IDC_NAME)) == 0)
                SetDlgItemTextW(hwnd_, IDC_NAME, file_display_name(p.front()).c_str());
        }
    }
    void browse_dir() {
        auto p = pick_folder(hwnd_); if (p) SetDlgItemTextW(hwnd_, IDC_WORKDIR, p->c_str());
    }
    void browse_icon() {
        IFileOpenDialog* dlg = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
        DWORD opts = 0;
        if(FAILED(dlg->GetOptions(&opts))||FAILED(dlg->SetOptions(opts|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_NODEREFERENCELINKS))){dlg->Release();return;}
        COMDLG_FILTERSPEC filters[] = {
            {tr(L"图标与程序").c_str(), L"*.ico;*.exe;*.dll;*.icl"},
            {tr(L"所有文件").c_str(), L"*.*"}
        };
        if(FAILED(dlg->SetFileTypes(2,filters))||FAILED(dlg->SetFileTypeIndex(1))){dlg->Release();return;}
        if (SUCCEEDED(dlg->Show(hwnd_))) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(dlg->GetResult(&it)) && it) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { SetDlgItemTextW(hwnd_, IDC_ICONPATH, p); CoTaskMemFree(p); }
                it->Release();
            }
        }
        dlg->Release();
    }
    void clear_icon() {
        SetDlgItemTextW(hwnd_, IDC_ICONPATH, L"");
    }
/* STAMKY_CN_DETAIL
 * OK 前集中读取/trim 必填字段并按项目类型校验。自定义项目可以拥有独立参数/图标，而普通文件/目录
 * 仍以 path 为启动依据。只有验证完成才设置 accepted_ 并销毁窗口，避免“窗口已关但数据其实无效”。
 */
    void accept() {
        Item updated = item_;
        updated.name = text(IDC_NAME);
        updated.path = text(IDC_PATH);
        updated.arguments = text(IDC_ARGS);
        updated.workingDirectory = text(IDC_WORKDIR);
        updated.customIcon = text(IDC_ICONPATH);
        if (updated.name.empty() || updated.path.empty()) {
            MessageBoxW(hwnd_, tr(L"名称和路径不能为空。").c_str(), tr(L"stamky").c_str(), MB_OK | MB_ICONWARNING);
            return;
        }
        updated.type = classify_path(std::filesystem::path(updated.path));
        updated.targetGroupId.clear();
        item_ = std::move(updated);
        accepted_ = true;
        DestroyWindow(hwnd_);
    }
    LRESULT proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_CTLCOLORSTATIC: { HDC dc=reinterpret_cast<HDC>(w); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT)); return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW)); }
        case WM_SIZE: layout(); return 0;
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER|SWP_NOACTIVATE);
            if (font_) DeleteObject(font_);
            font_ = create_ui_font(dpi_for_window(hwnd_), 9);
            for (HWND c = GetWindow(hwnd_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            layout(); return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
            case IDC_BROWSE: browse_file(); return 0;
            case IDC_BROWSEDIR: browse_dir(); return 0;
            case IDC_BROWSEICON: browse_icon(); return 0;
            case IDC_CLEARICON: clear_icon(); return 0;
            case IDC_OK: accept(); return 0;
            case IDC_CANCEL: DestroyWindow(hwnd_); return 0;
            }
            break;
        case WM_CLOSE: DestroyWindow(hwnd_); return 0;
        case WM_DESTROY:
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            hwnd_ = nullptr;
            return 0;
        case WM_NCDESTROY: SetWindowLongPtrW(h, GWLP_USERDATA, 0); break;
        }
        return DefWindowProcW(h, m, w, l);
    }
};
}

bool ItemEditor::edit(HWND owner, Item& item, bool newItem, const std::vector<std::wstring>* launchExts) {
    EditorImpl impl(owner, item, newItem, launchExts);
    return impl.run();
}
}
