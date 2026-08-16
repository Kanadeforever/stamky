/*
 * stamky / model.h
 *
 * Model/Group/Item 是用户内容的唯一持久业务模型，不包含 HWND/HICON 等进程资源。
 * ItemType::Group 表示“引用另一个 Group”的逻辑子菜单，不是磁盘文件夹；targetGroupId
 * 只对该类型有意义。items vector 的顺序就是运行菜单顺序，不能在缓存构建时另行排序。
 *
 * scan_folder 的 optional 语义必须保留：has_value()==false 是 I/O 失败，has_value()==true
 * 且为空才是成功无结果。would_create_group_cycle 用于在插入子组前维持引用图无环。
 */

#pragma once
#include "common.h"

namespace sm {
enum class ItemType : uint32_t {
    Program = 0,
    Folder = 1,
    Url = 2,
    File = 3,
    Group = 4
};

struct Item {
    std::wstring id;
    std::wstring name;
    std::wstring path;
    std::wstring sourceFolder;      // empty = manual; non-empty = imported from scan source
    ItemType type = ItemType::Program;
    std::wstring arguments;
    std::wstring workingDirectory;
    std::wstring targetGroupId;     // used only when type == Group
    std::wstring customIcon;        // 自定义图标源（.ico/.exe/.dll/.icl）；空 = 自动用程序/类型图标
};

struct Group {
    std::wstring id;
    std::wstring name;
    std::vector<Item> items; // single source of truth for runtime order
};

class Model {
public:
    std::vector<Group> groups;

    bool load();
    bool save() const;
    Group& ensure_default_group();
    Group* find_group(const std::wstring& id);
    const Group* find_group(const std::wstring& id) const;
    // exts 为空时使用内置默认全集（default_launch_extensions）。
    std::optional<std::vector<Item>> scan_folder(const std::wstring& folder, const std::vector<std::wstring>& exts = {}) const;
    bool would_create_group_cycle(const std::wstring& parentGroupId, const std::wstring& targetGroupId) const;
};

ItemType classify_path(const std::filesystem::path& p);
std::wstring item_type_name(ItemType type);
}
