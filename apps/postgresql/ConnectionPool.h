#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <list>
using std::string;
using std::vector;
using std::list;
#include <PostgreSqlAPI.h>
#include "Connection.h"
#include "Transaction.h"

class ConnectionPool;
class Worker;

class Worker : public ITransactionHandler,
               public IConnectionHandler
{
    int epoll_fd;
    string name;

    bool failover_to_slave;
    bool retransmit_enable;
    bool use_pipeline;
    uint32_t retransmit_interval;
    uint32_t reconnect_interval;
    uint32_t trans_wait_time;
    uint32_t batch_timeout;
    uint32_t batch_size;
    uint32_t max_queue_length;

    AmTimerFd workTimer;
    ConnectionPool* master;
    ConnectionPool* slave;

    vector<IPGConnection*> resetConnections;
    struct TransContainer
    {
        IPGTransaction* trans;
        ConnectionPool* currentPool;
        time_t createdTime;
        std::chrono::steady_clock::time_point sendTime;
        string token;
        string sender_id;
        TransContainer(IPGTransaction* trans, ConnectionPool* pool,
                       const string& sender, const string& token)
            : trans(trans), currentPool(pool), createdTime(time(0))
            , token(token), sender_id(sender) {}
    };

    AtomicCounter& tr_size;
    AtomicCounter& finished;
    AtomicCounter& queue_size;
    AtomicCounter& ret_size;
    AtomicCounter& dropped;
    AtomicCounter& finished_time;

    list<TransContainer> transactions;    //active transactions

    map<string,PGPrepareData> prepareds;  //prepared transaction for all connections that has connected
    vector<string> search_pathes;         //search pathes for all connections that has connected
    vector< std::unique_ptr<IPGQuery> > init_queries; //queries to run on connect
    vector<string> reconnect_errors;

    list<TransContainer> retransmit_q;    //queue of retransmit transactions
    list<TransContainer> queue;           //queue of transaction
    vector<IPGTransaction*> erased;       //temp container for finished transactions(on the next iteration they will be deleted)
    time_t retransmit_next_time;
    time_t wait_next_time;
    time_t reset_next_time;
    time_t send_next_time;

    void getFreeConnection(IPGConnection **conn, ConnectionPool **pool, std::function<void(const string&)> func);
    void checkQueue();
    int retransmitTransaction(TransContainer& trans);
    void setWorkTimer(bool immediately);
public:
    Worker(const string& name, int epollfd);
    ~Worker();
    
    bool processEvent(void* p);

    void createPool(PGWorkerPoolCreate::PoolType type, const PGPool& pool);

    void runPrepared(const PGPrepareData& prepared);
    void runInitial(IPGQuery *query);
    void setSearchPath(const vector<string>& search_path);
    void setReconnectErrors(const vector<string>& errors);

    void runTransaction(IPGTransaction* trans, const string& sender_id, const string& token);
    void configure(const PGWorkerConfig& e);
    void resetPools(PGWorkerPoolCreate::PoolType type);
    void resetPools();

    void onFireTransaction(const TransContainer& trans);
    void onErrorTransaction(const TransContainer& trans, const string& error);
    void onTimer();

    //IConnectionHandler
    void onSock(IPGConnection* conn, EventType type) override;
    void onConnect(IPGConnection* conn) override;
    void onConnectionFailed(IPGConnection* conn, const string& error) override;
    void onDisconnect(IPGConnection* conn) override;
    void onReset(IPGConnection* conn, bool connected) override;
    void onPQError(IPGConnection* conn, const string& error) override;
    void onStopTransaction(IPGTransaction* trans) override;

    //ITransactionHandler
    void onCancel(IPGTransaction* conn) override;
    void onSend(IPGTransaction* conn) override;
    void onError(IPGTransaction* trans, const string& error) override;
    void onErrorCode(IPGTransaction* trans, const string& error) override;
    void onPQError(IPGTransaction* trans, const string& error) override;
    void onFinish(IPGTransaction* trans, const AmArg& result) override;
    void onTuple(IPGTransaction* trans, const AmArg& result) override;

    void getStats(AmArg& ret);
    void getConfig(AmArg& ret);

    string get_name() { return name; }
};

class ConnectionPool
{
    vector<IPGConnection*> connections;
    Worker* worker;
    PGPool pool;
    PGWorkerPoolCreate::PoolType type;
    AtomicCounter& connected;
public:
    ConnectionPool(const PGPool& pool, Worker* worker, PGWorkerPoolCreate::PoolType type);
    ~ConnectionPool();

    IPGConnection* getFreeConnection();
    bool checkConnection(IPGConnection* conn, bool connect);
    void runTransactionForPool(IPGTransaction* trans);
    void resetConnections();
    void usePipeline(bool is_pipeline);

    void getStats(AmArg& stats);
    const PGPool& getInfo() { return pool; }
};
