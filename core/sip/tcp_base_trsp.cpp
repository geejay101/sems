#include <sys/ioctl.h>
#include <algorithm>
#include "tcp_base_trsp.h"
#include "socket_ssl.h"
#include "hash.h"
#include "ip_util.h"
#include "trans_layer.h"
#include "sip_parser.h"
#include "parse_common.h"
#include "parse_via.h"
#include "AmLcConfig.h"
#include "AmUtils.h"

static string generate_ssl_options_string(sockaddr_ssl* sa)
{
    string ret;
    ret += toString(sa->sig);
    ret += ";";
    ret += toString(sa->cipher);
    ret += ";";
    ret += toString(sa->mac);
    return ret;
}

void tcp_base_trsp::on_sock_read(int fd, short ev, void* arg)
{
    if(ev & (EV_READ|EV_TIMEOUT)) {
        ((tcp_base_trsp*)arg)->on_read(ev);
    }
}

void tcp_base_trsp::on_sock_write(int fd, short ev, void* arg)
{
    if(ev & (EV_WRITE|EV_TIMEOUT)) {
        ((tcp_base_trsp*)arg)->on_write(ev);
    }
}

tcp_base_trsp::tcp_base_trsp(trsp_server_socket* server_sock_, trsp_worker* server_worker_,
                             int sd, const sockaddr_storage* sa, trsp_socket::socket_transport transport,
                             event_base* evbase_)
    : trsp_socket(server_sock_->get_if(),0,0,transport,0,sd),
      server_sock(server_sock_), server_worker(server_worker_),
      closed(false), connected(false),
      input_len(0), evbase(evbase_),
      read_ev(NULL), write_ev(NULL)
{
    // local address
    actual_ip = ip = server_sock->get_ip();
    actual_port = port = server_sock->get_port();
    socket_options = server_sock->get_options();
    server_sock->copy_addr_to(&addr);

    // peer address
    memcpy(&peer_addr,sa,sizeof(sockaddr_storage));

    char host[NI_MAXHOST] = "";
    peer_ip = am_inet_ntop(&peer_addr,host,NI_MAXHOST);
    peer_port = am_get_port(&peer_addr);

    // async parser state
    pst.reset((char*)input_buf);

    if(sd > 0) {
        create_events();
    }
}

tcp_base_trsp::~tcp_base_trsp()
{
  DBG("********* connection destructor ***********");
  if(read_ev) {
      DBG("%p free read_ev %p",this, read_ev);
      event_free(read_ev);
  }
  if(write_ev) {
      DBG("%p free write_ev %p",this, write_ev);
      event_free(write_ev);
  }
}

int tcp_base_trsp::parse_input()
{
  for(;;) {
    int err = skip_sip_msg_async(&pst, (char*)(input_buf+input_len));
    if(err) {

      if(err == UNEXPECTED_EOT) {

	if(pst.orig_buf > (char*)input_buf) {

	  int addr_shift = pst.orig_buf - (char*)input_buf;
	  memmove(input_buf, pst.orig_buf, input_len - addr_shift);

	  pst.orig_buf = (char*)input_buf;
	  pst.c -= addr_shift;
	  if(pst.beg)
	    pst.beg -= addr_shift;
	  input_len -= addr_shift;

	  return 0;
	}
	else if(get_input_free_space()){
	  return 0;
	}

	ERROR("message way too big! drop connection...");
      }
      else {
	ERROR("parsing error %i",err);
      }

      pst.reset((char*)input_buf);
      reset_input();

      return -1;
    }

    int msg_len = pst.get_msg_len();
    sip_msg* s_msg = new sip_msg((const char*)pst.orig_buf,msg_len);

    gettimeofday(&s_msg->recv_timestamp,NULL);
    if(get_transport_id() == tls_ipv4 || get_transport_id() == tls_ipv6)
        s_msg->transport_id = sip_transport::TLS;
    else if(get_transport_id() == tcp_ipv4 || get_transport_id() == tcp_ipv6) {
        s_msg->transport_id = sip_transport::TCP;
    } else {
        s_msg->transport_id = sip_transport::TCP;
        ERROR("socket doesn't have transport id");
    }

    copy_peer_addr(&s_msg->remote_ip);
    copy_addr_to(&s_msg->local_ip);

    char host[NI_MAXHOST] = "";
    DBG("vv M [|] u recvd msg via TCP/%i from %s:%i to %s:%i vv\n"
        "--++--\n%.*s--++--\n",
        sd,
        am_inet_ntop_sip(&s_msg->remote_ip,host,NI_MAXHOST),
        am_get_port(&s_msg->remote_ip),
        actual_ip.c_str(), actual_port,
        /*am_inet_ntop_sip(&s_msg->local_ip,host,NI_MAXHOST),
        am_get_port(&s_msg->local_ip),*/
        s_msg->len, s_msg->buf);

    s_msg->local_socket = this;
    inc_ref(this);

    // pass message to the parser / transaction layer
    SIP_info *iface = AmConfig.sip_ifs[server_sock->get_if()].proto_info[server_sock->get_addr_if()];
    trans_layer::instance()->received_msg(s_msg,iface->acl,iface->opt_acl);

    char* msg_end = pst.orig_buf + msg_len;
    char* input_end = (char*)input_buf + input_len;

    if(msg_end < input_end) {
      pst.reset(msg_end);
    }
    else {
      pst.reset((char*)input_buf);
      reset_input();
      return 0;
    }
  }

  // fake:
  //return 0;
}

