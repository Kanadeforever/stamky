/*
 * stamky / item_editor.h
 *
 * ItemEditor::edit 是同步事务式编辑接口：返回 true 才表示 item 已被提交修改。
 * 调用方在 edit() 返回前应假定当前线程会处理嵌套窗口消息，因此不能跨调用保存可能因
 * Model 变更而失效的 vector 元素指针。launchExts 只影响浏览过滤，不改变项目数据格式。
 */

#pragma once
#include "common.h"
#include "model.h"

namespace sm {
class ItemEditor {
public:
    // launchExts 可空：浏览按钮的文件选择器使用内置默认全集；
    // 非空时使用调用方（内容管理）设置中的可启动格式集合。
    static bool edit(HWND owner, Item& item, bool newItem = false,
                     const std::vector<std::wstring>* launchExts = nullptr);
};
}
