/*
 * stamky / runtime_cache.cpp
 *
 * 运行时只读缓存格式的构建与映射读取。runtime.bin 把已验证 Model 与各尺寸图标像素打包成
 * 连续二进制，目标是让热路径弹菜单时只做顺序解析/内存读取，不重新读取 groups.tsv、
 * 不枚举 Shell 图标。它是派生文件，任何损坏都应通过重建恢复，绝不能反向修改 Model。
 *
 * 格式边界：文件头包含 stamky 专用 magic 与格式版本；读取端验证总文件尺寸、组/项目数量、
 * 字符串字节数、ItemType、图标边长/字节数、Group payload 边界、重复 Group/Item ID 以及精确 EOF。
 * 每次加字段或改变布局都必须升级版本，不能靠“旧读者碰巧还能走下去”维持兼容。
 *
 * 内存映射所有权：RuntimeCache::open 持有 file_、mapping_、base_；Runtime*View 中的图标像素
 * 指针直接指向 base_，所以 view 的有效期绝不能超过对应 RuntimeCache 的 open 周期。close()
 * 按 base_ -> mapping_ -> file_ 顺序释放。App 在替换 runtime.bin 前会先阻止活动菜单并 close。
 *
 * 构建提交：runtime.bin.tmp 在同目录完整写完、flush、尺寸确认后再原子替换正式文件；
 * 任何 put/put_str/图标读取失败都会中止临时构建，不留下一个“格式头正确但尾部不完整”的
 * 正式缓存。
 */

#include "runtime_cache.h"
#include <fstream>
#include <limits>
#include <unordered_set>

