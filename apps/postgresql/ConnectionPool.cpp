#include "ConnectionPool.h"
#include "PostgreSQL.h"
#include "pqtypes-int.h"

#include <stdio.h>
#include <sstream>

#include <AmEventDispatcher.h>
#include <AmStatistics.h>

static const string pool_type_master("master");
static const string pool_type_slave("slave");

#define ERROR_CALLBACK \
[token, sender_id](const string& error) {\
    if(!sender_id.empty())\
        AmEventDispatcher::instance()->post(sender_id, new PGResponseError(error, token));\
}

Worker::Worker(const std::string& name, int epollfd)
: name(name), epoll_fd(epollfd)
, failover_to_slave(false)
, retransmit_enable(false)
, use_pipeline(false)
, trans_wait_time(PG_DEFAULT_WAIT_TIME)
, retransmit_interval(PG_DEFAULT_RET_INTERVAL)
, reconnect_interval(PG_DEFAULT_REC_INTERVAL)
, batch_size(PG_DEFAULT_BATCH_SIZE)
, batch_timeout(PG_DEFAULT_BATCH_TIMEOUT)
, max_queue_length(PG_DEFAULT_MAX_Q_LEN)
, retransmit_next_time(0), wait_next_time(0)
, reset_next_time(0), send_next_time(0)
, master(0), slave(0)
, queue_size(stat_group(Gauge, MOD_NAME, "queue").addAtomicCounter().addLabel("worker", name))
, dropped(stat_group(Counter, MOD_NAME, "dropped").addAtomicCounter().addLabel("worker", name))
, ret_size(stat_group(Gauge, MOD_NAME, "retransmit").addAtomicCounter().addLabel("worker", name))
, tr_size(stat_group(Gauge, MOD_NAME, "active").addAtomicCounter().addLabel("worker", name))
, finished(stat_group(Counter, MOD_NAME, "finished").addAtomicCounter().addLabel("worker", name))
, finished_time(stat_group(Counter, MOD_NAME, "finished_time").addAtomicCounter().addLabel("worker", name))
{
    workTimer.link(epoll_fd, true);
}

Worker::~Worker()
{
    workTimer.unlink(epoll_fd);
    resetConnections.clear();
    for(auto& trans: transactions) delete trans.trans;
    transactions.clear();
    for(auto& trans: queue) delete trans.trans;
    queue.clear();
    for(auto& trans: retransmit_q) delete trans.trans;
    retransmit_q.clear();
    for(auto& tr : erased) delete tr;
    erased.clear();
    if(master) delete master;
    if(slave) delete slave;
}

void Worker::getConfig(AmArg& ret)
{
    ret["max_queue_length"] = max_queue_length;
    ret["batch_size"] = batch_size;
    ret["batch_timeout"] = batch_timeout;
    ret["trans_wait_time"] = trans_wait_time;
    ret["reconnect_interval"] = reconnect_interval;
    ret["retransmit_interval"] = retransmit_interval;
    ret["retransmit_enable"] = retransmit_enable;
    ret["failover_to_slave"] = failover_to_slave;
    ret["use_pipeline"] = use_pipeline;
}

void Worker::getStats(AmArg& ret)
{
    ret["queue"] = (long long)queue_size.get();
    ret["retransmit"] = (long long)ret_size.get();
    ret["dropped"] = (long long)dropped.get();
    ret["active"] = (long long)tr_size.get();
    ret["finished"] = (long long)finished.get();

    if(master)
        master->getStats(ret);
    if(slave)
        slave->getStats(ret);
}

void Worker::onConnect(IPGConnection* conn) {
    INFO("connection %s:%p/%s success", name.c_str(), conn, conn->getConnInfo().c_str());
    if(master && !master->checkConnection(conn, true) && slave) slave->checkConnection(conn, true); 
    if(use_pipeline)
        conn->startPipeline();
    if(!prepareds.empty() || !search_pathes.empty() || !init_queries.empty()) {
        IPGTransaction* trans = new ConfigTransaction(prepareds, search_pathes, init_queries, this);
        if(!conn->runTransaction(trans)) {
            ERROR("connection %p/%s of worker \'%s\' transaction already exists ",
                  conn, conn->getConnInfo().c_str(), name.c_str());
            delete trans;
        }
    }
    else
        setWorkTimer(true);
}
void Worker::onReset(IPGConnection* conn, bool connected) {
    INFO("pg connection %s:%p/%s reset", name.c_str(), conn, conn->getConnInfo().c_str());
    if(connected && master && !master->checkConnection(conn, false) && slave) slave->checkConnection(conn, false); 
}
void Worker::onPQError(IPGConnection* conn, const std::string& error) {
    ERROR("pg connection %s:%p/%s error: %s", name.c_str(), conn, conn->getConnInfo().c_str(), error.c_str());
}

