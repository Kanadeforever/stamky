/*
 * stamky / runtime_cache.h
 *
 * Runtime*View 是从 memory-mapped runtime.bin 解出的轻量视图。字符串会复制为 std::wstring，
 * RuntimeIconView::pixels 则是映射内裸指针：调用者必须确保 RuntimeCache 仍保持 open。
 * RuntimeItemView::hIcon 是唯一例外——它只用于文件夹实时枚举项，由 RuntimeMenu 临时拥有，
 * 不进入 runtime.bin。
 */

#pragma once
#include "common.h"
#include "model.h"
#include "icon_cache.h"

namespace sm {
struct RuntimeIconView { uint16_t size=0; const uint8_t* pixels=nullptr; uint32_t bytes=0; };
struct RuntimeItemView {
    ItemType type{};
    std::wstring id, name, path, arguments, workingDirectory, targetGroupId;
    std::vector<RuntimeIconView> icons;
    // 运行时图标（仅文件夹展开等临时枚举项使用；不参与 runtime.bin 序列化，
    // 由菜单生命周期负责 DestroyIcon，缓存读取的项恒为 nullptr）。
    HICON hIcon = nullptr;
};
struct RuntimeGroupView { std::wstring id,name; std::vector<RuntimeItemView> items; };

class RuntimeCacheBuilder { public: bool build(const Model& model); };
class RuntimeCache {
public:
    ~RuntimeCache();
    bool open();
    void close();
    std::optional<RuntimeGroupView> group(const std::wstring& id) const;
    std::vector<std::pair<std::wstring,std::wstring>> group_index() const;
private:
    HANDLE file_=INVALID_HANDLE_VALUE, mapping_=nullptr;
    const uint8_t* base_=nullptr;
    size_t size_=0;
    struct Index { std::wstring id,name; size_t offset=0; };
    std::vector<Index> index_;
    bool parse_index();
};
}
