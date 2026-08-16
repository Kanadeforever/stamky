/*
 * stamky / icon_cache.cpp
 *
 * 派生图标缓存层。它把 Shell/自定义图标提取为固定尺寸的预乘 BGRA 像素文件，供 runtime.bin
 * 构建器直接打包，避免每次弹出菜单都走昂贵的 Shell 图标解析。缓存是可重建派生数据，
 * 但仍严格校验尺寸与文件长度，防止“看似存在的半文件”被误当成有效缓存。
 *
 * 所有权：Shell 返回的 HICON/HBITMAP/HDC 只在当前函数范围使用，并按对应 Win32 规则释放；
 * 写入缓存前像素已脱离 GDI 对象。临时文件与目标文件位于同一目录，完整 flush 后通过
 * MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH) 提交，从而尽量避免崩溃/断电留下半写正式文件。
 *
 * 安全边界：任何来自位图头/文件尺寸的乘法先在 64 位空间验证，再转 size_t 分配；尺寸异常
 * 直接失败，不允许负数或溢出值变成巨大分配。
 */

#include "icon_cache.h"
#include <fstream>
#include <cmath>
#include <limits>

namespace sm {
namespace {
std::filesystem::path legacy_icon_path(const std::wstring& itemId, int size) {
    const auto dir=cache_dir();
    return dir.empty()?std::filesystem::path{}:dir/L"icons"/(safe_filename(itemId)+L"_"+std::to_wstring(size)+L".bgra");
}

void normalize_pbgra(std::vector<uint8_t>& pixels) {
    bool hasAnyAlpha=false;
    bool hasSemiTransparent=false;
    bool looksStraight=false;
    for(size_t i=0;i+3<pixels.size();i+=4){
        const uint8_t b=pixels[i],g=pixels[i+1],r=pixels[i+2],a=pixels[i+3];
        if(a!=0) hasAnyAlpha=true;
        if(a>0&&a<255){
            hasSemiTransparent=true;
            if(b>a||g>a||r>a){looksStraight=true;}
        }
    }

    // 极少数 Shell/GDI 路径可能给出 RGB 有内容但 alpha 全 0。避免把整个图标变成透明：
    // 黑色全零像素继续保持透明，其它像素退化为不透明。
    if(!hasAnyAlpha){
        for(size_t i=0;i+3<pixels.size();i+=4){
            if(pixels[i]||pixels[i+1]||pixels[i+2]) pixels[i+3]=255;
        }
        return;
    }

    if(looksStraight){
        for(size_t i=0;i+3<pixels.size();i+=4){
            const uint32_t a=pixels[i+3];
            pixels[i]=static_cast<uint8_t>((static_cast<uint32_t>(pixels[i])*a+127)/255);
            pixels[i+1]=static_cast<uint8_t>((static_cast<uint32_t>(pixels[i+1])*a+127)/255);
            pixels[i+2]=static_cast<uint8_t>((static_cast<uint32_t>(pixels[i+2])*a+127)/255);
        }
    }else if(!hasSemiTransparent){
        // 完全透明像素的 RGB 对 AlphaBlend 没有意义，清零可以避免某些 Shell 图标缓存留下的脏边颜色。
        for(size_t i=0;i+3<pixels.size();i+=4){
            if(pixels[i+3]==0) pixels[i]=pixels[i+1]=pixels[i+2]=0;
        }
    }
}

std::vector<uint8_t> resize_pbgra_bilinear(const uint8_t* src,int sw,int sh,int dw,int dh){
    if(!src||sw<=0||sh<=0||dw<=0||dh<=0)return {};
    const uint64_t bytes=static_cast<uint64_t>(dw)*static_cast<uint64_t>(dh)*4ull;
    if(bytes>static_cast<uint64_t>(std::numeric_limits<size_t>::max())||bytes>64ull*1024ull*1024ull)return {};
    std::vector<uint8_t> out(static_cast<size_t>(bytes));
    if(sw==dw&&sh==dh){std::memcpy(out.data(),src,out.size());return out;}
    for(int y=0;y<dh;++y){
        const double sy=((static_cast<double>(y)+0.5)*sh/dh)-0.5;
        const int y0=std::clamp(static_cast<int>(std::floor(sy)),0,sh-1);
        const int y1=std::min(y0+1,sh-1);
        const double fy=std::clamp(sy-std::floor(sy),0.0,1.0);
        for(int x=0;x<dw;++x){
            const double sx=((static_cast<double>(x)+0.5)*sw/dw)-0.5;
            const int x0=std::clamp(static_cast<int>(std::floor(sx)),0,sw-1);
            const int x1=std::min(x0+1,sw-1);
            const double fx=std::clamp(sx-std::floor(sx),0.0,1.0);
            const uint8_t* p00=src+(static_cast<size_t>(y0)*sw+x0)*4;
            const uint8_t* p10=src+(static_cast<size_t>(y0)*sw+x1)*4;
            const uint8_t* p01=src+(static_cast<size_t>(y1)*sw+x0)*4;
            const uint8_t* p11=src+(static_cast<size_t>(y1)*sw+x1)*4;
            uint8_t* d=out.data()+(static_cast<size_t>(y)*dw+x)*4;
            for(int c=0;c<4;++c){
                const double top=p00[c]+(p10[c]-p00[c])*fx;
                const double bottom=p01[c]+(p11[c]-p01[c])*fx;
                d[c]=static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(top+(bottom-top)*fy)),0,255));
            }
        }
    }
    return out;
}
}

