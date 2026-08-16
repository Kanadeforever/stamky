/*
 * stamky / model.cpp
 *
 * 用户配置权威数据层。groups.tsv 是可读、可迁移的持久格式；Model::load 采用“完整解析到
 * 临时 Model -> 全部验证 -> 一次替换当前对象”，因此坏 UTF-8、截断记录、非法类型、重复 ID
 * 或超限字段不会把半份数据混进当前内存状态。读取失败也不会自动保存一个空模型覆盖原文件。
 *
 * 数据不变量：Group.id 非空且唯一；Item.id 非空并在全模型范围唯一；Group 类型 Item 必须
 * 有 targetGroupId；类型值只能来自 ItemType。数量、单字段长度和文件总大小均设上限，既防止
 * 损坏文件造成异常分配，也让 runtime.bin 构建阶段可以依赖已验证的模型结构。
 *
 * 保存策略：先验证当前 Model，再在目标目录写 groups.tsv.tmp，flush/close 成功后用
 * MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH) 替换正式文件。正式文件在临时文件完整生成前不动，
 * 从而避免普通写入失败留下半个 TSV。
 *
 * scan_folder 返回 optional：nullopt 表示枚举过程失败/中断，空 vector 才表示“成功但没找到”。
 * 这一区分防止调用方把一次部分枚举误当完整结果导入。
 */

#include "model.h"
#include "lang.h"
#include <fstream>
#include <cerrno>
#include <unordered_set>
#include <sstream>

namespace sm {
namespace {
constexpr uintmax_t kMaxModelBytes = 64ull * 1024ull * 1024ull;
constexpr size_t kMaxGroups = 10000;
constexpr size_t kMaxItemsPerGroup = 100000;
// 4M UTF-16 code units 足以覆盖任何合理名称/路径，同时保证单字段转 UTF-8 后
// 不会逼近 runtime.bin 的 16 MiB 字符串上限或制造异常巨额临时分配。
constexpr size_t kMaxFieldChars = 4ull * 1024ull * 1024ull;

bool field_ok(const std::wstring& value){ return value.size()<=kMaxFieldChars; }

std::filesystem::path model_path() { const auto dir=data_dir(); return dir.empty()?std::filesystem::path{}:dir/L"groups.tsv"; }

bool group_reaches(const Model& model, const std::wstring& from, const std::wstring& target,
                   std::unordered_set<std::wstring>& visiting) {
    const auto key = lower_copy(from);
    if (!visiting.insert(key).second) return false;
    const Group* g = model.find_group(from);
    if (!g) return false;
    for (const auto& item : g->items) {
        if (item.type != ItemType::Group || item.targetGroupId.empty()) continue;
        if (_wcsicmp(item.targetGroupId.c_str(), target.c_str()) == 0) return true;
        if (group_reaches(model, item.targetGroupId, target, visiting)) return true;
    }
    return false;
}

bool parse_item_type(const std::wstring& text, ItemType& out) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long value = wcstoul(text.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != L'\0' || value > static_cast<unsigned long>(ItemType::Group)) return false;
    out = static_cast<ItemType>(value);
    return true;
}
}

ItemType classify_path(const std::filesystem::path& p) {
    const auto raw = lower_copy(p.wstring());
    if (raw.rfind(L"http://", 0) == 0 || raw.rfind(L"https://", 0) == 0 || raw.rfind(L"mailto:", 0) == 0) return ItemType::Url;
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) return ItemType::Folder;
    const auto ext = lower_copy(p.extension().wstring());
    if (ext == L".url") return ItemType::Url;
    if (ext == L".exe" || ext == L".lnk" || ext == L".com" || ext == L".bat" || ext == L".cmd" || ext == L".appref-ms")
        return ItemType::Program;
    return ItemType::File;
}

std::wstring item_type_name(ItemType type) {
    switch (type) {
    case ItemType::Program: return tr(L"程序");
    case ItemType::Folder:  return tr(L"文件夹");
    case ItemType::Url:     return tr(L"网址");
    case ItemType::File:    return tr(L"文件");
    case ItemType::Group:   return tr(L"子组");
    }
    return tr(L"项目");
}

Group& Model::ensure_default_group() {
    if (groups.empty()) groups.push_back({guid_string(), tr(L"常用"), {}});
    return groups.front();
}

Group* Model::find_group(const std::wstring& id) {
    const auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
        return _wcsicmp(g.id.c_str(), id.c_str()) == 0;
    });
    return it == groups.end() ? nullptr : &*it;
}

