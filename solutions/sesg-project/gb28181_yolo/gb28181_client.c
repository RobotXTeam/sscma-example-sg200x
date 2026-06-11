/*
 * gb28181_client.c - Minimal GB/T 28181 device-side client for reCamera.
 *
 * Pipeline: SIP (eXosip2/TCP) REGISTER + keepalive + answer INVITE,
 * then pull H.264 from local RTSP via ffmpeg, mux into MPEG-PS,
 * packetize into RTP (PT=96), and send over TCP (RFC4571 2-byte length
 * prefix) to the SRS GB28181 media port.
 */
#include <eXosip2/eXosip.h>
#include <osip2/osip_mt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ---- configuration (overridable by argv) ---- */
static char  g_dev_id[64]   = "34020000001320000001"; /* this device GB id  */
static char  g_srv_id[64]   = "34020000002000000001"; /* SRS server GB id   */
static char  g_realm[64]    = "3402000000";
static char  g_srv_ip[64]   = "192.168.2.113";
static int   g_srv_port     = 5060;
static char  g_local_ip[64] = "192.168.2.249";
static int   g_local_port   = 5060;
static char  g_user[64]     = "34020000001320000001";
static char  g_pass[64]     = "";                       /* SRS5 simplified: empty */
static char  g_rtsp[128]    = "rtsp://127.0.0.1:8554/live";
static int   g_expires      = 3600;

static struct eXosip_t *g_ctx = NULL;
static volatile int g_run = 1;

/* media session state filled from INVITE SDP */
static volatile int g_media_go = 0;
static char  g_media_ip[64]   = "";
static int   g_media_port     = 0;
static unsigned int g_ssrc    = 0;
static int   g_media_tcp      = 1;   /* 1 = TCP, 0 = UDP */
static int   g_setup_active   = 1;   /* device connects to SRS (active) */
static pthread_t g_media_thr;
static volatile int g_media_running = 0;

static void on_sig(int s){ (void)s; g_run = 0; }

/* =====================================================================
 *  MPEG Program Stream muxer  (H.264 -> PS)
 * ===================================================================== */

static void put_pack_header(unsigned char *b, int *n, unsigned long long pts){
    unsigned char *p = b + *n;
    unsigned long long scr = pts;            /* 33-bit system clock ref */
    unsigned int ext = 0, mux = 50000;       /* mux_rate in 50B/s units */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x01; *p++ = 0xBA;
    *p++ = 0x40 | (unsigned char)(((scr>>30)&0x07)<<3) | 0x04 | (unsigned char)((scr>>28)&0x03);
    *p++ = (unsigned char)((scr>>20)&0xFF);
    *p++ = (unsigned char)(((scr>>15)&0x1F)<<3) | 0x04 | (unsigned char)((scr>>13)&0x03);
    *p++ = (unsigned char)((scr>>5)&0xFF);
    *p++ = (unsigned char)((scr&0x1F)<<3) | 0x04 | (unsigned char)((ext>>7)&0x03);
    *p++ = (unsigned char)((ext&0x7F)<<1) | 0x01;
    *p++ = (unsigned char)((mux>>14)&0xFF);
    *p++ = (unsigned char)((mux>>6)&0xFF);
    *p++ = (unsigned char)((mux&0x3F)<<2) | 0x03;
    *p++ = 0xF8;                              /* reserved + stuffing=0 */
    *n = (int)(p - b);
}

static void put_system_header(unsigned char *b, int *n){
    unsigned char *p = b + *n;
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x01; *p++ = 0xBB;
    *p++ = 0x00; *p++ = 0x09;                 /* header_length = 9 */
    *p++ = 0x80; *p++ = 0xC4; *p++ = 0xE1;    /* rate_bound + markers */
    *p++ = 0x00;                              /* audio_bound/fixed/CSPS */
    *p++ = 0x21;                              /* marker + video_bound=1 */
    *p++ = 0x7F;                              /* restriction + reserved */
    /* one video stream entry (0xE0) */
    *p++ = 0xE0; *p++ = 0xE0; *p++ = 0xE0;
    *n = (int)(p - b);
}