std::filesystem::path IconCache::icon_path(const std::wstring& itemId, int size) {
    // p2 表示“已规范化 premultiplied BGRA”的第二代图标缓存。
    const auto dir=cache_dir();
    return dir.empty()?std::filesystem::path{}:dir/L"icons"/(safe_filename(itemId)+L"_p2_"+std::to_wstring(size)+L".bgra");
}

/* STAMKY_CN_DETAIL
 * 图标提取路径最终统一为预乘 alpha BGRA（PBGRA）。Windows 图标可能来自 ExtractIconEx/Shell，
 * DIB 像素的 alpha/颜色关系并不总是满足后续 AlphaBlend 预期，因此 normalize_pbgra 会校正通道语义。
 * 所有 HICON/HBITMAP/HDC 都必须在本函数离开前按各自所有权释放，像素 vector 才能跨模块持久化。
 */
bool IconCache::extract(const std::wstring& path, int size, std::vector<uint8_t>& out) {
    IShellItem* item=nullptr;
    if(FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&item)))) return false;
    IShellItemImageFactory* factory=nullptr;
    HRESULT hr=item->QueryInterface(IID_PPV_ARGS(&factory));
    item->Release();
    if(FAILED(hr)||!factory) return false;

    SIZE requested{size,size};
    HBITMAP hb=nullptr;
    hr=factory->GetImage(requested,static_cast<SIIGBF>(SIIGBF_RESIZETOFIT|SIIGBF_ICONONLY),&hb);
    factory->Release();
    if(FAILED(hr)||!hb) return false;

    BITMAP bm{};
    if(!GetObjectW(hb,sizeof(bm),&bm)||bm.bmWidth<=0||bm.bmHeight==0){DeleteObject(hb);return false;}
    const int sw=std::abs(static_cast<int>(bm.bmWidth));
    const int sh=std::abs(static_cast<int>(bm.bmHeight));
    constexpr int kMaxShellBitmapDimension=4096;
    const uint64_t rawBytes=static_cast<uint64_t>(sw)*static_cast<uint64_t>(sh)*4ull;
    if(sw<=0||sh<=0||sw>kMaxShellBitmapDimension||sh>kMaxShellBitmapDimension||
       rawBytes>static_cast<uint64_t>(std::numeric_limits<size_t>::max())){DeleteObject(hb);return false;}

    BITMAPINFO bi{};
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=sw;
    bi.bmiHeader.biHeight=-sh;
    bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32;
    bi.bmiHeader.biCompression=BI_RGB;

    std::vector<uint8_t> raw(static_cast<size_t>(rawBytes));
    HDC dc=GetDC(nullptr);
    if(!dc){DeleteObject(hb);return false;}
    const int got=GetDIBits(dc,hb,0,static_cast<UINT>(sh),raw.data(),&bi,DIB_RGB_COLORS);
    ReleaseDC(nullptr,dc);
    DeleteObject(hb);
    if(got!=sh) return false;

    // AlphaBlend 要求 AC_SRC_ALPHA 输入是 premultiplied alpha。
    // 原 Stacky 也显式把 HICON 转成 32bppPBGRA；这里对 Shell HBITMAP 做同样的语义规范化。
    normalize_pbgra(raw);
    out=resize_pbgra_bilinear(raw.data(),sw,sh,size,size);
    return out.size()==static_cast<size_t>(size)*size*4;
}