const Group* Model::find_group(const std::wstring& id) const {
    const auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) {
        return _wcsicmp(g.id.c_str(), id.c_str()) == 0;
    });
    return it == groups.end() ? nullptr : &*it;
}

/* STAMKY_CN_DETAIL
 * load() 是“事务式读取”，而不是边读边修改 this：先检查文件存在/大小，再一次性读取、去 BOM、
 * 严格 UTF-8 解码，随后把每条 G/I 记录解析进临时 Model。只有语法、数量、ID 唯一性、ItemType、
 * targetGroupId 等全部通过后才 move 到当前对象。任何一步失败都保留调用前模型，尤其禁止把损坏文件
 * 误判成“首次启动空配置”并在之后保存时覆盖原文件。
 */
bool Model::load() {
    ensure_directories();
    const auto path = model_path();
    if(path.empty())return false;
    std::error_code ec;
    const bool exists=std::filesystem::exists(path, ec);
    if(ec)return false;
    if (!exists) {
        Model fresh;
        fresh.ensure_default_group();
        if (!fresh.save()) return false;
        groups = std::move(fresh.groups);
        return true;
    }
    const uintmax_t fileSize = std::filesystem::file_size(path, ec);
    if (ec || fileSize == 0 || fileSize > kMaxModelBytes) return false;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string bytes(static_cast<size_t>(fileSize), '\0');
    f.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!f || static_cast<size_t>(f.gcount()) != bytes.size()) return false;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    if (bytes.empty()) return false;
    const std::wstring text = utf8_to_wide(bytes);
    if (text.empty()) return false;

    Model parsed;
    Group* current = nullptr;
    std::unordered_set<std::wstring> groupIds;
    std::unordered_set<std::wstring> itemIds;
    std::wistringstream stream(text);
    std::wstring line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        const auto p = split_tab(line);
        if (p.empty()) continue;
        if(std::any_of(p.begin(),p.end(),[](const std::wstring& value){return !field_ok(value);}))return false;
        if (p[0] == L"G") {
            if (p.size() < 3 || p[1].empty() || parsed.groups.size() >= kMaxGroups) return false;
            const std::wstring key = lower_copy(p[1]);
            if (!groupIds.insert(key).second) return false;
            parsed.groups.push_back({p[1], p[2], {}});
            current = &parsed.groups.back();
            continue;
        }
        if (p[0] == L"I") {
            if (!current || p.size() < 6 || p[1].empty() || current->items.size() >= kMaxItemsPerGroup) return false;
            Item it;
            if (!parse_item_type(p[2], it.type)) return false;
            const std::wstring key = lower_copy(p[1]);
            if (!itemIds.insert(key).second) return false;
            it.id = p[1];
            it.name = p[3];
            it.path = p[4];
            it.sourceFolder = p[5];
            if (p.size() >= 7) it.arguments = p[6];
            if (p.size() >= 8) it.workingDirectory = p[7];
            if (p.size() >= 9) it.targetGroupId = p[8];
            if (p.size() >= 10) it.customIcon = p[9];
            if (it.type == ItemType::Group && it.targetGroupId.empty()) return false;
            current->items.push_back(std::move(it));
            continue;
        }
        return false;
    }
    if (parsed.groups.empty()) return false;
    groups = std::move(parsed.groups);
    return true;
}

/* STAMKY_CN_DETAIL
 * save() 的原子性边界是“同一目录临时文件 -> Flush/Close -> MoveFileExW 替换”。同目录可避免跨卷
 * rename 退化为复制；MOVEFILE_REPLACE_EXISTING 保证正式文件只在临时文件完整生成后被替换，
 * MOVEFILE_WRITE_THROUGH 请求系统尽量把元数据提交到稳定存储。若任一步失败，调用方会保留/恢复
 * persistedModel_，因此返回值绝不能被忽略。
 */