static void put_psm(unsigned char *b, int *n){
    unsigned char *p = b + *n;
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x01; *p++ = 0xBC;
    *p++ = 0x00; *p++ = 0x12;                 /* program_stream_map_length=18 */
    *p++ = 0xE1;                              /* cur_next=1, version=1 */
    *p++ = 0xFF;                              /* reserved + marker */
    *p++ = 0x00; *p++ = 0x00;                 /* program_stream_info_length */
    *p++ = 0x00; *p++ = 0x08;                 /* elementary_stream_map_length=8 */
    /* H.264 video */
    *p++ = 0x1B; *p++ = 0xE0; *p++ = 0x00; *p++ = 0x00;
    /* G.711 audio placeholder (helps some parsers) */
    *p++ = 0x90; *p++ = 0xC0; *p++ = 0x00; *p++ = 0x00;
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;   /* CRC32 (0) */
    *n = (int)(p - b);
}

static void put_pts(unsigned char *p, int flag, unsigned long long pts){
    /* flag = 0x20 (PTS only) or 0x30 (PTS+DTS first nibble) */
    p[0] = (unsigned char)(flag | (((pts>>30)&0x07)<<1) | 0x01);
    p[1] = (unsigned char)((pts>>22)&0xFF);
    p[2] = (unsigned char)((((pts>>15)&0x7F)<<1) | 0x01);
    p[3] = (unsigned char)((pts>>7)&0xFF);
    p[4] = (unsigned char)(((pts&0x7F)<<1) | 0x01);
}

/* Build PES header for a video chunk. with_pts: include PTS field.
 * payload_len is the ES bytes that follow. returns header length. */
static int put_pes_header(unsigned char *b, int *n, int with_pts,
                          unsigned long long pts, int payload_len){
    unsigned char *p = b + *n;
    int hdrdata = with_pts ? 5 : 0;
    int pes_len = 3 + hdrdata + payload_len;   /* bytes after length field */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x01; *p++ = 0xE0;    /* video stream_id */
    *p++ = (unsigned char)((pes_len>>8)&0xFF);
    *p++ = (unsigned char)(pes_len&0xFF);
    *p++ = 0x80;                                /* '10' + flags */
    *p++ = with_pts ? 0x80 : 0x00;              /* PTS_DTS_flags */
    *p++ = (unsigned char)hdrdata;              /* PES_header_data_length */
    if (with_pts){ put_pts(p, 0x20, pts); p += 5; }
    *n = (int)(p - b);
    return 0;
}

/* =====================================================================
 *  RTP over TCP (RFC4571) sender
 * ===================================================================== */
#define RTP_PAYLOAD_MAX 1400
static int g_media_fd = -1;
static unsigned short g_rtp_seq = 0;

static int send_all(int fd, const unsigned char *buf, int len){
    int off = 0;
    while (off < len){
        int r = (int)send(fd, buf+off, len-off, MSG_NOSIGNAL);
        if (r <= 0){ if (errno==EINTR) continue; return -1; }
        off += r;
    }
    return 0;
}

/* Fragment a full PS packet into RTP packets sharing one timestamp. */
static int send_ps_as_rtp(const unsigned char *ps, int ps_len, unsigned int ts){
    int off = 0;
    while (off < ps_len){
        int chunk = ps_len - off;
        if (chunk > RTP_PAYLOAD_MAX) chunk = RTP_PAYLOAD_MAX;
        int last = (off + chunk >= ps_len);
        unsigned char pkt[2 + 12 + RTP_PAYLOAD_MAX];
        unsigned char *r = pkt + 2;            /* leave 2 bytes for tcp len */
        r[0] = 0x80;
        r[1] = (unsigned char)(96 | (last ? 0x80 : 0x00));  /* M bit on last */
        r[2] = (unsigned char)(g_rtp_seq >> 8);
        r[3] = (unsigned char)(g_rtp_seq & 0xFF);
        g_rtp_seq++;
        r[4] = (unsigned char)(ts>>24); r[5]=(unsigned char)(ts>>16);
        r[6] = (unsigned char)(ts>>8);  r[7]=(unsigned char)(ts);
        r[8] = (unsigned char)(g_ssrc>>24); r[9]=(unsigned char)(g_ssrc>>16);
        r[10]= (unsigned char)(g_ssrc>>8);  r[11]=(unsigned char)(g_ssrc);
        memcpy(r+12, ps+off, chunk);
        int rtp_len = 12 + chunk;
        if (g_media_tcp){
            pkt[0] = (unsigned char)(rtp_len >> 8);
            pkt[1] = (unsigned char)(rtp_len & 0xFF);
            if (send_all(g_media_fd, pkt, 2 + rtp_len) < 0) return -1;
        } else {
            if (send_all(g_media_fd, pkt+2, rtp_len) < 0) return -1;
        }
        off += chunk;
    }
    return 0;
}