namespace sm {
namespace {
constexpr char kMagic[8]={'S','T','A','M','K','Y','5','\0'};
constexpr uint32_t kVersion=5;
constexpr uint64_t kMaxCacheBytes=512ull*1024ull*1024ull;
constexpr uint32_t kMaxGroups=10000;
constexpr uint32_t kMaxItemsPerGroup=100000;
constexpr uint32_t kMaxIconsPerItem=16;
constexpr uint32_t kMaxStringBytes=16u*1024u*1024u;
constexpr uint16_t kMaxIconSize=512;

template<class T>bool put(std::ofstream& f,const T& v){
    f.write(reinterpret_cast<const char*>(&v),sizeof(v));
    return f.good();
}
bool put_str(std::ofstream& f,const std::wstring& w){
    const auto s=wide_to_utf8(w);
    if(!w.empty()&&s.empty()) return false;
    if(s.size()>kMaxStringBytes) return false;
    const uint32_t n=static_cast<uint32_t>(s.size());
    if(!put(f,n)) return false;
    if(n) f.write(s.data(),static_cast<std::streamsize>(n));
    return f.good();
}
template<class T>bool get(const uint8_t*& p,const uint8_t* end,T& v){
    if(p>end||static_cast<size_t>(end-p)<sizeof(T)) return false;
    memcpy(&v,p,sizeof(T));
    p+=sizeof(T);
    return true;
}
bool get_str(const uint8_t*& p,const uint8_t* end,std::wstring& w){
    uint32_t n=0;
    if(!get(p,end,n)||n>kMaxStringBytes||p>end||static_cast<size_t>(end-p)<n) return false;
    if(!n){w.clear();return true;}
    w=utf8_to_wide(std::string(reinterpret_cast<const char*>(p),n));
    if(w.empty()) return false;
    p+=n;
    return true;
}
bool valid_item_type(uint32_t v){return v<=static_cast<uint32_t>(ItemType::Group);}
bool valid_icon_payload(uint16_t size,uint32_t bytes){
    if(!size||size>kMaxIconSize) return false;
    const uint64_t expected=static_cast<uint64_t>(size)*size*4ull;
    return expected==bytes;
}

bool parse_group_payload(const uint8_t* p,const uint8_t* end,RuntimeGroupView* out,
                         std::unordered_set<std::wstring>* globalItemIds=nullptr){
    RuntimeGroupView group;
    std::unordered_set<std::wstring> localItemIds;
    if(!get_str(p,end,group.id)||group.id.empty()||!get_str(p,end,group.name)) return false;
    uint32_t count=0;
    if(!get(p,end,count)||count>kMaxItemsPerGroup) return false;
    if(out) group.items.reserve(count);
    for(uint32_t i=0;i<count;++i){
        RuntimeItemView item;
        uint32_t type=0;
        if(!get(p,end,type)||!valid_item_type(type)) return false;
        item.type=static_cast<ItemType>(type);
        if(!get_str(p,end,item.id)||item.id.empty()||!get_str(p,end,item.name)||!get_str(p,end,item.path)||
           !get_str(p,end,item.arguments)||!get_str(p,end,item.workingDirectory)||!get_str(p,end,item.targetGroupId)) return false;
        const auto itemKey=lower_copy(item.id);
        if(!localItemIds.insert(itemKey).second) return false;
        if(globalItemIds&&!globalItemIds->insert(itemKey).second) return false;
        if(item.type==ItemType::Group&&item.targetGroupId.empty()) return false;
        uint32_t iconCount=0;
        if(!get(p,end,iconCount)||iconCount>kMaxIconsPerItem) return false;
        if(out) item.icons.reserve(iconCount);
        for(uint32_t j=0;j<iconCount;++j){
            uint16_t size=0;
            uint32_t bytes=0;
            if(!get(p,end,size)||!get(p,end,bytes)||!valid_icon_payload(size,bytes)||p>end||static_cast<size_t>(end-p)<bytes) return false;
            if(out) item.icons.push_back({size,p,bytes});
            p+=bytes;
        }
        if(out) group.items.push_back(std::move(item));
    }
    if(p!=end) return false;
    if(out) *out=std::move(group);
    return true;
}
}

/* STAMKY_CN_DETAIL
 * runtime.bin 是只读热路径缓存，不是用户数据源。Builder 把已验证 Model 编成紧凑二进制，所有字符串
 * 使用“字节长度 + UTF-8”，图标记录包含尺寸和 payload 长度；写入过程中任何计数/字段越界都会立即失败。
 * 输出先进入临时文件，关闭后再核对实际文件尺寸并原子替换，避免 Host 下一次 mmap 到半写入文件。
 */
bool RuntimeCacheBuilder::build(const Model& model){
    if(model.groups.empty()||model.groups.size()>kMaxGroups) return false;
    ensure_directories();
    const auto dir=cache_dir();
    if(dir.empty())return false;
    const auto tmp=dir/L"runtime.bin.tmp";
    const auto dst=dir/L"runtime.bin";
    std::ofstream f(tmp,std::ios::binary|std::ios::trunc);
    if(!f) return false;
    f.write(kMagic,sizeof(kMagic));
    const uint32_t groupCount=static_cast<uint32_t>(model.groups.size());
    if(!f.good()||!put(f,kVersion)||!put(f,groupCount)){f.close();DeleteFileW(tmp.c_str());return false;}
    IconCache icons;
    bool ok=true;
    for(const auto& group:model.groups){
        if(group.id.empty()||group.items.size()>kMaxItemsPerGroup){ok=false;break;}
        const std::streampos sizePos=f.tellp();
        const uint32_t zero=0;
        if(sizePos==std::streampos(-1)||!put(f,zero)){ok=false;break;}
        const std::streampos start=f.tellp();
        if(start==std::streampos(-1)||!put_str(f,group.id)||!put_str(f,group.name)){ok=false;break;}
        const uint32_t itemCount=static_cast<uint32_t>(group.items.size());
        if(!put(f,itemCount)){ok=false;break;}
        for(const auto& item:group.items){
            if(item.id.empty()){ok=false;break;}
            const uint32_t type=static_cast<uint32_t>(item.type);
            if(!valid_item_type(type)||!put(f,type)||!put_str(f,item.id)||!put_str(f,item.name)||!put_str(f,item.path)||
               !put_str(f,item.arguments)||!put_str(f,item.workingDirectory)||!put_str(f,item.targetGroupId)){ok=false;break;}
            const std::streampos iconCountPos=f.tellp();
            uint32_t iconCount=0;
            if(iconCountPos==std::streampos(-1)||!put(f,iconCount)){ok=false;break;}
            if(item.type!=ItemType::Group){
                icons.ensure(item);
                for(int s:kIconSizes){
                    auto px=icons.load(item.id,s);
                    if(!px) continue;
                    if(s<=0||s>kMaxIconSize||px->bgra.size()!=static_cast<size_t>(s)*s*4||px->bgra.size()>std::numeric_limits<uint32_t>::max()) continue;
                    const uint16_t size=static_cast<uint16_t>(s);
                    const uint32_t bytes=static_cast<uint32_t>(px->bgra.size());
                    if(!put(f,size)||!put(f,bytes)){ok=false;break;}
                    f.write(reinterpret_cast<const char*>(px->bgra.data()),static_cast<std::streamsize>(bytes));
                    if(!f.good()){ok=false;break;}
                    ++iconCount;
                }
            }
            if(!ok) break;
            const std::streampos current=f.tellp();
            if(current==std::streampos(-1)){ok=false;break;}
            f.seekp(iconCountPos);
            if(!put(f,iconCount)){ok=false;break;}
            f.seekp(current);
            if(!f.good()){ok=false;break;}
        }
        if(!ok) break;
        const std::streampos finish=f.tellp();
        if(finish==std::streampos(-1)||finish<start){ok=false;break;}
        const auto groupBytes64=static_cast<uint64_t>(finish-start);
        if(groupBytes64>std::numeric_limits<uint32_t>::max()){ok=false;break;}
        const uint32_t groupBytes=static_cast<uint32_t>(groupBytes64);
        f.seekp(sizePos);
        if(!put(f,groupBytes)){ok=false;break;}
        f.seekp(finish);
        if(!f.good()){ok=false;break;}
    }
    f.flush();
    ok=ok&&f.good();
    f.close();
    if(!ok){DeleteFileW(tmp.c_str());return false;}
    std::error_code ec;
    const auto tmpSize=std::filesystem::file_size(tmp,ec);
    if(ec||tmpSize<16||tmpSize>kMaxCacheBytes){DeleteFileW(tmp.c_str());return false;}
    if(!MoveFileExW(tmp.c_str(),dst.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

RuntimeCache::~RuntimeCache(){close();}
void RuntimeCache::close(){
    index_.clear();
    if(base_){UnmapViewOfFile(base_);base_=nullptr;}
    if(mapping_){CloseHandle(mapping_);mapping_=nullptr;}
    if(file_!=INVALID_HANDLE_VALUE){CloseHandle(file_);file_=INVALID_HANDLE_VALUE;}
    size_=0;
}
/* STAMKY_CN_DETAIL
 * open() 先 CreateFileW/GetFileSizeEx，再 CreateFileMappingW/MapViewOfFile。file_/mapping_/view_ 的释放顺序
 * 由 close() 统一管理：所有 RuntimeGroupView/RuntimeItemView 都只是映射区域的轻量视图，不能在 unmap
 * 后继续持有。parse_index() 失败会立刻 close，确保调用者永远不会看到“映射成功但索引无效”的半状态。
 */
bool RuntimeCache::open(){
    close();
    const auto dir=cache_dir();
    if(dir.empty())return false;
    const auto path=dir/L"runtime.bin";
    file_=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file_==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    if(!GetFileSizeEx(file_,&li)||li.QuadPart<=0||static_cast<unsigned long long>(li.QuadPart)>kMaxCacheBytes||
       static_cast<unsigned long long>(li.QuadPart)>static_cast<unsigned long long>(std::numeric_limits<size_t>::max())){
        close();return false;
    }
    size_=static_cast<size_t>(li.QuadPart);
    mapping_=CreateFileMappingW(file_,nullptr,PAGE_READONLY,0,0,nullptr);
    if(!mapping_){close();return false;}
    base_=static_cast<const uint8_t*>(MapViewOfFile(mapping_,FILE_MAP_READ,0,0,0));
    if(!base_){close();return false;}
    if(!parse_index()){close();return false;}
    return true;
}
/* STAMKY_CN_DETAIL
 * 解析器把文件当作不可信输入：验证 STAMKY5 magic/version、顶层计数、每组边界、字符串长度、ItemType、
 * 图标尺寸/payload、重复 Group/Item ID，并要求最后一个字节恰好落在 EOF。尤其不能只做“p+n <= end”
 * 而忽略整数溢出或 x86 size_t 可表示范围，否则同一缓存可能在 x64 正常、x86 越界。
 */
bool RuntimeCache::parse_index(){
    index_.clear();
    if(!base_||size_<16||memcmp(base_,kMagic,sizeof(kMagic))!=0) return false;
    const uint8_t* p=base_+sizeof(kMagic);
    const uint8_t* end=base_+size_;
    uint32_t version=0,groupCount=0;
    if(!get(p,end,version)||!get(p,end,groupCount)||version!=kVersion||groupCount>kMaxGroups) return false;
    if(groupCount==0) return false;
    std::unordered_set<std::wstring> ids;
    std::unordered_set<std::wstring> itemIds;
    index_.reserve(groupCount);
    for(uint32_t i=0;i<groupCount;++i){
        const uint8_t* record=p;
        uint32_t groupBytes=0;
        if(!get(p,end,groupBytes)||p>end||static_cast<size_t>(end-p)<groupBytes) return false;
        const uint8_t* groupEnd=p+groupBytes;
        RuntimeGroupView header;
        const uint8_t* hp=p;
        if(!get_str(hp,groupEnd,header.id)||header.id.empty()||!get_str(hp,groupEnd,header.name)) return false;
        const auto key=lower_copy(header.id);
        if(!ids.insert(key).second) return false;
        if(!parse_group_payload(p,groupEnd,nullptr,&itemIds)) return false;
        index_.push_back({std::move(header.id),std::move(header.name),static_cast<size_t>(record-base_)});
        p=groupEnd;
    }
    return p==end;
}
std::vector<std::pair<std::wstring,std::wstring>> RuntimeCache::group_index()const{
    std::vector<std::pair<std::wstring,std::wstring>> out;
    out.reserve(index_.size());
    for(const auto& x:index_) out.push_back({x.id,x.name});
    return out;
}
std::optional<RuntimeGroupView> RuntimeCache::group(const std::wstring& id)const{
    if(!base_) return std::nullopt;
    const auto ix=std::find_if(index_.begin(),index_.end(),[&](const Index& x){return _wcsicmp(x.id.c_str(),id.c_str())==0;});
    if(ix==index_.end()||ix->offset>=size_) return std::nullopt;
    const uint8_t* p=base_+ix->offset;
    const uint8_t* fileEnd=base_+size_;
    uint32_t groupBytes=0;
    if(!get(p,fileEnd,groupBytes)||p>fileEnd||static_cast<size_t>(fileEnd-p)<groupBytes) return std::nullopt;
    RuntimeGroupView group;
    if(!parse_group_payload(p,p+groupBytes,&group)) return std::nullopt;
    return group;
}
}