void Worker::onStopTransaction(IPGTransaction* trans)
{
    ERROR("pg connection %s:%p/%s stopped transaction %s",
          name.c_str(), trans->get_conn(), trans->get_conn()->getConnInfo().c_str(), trans->get_query()->get_query().c_str());
    for(auto tr_it = transactions.begin();
        tr_it != transactions.end(); tr_it++) {
        if(trans == tr_it->trans) {
            retransmit_q.emplace_back(tr_it->trans, (ConnectionPool*)0, tr_it->sender_id, tr_it->token);
            tr_size.dec((long long)tr_it->trans->get_size());
            ret_size.inc((long long)tr_it->trans->get_size());
            transactions.erase(tr_it);
            return;
        }
    }
}

void Worker::onConnectionFailed(IPGConnection* conn, const std::string& error) {
    ERROR("pg connection %s:%p/%s failed: %s", name.c_str(), conn, conn->getConnInfo().c_str(), error.c_str());
    resetConnections.push_back(conn);
    reset_next_time = resetConnections[0]->getDisconnectedTime() + reconnect_interval;
    //DBG("worker \'%s\' set next reset time: %lu", name.c_str(), reset_next_time);
    setWorkTimer(false);
}

void Worker::onDisconnect(IPGConnection* conn) {
    INFO("pg connection %s:%p/%s disconnect", name.c_str(), conn, conn->getConnInfo().c_str());
    if(master && !master->checkConnection(conn, false) && slave) slave->checkConnection(conn, false); 
    resetConnections.push_back(conn);
    reset_next_time = resetConnections[0]->getDisconnectedTime();
    //DBG("worker \'%s\' set next reset time: %lu", name.c_str(), reset_next_time);
    setWorkTimer(false);
}

void Worker::onSock(IPGConnection* conn, IConnectionHandler::EventType type)
{
    int ret = 0;
    if(type == PG_SOCK_NEW) {
        epoll_event event;
        event.events = EPOLLIN | EPOLLERR | EPOLLET;
        event.data.ptr = conn;

        // add the socket to the epoll file descriptors
        ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn->getSocket(), &event);
    } else if(type == PG_SOCK_DEL) {
        ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->getSocket(), nullptr);
    } else {
        epoll_event event;
        event.events = EPOLLERR;
        event.data.ptr = conn;
        if(type == PG_SOCK_WRITE) event.events |= EPOLLOUT;
        if(type == PG_SOCK_READ) event.events |= EPOLLIN;
        if(type == PG_SOCK_RW) event.events |= EPOLLOUT | EPOLLIN;

        ret = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->getSocket(), &event);
    }

    if(ret < 0) {
        resetConnections.push_back(conn);
        reset_next_time = resetConnections[0]->getDisconnectedTime() + reconnect_interval;
        //DBG("worker \'%s\' set next reset time: %lu", name.c_str(), reset_next_time);
        setWorkTimer(false);
    }
}

void Worker::onError(IPGTransaction* trans, const string& error) {
    ERROR("Error of transaction \'%p/%s\' : %s", trans, trans->get_query()->get_query().c_str(), error.c_str());
    for(auto tr_it = transactions.begin();
        tr_it != transactions.end(); tr_it++) {
        if(trans == tr_it->trans) {
            onErrorTransaction(*tr_it, error);
            tr_size.dec((long long)tr_it->trans->get_size());
            transactions.erase(tr_it);
            return;
        }
    }
}