/* Mux one H.264 access unit into a PS packet and send it. */
static unsigned char g_ps[512*1024];
static int g_au_count = 0;
static int mux_and_send_au(const unsigned char *au, int au_len, int keyframe,
                           unsigned long long pts){
    int n = 0;
    put_pack_header(g_ps, &n, pts);
    if (keyframe){ put_system_header(g_ps, &n); put_psm(g_ps, &n); }
    /* PES (unbounded length=0 for large video AUs) */
    {
        int payload = au_len;
        int use_len = (payload + 8 <= 65535) ? payload : 0;
        put_pes_header(g_ps, &n, 1, pts, use_len);
    }
    if (n + au_len > (int)sizeof(g_ps)) return -1;
    memcpy(g_ps + n, au, au_len);
    n += au_len;
    unsigned int ts = (unsigned int)(pts & 0xFFFFFFFF);
    return send_ps_as_rtp(g_ps, n, ts);
}

/* =====================================================================
 *  Media thread: ffmpeg RTSP -> Annex-B H264 -> AU assembler -> PS/RTP
 * ===================================================================== */
static int connect_media(void){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa; memset(&sa,0,sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_media_port);
    inet_pton(AF_INET, g_media_ip, &sa.sin_addr);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0){
        fprintf(stderr,"[media] connect %s:%d failed: %s\n",
                g_media_ip, g_media_port, strerror(errno));
        close(fd); return -1;
    }
    fprintf(stderr,"[media] connected to %s:%d (tcp)\n", g_media_ip, g_media_port);
    return fd;
}

/* Return nal_unit_type from a NAL that starts right after the start code. */
static int nal_type(const unsigned char *p){ return p[0] & 0x1F; }

