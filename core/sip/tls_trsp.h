#ifndef _tls_trsp_h_
#define _tls_trsp_h_

#include "singleton.h"
#include "transport.h"
#include "tcp_base_trsp.h"
#include "sip_parser_async.h"
#include "ssl_settings.h"

#include <vector>
using std::vector;

#include <sys/socket.h>
#include <event2/event.h>

#include <map>
#include <deque>
#include <string>
using std::map;
using std::deque;
using std::string;

#include <botan/auto_rng.h>
#include <botan/tls_policy.h>
#include <botan/tls_channel.h>
#include <botan/tls_callbacks.h>
#include <botan/credentials_manager.h>

class tls_conf : public Botan::TLS::Policy, public Botan::Credentials_Manager
{
    friend class tls_trsp_socket;
    tls_client_settings* s_client;
    tls_server_settings* s_server;
    std::unique_ptr<Botan::X509_Certificate> certificate;
    std::unique_ptr<Botan::Private_Key> key;

    //ciphersuite policy overrides
    bool policy_override;
    std::string cipher;
    std::string mac;
    std::string sig;
public:
    tls_conf(tls_settings* settings);
    tls_conf(const tls_conf& conf);

    void operator=(const tls_conf& conf);

    //Policy functions
    vector<string> allowed_key_exchange_methods() const override;
    vector<string> allowed_signature_methods() const override;
    vector<string> allowed_ciphers() const override;
    vector<string> allowed_macs() const override;
    size_t minimum_rsa_bits() const override;
    bool allow_tls10()  const override;
    bool allow_tls11()  const override;
    bool allow_tls12()  const override;
    bool allow_dtls10() const override { return false;}
    bool allow_dtls12() const override { return false;}
    bool require_cert_revocation_info() const override { return false; }
    bool require_client_certificate_authentication() const override;

    //Credentials_Manager functions
    vector<Botan::Certificate_Store*> trusted_certificate_authorities(const string& type, const string& context) override;
    vector<Botan::X509_Certificate> cert_chain(const vector<string>& cert_key_types, const string& type, const string& context) override;
    Botan::Private_Key* private_key_for(const Botan::X509_Certificate& cert, const string& type, const string& context) override;

    void set_policy_overrides(std::string sig_, std::string cipher_, std::string mac_);
};

class tls_rand_generator
{
public:
    Botan::AutoSeeded_RNG rng;
    operator Botan::RandomNumberGenerator& () {
        return rng;
    }
    
    void dispose(){}
};

class tls_session_manager
{
    tls_rand_generator rand_tls;
public:
    Botan::TLS::Session_Manager_In_Memory ssm;
    tls_session_manager() : ssm(rand_tls){}
    operator Botan::TLS::Session_Manager_In_Memory& () {
        return ssm;
    }
    
    void dispose(){}
};

typedef singleton<tls_session_manager> session_manager_tls;

class tls_input : public trsp_base_input
{
    unsigned char    orig_input_buf[MAX_TCP_MSGLEN];
    int              orig_input_len;
public:
    tls_input();
    unsigned char*   get_input() { return orig_input_buf + orig_input_len; }
    int              get_input_free_space() {
        if(orig_input_len > MAX_TCP_MSGLEN) return 0;
        return MAX_TCP_MSGLEN - orig_input_len;
    }
    void reset_input() {
        orig_input_len = 0;
    }
    void add_input_len(int len){
        orig_input_len += len;
    }
    int on_input(tcp_base_trsp* trsp);

    virtual int on_tls_record(tcp_base_trsp* trsp, const uint8_t data[], size_t size);
};

class tls_trsp_socket: public tcp_base_trsp, public Botan::TLS::Callbacks
{
    bool tls_connected;
    uint16_t ciphersuite;

    tls_rand_generator rand_gen;
    Botan::TLS::Channel* tls_channel;
    tls_conf settings;

    friend class tls_socket_factory;
    friend class tls_input;
    tls_trsp_socket(trsp_server_socket* server_sock, trsp_worker* server_worker, int sd,
                  const sockaddr_storage* sa, socket_transport transport, event_base* evbase);

    void init(const sockaddr_storage* sa);

    void generate_transport_errors();

    void tls_emit_data(const uint8_t data[], size_t size);
    void tls_record_received(uint64_t seq_no, const uint8_t data[], size_t size);
    void tls_alert(Botan::TLS::Alert alert);
    bool tls_session_established(const Botan::TLS::Session& session);
    void tls_verify_cert_chain(const std::vector<Botan::X509_Certificate>& cert_chain,
                                const std::vector<std::shared_ptr<const Botan::OCSP::Response>>& ocsp_responses,
                                const std::vector<Botan::Certificate_Store*>& trusted_roots,
                                Botan::Usage_Type usage,
                                const std::string& hostname,
                                const Botan::TLS::Policy& policy);
protected:
    deque<msg_buf*> orig_send_q;
protected:
    const char* get_transport() const { return "tls"; }
    tls_trsp_socket(trsp_server_socket* server_sock, trsp_worker* server_worker, int sd,
                            const sockaddr_storage* sa, socket_transport transport, event_base* evbase, trsp_input* input);
public:
    virtual ~tls_trsp_socket();

    void pre_write();
    void post_write();

    void copy_peer_addr(sockaddr_storage* sa);

    int send(const sockaddr_storage* sa, const char* msg,
	   const int msg_len, unsigned int flags);

    void getInfo(AmArg &ret);
};

class tls_socket_factory : public trsp_socket_factory
{
public:
    tls_socket_factory(tcp_base_trsp::socket_transport transport);

    tcp_base_trsp* create_socket(trsp_server_socket* server_sock, trsp_worker* server_worker,
                                         int sd, const sockaddr_storage* sa, event_base* evbase);
};

class tls_server_socket : public trsp_server_socket
{
public:
    tls_server_socket(unsigned short if_num, unsigned short proto_idx,
                      unsigned int opts, socket_transport transport);

    const char* get_transport() const { return "tls"; }

};

void tls_cleanup();

#endif/*_tls_trsp_h_*/
