#include "hotstuff/hotstuff.h"
#include "hotstuff/client.h"

using salticidae::static_pointer_cast;

#define LOG_INFO HOTSTUFF_LOG_INFO
#define LOG_DEBUG HOTSTUFF_LOG_DEBUG
#define LOG_WARN HOTSTUFF_LOG_WARN

namespace hotstuff {

const opcode_t MsgPropose::opcode;
MsgPropose::MsgPropose(const Proposal &proposal) { serialized << proposal; }
void MsgPropose::postponed_parse(HotStuffCore *hsc) {
    proposal.hsc = hsc;
    serialized >> proposal;
}

const opcode_t MsgVote::opcode;
MsgVote::MsgVote(const Vote &vote) { serialized << vote; }
void MsgVote::postponed_parse(HotStuffCore *hsc) {
    vote.hsc = hsc;
    serialized >> vote;
}

const opcode_t MsgReqBlock::opcode;
MsgReqBlock::MsgReqBlock(const std::vector<uint256_t> &blk_hashes) {
    serialized << (uint32_t)htole(blk_hashes.size());
    for (const auto &h: blk_hashes)
        serialized << h;
}

MsgReqBlock::MsgReqBlock(DataStream &&s) {
    uint32_t size;
    s >> size;
    size = letoh(size);
    blk_hashes.resize(size);
    for (auto &h: blk_hashes) s >> h;
}

const opcode_t MsgRespBlock::opcode;
MsgRespBlock::MsgRespBlock(const std::vector<block_t> &blks) {
    serialized << htole((uint32_t)blks.size());
    for (auto blk: blks) serialized << *blk;
}

void MsgRespBlock::postponed_parse(HotStuffCore *hsc) {
    uint32_t size;
    serialized >> size;
    size = letoh(size);
    blks.resize(size);
    for (auto &blk: blks)
    {
        Block _blk;
        _blk.unserialize(serialized, hsc);
        if (!_blk.verify(hsc->get_config()))
            blk = hsc->storage->add_blk(std::move(_blk));
        else
        {
            blk = nullptr;
            LOG_WARN("block is invalid");
        }
    }
}

promise_t HotStuffBase::exec_command(command_t cmd) {
    const uint256_t &cmd_hash = cmd->get_hash();
    ReplicaID proposer = pmaker->get_proposer();
    /* not the proposer */
    if (proposer != get_id())
        return promise_t([proposer, cmd_hash](promise_t &pm) {
            pm.resolve(Finality(proposer, -1, 0, 0,
                                cmd_hash, uint256_t()));
        });
    cmd_pending.push(storage->add_cmd(cmd));
    if (cmd_pending.size() >= blk_size)
    {
        std::vector<command_t> cmds;
        for (uint32_t i = 0; i < blk_size; i++)
        {
            cmds.push_back(cmd_pending.front());
            cmd_pending.pop();
        }
        pmaker->beat().then([this, cmds = std::move(cmds)]() {
            on_propose(cmds, pmaker->get_parents());
        });
    }
    auto it = decision_waiting.find(cmd_hash);
    if (it == decision_waiting.end())
    {
        promise_t pm{[](promise_t){}};
        it = decision_waiting.insert(std::make_pair(cmd_hash, pm)).first;
    }
    return it->second;
}

void HotStuffBase::add_replica(ReplicaID idx, const NetAddr &addr,
                                pubkey_bt &&pub_key) {
    HotStuffCore::add_replica(idx, addr, std::move(pub_key));
    if (addr != listen_addr)
        pn.add_peer(addr);
}

void HotStuffBase::on_fetch_blk(const block_t &blk) {
#ifdef HOTSTUFF_ENABLE_TX_PROFILE
    blk_profiler.get_tx(blk->get_hash());
#endif
    LOG_DEBUG("fetched %.10s", get_hex(blk->get_hash()).c_str());
    part_fetched++;
    fetched++;
    for (auto cmd: blk->get_cmds()) on_fetch_cmd(cmd);
    const uint256_t &blk_hash = blk->get_hash();
    auto it = blk_fetch_waiting.find(blk_hash);
    if (it != blk_fetch_waiting.end())
    {
        it->second.resolve(blk);
        blk_fetch_waiting.erase(it);
    }
}

void HotStuffBase::on_fetch_cmd(const command_t &cmd) {
    const uint256_t &cmd_hash = cmd->get_hash();
    auto it = cmd_fetch_waiting.find(cmd_hash);
    if (it != cmd_fetch_waiting.end())
    {
        it->second.resolve(cmd);
        cmd_fetch_waiting.erase(it);
    }
}

void HotStuffBase::on_deliver_blk(const block_t &blk) {
    const uint256_t &blk_hash = blk->get_hash();
    bool valid;
    /* sanity check: all parents must be delivered */
    for (const auto &p: blk->get_parent_hashes())
        assert(storage->is_blk_delivered(p));
    if ((valid = HotStuffCore::on_deliver_blk(blk)))
    {
        LOG_DEBUG("block %.10s delivered",
                get_hex(blk_hash).c_str());
        part_parent_size += blk->get_parent_hashes().size();
        part_delivered++;
        delivered++;
    }
    else
    {
        LOG_WARN("dropping invalid block");
    }

    auto it = blk_delivery_waiting.find(blk_hash);
    if (it != blk_delivery_waiting.end())
    {
        auto &pm = it->second;
        if (valid)
        {
            pm.elapsed.stop(false);
            auto sec = pm.elapsed.elapsed_sec;
            part_delivery_time += sec;
            part_delivery_time_min = std::min(part_delivery_time_min, sec);
            part_delivery_time_max = std::max(part_delivery_time_max, sec);

            pm.resolve(blk);
        }
        else
        {
            pm.reject(blk);
            // TODO: do we need to also free it from storage?
        }
        blk_delivery_waiting.erase(it);
    }
}

promise_t HotStuffBase::async_fetch_blk(const uint256_t &blk_hash,
                                        const NetAddr *replica_id,
                                        bool fetch_now) {
    if (storage->is_blk_fetched(blk_hash))
        return promise_t([this, &blk_hash](promise_t pm){
            pm.resolve(storage->find_blk(blk_hash));
        });
    auto it = blk_fetch_waiting.find(blk_hash);
    if (it == blk_fetch_waiting.end())
    {
#ifdef HOTSTUFF_ENABLE_TX_PROFILE
        blk_profiler.rec_tx(blk_hash, false);
#endif
        it = blk_fetch_waiting.insert(
            std::make_pair(
                blk_hash,
                BlockFetchContext(blk_hash, this))).first;
    }
    if (replica_id != nullptr)
        it->second.add_replica(*replica_id, fetch_now);
    return static_cast<promise_t &>(it->second);
}

promise_t HotStuffBase::async_fetch_cmd(const uint256_t &cmd_hash,
                                    const NetAddr *replica_id,
                                    bool fetch_now) {
    if (storage->is_cmd_fetched(cmd_hash))
        return promise_t([this, &cmd_hash](promise_t pm){
            pm.resolve(storage->find_cmd(cmd_hash));
        });
    auto it = cmd_fetch_waiting.find(cmd_hash);
    if (it == cmd_fetch_waiting.end())
    {
        it = cmd_fetch_waiting.insert(
            std::make_pair(cmd_hash, CmdFetchContext(cmd_hash, this))).first;
    }
    if (replica_id != nullptr)
        it->second.add_replica(*replica_id, fetch_now);
    return static_cast<promise_t &>(it->second);
}

promise_t HotStuffBase::async_deliver_blk(const uint256_t &blk_hash,
                                        const NetAddr &replica_id) {
    if (storage->is_blk_delivered(blk_hash))
        return promise_t([this, &blk_hash](promise_t pm) {
            pm.resolve(storage->find_blk(blk_hash));
        });
    auto it = blk_delivery_waiting.find(blk_hash);
    if (it != blk_delivery_waiting.end())
        return static_cast<promise_t &>(it->second);
    BlockDeliveryContext pm{[](promise_t){}};
    it = blk_delivery_waiting.insert(std::make_pair(blk_hash, pm)).first;
    /* otherwise the on_deliver_batch will resolve */
    async_fetch_blk(blk_hash, &replica_id).then([this, replica_id](block_t blk) {
        /* qc_ref should be fetched */
        std::vector<promise_t> pms;
        const auto &qc = blk->get_qc();
        if (qc)
            pms.push_back(async_fetch_blk(qc->get_blk_hash(), &replica_id));
        /* the parents should be delivered */
        for (const auto &phash: blk->get_parent_hashes())
            pms.push_back(async_deliver_blk(phash, replica_id));
        promise::all(pms).then([this, blk]() {
            on_deliver_blk(blk);
        });
    });
    return static_cast<promise_t &>(pm);
}

void HotStuffBase::propose_handler(MsgPropose &&msg, conn_t conn) {
    const NetAddr &peer = conn->get_peer();
    msg.postponed_parse(this);
    auto &prop = msg.proposal;
    block_t blk = prop.blk;
    promise::all(std::vector<promise_t>{
        async_deliver_blk(prop.bqc_hash, peer),
        async_deliver_blk(blk->get_hash(), peer),
    }).then([this, prop = std::move(prop)]() {
        on_receive_proposal(prop);
    });
}

void HotStuffBase::vote_handler(MsgVote &&msg, conn_t conn) {
    const NetAddr &peer = conn->get_peer();
    msg.postponed_parse(this);
    auto &vote = msg.vote;
    promise::all(std::vector<promise_t>{
        async_deliver_blk(vote.bqc_hash, peer),
        async_deliver_blk(vote.blk_hash, peer)
    }).then([this, vote = std::move(vote)]() {
        on_receive_vote(vote);
    });
}

void HotStuffBase::req_blk_handler(MsgReqBlock &&msg, conn_t conn) {
    const NetAddr replica = conn->get_peer();
    auto &blk_hashes = msg.blk_hashes;
    std::vector<promise_t> pms;
    for (const auto &h: blk_hashes)
        pms.push_back(async_fetch_blk(h, nullptr));
    promise::all(pms).then([replica, this](const promise::values_t values) {
        std::vector<block_t> blks;
        for (auto &v: values)
        {
            auto blk = promise::any_cast<block_t>(v);
            blks.push_back(blk);
        }
        pn.send_msg(MsgRespBlock(blks), replica);
    });
}

void HotStuffBase::resp_blk_handler(MsgRespBlock &&msg, conn_t) {
    msg.postponed_parse(this);
    for (const auto &blk: msg.blks)
        if (blk) on_fetch_blk(blk);
}

void HotStuffBase::print_stat() const {
    LOG_INFO("===== begin stats =====");
    LOG_INFO("-------- queues -------");
    LOG_INFO("blk_fetch_waiting: %lu", blk_fetch_waiting.size());
    LOG_INFO("blk_delivery_waiting: %lu", blk_delivery_waiting.size());
    LOG_INFO("cmd_fetch_waiting: %lu", cmd_fetch_waiting.size());
    LOG_INFO("decision_waiting: %lu", decision_waiting.size());
    LOG_INFO("-------- misc ---------");
    LOG_INFO("fetched: %lu", fetched);
    LOG_INFO("delivered: %lu", delivered);
    LOG_INFO("cmd_cache: %lu", storage->get_cmd_cache_size());
    LOG_INFO("blk_cache: %lu", storage->get_blk_cache_size());
    LOG_INFO("------ misc (10s) -----");
    LOG_INFO("fetched: %lu", part_fetched);
    LOG_INFO("delivered: %lu", part_delivered);
    LOG_INFO("decided: %lu", part_decided);
    LOG_INFO("gened: %lu", part_gened);
    LOG_INFO("avg. parent_size: %.3f",
            part_delivered ? part_parent_size / double(part_delivered) : 0);
    LOG_INFO("delivery time: %.3f avg, %.3f min, %.3f max",
            part_delivered ? part_delivery_time / double(part_delivered) : 0,
            part_delivery_time_min == double_inf ? 0 : part_delivery_time_min,
            part_delivery_time_max);

    part_parent_size = 0;
    part_fetched = 0;
    part_delivered = 0;
    part_decided = 0;
    part_gened = 0;
    part_delivery_time = 0;
    part_delivery_time_min = double_inf;
    part_delivery_time_max = 0;
#ifdef HOTSTUFF_MSG_STAT
    LOG_INFO("-- sent opcode (10s) --");
    auto &sent_op = pn.get_sent_by_opcode();
    for (auto &op: sent_op)
    {
        auto &val = op.second;
        LOG_INFO("%02x: %lu, %.2fBpm", op.first,
                val.first, val.first ? val.second / double(val.first) : 0);
        val.first = val.second = 0;
    }
    LOG_INFO("-- recv opcode (10s) --");
    auto &recv_op = pn.get_recv_by_opcode();
    for (auto &op: recv_op)
    {
        auto &val = op.second;
        LOG_INFO("%02x: %lu, %.2fBpm", op.first,
                val.first, val.first ? val.second / double(val.first) : 0);
        val.first = val.second = 0;
    }
    LOG_INFO("--- replica msg. (10s) ---");
    size_t _nsent = 0;
    size_t _nrecv = 0;
    for (const auto &replica: pn.all_peers())
    {
        auto conn = pn.get_peer_conn(replica);
        size_t ns = conn->get_nsent();
        size_t nr = conn->get_nrecv();
        conn->clear_nsent();
        conn->clear_nrecv();
        LOG_INFO("%s: %u, %u, %u",
            std::string(replica).c_str(), ns, nr, part_fetched_replica[replica]);
        _nsent += ns;
        _nrecv += nr;
        part_fetched_replica[replica] = 0;
    }
    nsent += _nsent;
    nrecv += _nrecv;
    LOG_INFO("sent: %lu", _nsent);
    LOG_INFO("recv: %lu", _nrecv);
    LOG_INFO("--- replica msg. total ---");
    LOG_INFO("sent: %lu", nsent);
    LOG_INFO("recv: %lu", nrecv);
#endif
    LOG_INFO("====== end stats ======");
}

HotStuffBase::HotStuffBase(uint32_t blk_size,
                    ReplicaID rid,
                    privkey_bt &&priv_key,
                    NetAddr listen_addr,
                    pacemaker_bt pmaker,
                    EventContext eb):
        HotStuffCore(rid, std::move(priv_key)),
        listen_addr(listen_addr),
        blk_size(blk_size),
        eb(eb),
        pmaker(std::move(pmaker)),
        pn(eb),

