#include "log.h"
#include "sys/thread.h"
#include "cstringext.h"
#include "ctypedef.h"
#include "libaio/aio-worker.h"
#include "librtsp/rtsp-server-aio.h"
#include "ntp-time.h"
#include "uri-parse.h"
#include "urlcodec.h"
#include "rtp-media.h"
#include "rtp-transport.h"

typedef struct {
    void* rtsp;
    struct rtp_media_t* media;
    int channel;
} rtsp_server_priv_t;

int rtsp_uri_parse(const char* uri, char* path)
{
    char path1[256];
    struct uri_t* r = uri_parse(uri, strlen(uri));
    if (!r) {
        return -1;
    }

    url_decode(r->path, strlen(r->path), path1, sizeof(path1));
    strncpy(path, path1, sizeof(path1));
    uri_free(r);

    return 0;
}

static int rtsp_ondescribe(void* ptr, rtsp_server_t* rtsp, const char* uri)
{
    const char* pattern_live =
        "v=0\n"
        "o=- %llu %llu IN IP4 %s\n"
        "s=%s\n"
        "c=IN IP4 0.0.0.0\n"
        "t=0 0\n"
        "a=control:*\n";
    rtsp_server_priv_t* priv = ptr;
    tracef("ptr:%p rtsp:%p uri:%s", ptr, rtsp, uri);

    char filename[256] = {};
    rtsp_uri_parse(uri, filename);

    if (!priv->media) {
        priv->media = rtp_media_live_new();
    }

    char sdp[2048] = {};
    snprintf(sdp, sizeof(sdp), pattern_live, ntp64_now(), ntp64_now(), "0.0.0.0", uri);
    priv->media->get_sdp(priv->media, sdp);

    return rtsp_server_reply_describe(rtsp, 200, sdp);
}

static int rtsp_onsetup(void* ptr,
                        rtsp_server_t* rtsp,
                        const char* uri,
                        const char* session,
                        const struct rtsp_header_transport_t transports[],
                        size_t num)
{
    size_t i;
    rtsp_server_priv_t* priv = ptr;
    tracef("ptr:%p rtsp:%p uri:%s session:%s transports:%p num:%d",
        ptr, rtsp, uri, session, transports, num);

    if (priv->media == NULL) {
        // 454 Session Not Found
        return rtsp_server_reply_setup(rtsp, 454, NULL, NULL);
    }

    char filename[256] = {0};
    rtsp_uri_parse(uri, filename);

    char rtsp_transport[128];
    const struct rtsp_header_transport_t* transport = NULL;
    for (i = 0; i < num && !transport; i++) {
        if (RTSP_TRANSPORT_RTP_UDP == transports[i].transport) {
            // RTP/AVP/UDP
            transport = &transports[i];
        } else if (RTSP_TRANSPORT_RTP_TCP == transports[i].transport) {
            // RTP/AVP/TCP
            // 10.12 Embedded (Interleaved) Binary Data (p40)
            transport = &transports[i];
        }
    }
    if (!transport) {
        // 461 Unsupported Transport
        return rtsp_server_reply_setup(rtsp, 461, NULL, NULL);
    }

    if (RTSP_TRANSPORT_RTP_TCP == transport->transport) {
        // 10.12 Embedded (Interleaved) Binary Data (p40)
        int interleaved[2];
        if (transport->interleaved1 == transport->interleaved2) {
            interleaved[0] = priv->channel++;
            interleaved[1] = priv->channel++;
        } else {
            interleaved[0] = transport->interleaved1;
            interleaved[1] = transport->interleaved2;
        }

        int ret = priv->media->add_transport(priv->media, "video", rtp_tcp_transport_new(rtsp, interleaved[0], interleaved[1]));
        if (ret < 0) {
            // 451 Invalid parameter
            return rtsp_server_reply_setup(rtsp, 451, NULL, NULL);
        }

        // RTP/AVP/TCP;interleaved=0-1
        snprintf(rtsp_transport, sizeof(rtsp_transport), "RTP/AVP/TCP;interleaved=%d-%d", interleaved[0], interleaved[1]);
    } else if (transport->multicast) {
        // RFC 2326 1.6 Overall Operation p12
        // Multicast, client chooses address
        // Multicast, server chooses address
        assert(0);
        // 461 Unsupported Transport
        return rtsp_server_reply_setup(rtsp, 461, NULL, NULL);
    } else {
        // unicast
        assert(transport->rtp.u.client_port1 && transport->rtp.u.client_port2);
        uint16_t port[2] = {transport->rtp.u.client_port1, transport->rtp.u.client_port2};
        const char* ip = transport->destination[0] ? transport->destination : rtsp_server_get_client(rtsp, NULL);

        int ret = priv->media->add_transport(priv->media, "video", rtp_udp_transport_new(ip, port));
        if (ret < 0) {
            // 451 Invalid parameter
            return rtsp_server_reply_setup(rtsp, 451, NULL, NULL);
        }

        // RTP/AVP;unicast;client_port=4588-4589;server_port=6256-6257;destination=xxxx
        snprintf(rtsp_transport, sizeof(rtsp_transport),
                 "RTP/AVP;unicast;client_port=%hu-%hu;server_port=%hu-%hu%s%s",
                 transport->rtp.u.client_port1, transport->rtp.u.client_port2,
                 port[0], port[1],
                 transport->destination[0] ? ";destination=" : "",
                 transport->destination[0] ? transport->destination : "");
    }

    tracef("%s", rtsp_transport);
    char sessionid[64] = {0};
    sprintf(sessionid, "%p", ptr);
    return rtsp_server_reply_setup(rtsp, 200, sessionid, rtsp_transport);
}