void Worker::onErrorCode(IPGTransaction* trans, const string& error) {
    ERROR("error code: \"%s\"", error.c_str());
    if(reconnect_errors.empty() ||
       reconnect_errors.end() != std::find(reconnect_errors.begin(), reconnect_errors.end(), error))
    {
        if(master && !master->checkConnection(trans->get_conn(), false) && slave)
            slave->checkConnection(trans->get_conn(), false);

        resetConnections.push_back(trans->get_conn());
        reset_next_time = resetConnections[0]->getDisconnectedTime();
        //DBG("worker \'%s\' set next reset time: %lu", name.c_str(), reset_next_time);
        setWorkTimer(false);
        return;
    }
}

void Worker::onTuple(IPGTransaction* trans, const AmArg& result) {
}

void Worker::onFinish(IPGTransaction* trans, const AmArg& result) {
    setWorkTimer(true);
    erased.push_back(trans);
    //DBG("transaction query \'%p/%s\' finished", trans, trans->get_query()->get_query().c_str());
    for(auto tr_it = transactions.begin();
        tr_it != transactions.end(); tr_it++){
        if(trans == tr_it->trans) {
            /*DBG("post PGResponse %s/%s: %s",
                tr_it->sender_id.data(),
                tr_it->token.data(),
                AmArg::print(result).c_str());*/

            if(!tr_it->sender_id.empty())
                AmEventDispatcher::instance()->post(tr_it->sender_id, new PGResponse(result, tr_it->token));
            finished.inc((long long)tr_it->trans->get_size());
            tr_size.dec((long long)tr_it->trans->get_size());
            finished_time.inc(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - tr_it->sendTime).count());
            transactions.erase(tr_it);
            return;
        }
    }
}

void Worker::onPQError(IPGTransaction* trans, const std::string& error) {
    DBG("Error of transaction \'%s\' : %s", trans->get_query()->get_query().c_str(), error.c_str());
    for(auto tr_it = transactions.begin();
        tr_it != transactions.end(); tr_it++) {
        if(trans == tr_it->trans) {
            onErrorTransaction(*tr_it, error);
            tr_size.dec((long long)tr_it->trans->get_size());
            transactions.erase(tr_it);
            return;
        }
    }
}

void Worker::onCancel(IPGTransaction* trans) {
    //DBG("transaction with query %s canceling", conn->get_query()->get_query().c_str());
}

void Worker::onSend(IPGTransaction* trans)
{
    for(auto tr_it = transactions.begin();
        tr_it != transactions.end(); tr_it++) {
        if(trans == tr_it->trans) {
            if(!tr_it->sendTime.time_since_epoch().count()) {
                tr_it->sendTime = std::chrono::steady_clock::now();
            }
            return;
        }
    }
}

bool Worker::processEvent(void* p)
{
    if(p == &workTimer) {
        onTimer();
        workTimer.read();
        return true;
    }
    return false;
}

void Worker::createPool(PGWorkerPoolCreate::PoolType type, const PGPool& pool)
{
    if(type == PGWorkerPoolCreate::Master) {
        if(!master) master = new ConnectionPool(pool, this,  PGWorkerPoolCreate::Master);
        else ERROR("master connection pool of worker %s already created", name.c_str());
    }
    if(type == PGWorkerPoolCreate::Slave) {
        if(!slave) slave = new ConnectionPool(pool, this, PGWorkerPoolCreate::Slave);
        else ERROR("slave connection pool of worker %s already created", name.c_str());
    }
}

void Worker::getFreeConnection(IPGConnection **conn, ConnectionPool **pool, std::function<void(const string&)> func)
{
    do {
        if(!*pool && master)
            *pool = master;
        else if(slave && (!*pool ||
                         *pool == master))
            *pool = slave;
        else {
            func("worker not found");
            return;
        }

        *conn = (*pool)->getFreeConnection();
        if(!*conn) {
            continue;
        }
    } while(!*pool);
}