/* STAMKY_CN_DETAIL
 * ensure() 生成的是派生缓存：先尝试目标尺寸，失败时不得把旧缓存“看起来还能读”当成成功。
 * 每个尺寸文件使用临时文件完整写入后再替换；如果提取失败，调用者仍可使用 stock fallback，
 * 但 runtime.bin 绝不能引用尺寸正确、内容却是上次失败遗留的半文件。
 */
bool IconCache::ensure(const Item& item) {
    ensure_directories();
    bool any=false;
    for(int size:kIconSizes){
        auto p=icon_path(item.id,size);
        if(p.empty())continue;
        std::error_code ec;
        if(std::filesystem::exists(p,ec)&&std::filesystem::file_size(p,ec)==static_cast<uintmax_t>(size*size*4)){
            any=true;
            continue;
        }
        std::vector<uint8_t> px;
        // 自定义图标优先（.ico/.exe/.dll/.icl）；空则用项目自身路径（程序/类型图标）。
        const std::wstring& src=item.customIcon.empty()?item.path:item.customIcon;
        if(!extract(src,size,px)) continue;
        const auto tmp=p.parent_path()/(p.filename().wstring()+L".tmp");
        std::ofstream f(tmp,std::ios::binary|std::ios::trunc);
        if(!f) continue;
        f.write(reinterpret_cast<const char*>(px.data()),static_cast<std::streamsize>(px.size()));
        f.flush();
        const bool writeOk=f.good();
        f.close();
        if(writeOk&&MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
            any=true;
            std::filesystem::remove(legacy_icon_path(item.id,size),ec);
        }else{
            DeleteFileW(tmp.c_str());
        }
    }
    return any;
}

void IconCache::invalidate(const std::wstring& itemId) {
    std::error_code ec;
    for(int size:kIconSizes){
        const auto current=icon_path(itemId,size);
        const auto legacy=legacy_icon_path(itemId,size);
        if(!current.empty())std::filesystem::remove(current,ec);
        ec.clear();
        if(!legacy.empty())std::filesystem::remove(legacy,ec);
    }
}

/* STAMKY_CN_DETAIL
 * load() 不只检查“至少有 width*height*4 字节”，而要求文件长度精确等于预期 payload。
 * 这是对异常终止/旧格式尾随数据的廉价完整性校验。尺寸和乘法先用 64 位检查，再转换 size_t，
 * 防止负值或溢出在分配前变成巨大的无符号长度。
 */
std::optional<IconPixels> IconCache::load(const std::wstring& itemId, int size) const {
    if(size<=0||size>512)return std::nullopt;
    const uint64_t expected64=static_cast<uint64_t>(size)*static_cast<uint64_t>(size)*4ull;
    if(expected64>static_cast<uint64_t>(std::numeric_limits<size_t>::max())||
       expected64>static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))return std::nullopt;
    auto p=icon_path(itemId,size);
    if(p.empty())return std::nullopt;
    std::error_code ec;
    const auto fileSize=std::filesystem::file_size(p,ec);
    if(ec||fileSize!=expected64)return std::nullopt;
    std::ifstream f(p,std::ios::binary);
    if(!f) return std::nullopt;
    IconPixels out;
    out.size=size;
    out.bgra.resize(static_cast<size_t>(expected64));
    if(!f.read(reinterpret_cast<char*>(out.bgra.data()),static_cast<std::streamsize>(out.bgra.size())))return std::nullopt;
    return out;
}
}
