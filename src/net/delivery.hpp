#pragma once
#include <set>

namespace scrapds::net {
// Reliable send acceptance transfers retransmission to Steam. If a send is
// rejected, disconnect the peer: a fresh bootstrap is required before it can
// safely issue more commands against the world.
template<class Handle, class Send>
void deliver_world_update(const std::set<Handle>& peers, std::set<Handle>& closing,
                          std::set<Handle>& desynchronized, Send&& send) {
    for(const auto peer:peers) {
        if(closing.contains(peer))continue;
        if(!send(peer)) { closing.insert(peer); desynchronized.insert(peer); }
    }
}
}
