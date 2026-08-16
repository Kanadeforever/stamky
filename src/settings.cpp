/*
 * stamky / settings.cpp
 *
 * settings.ini 的加载、规范化与原子保存。所有 UI/扫描/文件夹菜单都会调用同一扩展名规范化规则，
 * 避免“设置页允许、重启后却被丢弃”这种双重校验语义。扩展名数量、单 token 长度和整行长度
 * 都有限制，以适配 Win32 Profile API 的现实边界并阻止无界配置。
 *
 * 保存不是逐项 WritePrivateProfileStringW：那种方式在中途失败时会留下半套新设置。本实现先
 * 生成完整 UTF-16LE+BOM 文件到 settings.ini.tmp，全部写入/flush 成功后才 MoveFileExW 原子替换。
 * load() 对截断/非法语言代码采用已知安全默认值，不把部分缓冲区当成完整用户设置。
 */

#include "settings.h"
#include <fstream>

namespace sm {
namespace {
std::wstring ini_path() { const auto dir=data_dir(); return dir.empty()?std::wstring{}:(dir/L"settings.ini").wstring(); }

// 与 Settings 构造默认值保持一致；新装用户无配置时读到此默认串。
constexpr wchar_t kDefaultExts[] = L"lnk,url,exe,com,bat,cmd,appref-ms";

/* STAMKY_CN_DETAIL
 * 启动扩展名只有一种规范化入口：trim -> 去前导点 -> 小写 -> 长度/字符白名单。Settings 加载、
 * FormatEditor 新增和保存都复用它，避免“本次 UI 接受，但重启加载器丢弃”的双重规则。
 * token 不包含通配符；真正传给 FileDialog 的筛选串由 Shell 层生成。
 */
std::wstring normalize_ext_token_impl(std::wstring tok) {
    while (!tok.empty() && (tok.front() == L' ' || tok.front() == L'\t')) tok.erase(tok.begin());
    while (!tok.empty() && (tok.back() == L' ' || tok.back() == L'\t')) tok.pop_back();
    tok = lower_copy(tok);
    if (!tok.empty() && tok[0] == L'.') tok.erase(0, 1);
    if (tok.empty() || tok.size() > 32) return {};
    static const wchar_t* invalid = L"\\/:*?\"<>|.,;";
    for (const wchar_t c : tok) {
        if (c < 33 || wcschr(invalid, c)) return {};
    }
    return tok;
}
}

std::wstring normalize_launch_extension(std::wstring token) {
    return normalize_ext_token_impl(std::move(token));
}

/* STAMKY_CN_DETAIL
 * INI 仍使用 Win32 profile API 读取，但扩展名列表给足受控缓冲并显式检测可能截断的返回长度。
 * 截断配置不应被当作合法前缀继续使用，否则用户保存的后半扩展名会在重启后静默消失。
 * 各数值还会在这里夹取到 UI/渲染可接受范围。
 */
void Settings::load() {
    const auto p = ini_path();
    if(p.empty()){set_extensions_text(kDefaultExts);return;}
    menuFontPoints = std::clamp<int>(GetPrivateProfileIntW(L"Menu", L"FontPoints", 10, p.c_str()), 8, 20);
    iconLogicalSize = std::clamp<int>(GetPrivateProfileIntW(L"Menu", L"IconLogicalSize", 28, p.c_str()), 16, 64);
    columnSeparator = GetPrivateProfileIntW(L"Menu", L"ColumnSeparator", 1, p.c_str()) != 0;

    wchar_t lbuf[64]{};
    GetPrivateProfileStringW(L"General", L"Language", L"cn", lbuf, static_cast<DWORD>(_countof(lbuf)), p.c_str());
    language = lbuf;
    const bool validLanguage=!language.empty()&&language.size()<=32&&
        std::all_of(language.begin(),language.end(),[](wchar_t c){
            return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'-'||c==L'_';
        });
    language=validLanguage?lower_copy(std::move(language)):L"cn";

    // 自身保存的最多 512 个、每个最多 32 字符，32768 wchar 足够完整回读。
    // 若外部手工写入超长值导致 Profile API 截断，宁可回退默认，也不接受半截配置。
    std::vector<wchar_t> buf(32768,L'\0');
    const DWORD got=GetPrivateProfileStringW(L"Extensions", L"Launch", kDefaultExts, buf.data(), static_cast<DWORD>(buf.size()), p.c_str());
    if(got>=buf.size()-1){
        startup_log(L"settings.ini 的 Extensions/Launch 过长，已回退默认扩展名列表");
        set_extensions_text(kDefaultExts);
    }else set_extensions_text(buf.data());
}

/* STAMKY_CN_DETAIL
 * 保存不逐项调用 WritePrivateProfileStringW，因为多次写入中途失败会留下“半新半旧”的设置。
 * 这里先在内存构造完整 UTF-16LE+BOM INI，写 settings.ini.tmp，flush/close 后原子替换正式文件；
 * 因而一次 Save 对观察者表现为旧配置或新配置二选一。
 */
bool Settings::save() const {
    ensure_directories();
    const std::filesystem::path finalPath=ini_path();
    if(finalPath.empty()||finalPath.parent_path().empty())return false;
    std::filesystem::path tempPath=finalPath;
    tempPath+=L".tmp";

    if(launchExtensions.empty()||launchExtensions.size()>kMaxLaunchExtensions)return false;
    for(const auto& ext:launchExtensions){
        if(normalize_launch_extension(ext)!=ext)return false;
    }
    const std::wstring extText=extensions_text();
    if(extText.size()>=32768)return false;

    std::wstring safeLanguage=language;
    if(safeLanguage.empty()||safeLanguage.size()>32||
       std::any_of(safeLanguage.begin(),safeLanguage.end(),[](wchar_t c){
           return !((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'-'||c==L'_');
       })) safeLanguage=L"cn";

    const std::wstring text=
        L"[General]\r\nLanguage="+safeLanguage+
        L"\r\n\r\n[Menu]\r\nFontPoints="+std::to_wstring(std::clamp(menuFontPoints,8,20))+
        L"\r\nIconLogicalSize="+std::to_wstring(std::clamp(iconLogicalSize,16,64))+
        L"\r\nColumnSeparator="+std::wstring(columnSeparator?L"1":L"0")+
        L"\r\n\r\n[Extensions]\r\nLaunch="+extText+L"\r\n";

    std::ofstream f(tempPath,std::ios::binary|std::ios::trunc);
    if(!f)return false;
    const unsigned char bom[2]={0xFF,0xFE};
    f.write(reinterpret_cast<const char*>(bom),2);
    // Windows/MSVC 的 wchar_t 为 UTF-16 code unit；逐单元写成 LE，避免 locale/codecvt 依赖。
    for(wchar_t wc:text){
        const uint16_t u=static_cast<uint16_t>(wc);
        const unsigned char bytes[2]={static_cast<unsigned char>(u&0xFFu),static_cast<unsigned char>((u>>8)&0xFFu)};
        f.write(reinterpret_cast<const char*>(bytes),2);
        if(!f){f.close();DeleteFileW(tempPath.c_str());return false;}
    }
    f.flush();
    if(!f.good()){f.close();DeleteFileW(tempPath.c_str());return false;}
    f.close();
    if(!MoveFileExW(tempPath.c_str(),finalPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

bool Settings::is_launchable_ext(const std::wstring& ext) const {
    const std::wstring e = normalize_launch_extension(ext);
    if (e.empty()) return false;
    return std::find(launchExtensions.begin(), launchExtensions.end(), e) != launchExtensions.end();
}

std::wstring Settings::extensions_text() const {
    std::wstring out;
    for (const auto& e : launchExtensions) {
        if (!out.empty()) out += L",";
        out += e;
    }
    return out;
}

void Settings::set_extensions_text(const std::wstring& text) {
    std::vector<std::wstring> parsed;
    std::wstring cur;
    auto flush = [&] {
        std::wstring tok = normalize_launch_extension(std::move(cur));
        cur.clear();
        if (!tok.empty() && parsed.size()<kMaxLaunchExtensions &&
            std::find(parsed.begin(), parsed.end(), tok) == parsed.end()) parsed.push_back(std::move(tok));
    };
    for (wchar_t c : text) {
        if (c == L',' || c == L';' || c == L' ' || c == L'\t') flush();
        else cur += c;
    }
    flush();
    launchExtensions = parsed.empty() ? default_launch_extensions() : std::move(parsed);
}
}