// -1 - no free connection, wait and break from queue circle
//  0 - transaction has executed or deleted, delete from queue
//  1 - no free connection in slave pool or retransmit time not expired,
//      drop transaction to next timer
int Worker::retransmitTransaction(TransContainer& trans)
{
    retransmit_next_time = 0;
    bool is_ret_timer_set = false;
    time_t current_time = time(0);
    if(!trans.currentPool) {
        IPGConnection* conn = 0;
        ConnectionPool* pool = 0;
        string query = trans.trans->get_query()->get_query();
        string sender_id = trans.sender_id;
        string token = trans.token;
        getFreeConnection(&conn, &pool, ERROR_CALLBACK);
        if(!conn) return -1;
        transactions.emplace_back(trans.trans, pool, sender_id, token);
        tr_size.inc((long long)trans.trans->get_size());
        wait_next_time = transactions.front().createdTime + trans_wait_time;
        //DBG("worker \'%s\' set next wait time %lu", name.c_str(), wait_next_time);
        conn->runTransaction(trans.trans);
        setWorkTimer(false);
        return 0;
    } else if(trans.currentPool == master){
        if(!failover_to_slave && !retransmit_enable) {
            delete trans.trans;
            return 0;
        } else if(current_time - trans.createdTime < retransmit_interval) {
            if(!is_ret_timer_set) {
                retransmit_next_time = trans.createdTime + retransmit_interval;
                is_ret_timer_set = true;
                //DBG("worker \'%s\' set next retransmit time: %lu", name.c_str(), retransmit_next_time);
            }
            return 1;
        }
        IPGConnection* conn = 0;
        ConnectionPool* pool = master;
        string query = trans.trans->get_query()->get_query();
        string sender_id = "";
        string token = trans.token;
        getFreeConnection(&conn, &pool, ERROR_CALLBACK);
        if(failover_to_slave && slave && !conn)
            return 1;
        else if(failover_to_slave && conn) {
            transactions.emplace_back(trans.trans, pool, trans.sender_id, token);
            tr_size.inc((long long)trans.trans->get_size());
            wait_next_time = transactions.front().createdTime + trans_wait_time;
            //DBG("worker \'%s\' set next wait time %lu", name.c_str(), wait_next_time);
            setWorkTimer(false);
            conn->runTransaction(trans.trans);
            return 0;
        } else if((failover_to_slave && !slave) ||
                  !failover_to_slave) {
            if(!retransmit_enable) {
                delete trans.trans;
                return 0;
            } else {
                trans.currentPool = 0;
                return retransmitTransaction(trans);
            }
        }
    } if(!retransmit_enable) {
        delete trans.trans;
        return 0;
    } else if(current_time - trans.createdTime < retransmit_interval) {
        if(!is_ret_timer_set) {
            retransmit_next_time = trans.createdTime + retransmit_interval;
            is_ret_timer_set = true;
            //DBG("worker \'%s\' set next retransmit time: %lu", name.c_str(), retransmit_next_time);
        }
        return 1;
    }

    trans.currentPool = 0;
    return retransmitTransaction(trans);
}

void Worker::setWorkTimer(bool immediately)
{
    time_t current = time(0);
    if(immediately) {
        workTimer.set(1, false);
        //DBG("set timer immediately");
    } else {
        time_t interval = 0;
        if(reset_next_time && 
          (!interval || reset_next_time < current || reset_next_time - current < interval)) {
            interval = reset_next_time - current > 0 ? reset_next_time - current : 1;
        }
        if(retransmit_next_time &&
          (!interval || retransmit_next_time < current || retransmit_next_time - current < interval)) {
            interval = retransmit_next_time - current > 0 ? retransmit_next_time - current : 1;
        }
        if(wait_next_time &&
          (!interval || wait_next_time < current || wait_next_time - current < interval)) {
            interval = wait_next_time - current > 0 ? wait_next_time - current : 1;
        }
        if(send_next_time &&
          (!interval || send_next_time < current || send_next_time - current < interval)) {
            interval = send_next_time - current > 0 ? send_next_time - current : 1;
        }
        workTimer.set(interval*1000000, false);
        //DBG("set timer %lu", interval);
    }
    //DBG("current_time - %lu, reset_next_time - %lu, retransmit_next_time - %lu, wait_next_time - %lu, send_next_time - %lu",
    //    current, reset_next_time, retransmit_next_time, wait_next_time, send_next_time);
}