void tcp_base_trsp::close()
{
    inc_ref((atomic_ref_cnt*)this);

    server_worker->remove_connection(this);

    closed = true;
    DBG("********* closing connection ***********");
    DBG("connection type %s", get_transport());

    DBG("%p del read_ev %p", this, read_ev);
    event_del(read_ev);

    DBG("%p del write_ev %p", this, write_ev);
    event_del(write_ev);

    if(sd > 0) {
        ::close(sd);
        sd = -1;
    }

    generate_transport_errors();

    dec_ref((atomic_ref_cnt*)this);
}

void tcp_base_trsp::generate_transport_errors()
{

    /* avoid deadlock between session processor and tcp worker.
       it is safe to unlock here because 'closed' flag is set to true and
       send_q will not be affected by send() anymore.
       do not forget to avoid double mutex unlock in places where close() is called
    */
    sock_mut.unlock();

    while(!send_q.empty()) {

        msg_buf* msg = send_q.front();
        send_q.pop_front();

        sip_msg s_msg(msg->msg,msg->msg_len);
        delete msg;

        copy_peer_addr(&s_msg.remote_ip);
        copy_addr_to(&s_msg.local_ip);

        trans_layer::instance()->transport_error(&s_msg);
    }
}

void tcp_base_trsp::add_read_event_ul()
{
  sock_mut.unlock();
  add_read_event();
  sock_mut.lock();
}

void tcp_base_trsp::add_read_event()
{
    DBG("%p add read_ev %p",this, read_ev);
    event_add(read_ev, server_sock->get_idle_timeout());
}

void tcp_base_trsp::add_write_event_ul(struct timeval* timeout)
{
  sock_mut.unlock();
  add_write_event(timeout);
  sock_mut.lock();
}

void tcp_base_trsp::add_write_event(struct timeval* timeout)
{
    DBG("%p add write_ev %p",this, write_ev);
    event_add(write_ev, timeout);
}

void tcp_base_trsp::create_events()
{
    read_ev = event_new(evbase, sd, EV_READ|EV_PERSIST,
                        tcp_base_trsp::on_sock_read,
                        (void *)this);
    DBG("%p created read_ev %p with base %p",this, read_ev, evbase);

    write_ev = event_new(evbase, sd, EV_WRITE,
                         tcp_base_trsp::on_sock_write,
                         (void *)this);
    DBG("%p created write_ev %p with base %p",this, write_ev, evbase);
}

