#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace scrapds::steam {
struct InventoryApi {
    void* instance{};
    bool (*deserialize)(void*,std::int32_t*,const void*,std::uint32_t,bool){};
    int (*status)(void*,std::int32_t){};
    bool (*check_owner)(void*,std::int32_t,std::uint64_t){};
    void (*destroy)(void*,std::int32_t){};
};
struct InventoryOutcome {std::uint32_t connection{};int status{};bool owner_valid{};};
class InventoryValidation {
public:
    explicit InventoryValidation(InventoryApi api):api_(api){}
    InventoryValidation(const InventoryValidation&)=delete;
    InventoryValidation& operator=(const InventoryValidation&)=delete;
    ~InventoryValidation(){for(const auto& [key,request]:pending_){(void)key;api_.destroy(api_.instance,request.handle);}}
    bool begin(std::uint32_t connection,std::uint64_t owner,std::span<const std::uint8_t> serialized) {
        if(!api_.instance||!api_.deserialize||!api_.status||!api_.check_owner||!api_.destroy||!owner||serialized.empty()||serialized.size()>1024*1024||pending_.contains(connection))return false;
        std::int32_t handle=-1;
        const bool started=api_.deserialize(api_.instance,&handle,serialized.data(),static_cast<std::uint32_t>(serialized.size()),false);
        if(!started||handle<0) {if(handle>=0)api_.destroy(api_.instance,handle);return false;}
        try {pending_.emplace(connection,Request{handle,owner,std::chrono::steady_clock::now()+std::chrono::seconds(30)});}
        catch(...) {api_.destroy(api_.instance,handle);throw;}
        return true;
    }
    std::vector<InventoryOutcome> poll() {
        std::vector<InventoryOutcome> done;
        for(auto it=pending_.begin();it!=pending_.end();) {
            const auto status=api_.status(api_.instance,it->second.handle);
            if(status==22&&std::chrono::steady_clock::now()<it->second.deadline){++it;continue;}
            const int final_status=status==22?16:status;
            const bool owner_valid=(final_status==1||final_status==27)&&api_.check_owner(api_.instance,it->second.handle,it->second.owner);
            done.push_back({it->first,final_status,owner_valid});
            api_.destroy(api_.instance,it->second.handle);it=pending_.erase(it);
        }
        return done;
    }
    void cancel(std::uint32_t connection) noexcept {const auto it=pending_.find(connection);if(it==pending_.end())return;api_.destroy(api_.instance,it->second.handle);pending_.erase(it);}
private:
    struct Request {std::int32_t handle;std::uint64_t owner;std::chrono::steady_clock::time_point deadline;};
    InventoryApi api_;std::map<std::uint32_t,Request> pending_;
};
}