void Worker::checkQueue()
{
    for(auto trans_it = retransmit_q.begin();
        trans_it != retransmit_q.end();) {
        int tr_size = trans_it->trans->get_size();
        int ret = retransmitTransaction(*trans_it);
        if(ret < 0) break;
        else if(ret > 0) trans_it++;
        else {
            ret_size.dec((long long)tr_size);
            trans_it = retransmit_q.erase(trans_it);
        }
    }

    if(send_next_time > time(0) && queue.size() < batch_size) return;

    IPGTransaction* trans = 0;
    size_t count = 0;
    bool need_send = false;
    for(auto trans_it = queue.begin();
        trans_it != queue.end();) {
        if(!trans) {
            trans = trans_it->trans->clone();
            count += trans_it->trans->get_size();
        } else if(!trans->merge(trans_it->trans)) {
            need_send = true;
            trans_it--;
        } else {
            count += trans_it->trans->get_size();
        }

        auto next_it = trans_it; next_it++;
        if(count >= batch_size || need_send || next_it == queue.end()) {
            TransContainer tr(trans, (ConnectionPool*)0, trans_it->sender_id, trans_it->token);
            int ret = retransmitTransaction(tr);
            if(ret < 0) {
                delete trans;
                break;
            } else {
                for(auto it = queue.begin(); it != next_it; it++)
                    delete it->trans;
                trans_it = queue.erase(queue.begin(), next_it);
                queue_size.dec(count);
            }
            count = 0;
            need_send = false;
            trans = 0;
        } else {
            trans_it++;
        }
    }
    if(!queue.size()) send_next_time = 0;
    else send_next_time = time(0) + batch_timeout;

    //DBG("worker \'%s\' set next batch time: %lu", name.c_str(), send_next_time);
}

void Worker::runTransaction(IPGTransaction* trans, const string& sender_id, const std::string& token)
{
    string sender = sender_id;
    if(batch_size > 1 && !sender.empty()) {
        WARN("batch size of worker \'%s\' is not null, sender_id \'%s\' is not null, will ignore sender_id and erase it", name.c_str(), sender.c_str());
        sender.clear();
    }
    if(max_queue_length && queue_size.get() >= max_queue_length) {
        if(!sender.empty())
            AmEventDispatcher::instance()->post(sender, new PGResponseError("queue is full", token));
        dropped.inc((long long)trans->get_size());
        delete trans;
        return;
    }
    queue.emplace_back(trans, (ConnectionPool*)0, sender, token);
    queue_size.inc((long long)trans->get_size());
    if(!send_next_time) {
        send_next_time = time(0) + batch_timeout;
        //DBG("worker \'%s\' set next batch time: %lu", name.c_str(), send_next_time);
    }
    setWorkTimer(false);
}

void Worker::runPrepared(const PGPrepareData& prepared)
{
    prepareds.emplace(prepared.stmt, prepared);

    std::unique_ptr<PreparedTransaction> trans;
    if(prepared.sql_types.empty()) {
        trans.reset(new PreparedTransaction(prepared.stmt, prepared.query, prepared.oids, this));
    } else {
        vector<unsigned int> oids;
        for(const auto &sql_type: prepared.sql_types) {
            auto oid = pg_typname2oid(sql_type);
            if(oid == INVALIDOID) {
                ERROR("unsupported typname '%s' for prepared statement: %s. skip",
                    sql_type.data(), prepared.stmt.data());
                return;
            }
            oids.emplace_back(oid);
        }
        trans.reset(new PreparedTransaction(prepared.stmt, prepared.query, oids, this));
    }

    if(master)
        master->runTransactionForPool(trans.get());
    if(slave)
        slave->runTransactionForPool(trans.get());
}

void Worker::runInitial(IPGQuery *query)
{
    init_queries.emplace_back(query->clone());

    NonTransaction tr(this);
    tr.exec(query);

    if(master)
        master->runTransactionForPool(&tr);
    if(slave)
        slave->runTransactionForPool(&tr);
}

void Worker::setSearchPath(const vector<string>& search_path)
{
    search_pathes = search_path;
    if(search_pathes.empty()) return;
    string query("SET search_path TO ");
    for(auto& path : search_pathes) {
        query += path + ",";
    }
    query.pop_back();

    IPGTransaction* tr = new NonTransaction(this);
    tr->exec(new QueryParams(query, false, false));
    if(master)
        master->runTransactionForPool(tr);
    if(slave)
        slave->runTransactionForPool(tr);
}