static int rtsp_onplay(void* ptr,
                       rtsp_server_t* rtsp,
                       const char* uri,
                       const char* session,
                       const int64_t* npt,
                       const double* scale)
{
    rtsp_server_priv_t* priv = ptr;
    tracef("ptr:%p rtsp:%p uri:%s session:%s npt:%p scale:%p",
        ptr, rtsp, uri, session, npt, scale);

    if (priv->media == NULL) {
        return rtsp_server_reply_play(rtsp, 454, NULL, NULL, NULL);
    }

    priv->media->play(priv->media);

    // RFC 2326 12.33 RTP-Info (p55)
    // 1. Indicates the RTP timestamp corresponding to the time value in the
    // Range response header.
    // 2. A mapping from RTP timestamps to NTP timestamps (wall clock) is
    // available via RTCP.
    char rtpinfo[512] = {0};
    priv->media->get_rtpinfo(priv->media, uri, rtpinfo, sizeof(rtpinfo));
    return rtsp_server_reply_play(rtsp, 200, npt, NULL, rtpinfo);
}

static int rtsp_onpause(void* ptr,
                        rtsp_server_t* rtsp,
                        const char* uri,
                        const char* session,
                        const int64_t* npt)
{
    tracef("ptr:%p rtsp:%p uri:%s", ptr, rtsp, uri, npt);
    // 457 Invalid Range
    return rtsp_server_reply_pause(rtsp, 200);
}

static int rtsp_onteardown(void* ptr,
                           rtsp_server_t* rtsp,
                           const char* uri,
                           const char* session)
{
    struct rtp_media_t* m = NULL;
    rtsp_server_priv_t* priv = ptr;
    tracef("ptr:%p rtsp:%p uri:%s session:%s", ptr, rtsp, uri, session);

    if (priv->media) {
        m = priv->media;
        priv->media = NULL;
        rtp_media_live_free(m);
    }

    return rtsp_server_reply_teardown(rtsp, 200);
}

static int rtsp_onannounce(void* ptr,
                           rtsp_server_t* rtsp,
                           const char* uri,
                           const char* sdp)
{
    tracef("ptr:%p rtsp:%p uri:%s sdp:%p", ptr, rtsp, uri, sdp);

    return rtsp_server_reply_announce(rtsp, 200);
}

