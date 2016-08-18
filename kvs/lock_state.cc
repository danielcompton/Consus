// Copyright (c) 2016, Robert Escriva
// All rights reserved.

// e
#include <e/strescape.h>

// BusyBee
#include <busybee_constants.h>

// Google Log
#include <glog/logging.h>

// consus
#include "common/network_msgtype.h"
#include "kvs/configuration.h"
#include "kvs/daemon.h"
#include "kvs/lock_state.h"

using consus::lock_state;

extern bool s_debug_mode;

struct lock_state::request
{
    request() : id(), nonce(), tg() {}
    request(comm_id i, uint64_t n, const transaction_group& x)
        : id(i), nonce(n), tg(x) {}
    ~request() throw () {}
    comm_id id;
    uint64_t nonce;
    transaction_group tg;
};

lock_state :: lock_state(const table_key_pair& tk)
    : m_state_key(tk)
    , m_mtx()
    , m_init(false)
    , m_holder()
    , m_reqs()
{
}

lock_state :: ~lock_state() throw ()
{
}

consus :: table_key_pair
lock_state :: state_key()
{
    return m_state_key;
}

bool
lock_state :: finished()
{
    po6::threads::mutex::hold hold(&m_mtx);
    return !m_init || (m_reqs.empty() && m_holder == transaction_group());
}

void
lock_state :: enqueue_lock(comm_id id, uint64_t nonce,
                           const transaction_group& tg,
                           daemon* d)
{
    po6::threads::mutex::hold hold(&m_mtx);

    if (!ensure_initialized(d))
    {
        return;
    }

    if (m_holder == tg)
    {
        send_response(id, nonce, tg, d);
        return;
    }

    bool found = false;

    for (std::list<request>::iterator it = m_reqs.begin();
            it != m_reqs.end(); ++it)
    {
        if (it->tg == tg)
        {
            found = true;

            if (it->nonce > nonce)
            {
                send_wound_drop(it->id, it->nonce, it->tg, d);
                it->id = id;
                it->nonce = nonce;
            }
            else
            {
                send_wound_drop(id, nonce, tg, d);
            }
        }

        if (it->tg.txid.preempts(m_holder.txid))
        {
            send_wound_abort(id, nonce, m_holder, d);
            LOG_IF(INFO, s_debug_mode) << it->tg << " initiates wound of " << m_holder;
        }
    }

    if (!found)
    {
        m_reqs.push_back(request(id, nonce, tg));
    }

    if (m_holder == transaction_group())
    {
        consus_returncode rc = d->m_data->write_lock(m_state_key.table,
                                                     m_state_key.key, tg);

        if (rc != CONSUS_SUCCESS)
        {
            LOG(ERROR) << "failed lock(\""
                       << e::strescape(m_state_key.table)
                       << "\", \""
                       << e::strescape(m_state_key.key)
                       << "\") nonce=" << nonce;
            return;
        }

        send_response(id, nonce, tg, d);
        m_holder = tg;
    }

    if (tg.txid.preempts(m_holder.txid))
    {
        send_wound_abort(id, nonce, m_holder, d);
        LOG_IF(INFO, s_debug_mode) << tg << " initiates wound of " << m_holder;
    }
}

void
lock_state :: unlock(comm_id id, uint64_t nonce,
                     const transaction_group& tg,
                     daemon* d)
{
    po6::threads::mutex::hold hold(&m_mtx);

    if (!ensure_initialized(d))
    {
        return;
    }

    if (m_holder == tg)
    {
        assert(!m_reqs.empty());
        assert(m_reqs.front().tg == tg);
        request next;

        if (m_reqs.size() > 1)
        {
            std::list<request>::iterator it = m_reqs.begin();
            ++it;
            assert(it != m_reqs.end());
            next = *it;
        }

        consus_returncode rc = d->m_data->write_lock(m_state_key.table,
                                                     m_state_key.key,
                                                     next.tg);

        if (rc != CONSUS_SUCCESS)
        {
            LOG(ERROR) << "failed unlock(\""
                       << e::strescape(m_state_key.table)
                       << "\", \""
                       << e::strescape(m_state_key.key)
                       << "\") nonce=" << nonce;
            return;
        }

        m_reqs.pop_front();
        m_holder = next.tg;

        if (next.tg != transaction_group())
        {
            send_response(next.id, next.nonce, next.tg, d);
        }
    }
    else
    {
        for (std::list<request>::iterator it = m_reqs.begin(); it != m_reqs.end(); )
        {
            if (it->tg == tg)
            {
                it = m_reqs.erase(it);
                send_wound_abort(it->id, it->nonce, it->tg, d);
            }
            else
            {
                ++it;
            }
        }
    }

    // see reasoning in lock_replicator.cc for why we unconditionally act as if
    // we unlocked the lock
    send_response(id, nonce, tg, d);
}

bool
lock_state :: ensure_initialized(daemon* d)
{
    if (m_init)
    {
        return true;
    }

    transaction_group tg;
    consus_returncode rc = d->m_data->read_lock(m_state_key.table,
                                                m_state_key.key, &tg);

    if (rc != CONSUS_SUCCESS && rc != CONSUS_NOT_FOUND)
    {
        LOG(ERROR) << "failed to initialize lock (\""
                   << e::strescape(m_state_key.table)
                   << "\", \""
                   << e::strescape(m_state_key.key)
                   << "\")";
        return false;
    }

    if (tg != transaction_group())
    {
        m_reqs.push_back(request(comm_id(), 0, tg));
        m_holder = tg;
    }

    m_init = true;
    return true;
}

void
lock_state :: send_wound(comm_id id, uint64_t nonce, uint8_t flags,
                         const transaction_group& tg,
                         daemon* d)
{
    if (id == comm_id())
    {
        return;
    }

    LOG_IF(INFO, s_debug_mode) << "sending wound message for " << tg;
    const size_t sz = BUSYBEE_HEADER_SIZE
                    + pack_size(KVS_WOUND_XACT)
                    + sizeof(uint64_t)
                    + sizeof(uint8_t)
                    + pack_size(tg);
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    msg->pack_at(BUSYBEE_HEADER_SIZE)
        << KVS_WOUND_XACT << nonce << flags << tg;
    d->send(id, msg);
}

void
lock_state :: send_wound_drop(comm_id id, uint64_t nonce,
                              const transaction_group& tg,
                              daemon* d)
{
    send_wound(id, nonce, WOUND_XACT_DROP_REQ, tg, d);
}

void
lock_state :: send_wound_abort(comm_id id, uint64_t nonce,
                               const transaction_group& tg,
                               daemon* d)
{
    send_wound(id, nonce, WOUND_XACT_ABORT, tg, d);
}

void
lock_state :: send_response(comm_id id, uint64_t nonce,
                            const transaction_group& tg, daemon* d)
{
    if (id == comm_id())
    {
        return;
    }

    configuration* c = d->get_config();
    replica_set rs;

    if (!c->hash(d->m_us.dc, m_state_key.table, m_state_key.key, &rs))
    {
        return;
    }

    const size_t sz = BUSYBEE_HEADER_SIZE
                    + pack_size(KVS_RAW_LK_RESP)
                    + sizeof(uint64_t)
                    + pack_size(tg)
                    + pack_size(rs);
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    msg->pack_at(BUSYBEE_HEADER_SIZE)
        << KVS_RAW_LK_RESP << nonce << tg << rs;
    d->send(id, msg);
}