static void *media_thread(void *arg){
    (void)arg;
    g_media_running = 1;
    g_media_fd = connect_media();
    if (g_media_fd < 0){ g_media_running = 0; return NULL; }

    /* ffmpeg: read RTSP (tcp), copy video to Annex-B H264 on stdout */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "unset LD_LIBRARY_PATH; "
        "/usr/bin/ffmpeg -nostdin -loglevel error -rtsp_transport tcp "
        "-i '%s' -an -c:v copy -bsf:v h264_mp4toannexb -f h264 - 2>/tmp/ff_media.log",
        g_rtsp);
    FILE *fp = popen(cmd, "r");
    if (!fp){ fprintf(stderr,"[media] popen ffmpeg failed\n"); goto done; }

    /* streaming NAL parser over the pipe */
    static unsigned char buf[256*1024];
    static unsigned char au[512*1024];
    int au_len = 0, au_key = 0, au_has_vcl = 0;
    unsigned long long pts = 0;
    const unsigned long long PTS_STEP = 3600;  /* 90kHz / 25fps */

    /* sliding window: accumulate, find start codes */
    static unsigned char acc[1024*1024];
    int acc_len = 0;
    int got;
    while (g_run && (got = (int)fread(buf,1,sizeof(buf),fp)) > 0){
        if (acc_len + got > (int)sizeof(acc)){
            /* overflow guard: reset */
            acc_len = 0;
        }
        memcpy(acc+acc_len, buf, got); acc_len += got;

        /* find NAL boundaries (00 00 01) and flush complete NALs,
         * keeping the trailing partial NAL in acc */
        int i = 0, last_start = -1;
        /* locate first start code */
        while (i+3 <= acc_len){
            if (acc[i]==0 && acc[i+1]==0 && acc[i+2]==1){ last_start = i; break; }
            i++;
        }
        if (last_start < 0){ acc_len = 0; continue; }
        int sc = last_start;
        while (1){
            /* find next start code after sc */
            int j = sc + 3;
            int next = -1;
            while (j+3 <= acc_len){
                if (acc[j]==0 && acc[j+1]==0 && acc[j+2]==1){ next = j; break; }
                j++;
            }
            if (next < 0) break;       /* incomplete NAL, wait for more data */
            /* NAL payload spans [sc+3 .. next), trim trailing 00 of 4-byte sc */
            int nal_beg = sc + 3;
            int nal_end = next;
            if (nal_end>nal_beg && acc[nal_end-1]==0) nal_end--; /* 4-byte code */
            int nlen = nal_end - nal_beg;
            if (nlen > 0){
                int t = nal_type(acc + nal_beg);
                int is_vcl = (t>=1 && t<=5);
                /* AU boundary: a new VCL after we already have a VCL,
                 * or any AUD/SPS starting a new picture */
                if (au_has_vcl && (is_vcl || t==9 || t==7)){
                    if (mux_and_send_au(au, au_len, au_key, pts) < 0){
                        fprintf(stderr,"[media] send failed\n");
                        pclose(fp); goto done;
                    }
                    g_au_count++;
                    if (g_au_count <= 3 || (g_au_count % 50)==0)
                        fprintf(stderr,"[media] sent AU #%d len=%d key=%d\n",
                                g_au_count, au_len, au_key);
                    pts += PTS_STEP;
                    au_len = 0; au_key = 0; au_has_vcl = 0;
                }
                /* append this NAL (with 4-byte start code) into AU buffer */
                if (au_len + 4 + nlen < (int)sizeof(au)){
                    au[au_len++]=0; au[au_len++]=0; au[au_len++]=0; au[au_len++]=1;
                    memcpy(au+au_len, acc+nal_beg, nlen); au_len += nlen;
                    if (t==5) au_key = 1;
                    if (is_vcl) au_has_vcl = 1;
                }
            }
            sc = next;
        }
        /* shift remaining (from sc) to front */
        if (sc > 0){ memmove(acc, acc+sc, acc_len - sc); acc_len -= sc; }
    }
    pclose(fp);
done:
    if (g_media_fd>=0){ close(g_media_fd); g_media_fd=-1; }
    g_media_running = 0;
    fprintf(stderr,"[media] thread exit\n");
    return NULL;
}

/* =====================================================================
 *  SIP layer
 * ===================================================================== */
static int g_reg_id = -1;

static int do_register(void){
    char from[128], proxy[128], contact[128];
    osip_message_t *reg = NULL;
    snprintf(from,   sizeof(from),   "sip:%s@%s", g_dev_id, g_realm);
    snprintf(proxy,  sizeof(proxy),  "sip:%s@%s:%d", g_srv_id, g_srv_ip, g_srv_port);
    snprintf(contact,sizeof(contact),"sip:%s@%s:%d", g_dev_id, g_local_ip, g_local_port);

    g_reg_id = eXosip_register_build_initial_register(g_ctx, from, proxy, contact,
                                                      g_expires, &reg);
    if (g_reg_id < 0 || !reg){ fprintf(stderr,"[sip] build register failed\n"); return -1; }
    eXosip_lock(g_ctx);
    int r = eXosip_register_send_register(g_ctx, g_reg_id, reg);
    eXosip_unlock(g_ctx);
    fprintf(stderr,"[sip] REGISTER sent (id=%d rc=%d) -> %s\n", g_reg_id, r, proxy);
    return r;
}

static int refresh_register(void){
    osip_message_t *reg = NULL;
    eXosip_lock(g_ctx);
    int r = eXosip_register_build_register(g_ctx, g_reg_id, g_expires, &reg);
    if (r >= 0 && reg) r = eXosip_register_send_register(g_ctx, g_reg_id, reg);
    eXosip_unlock(g_ctx);
    fprintf(stderr,"[sip] REGISTER refresh rc=%d\n", r);
    return r;
}