void Worker::setReconnectErrors(const vector<std::string>& errors)
{
    reconnect_errors = errors;
}

void Worker::configure(const PGWorkerConfig& e)
{
    prepareds.clear();
    search_pathes.clear();
    init_queries.clear();
    reconnect_errors.clear();

    failover_to_slave = e.failover_to_slave;
    retransmit_enable = e.retransmit_enable;
    use_pipeline = e.use_pipeline;
    trans_wait_time = e.trans_wait_time;
    retransmit_interval = e.retransmit_interval;
    reconnect_interval = e.reconnect_interval;
    batch_size = e.batch_size;
    batch_timeout = e.batch_timeout;
    max_queue_length = e.max_queue_length;

    setSearchPath(e.search_pathes);
    setReconnectErrors(e.reconnect_errors);
    for(auto& prepared : e.prepeared)
        runPrepared(prepared);

    if(master) master->usePipeline(use_pipeline);
    if(slave) slave->usePipeline(use_pipeline);

    reset_next_time = 0;
    resetConnections.clear();
    retransmit_next_time = 0;
    wait_next_time = 0;

    setWorkTimer(false);
}

void Worker::resetPools(PGWorkerPoolCreate::PoolType type)
{
    if(master && type == PGWorkerPoolCreate::Master) master->resetConnections();
    if(slave && type == PGWorkerPoolCreate::Slave) slave->resetConnections();
}

void Worker::resetPools()
{
    if(master) master->resetConnections();
    if(slave) slave->resetConnections();
}

void Worker::onFireTransaction(const TransContainer& trans)
{
    if(!retransmit_enable && !failover_to_slave && !trans.sender_id.empty())
        AmEventDispatcher::instance()->post(trans.sender_id, new PGTimeout(trans.token));
    trans.trans->cancel();
}

void Worker::onErrorTransaction(const Worker::TransContainer& trans, const string& error)
{
    if(!retransmit_enable && !failover_to_slave && !trans.sender_id.empty())
        AmEventDispatcher::instance()->post(trans.sender_id, new PGResponseError(error, trans.token));
    else {
        IPGTransaction* trans_ = 0;
        if(trans.trans->get_type() == TR_NON && !use_pipeline) {
            trans_ = new NonTransaction(this);
            trans_->exec(trans.trans->get_query()->get_current_query()->clone());
            retransmit_q.emplace_back(trans_, trans.currentPool, trans.sender_id, trans.token);
            ret_size.inc((long long)trans_->get_size());
        } else if(trans.trans->get_type() == TR_POLICY ||
                (trans.trans->get_type() == TR_NON && use_pipeline)){
            IPGQuery* query = trans.trans->get_query();
            int qsize = query->get_size();
            IPGQuery* q_ret = 0;
            if(qsize > 1) {
                q_ret = query->get_current_query();
                QueryChain* chain = dynamic_cast<QueryChain*>(query);
                assert(chain);
                chain->removeQuery(q_ret);
            }
            trans_ = trans.trans->clone();
            retransmit_q.emplace_back(trans_, trans.currentPool, trans.sender_id, trans.token);
            ret_size.inc((long long)trans.trans->get_size());
            if(qsize > 1) {
                if(trans.trans->get_type() == TR_POLICY)
                    trans_ = createDbTransaction(this, trans.trans->get_policy().il, trans.trans->get_policy().wp);
                else
                    trans_ = new NonTransaction(this);
                trans_->exec(q_ret);
                retransmit_q.emplace_back(trans_, trans.currentPool, trans.sender_id, trans.token);
                ret_size.inc((long long)trans_->get_size());
            }
        } else {
            trans_ = trans.trans->clone();
            retransmit_q.emplace_back(trans_, trans.currentPool, trans.sender_id, trans.token);
            ret_size.inc((long long)trans.trans->get_size());
        }
    }
}

