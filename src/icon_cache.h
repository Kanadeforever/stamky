/*
 * stamky / icon_cache.h
 *
 * IconCache 是“可丢弃、可重建”的派生缓存，不是用户配置的权威来源。Item.id 是缓存键，
 * 因此 Model/RuntimeCache 会保证项目 ID 全局唯一。IconPixels 拥有自己的 BGRA 字节；
 * 返回 optional 表示缓存缺失/损坏，而不是以空像素伪装成功。
 */

#pragma once
#include "common.h"
#include "model.h"
namespace sm {
constexpr int kIconSizes[] = {32,40,48,56,64};
struct IconPixels { int size=0; std::vector<uint8_t> bgra; };
class IconCache {
public:
    bool ensure(const Item& item);
    void invalidate(const std::wstring& itemId);
    std::optional<IconPixels> load(const std::wstring& itemId, int size) const;
    static std::filesystem::path icon_path(const std::wstring& itemId, int size);
private:
    bool extract(const std::wstring& path, int size, std::vector<uint8_t>& out);
};
}