/* Send a GB28181 Keepalive MESSAGE to the server. */
static int g_ka_sn = 1;
static void send_keepalive(void){
    char from[128], to[128], body[512];
    snprintf(from, sizeof(from), "sip:%s@%s", g_dev_id, g_realm);
    snprintf(to,   sizeof(to),   "sip:%s@%s:%d", g_srv_id, g_srv_ip, g_srv_port);
    snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n"
        "<Notify>\r\n<CmdType>Keepalive</CmdType>\r\n"
        "<SN>%d</SN>\r\n<DeviceID>%s</DeviceID>\r\n"
        "<Status>OK</Status>\r\n</Notify>\r\n",
        g_ka_sn++, g_dev_id);
    osip_message_t *msg = NULL;
    eXosip_lock(g_ctx);
    int r = eXosip_message_build_request(g_ctx, &msg, "MESSAGE", to, from, NULL);
    if (r==0 && msg){
        osip_message_set_content_type(msg, "Application/MANSCDP+xml");
        osip_message_set_body(msg, body, strlen(body));
        eXosip_message_send_request(g_ctx, msg);
    }
    eXosip_unlock(g_ctx);
}

/* crude SDP field extraction */
static void parse_sdp(const char *sdp){
    const char *p;
    /* connection: c=IN IP4 <ip> */
    if ((p = strstr(sdp, "c=IN IP4 "))){
        sscanf(p+9, "%63[0-9.]", g_media_ip);
    }
    /* media: m=video <port> <proto> ... */
    if ((p = strstr(sdp, "m=video "))){
        sscanf(p+8, "%d", &g_media_port);
        g_media_tcp = (strstr(p, "TCP") != NULL);
    }
    /* ssrc:  y=<10 digit ssrc>  (GB28181 specific line) */
    if ((p = strstr(sdp, "y="))){
        unsigned int s=0; sscanf(p+2, "%u", &s); if (s) g_ssrc = s;
    }
    /* setup: a=setup:active/passive ; for TCP media. SRS is passive(server),
     * device acts as active and connects out. */
    if (strstr(sdp, "a=setup:passive")) g_setup_active = 1;
    fprintf(stderr,"[sip] SDP parsed: media=%s:%d tcp=%d ssrc=%u\n",
            g_media_ip, g_media_port, g_media_tcp, g_ssrc);
}

/* Build our answer SDP and send 200 OK to the INVITE. */
static void answer_invite(eXosip_event_t *ev){
    if (!ev->request){ return; }
    /* extract SDP from request body */
    osip_body_t *body = NULL;
    osip_message_get_body(ev->request, 0, &body);
    if (body && body->body){ parse_sdp(body->body); }

    if (g_ssrc == 0) g_ssrc = 1234567890u;
    char sdp[1024];
    /* Our media side: device is the source. For TCP passive-server SRS,
     * we declare active and will connect to SRS media port. */
    snprintf(sdp, sizeof(sdp),
        "v=0\r\n"
        "o=%s 0 0 IN IP4 %s\r\n"
        "s=Play\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=video %d TCP/RTP/AVP 96\r\n"
        "a=setup:active\r\n"
        "a=connection:new\r\n"
        "a=sendonly\r\n"
        "a=rtpmap:96 PS/90000\r\n"
        "y=%010u\r\n"
        "f=\r\n",
        g_dev_id, g_local_ip, g_local_ip, g_local_port, g_ssrc);

    eXosip_lock(g_ctx);
    eXosip_call_send_answer(g_ctx, ev->tid, 100, NULL);  /* Trying */
    osip_message_t *ans = NULL;
    int r = eXosip_call_build_answer(g_ctx, ev->tid, 200, &ans);
    if (r==0 && ans){
        osip_message_set_content_type(ans, "application/sdp");
        osip_message_set_body(ans, sdp, strlen(sdp));
        eXosip_call_send_answer(g_ctx, ev->tid, 200, ans);
        fprintf(stderr,"[sip] 200 OK sent for INVITE\n");
    } else {
        fprintf(stderr,"[sip] build_answer failed rc=%d\n", r);
    }
    eXosip_unlock(g_ctx);
}