static int rtsp_onrecord(void* ptr,
                         rtsp_server_t* rtsp,
                         const char* uri,
                         const char* session,
                         const int64_t* npt,
                         const double* scale)
{
    tracef("ptr:%p rtsp:%p uri:%s, session:%s, npt:%p, scale:%p",
        ptr, rtsp, uri, session, npt, npt);

    return rtsp_server_reply_record(rtsp, 200, NULL, NULL);
}

static int rtsp_onoptions(void* ptr, rtsp_server_t* rtsp, const char* uri)
{
    tracef("ptr:%p rtsp:%p uri:%s", ptr, rtsp, uri);

    // const char* require = rtsp_server_get_header(rtsp, "Require");
    return rtsp_server_reply_options(rtsp, 200);
}

static int rtsp_ongetparameter(void* ptr,
                               rtsp_server_t* rtsp,
                               const char* uri,
                               const char* session,
                               const void* content,
                               int bytes)
{
    tracef("ptr:%p rtsp:%p uri:%s, session:%s, content:%s, bytes:%d",
        ptr, rtsp, uri, session, content, bytes);

    // const char* ctype = rtsp_server_get_header(rtsp, "Content-Type");
    // const char* encoding = rtsp_server_get_header(rtsp, "Content-Encoding");
    // const char* language = rtsp_server_get_header(rtsp, "Content-Language");
    return rtsp_server_reply_get_parameter(rtsp, 200, NULL, 0);
}

static int rtsp_onsetparameter(void* ptr,
                               rtsp_server_t* rtsp,
                               const char* uri,
                               const char* session,
                               const void* content,
                               int bytes)
{
    tracef("ptr:%p rtsp:%p uri:%s, session:%s, content:%s, bytes:%d",
        ptr, rtsp, uri, session, content, bytes);

    // const char* ctype = rtsp_server_get_header(rtsp, "Content-Type");
    // const char* encoding = rtsp_server_get_header(rtsp, "Content-Encoding");
    // const char* language = rtsp_server_get_header(rtsp, "Content-Language");
    return rtsp_server_reply_set_parameter(rtsp, 200);
}

static void rtsp_onerror(void* ptr, rtsp_server_t* rtsp, int code)
{
    tracef("code:%s", strerror(code));
}

void* rtsp_server_init(const char* ip, int port)
{
    rtsp_server_priv_t* priv = malloc(sizeof(rtsp_server_priv_t));
    if (priv == NULL) {
        return NULL;
    }

    struct aio_rtsp_handler_t handler = {};
    handler.base.ondescribe = rtsp_ondescribe;
    handler.base.onsetup = rtsp_onsetup;
    handler.base.onplay = rtsp_onplay;
    handler.base.onpause = rtsp_onpause;
    handler.base.onteardown = rtsp_onteardown;
    handler.base.onannounce = rtsp_onannounce;
    handler.base.onrecord = rtsp_onrecord;
    handler.base.onoptions = rtsp_onoptions;
    handler.base.ongetparameter = rtsp_ongetparameter;
    handler.base.onsetparameter = rtsp_onsetparameter;
    handler.onerror = rtsp_onerror;

    memset(priv, 0, sizeof(rtsp_server_priv_t));
    priv->rtsp = rtsp_server_listen(ip, port, &handler, priv);

    tracef("priv:%p", priv);
    return priv;
}

int rtsp_server_uninit(void* rtsp)
{
    rtsp_server_priv_t* priv = rtsp;

    rtsp_server_unlisten(priv->rtsp);

    if (priv->media) {
        rtp_media_live_free(priv->media);
    }
    free(priv);

    return 0;
}

static void __attribute__((constructor)) __init()
{
    aio_worker_init(4);
}

static void __attribute__((destructor)) __deinit()
{
    aio_worker_clean(4);
}