bool Model::save() const {
    ensure_directories();
    const auto dst = model_path();
    if (dst.parent_path().empty() || groups.empty() || groups.size()>kMaxGroups) return false;
    std::unordered_set<std::wstring> groupIds;
    std::unordered_set<std::wstring> itemIds;
    for(const auto& g:groups){
        if(g.id.empty()||!field_ok(g.id)||!field_ok(g.name)||g.items.size()>kMaxItemsPerGroup||
           !groupIds.insert(lower_copy(g.id)).second)return false;
        for(const auto& it:g.items){
            const auto type=static_cast<uint32_t>(it.type);
            if(it.id.empty()||type>static_cast<uint32_t>(ItemType::Group)||!itemIds.insert(lower_copy(it.id)).second)return false;
            if(!field_ok(it.id)||!field_ok(it.name)||!field_ok(it.path)||!field_ok(it.sourceFolder)||
               !field_ok(it.arguments)||!field_ok(it.workingDirectory)||!field_ok(it.targetGroupId)||!field_ok(it.customIcon))return false;
            if(it.type==ItemType::Group&&it.targetGroupId.empty())return false;
        }
    }
    const auto tmp = dst.parent_path() / L"groups.tsv.tmp";
    std::wstring text = L"# stamky groups.tsv v2\r\n";
    for (const auto& g : groups) {
        text += L"G\t" + tsv_escape(g.id) + L"\t" + tsv_escape(g.name) + L"\r\n";
        for (const auto& it : g.items) {
            text += L"I\t" + tsv_escape(it.id) + L"\t" + std::to_wstring(static_cast<uint32_t>(it.type)) +
                    L"\t" + tsv_escape(it.name) + L"\t" + tsv_escape(it.path) + L"\t" + tsv_escape(it.sourceFolder) +
                    L"\t" + tsv_escape(it.arguments) + L"\t" + tsv_escape(it.workingDirectory) +
                    L"\t" + tsv_escape(it.targetGroupId) + L"\t" + tsv_escape(it.customIcon) + L"\r\n";
        }
    }
    const auto u = wide_to_utf8(text);
    if (text.size() && u.empty()) return false;
    if (u.size()+3>kMaxModelBytes) return false;

    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    f.write(reinterpret_cast<const char*>(bom), 3);
    if (!u.empty()) f.write(u.data(), static_cast<std::streamsize>(u.size()));
    f.flush();
    const bool writeOk = f.good();
    f.close();
    if (!writeOk) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

/* STAMKY_CN_DETAIL
 * 文件夹扫描用 optional 区分三种状态：nullopt=枚举发生 I/O/权限错误，空 vector=完整扫描成功但无匹配，
 * 非空 vector=完整结果。这个区别很重要：若 Find/目录迭代中途失败却返回已经收集的半份列表，Manager
 * 会把“部分成功”当成完整导入并持久化，用户很难意识到有文件被遗漏。
 */
std::optional<std::vector<Item>> Model::scan_folder(const std::wstring& folder, const std::vector<std::wstring>& exts) const {
    std::vector<Item> out;
    const std::vector<std::wstring>& list = exts.empty() ? default_launch_extensions() : exts;
    std::error_code ec;
    std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end;
    if(ec)return std::nullopt;
    for (; it != end; it.increment(ec)) {
        if(ec)return std::nullopt;
        const auto& de = *it;
        std::error_code statusEc;
        if (!de.is_regular_file(statusEc) || statusEc) continue;
        const auto ext = lower_copy(de.path().extension().wstring());
        const auto hit = std::find_if(list.begin(), list.end(), [&](const std::wstring& e) {
            return ext == L"." + e;
        });
        if (hit == list.end()) continue;
        Item item;
        item.id = guid_string();
        item.name = file_display_name(de.path());
        item.path = de.path().wstring();
        item.sourceFolder = folder;
        item.type = classify_path(de.path());
        out.push_back(std::move(item));
    }
    if(ec)return std::nullopt;
    std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                               a.name.c_str(), -1, b.name.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return out;
}

/* STAMKY_CN_DETAIL
 * 子组关系在 UI 上看起来像树，但持久格式允许按 ID 任意引用，因此新增引用前必须显式检测环。
 * 这里从目标组沿 Group 类型项目做可达性搜索；如果能回到 parentGroupId，就拒绝这条边。
 * 运行菜单也有递归深度上限，但那只是损坏数据的防御，不应替代模型层的不变量。
 */
bool Model::would_create_group_cycle(const std::wstring& parentGroupId, const std::wstring& targetGroupId) const {
    if (parentGroupId.empty() || targetGroupId.empty()) return true;
    if (_wcsicmp(parentGroupId.c_str(), targetGroupId.c_str()) == 0) return true;
    std::unordered_set<std::wstring> visiting;
    return group_reaches(*this, targetGroupId, parentGroupId, visiting);
}
}
