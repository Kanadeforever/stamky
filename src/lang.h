/*
 * stamky / lang.h
 *
 * Language 的 map_ 只保存当前已加载语言的显式覆盖；中文原文不需要重复缓存。切换语言时
 * load() 整体替换 map_，失败则保持“回退中文”的可用状态。tr() 返回 std::wstring 副本，
 * 调用方不持有 map_ 内部字符串地址，因此后续切换语言不会制造悬空指针。
 */

#pragma once
#include "common.h"
#include <map>

namespace sm {
// 多语言：翻译文件为 data\lang\<code>.ini（如 en.ini、cn.ini、ja.ini），
// [UI] 段内 key=value。key 使用中文原文：当前语言文件缺失某键时
// 自动回退中文原文（默认中文），因此翻译不完整也能正常显示。
// 首次运行自动释放 cn.ini / en.ini 默认文件（不存在时才写）。
class Language {
public:
    // 释放默认翻译文件（cn.ini / en.ini），已存在则不覆盖。
    void ensure_default_files();
    // 加载指定语言代码对应的 ini（不存在则保持空映射，全部回退中文）。
    void load(const std::wstring& code);
    // 翻译：命中返回翻译，未命中返回原文（中文）；值内字面 \n 转为换行。
    std::wstring t(const wchar_t* text) const;
    // 枚举 data\lang\*.ini 的语言代码（en、cn、ja…），按名称排序。
    std::vector<std::wstring> available_languages() const;
    const std::wstring& code() const { return code_; }

private:
    std::wstring code_ = L"cn";
    std::map<std::wstring, std::wstring> map_;
};

Language& lang();
inline std::wstring tr(const wchar_t* text) { return lang().t(text); }
}
