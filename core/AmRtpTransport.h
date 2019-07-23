#ifndef AM_RTP_TRANSPORT_H
#define AM_RTP_TRANSPORT_H

#include "AmRtpSession.h"
#include "AmRtpConnection.h"
#include "AmArg.h"
#include "AmSdp.h"

#include "sip/ip_util.h"
#include "sip/types.h"
#include "sip/msg_logger.h"

class AmRtpStream;

#define RTP_PACKET_BUF_SIZE 4096
#define RTP_PACKET_TIMESTAMP_DATASIZE (CMSG_SPACE(sizeof(struct timeval)))

class AmRtpTransport : public AmObject,
                       public AmRtpSession
{
public:
    AmRtpTransport(AmRtpStream* _stream, int _if, AddressType type);
    ~AmRtpTransport();

    int getLocalIf() { return l_if; }
    int getLocalProtoId() { return lproto_id; }

    /** set destination for logging all received/sent packets */
    void setLogger(msg_logger *_logger);

    /**
    * Gets RTP local address. If no RTP address in assigned, assigns a new one.
    * @param out local RTP addess.
    */
    void getLocalAddr(struct sockaddr_storage* addr);

    /**
    * Gets RTP port number. If no RTP port in assigned, assigns a new one.
    * @return local RTP port.
    */
    int getLocalPort();

    /**
    * Initializes with a new random local port if 'p' is 0,
    * else binds the given port, and sets own attributes properly.
    */
    void setLocalPort(unsigned short p = 0);

    /**
    * Enables RTP stream.
    * @param local the SDP message generated by the local UA.
    * @param remote the SDP message generated by the remote UA.
    * @warning It is necessary to call getSdpOffer/getSdpAnswer prior to init(...)
    * @warning so that the internal SDP media line index is set properly.
    */
    int init(const SdpMedia& local, const SdpMedia& remote, bool force_passive_mode = false);

    void addConnection(AmStreamConnection* conn);
    void removeConnection(AmStreamConnection* conn);

    int send(sockaddr_storage* raddr, unsigned char* buf, int size);
    int sendmsg(unsigned char* buf, int size);

    void allowStunConnection(sockaddr_storage* remote_addr);
    void dtlsSessionEsteblished(uint16_t srtp_profile);

    AmRtpStream* getRtpStream() { return stream; }
protected:
    int recv(int sd);

    void recvPacket(int fd) override;

    /** initializes and gets the socket descriptor for local socket */
    int getLocalSocket();

    void log_rcvd_packet(const char *buffer, int len, struct sockaddr_storage &recv_addr);
    void log_sent_packet(const char *buffer, int len, struct sockaddr_storage &send_addr);

    AmStreamConnection::ConnectionType GetConnectionType(unsigned char* buf, int size);
    bool isStunMessage(unsigned char* buf, unsigned int size);
    bool isRTPMessage(unsigned char* buf, unsigned int size);
    bool isDTLSMessage(unsigned char* buf, unsigned int size);
    bool isRTCPMessage(unsigned char* buf, unsigned int size);
private:
    msg_logger *logger;

    /** Stream owning this transport */
    AmRtpStream* stream;

    /** Local socket */
    int                l_sd;

    /** Context index in receiver for local socket */
    int                l_sd_ctx;

    /** Local port */
    unsigned short     l_port;

    /**
    * Local interface used for this stream
    * (index into @AmLcConfig::Ifs)
    */
    int l_if;

    /**
    * Local addr index from local interface
    * (index into @AmLcConfig::Ifs.proto_info)
    */
    int lproto_id;

    struct sockaddr_storage l_saddr;

    msghdr recv_msg;
    iovec recv_iov[1];
    unsigned int   b_size;
    unsigned char  buffer[RTP_PACKET_BUF_SIZE];
    unsigned char recv_ctl_buf[RTP_PACKET_TIMESTAMP_DATASIZE];
    struct timeval recv_time;
    struct sockaddr_storage saddr;

    vector<AmStreamConnection*> connections;
};

#endif/*AM_RTP_TRANSPORT_H*/