        fetched(0), delivered(0),
        nsent(0), nrecv(0),
        part_parent_size(0),
        part_fetched(0),
        part_delivered(0),
        part_decided(0),
        part_gened(0),
        part_delivery_time(0),
        part_delivery_time_min(double_inf),
        part_delivery_time_max(0)
{
    /* register the handlers for msg from replicas */
    pn.reg_handler(salticidae::handler_bind(&HotStuffBase::propose_handler, this, _1, _2));
    pn.reg_handler(salticidae::handler_bind(&HotStuffBase::vote_handler, this, _1, _2));
    pn.reg_handler(salticidae::handler_bind(&HotStuffBase::req_blk_handler, this, _1, _2));
    pn.reg_handler(salticidae::handler_bind(&HotStuffBase::resp_blk_handler, this, _1, _2));
    pn.listen(listen_addr);
}

void HotStuffBase::do_broadcast_proposal(const Proposal &prop) {
    MsgPropose prop_msg(prop);
    for (const auto &replica: pn.all_peers())
        pn.send_msg(prop_msg, replica);
}

void HotStuffBase::do_vote(ReplicaID last_proposer, const Vote &vote) {
    pmaker->next_proposer(last_proposer)
            .then([this, vote](ReplicaID proposer) {
        if (proposer == get_id())
            on_receive_vote(vote);
        else
            pn.send_msg(MsgVote(vote), get_config().get_addr(proposer));
    });
}

void HotStuffBase::do_decide(Finality &&fin) {
    state_machine_execute(fin);
    auto it = decision_waiting.find(fin.cmd_hash);
    if (it != decision_waiting.end())
    {
        it->second.resolve(std::move(fin));
        decision_waiting.erase(it);
    }
}

HotStuffBase::~HotStuffBase() {}

void HotStuffBase::start(bool eb_loop) {
    /* ((n - 1) + 1 - 1) / 3 */
    uint32_t nfaulty = pn.all_peers().size() / 3;
    if (nfaulty == 0)
        LOG_WARN("too few replicas in the system to tolerate any failure");
    pmaker->init(this);
    on_init(nfaulty);
    if (eb_loop)
        eb.dispatch();
}

}