int tcp_base_trsp::connect()
{
  int true_opt = 1;

  if(sd > 0) {
    ERROR("pending connection request: close first.");
    return -1;
  }

  if((sd = socket(peer_addr.ss_family,SOCK_STREAM,0)) == -1){
    ERROR("socket: %s\n",strerror(errno));
    return -1;
  }

  if(ioctl(sd, FIONBIO , &true_opt) == -1) {
    ERROR("could not make new connection non-blocking: %s\n",strerror(errno));
    ::close(sd);
    sd = -1;
    return -1;
  }

  if(setsockopt(sd, SOL_SOCKET, SO_REUSEADDR,
     (void*)&true_opt, sizeof (true_opt)) == -1)
  {
    ERROR("setsockopt(SO_REUSEADDR): %s\n",strerror(errno));
    ::close(sd);
    return -1;
  }

  if(socket_options & static_client_port) {
    if(setsockopt(sd, SOL_SOCKET, SO_REUSEPORT,
       (void*)&true_opt, sizeof (true_opt)) == -1)
    {
      ERROR("setsockopt(SO_REUSEPORT): %s\n",strerror(errno));
      ::close(sd);
      return -1;
    }
#if TCP_STATIC_CLIENT_PORT_CLOSE_NOWAIT==1
    struct linger linger_opt = {
      .l_onoff = 1,
      .l_linger = 0
    };
    if(setsockopt(sd, SOL_SOCKET, SO_LINGER,
       (void*)&linger_opt, sizeof (struct linger)) == -1)
    {
      ERROR("setsockopt(SO_LINGER): %s\n",strerror(errno));
      return -1;
    }
#endif
  } else {
      am_set_port(&addr,0);
  }

  if(::bind(sd,(const struct sockaddr*)&addr,SA_len(&addr)) < 0) {
    ERROR("bind: %s\n",strerror(errno));
    ::close(sd);
    return -1;
  }

  DBG("connecting to %s:%i...",
      am_inet_ntop(&peer_addr).c_str(),
      am_get_port(&peer_addr));

  return ::connect(sd, (const struct sockaddr*)&peer_addr,
		   SA_len(&peer_addr));
}

int tcp_base_trsp::check_connection()
{
  if(sd < 0){
    int ret = connect();
    if(ret < 0) {
      if(errno != EINPROGRESS && errno != EALREADY) {
	ERROR("could not connect: %s",strerror(errno));
	::close(sd);
	sd = -1;
	return -1;
      }
    }

    //memorize actual ip/port
    sockaddr_storage actual_addr;
    socklen_t actual_addr_len = sizeof(actual_addr);
    getsockname(sd,(sockaddr *)&actual_addr,&actual_addr_len);
    actual_ip = am_inet_ntop(&actual_addr);
    actual_port = am_get_port(&actual_addr);

    // it's time to create the events...
    create_events();

    if(ret < 0) {
      add_write_event(server_sock->get_connect_timeout());
      DBG("connect event added...");

      // because of unlock in ad_write_event_ul,
      // on_connect() might already have been scheduled
      if(closed)
	return -1;
    }
    else {
      // connect succeeded immediatly
      connected = true;
      add_read_event();
    }
  }

  return 0;
}

void tcp_base_trsp::on_read(short ev)
{
    int bytes = 0;
    {   // locked section

        AmControlledLock _l(sock_mut);

        if(ev & EV_TIMEOUT) {
            DBG("************ idle timeout: closing connection **********");
            close();
            _l.release_ownership();
            return;
        }

        DBG("on_read (connected = %i, transport = %s)",connected, get_transport());

        bytes = ::read(sd,get_input(),get_input_free_space());
        if(bytes < 0) {
            switch(errno) {
            case EAGAIN:
                return; // nothing to read

            case ECONNRESET:
            case ENOTCONN:
                DBG("connection has been closed (sd=%i)",sd);
                close();
                _l.release_ownership();
                return;

            case ETIMEDOUT:
                DBG("transmission timeout (sd=%i)",sd);
                close();
                _l.release_ownership();
                return;

            default:
                DBG("unknown error (%i): %s",errno,strerror(errno));
                close();
                _l.release_ownership();
                return;
            }
        }
        else if(bytes == 0) {
            // connection closed
            DBG("connection has been closed (sd=%i)",sd);
            close();
            _l.release_ownership();
            return;
        }
    }// end of - locked section

    add_input_len(bytes);

    // ... and parse it
    if(on_input() < 0) {
        DBG("Error while parsing input: closing connection!");
        sock_mut.lock();
        close();
        //sock_mut.unlock();
    }
}

void tcp_base_trsp::getInfo(AmArg &ret)
{
    AmLock l(sock_mut);

    ret["sd"] = sd;
    ret["queue_size"] = send_q.size();
}