static void answer_message(eXosip_event_t *ev){
    /* reply 200 to any incoming MESSAGE (catalog/keepalive queries) */
    eXosip_lock(g_ctx);
    eXosip_message_send_answer(g_ctx, ev->tid, 200, NULL);
    eXosip_unlock(g_ctx);
}

static void start_media_once(void){
    if (g_media_running) return;
    if (g_media_ip[0]==0 || g_media_port==0){
        fprintf(stderr,"[media] no target, skip\n"); return;
    }
    g_media_go = 1;
    if (pthread_create(&g_media_thr, NULL, media_thread, NULL)==0)
        pthread_detach(g_media_thr);
}

int main(int argc, char **argv){
    /* argv: dev_id srv_ip srv_port local_ip srv_id rtsp */
    if (argc>1) strncpy(g_dev_id, argv[1], sizeof(g_dev_id)-1);
    if (argc>2) strncpy(g_srv_ip, argv[2], sizeof(g_srv_ip)-1);
    if (argc>3) g_srv_port = atoi(argv[3]);
    if (argc>4) strncpy(g_local_ip, argv[4], sizeof(g_local_ip)-1);
    if (argc>5) strncpy(g_srv_id, argv[5], sizeof(g_srv_id)-1);
    if (argc>6) strncpy(g_rtsp, argv[6], sizeof(g_rtsp)-1);
    strncpy(g_user, g_dev_id, sizeof(g_user)-1);

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    g_ctx = eXosip_malloc();
    if (eXosip_init(g_ctx)){ fprintf(stderr,"eXosip_init failed\n"); return 1; }
    /* listen on TCP */
    if (eXosip_listen_addr(g_ctx, IPPROTO_TCP, NULL, g_local_port, AF_INET, 0)){
        fprintf(stderr,"eXosip_listen_addr TCP %d failed\n", g_local_port);
        return 1;
    }
    eXosip_set_user_agent(g_ctx, "reCamera-GB28181/1.0");
    if (g_pass[0]) eXosip_add_authentication_info(g_ctx, g_user, g_user, g_pass, "MD5", NULL);

    fprintf(stderr,"[main] dev=%s srv=%s:%d local=%s rtsp=%s\n",
            g_dev_id, g_srv_ip, g_srv_port, g_local_ip, g_rtsp);
    do_register();

    time_t last_ka = time(NULL), last_reg = time(NULL);
    while (g_run){
        eXosip_event_t *ev = eXosip_event_wait(g_ctx, 0, 200);
        eXosip_lock(g_ctx);
        eXosip_automatic_action(g_ctx);
        eXosip_unlock(g_ctx);
        if (ev){
            switch (ev->type){
            case EXOSIP_REGISTRATION_SUCCESS:
                fprintf(stderr,"[sip] *** REGISTER SUCCESS ***\n"); break;
            case EXOSIP_REGISTRATION_FAILURE:
                fprintf(stderr,"[sip] REGISTER failure (status=%d)\n",
                        ev->response?ev->response->status_code:-1);
                break;
            case EXOSIP_CALL_INVITE:
                fprintf(stderr,"[sip] <<< INVITE received\n");
                answer_invite(ev);
                break;
            case EXOSIP_CALL_ACK:
                fprintf(stderr,"[sip] <<< ACK -> start media\n");
                start_media_once();
                break;
            case EXOSIP_CALL_CLOSED:
            case EXOSIP_CALL_RELEASED:
                fprintf(stderr,"[sip] call closed\n");
                break;
            case EXOSIP_MESSAGE_NEW:
                fprintf(stderr,"[sip] <<< MESSAGE\n");
                answer_message(ev);
                break;
            default: break;
            }
            eXosip_event_free(ev);
        }
        time_t now = time(NULL);
        if (now - last_ka >= 30){ send_keepalive(); last_ka = now; }
        if (now - last_reg >= g_expires - 60){ refresh_register(); last_reg = now; }
    }
    eXosip_quit(g_ctx);
    return 0;
}