void Worker::onTimer()
{
    time_t current = time(0);

    for(auto& tr : erased) delete tr;
    erased.clear();

    auto conns = resetConnections;
    resetConnections.clear();
    reset_next_time = 0;
    for(auto conn_it = conns.begin();
        conn_it != conns.end();) {
        if((*conn_it)->getDisconnectedTime() + reconnect_interval < current) {
            (*conn_it)->reset();
            conn_it = conns.erase(conn_it);
            continue;
        }
        reset_next_time = current - (*conn_it)->getDisconnectedTime() + reconnect_interval;
        //DBG("worker \'%s\' set next reset time: %lu", name.c_str(), reset_next_time);
        break;
    }
    resetConnections.insert(resetConnections.begin(), conns.begin(), conns.end());

    for(auto trans_it = transactions.begin();
        trans_it != transactions.end();) {
        if(current - trans_it->createdTime > trans_wait_time &&
            trans_it->trans->get_status() != IPGTransaction::CANCELING) {
            onFireTransaction(*trans_it);
        } else if(current - trans_it->createdTime > trans_wait_time*2 &&
                trans_it->trans->get_status() == IPGTransaction::CANCELING) {
            resetConnections.emplace_back(trans_it->trans->get_conn());
        } else {
            wait_next_time = trans_it->createdTime + trans_wait_time;
            //DBG("worker \'%s\' set next wait time %lu", name.c_str(), wait_next_time);
            trans_it++;
        }
    }
    checkQueue();
    setWorkTimer(false);
}

ConnectionPool::ConnectionPool(const PGPool& pool, Worker* worker, PGWorkerPoolCreate::PoolType type)
  : worker(worker), pool(pool), type(type),
    connected(stat_group(Gauge, MOD_NAME, "connected").addAtomicCounter()
        .addLabel("worker", worker->get_name())
        .addLabel("type", type == PGWorkerPoolCreate::Master ? pool_type_master : pool_type_slave))
{
    std::stringstream conn_info, conn_log_info;

    conn_info << "host=" << pool.host;
    conn_info << " port=" << pool.port;
    conn_info << " dbname=" << pool.name;
    conn_info << " user=" << pool.user;
    conn_info << " password=" << pool.pass;

    conn_log_info << pool.host << ":" << pool.port << "/" << pool.name;

    for(int i = 0; i < pool.pool_size; i++) {
        connections.push_back(PolicyFactory::instance()->createConnection(
            conn_info.str(), conn_log_info.str(), worker));
        connections.back()->reset();
    }
}

ConnectionPool::~ConnectionPool()
{
    auto connections_copy = connections;
    connections.clear();
    for(auto& conn : connections_copy) delete conn;
}

IPGConnection * ConnectionPool::getFreeConnection()
{
    for(auto& conn : connections) {
        if(!conn->isBusy() && conn->getStatus() == CONNECTION_OK) return conn;
    }
    return 0;
}

bool ConnectionPool::checkConnection(IPGConnection* conn, bool connect)
{
    for(auto& conn_ : connections) {
        if(conn_ == conn) {
            connect ? connected.inc() : connected.dec();
            return true;
        }
    }
    return false;
}

void ConnectionPool::runTransactionForPool(IPGTransaction* trans)
{
    for(auto& conn : connections) {
        if(conn->getStatus() == CONNECTION_OK) {
            if(!conn->isBusy())
                conn->runTransaction(trans->clone());
            else
                conn->addPlannedTransaction(trans->clone());
        }
    }
}

void ConnectionPool::resetConnections()
{
    for(auto& conn : connections) {
        conn->reset();
    }
}

void ConnectionPool::usePipeline(bool is_pipeline)
{
    for(auto& conn : connections) {
        if(is_pipeline)
            conn->startPipeline();
        else
            conn->exitPipeline();
    }
}

void ConnectionPool::getStats(AmArg& stats)
{
    AmArg &pool_stats = (type==PGWorkerPoolCreate::Master) ?
        stats[pool_type_master] : stats[pool_type_slave];

    pool_stats["connected"] = (long long)connected.get();
    AmArg &conns = pool_stats["connections"];
    for(auto& conn : connections) {
        conns.push(AmArg());
        auto &conn_info = conns.back();
        conn_info["status"] = conn->getStatus();
        conn_info["socket"] = conn->getSocket();
        conn_info["busy"] = conn->isBusy();
    }

}