void tcp_base_trsp::on_write(short ev)
{
    AmControlledLock _l(sock_mut);

    DBG("on_write (connected = %i, transport = %s)",connected, get_transport());
    if(!connected) {
        if(on_connect(ev) != 0) {
            _l.release_ownership();
            return;
        }
    }

    pre_write();
    while(!send_q.empty()) {

        msg_buf* msg = send_q.front();
        if(!msg || !msg->bytes_left()) {
            send_q.pop_front();
            delete msg;
            continue;
        }

        // send msg
        int bytes = write(sd,msg->cursor,msg->bytes_left());
        if(bytes < 0) {
            DBG("error on write: %i",bytes);
            switch(errno) {
            case EINTR:
            case EAGAIN: // would block
                add_write_event();
                break;

            default: // unforseen error: close connection
                ERROR("unforseen error: close connection (%i/%s)",
                      errno,strerror(errno));
                close();
                _l.release_ownership();
                break;
            }
            return;
        }

        DBG("send msg via %s/%i from %s:%i to %s:%i\n",
            get_transport(),
            sd,
            actual_ip.c_str(), actual_port,
            get_addr_str(&msg->addr).c_str(),
            am_get_port(&msg->addr));

        if(bytes < msg->bytes_left()) {
            msg->cursor += bytes;
            add_write_event();
            return;
        }

        send_q.pop_front();
        delete msg;
    }

    post_write();
}

int tcp_base_trsp::on_connect(short ev)
{
    DBG("************ on_connect() ***********");
    DBG("connection type %s", get_transport());

    if(ev & EV_TIMEOUT) {
        DBG("********** connection timeout on sd=%i ************\n",sd);
        close();
        return -1;
    }

    socklen_t len = sizeof(int);
    int error = 0;
    if(getsockopt(sd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        ERROR("getsockopt: %s",strerror(errno));
        close();
        return -1;
    }

    if(error != 0) {
        DBG("*********** connection error (sd=%i): %s *********",
            sd,strerror(error));
        close();
        return -1;
    }

    DBG("TCP connection from %s:%u",
        get_peer_ip().c_str(),
        get_peer_port());
    connected = true;
    add_read_event();

    return 0;
}

tcp_base_trsp::msg_buf::msg_buf(const sockaddr_storage* sa, const char* msg,
				  const int msg_len)
  : msg_len(msg_len)
{
  memcpy(&addr,sa,sizeof(sockaddr_storage));
  cursor = this->msg = new char[msg_len];
  memcpy(this->msg,msg,msg_len);
}

tcp_base_trsp::msg_buf::~msg_buf()
{
  delete [] msg;
}

void tcp_base_trsp::copy_peer_addr(sockaddr_storage* sa)
{
    memcpy(sa,&peer_addr,sizeof(sockaddr_storage));
}

tcp_base_trsp* trsp_socket_factory::create_connected(trsp_server_socket* server_sock,
				       trsp_worker* server_worker,
				       int sd, const sockaddr_storage* sa,
					   struct event_base* evbase)
{
  if(sd < 0)
    return 0;

  tcp_base_trsp* sock = create_socket(server_sock, server_worker,sd,sa,evbase);
  sock->connected = true;
  sock->add_read_event();
  return sock;
}

tcp_base_trsp* trsp_socket_factory::new_connection(trsp_server_socket* server_sock,
						 trsp_worker* server_worker,
						 const sockaddr_storage* sa,
						 struct event_base* evbase)
{
  return create_socket(server_sock, server_worker,-1,sa,evbase);
}

class trsp_compare
{
    string opt_str;
public:
    trsp_compare(string opt_string) : opt_str(opt_string){}

    bool operator () (tcp_base_trsp* trsp) {
        sockaddr_storage sa = {0};
        sockaddr_ssl* sa_ssl = 0;
        string ssl_opt;

        trsp->copy_peer_addr(&sa);
        sa_ssl = (sockaddr_ssl*)&sa;
        if(sa_ssl->ssl_marker) {
            ssl_opt = generate_ssl_options_string(sa_ssl);
        }
        return ssl_opt == opt_str;
    }
};

trsp_worker::trsp_worker()
{
    evbase = event_base_new();
}

trsp_worker::~trsp_worker()
{
    event_base_free(evbase);
}

void trsp_worker::add_connection(tcp_base_trsp* client_sock)
{
    string conn_id = client_sock->get_peer_ip()
                     + ":" + int2str(client_sock->get_peer_port());

    DBG("new TCP connection from %s:%u",
        client_sock->get_peer_ip().c_str(),
        client_sock->get_peer_port());

    connections_mut.lock();
    bool bfind = false;
    auto sock_it = connections.find(conn_id);
    if(sock_it != connections.end()) {
        sockaddr_storage sa = {0};
        sockaddr_ssl* sa_ssl = 0;
        string ssl_opt;

        client_sock->copy_peer_addr(&sa);
        sa_ssl = (sockaddr_ssl*)&sa;
        if(sa_ssl->ssl_marker) {
            ssl_opt = generate_ssl_options_string(sa_ssl);
        }
        auto trsp_it = find_if(sock_it->second.begin(), sock_it->second.end(), trsp_compare(ssl_opt));
        if(trsp_it != sock_it->second.end()) {
                dec_ref(*trsp_it);
                *trsp_it = client_sock;
                bfind = true;
        }
    }

    if(!bfind){
        connections[conn_id].push_back(client_sock);
    }

    inc_ref(client_sock);
    connections_mut.unlock();
}

void trsp_worker::remove_connection(tcp_base_trsp* client_sock)
{
    string conn_id = client_sock->get_peer_ip()
                     + ":" + int2str(client_sock->get_peer_port());

    DBG("removing TCP connection from %s",conn_id.c_str());

    connections_mut.lock();
    auto sock_it = connections.find(conn_id);
    if(sock_it != connections.end()) {
        sockaddr_storage sa = {0};
        sockaddr_ssl* sa_ssl = 0;
        string ssl_opt;

        client_sock->copy_peer_addr(&sa);
        sa_ssl = (sockaddr_ssl*)&sa;
        if(sa_ssl->ssl_marker) {
            ssl_opt += generate_ssl_options_string(sa_ssl);
        }
        auto trsp_it = find_if(sock_it->second.begin(), sock_it->second.end(), trsp_compare(ssl_opt));
        if(trsp_it != sock_it->second.end()) {
                dec_ref(*trsp_it);
                sock_it->second.erase(trsp_it);
        }

        DBG("TCP connection from %s removed",conn_id.c_str());

        if(sock_it->second.empty()) {
            connections.erase(sock_it);
        }

    }
    connections_mut.unlock();
}

int trsp_worker::send(trsp_server_socket* server_sock, const sockaddr_storage* sa, const char* msg,
                             const int msg_len, unsigned int flags)
{
    char host_buf[NI_MAXHOST];
    string dest = am_inet_ntop(sa,host_buf,NI_MAXHOST);
    dest += ":" + int2str(am_get_port(sa));
    tcp_base_trsp* sock = NULL;

    bool new_conn=false;
    connections_mut.lock();
    auto sock_it = connections.find(dest);
    if(sock_it != connections.end()) {
        sockaddr_ssl* sa_ssl = (sockaddr_ssl*)sa;
        if(!sa_ssl->ssl_marker) {
            sock = sock_it->second[0];
            inc_ref(sock);
        } else {
            auto trsp_it = find_if(sock_it->second.begin(), sock_it->second.end(), trsp_compare(generate_ssl_options_string(sa_ssl)));
            if(trsp_it != sock_it->second.end()) {
                sock = *trsp_it;
                inc_ref(sock);
            }
        }
    }
    
    if(!sock) {
        //TODO: add flags to avoid new connections (ex: UAs behind NAT)
        tcp_base_trsp* new_sock = new_connection(server_sock, sa);
        sock = new_sock;
        inc_ref(sock);
        new_conn = true;
    }
    connections_mut.unlock();

    // must be done outside from connections_mut
    // to avoid dead-lock with the event base
    int ret = sock->send(sa,msg,msg_len,flags);
    if((ret < 0) && new_conn) {
        remove_connection(sock);
    }
    dec_ref(sock);

    return ret;
}

void trsp_worker::create_connected(trsp_server_socket* server_sock, int sd, const sockaddr_storage* sa)
{
    tcp_base_trsp* new_sock = server_sock->sock_factory->create_connected(server_sock,this,sd,sa,evbase);
    add_connection(new_sock);
}


tcp_base_trsp* trsp_worker::new_connection(trsp_server_socket* server_sock, const sockaddr_storage* sa)
{
    char host_buf[NI_MAXHOST];
    string dest = am_inet_ntop(sa,host_buf,NI_MAXHOST);
    dest += ":" + int2str(am_get_port(sa));
    tcp_base_trsp* new_sock = server_sock->sock_factory->new_connection(server_sock,this,
                                sa,evbase);
    connections[dest].push_back(new_sock);
    inc_ref(new_sock);
    return new_sock;
}

void trsp_worker::getInfo(AmArg &ret)
{
    AmLock l(connections_mut);

    ret.assertStruct();
    for(auto const &con_it: connections) {
        for(auto const &it: con_it.second)
            it->getInfo(ret[con_it.first]);
    }
}

void trsp_worker::run()
{
    // fake event to prevent the event loop from exiting
    int fake_fds[2];
    int ret = pipe(fake_fds);
    (void)ret;
    struct event* ev_default =
        event_new(evbase,fake_fds[0],
                  EV_READ|EV_PERSIST,
                  NULL,NULL);
    event_add(ev_default,NULL);

    setThreadName("sip-worker");

    /* Start the event loop. */
    /*int ret = */event_base_dispatch(evbase);

    // clean-up fake fds/event
    event_free(ev_default);
    close(fake_fds[0]);
    close(fake_fds[1]);
}

void trsp_worker::on_stop()
{
    event_base_loopbreak(evbase);
    join();
}

trsp_server_socket::trsp_server_socket(unsigned short if_num, unsigned short addr_num, unsigned int opts, trsp_socket_factory* sock_factory)
    : trsp_socket(if_num, addr_num, opts, sock_factory->transport), sock_factory(sock_factory)
{
    inc_ref(sock_factory);
}

trsp_server_socket::~trsp_server_socket()
{
    dec_ref(sock_factory);
}

int trsp_server_socket::bind(const string& bind_ip, unsigned short bind_port)
{
    if(sd) {
        WARN("re-binding socket\n");
        close(sd);
    }

    if(am_inet_pton(bind_ip.c_str(),&addr) == 0) {

        ERROR("am_inet_pton(%s): %s\n",bind_ip.c_str(),strerror(errno));
        return -1;
    }

    if( ((addr.ss_family == AF_INET) &&
            (SAv4(&addr)->sin_addr.s_addr == INADDR_ANY)) ||
            ((addr.ss_family == AF_INET6) &&
             IN6_IS_ADDR_UNSPECIFIED(&SAv6(&addr)->sin6_addr)) ) {

        ERROR("Sorry, we cannot bind to 'ANY' address\n");
        return -1;
    }

    am_set_port(&addr,bind_port);

    if((sd = socket(addr.ss_family,SOCK_STREAM,0)) == -1) {
        ERROR("socket: %s\n",strerror(errno));
        return -1;
    }

    int true_opt = 1;
    if(setsockopt(sd, SOL_SOCKET, SO_REUSEADDR,
                  (void*)&true_opt, sizeof (true_opt)) == -1) {
        ERROR("%s\n",strerror(errno));
        close(sd);
        return -1;
    }

    if(socket_options & static_client_port) {
        if(setsockopt(sd, SOL_SOCKET, SO_REUSEPORT,
                      (void*)&true_opt, sizeof (true_opt)) == -1) {
            ERROR("%s\n",strerror(errno));
            close(sd);
            return -1;
        }
    }

    if(ioctl(sd, FIONBIO , &true_opt) == -1) {
        ERROR("setting non-blocking: %s\n",strerror(errno));
        close(sd);
        return -1;
    }

    if(::bind(sd,(const struct sockaddr*)&addr,SA_len(&addr)) < 0) {

        ERROR("bind: %s\n",strerror(errno));
        close(sd);
        return -1;
    }

    if(::listen(sd, 16) < 0) {
        ERROR("listen: %s\n",strerror(errno));
        close(sd);
        return -1;
    }

    actual_port = port = bind_port;
    actual_ip = ip   = bind_ip;

    DBG("TCP transport bound to %s/%i\n",ip.c_str(),port);

    return 0;
}

void trsp_server_socket::on_accept(int fd, short ev, void* arg)
{
    trsp_server_socket* trsp = (trsp_server_socket*)arg;
    trsp->on_accept(fd,ev);
}

uint32_t trsp_server_socket::hash_addr(const sockaddr_storage* addr)
{
    unsigned int port = am_get_port(addr);
    uint32_t h=0;
    if(addr->ss_family == AF_INET) {
        h = hashlittle(&SAv4(addr)->sin_addr,sizeof(in_addr),port);
    }
    else {
        h = hashlittle(&SAv6(addr)->sin6_addr,sizeof(in6_addr),port);
    }
    return h;
}

void trsp_server_socket::add_event(struct event_base *evbase)
{
    this->evbase = evbase;

    if(!ev_accept) {
        ev_accept = event_new(evbase, sd, EV_READ|EV_PERSIST,
                              trsp_server_socket::on_accept, (void *)this);
        DBG("%p created ev_accept %p with base %p",this, ev_accept, evbase);
        DBG("%p add ev_accept %p",this, ev_accept);
        event_add(ev_accept, NULL); // no timeout
    }
}

void trsp_server_socket::add_workers(trsp_worker **trsp_workers, unsigned short n_trsp_workers)
{
    for(unsigned int i=0; i<n_trsp_workers; i++) {
        workers.push_back(trsp_workers[i]);
    }
}

void trsp_server_socket::on_accept(int sd, short ev)
{
    sockaddr_storage src_addr = {0};
    socklen_t        src_addr_len = sizeof(sockaddr_storage);

    int connection_sd = accept(sd,(sockaddr*)&src_addr,&src_addr_len);
    if(connection_sd < 0) {
        WARN("error while accepting connection");
        return;
    }

    int true_opt = 1;
    if(ioctl(connection_sd, FIONBIO , &true_opt) == -1) {
        ERROR("could not make new connection non-blocking: %s\n",strerror(errno));
        close(connection_sd);
        return;
    }

    uint32_t h = hash_addr(&src_addr);
    unsigned int idx = h % workers.size();

    // in case of thread pooling, do following in worker thread
    DBG("trsp_server_socket::create_connected (idx = %u)",idx);
    workers[idx]->create_connected(this, connection_sd,&src_addr);
}

int trsp_server_socket::send(const sockaddr_storage* sa, const char* msg,
                             const int msg_len, unsigned int flags)
{
    uint32_t h = hash_addr(sa);
    unsigned int idx = h % workers.size();
    DBG("trsp_server_socket::send: idx = %u",idx);
    return workers[idx]->send(this, sa,msg,msg_len,flags);
}

void trsp_server_socket::set_connect_timeout(unsigned int ms)
{
    connect_timeout.tv_sec = ms / 1000;
    connect_timeout.tv_usec = (ms % 1000) * 1000;
}

void trsp_server_socket::set_idle_timeout(unsigned int ms)
{
    idle_timeout.tv_sec = ms / 1000;
    idle_timeout.tv_usec = (ms % 1000) * 1000;
}

struct timeval* trsp_server_socket::get_connect_timeout()
{
    if(connect_timeout.tv_sec || connect_timeout.tv_usec)
        return &connect_timeout;

    return NULL;
}

struct timeval* trsp_server_socket::get_idle_timeout()
{
    if(idle_timeout.tv_sec || idle_timeout.tv_usec)
        return &idle_timeout;

    return NULL;
}

void trsp_server_socket::getInfo(AmArg &ret)
{
    for(unsigned int i=0; i<workers.size(); i++) {
        AmArg &r = ret[int2str(i)];
        workers[i]->getInfo(r);
    }
}

trsp::trsp()
{
  evbase = event_base_new();
}

trsp::~trsp()
{
  if(evbase) {
    event_base_free(evbase);
  }
}

void trsp::add_socket(trsp_server_socket* sock)
{
    sock->add_event(evbase);
    INFO("Added SIP server %s transport on %s:%i\n",
        sock->get_transport(), sock->get_ip(),sock->get_port());
}

/** @see AmThread */
void trsp::run()
{
    INFO("Started SIP server thread\n");
    setThreadName("sip-server-trsp");

    /* Start the event loop. */
    event_base_dispatch(evbase);

    INFO("SIP server thread finished");
}

/** @see AmThread */
void trsp::on_stop()
{
  event_base_loopbreak(evbase);
  join();
}
