/*
Copyright (c) 2015, Plume Design Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
   1. Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
   3. Neither the name of the Plume Design Inc. nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL Plume Design Inc. BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ev.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include "os_types.h"
#include "os.h"
#include "log.h"
#include "ds.h"
#include "fcm.h"
#include "ct_stats.h"
#include "network_metadata_report.h"
#include "network_metadata.h"
#include "fcm_filter.h"
#include "fcm_report_filter.h"
#include "imc.h"
#include "neigh_table.h"

#include <netdb.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libmnl/libmnl.h>

#if defined(CONFIG_PLATFORM_IS_BCM)
// on BCM the kernel header is missing CTA_TUPLE_ZONE
#include <libnetfilter_conntrack/linux_nfnetlink_conntrack.h>
#else
#include <linux/netfilter/nfnetlink_conntrack.h>
#endif

#include <linux/netfilter/nf_conntrack_tcp.h>

/**
 * IMC server used for fsm -> fcm flow tags communication
 */
static struct imc_context g_imc_server =
{
    .initialized = false,
    .endpoint = "ipc:///tmp/imc_fsm2fcm",
};


/**
 * IMC shared library context, when loaded through dlopen()
 */
static struct imc_dso g_imc_context = { 0 };


/**
 * @brief dynamically load the imc library and initializes its context
 */
static bool
ct_stats_load_imc(void)
{
    char *dso = CONFIG_INSTALL_PREFIX"/lib/libimc.so";
    char *init = "imc_init_dso";
    struct stat st;
    char *error;
    int rc;

    rc = stat(dso, &st);
    if (rc != 0) return true; /* All ops will be void */

    dlerror();
    g_imc_context.handle = dlopen(dso, RTLD_NOW);
    if (g_imc_context.handle == NULL)
    {
        LOGE("%s: dlopen %s failed: %s", __func__, dso, dlerror());
        return false;
    }

    dlerror();
    *(void **)(&g_imc_context.init) = dlsym(g_imc_context.handle, init);
    error = dlerror();
    if (error != NULL) {
        LOGE("%s: could not get symbol %s: %s",
             __func__, init, error);
        dlclose(g_imc_context.handle);
        return false;
    }

    g_imc_context.init(&g_imc_context);

    return true;
}


/**
 * @brief starts the fsm -> fcm IMC server
 *
 * @param server the imc context of the server
 * @param loop the FCM manager's ev loop
 * @param recv_cb the ct_stats handler for the data received from fsm
 */
static int
ct_stats_init_server(struct imc_context *server, struct ev_loop *loop,
                     imc_recv recv_cb)
{
    int ret;

    if (g_imc_context.init_server == NULL) return 0;

    ret = g_imc_context.init_server(server, loop, recv_cb);

    return ret;
}


/**
 * @brief stops the fsm -> fcm IMC server
 *
 * @param server the imc context of the server
 */
static void
ct_stats_terminate_server(struct imc_context *server)
{
    if (g_imc_context.terminate_server == NULL) return;

    g_imc_context.terminate_server(server);

}


/**
 * Singleton tracking the plugin state
 */
static flow_stats_t g_ct_stats;


/**
 * @brief returns the pointer to the plugin's global state tracker
 */
flow_stats_t *
ct_stats_get_mgr(void)
{
    return &g_ct_stats;
}


/**
 * @brief popludates a sockaddr_storage structure from ip parameters
 *
 * @param af the the inet family
 * @param ip a pointer to the ip address buffer
 * @param dst the sockaddr_storage structure to fill
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static void
ct_stats_populate_sockaddr(int af, void *ip, struct sockaddr_storage *dst)
{
    if (af == AF_INET)
    {
        struct sockaddr_in *in4 = (struct sockaddr_in *)dst;

        memset(in4, 0, sizeof(struct sockaddr_in));
        in4->sin_family = af;
        memcpy(&in4->sin_addr, ip, sizeof(in4->sin_addr));
    }
    else if (af == AF_INET6)
    {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)dst;

        memset(in6, 0, sizeof(struct sockaddr_in6));
        in6->sin6_family = af;
        memcpy(&in6->sin6_addr, ip, sizeof(in6->sin6_addr));
    }
    return;
}


/**
 * @brief validates and stores conntrack counters
 *
 * @param attr the netlink attribute
 * @param data the table of <attribute, value>
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static int
parse_counters_cb(const struct nlattr *attr, void *data)
{
    const struct nlattr **tb;
    int type;
    int rc;

    tb = data;
    type = mnl_attr_get_type(attr);

    rc = mnl_attr_type_valid(attr, CTA_COUNTERS_MAX);
    if (rc < 0) return MNL_CB_OK;

    switch (type)
    {
    case CTA_COUNTERS_PACKETS:
    case CTA_COUNTERS_BYTES:
        rc = mnl_attr_validate(attr, MNL_TYPE_U64);
        if (rc < 0) return MNL_CB_ERROR;
        break;

    case CTA_COUNTERS32_PACKETS:
    case CTA_COUNTERS32_BYTES:
        rc = mnl_attr_validate(attr, MNL_TYPE_U32);
        if (rc < 0) return MNL_CB_ERROR;
        break;
    }
    tb[type] = attr;

    return MNL_CB_OK;
}


/**
 * @brief validates and stores conntrack IP info
 *
 * @param attr the netlink attribute
 * @param data the table of <attribute, value>
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static int
parse_ip_cb(const struct nlattr *attr, void *data)
{
    const struct nlattr **tb;
    int type;
    int rc;

    tb = data;
    type = mnl_attr_get_type(attr);

    rc =  mnl_attr_type_valid(attr, CTA_IP_MAX);
    if (rc < 0) return MNL_CB_OK; /* Why ok? */

    switch (type)
    {
    case CTA_IP_V4_SRC:
    case CTA_IP_V4_DST:
        rc = mnl_attr_validate(attr, MNL_TYPE_U32);
        if (rc < 0) return MNL_CB_ERROR;
        break;

    case CTA_IP_V6_SRC:
    case CTA_IP_V6_DST:
        rc =  mnl_attr_validate2(attr, MNL_TYPE_BINARY,
                                 sizeof(struct in6_addr));
        if (rc < 0) return MNL_CB_ERROR;
        break;
    }

    tb[type] = attr;

    return MNL_CB_OK;
}


/**
 * @brief validates and stores conntrack network proto
 *
 * @param attr the netlink attribute
 * @param data the table of <attribute, value>
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static int
parse_proto_cb(const struct nlattr *attr, void *data)
{
    const struct nlattr **tb;
    int type;
    int rc;

    tb = data;
    type = mnl_attr_get_type(attr);

    rc = mnl_attr_type_valid(attr, CTA_PROTO_MAX);
    if (rc < 0) return MNL_CB_OK;

    switch (type)
    {
    case CTA_PROTO_NUM:
    case CTA_PROTO_ICMP_TYPE:
    case CTA_PROTO_ICMP_CODE:
        rc = mnl_attr_validate(attr, MNL_TYPE_U8);
        if (rc < 0) return MNL_CB_ERROR;
        break;

    case CTA_PROTO_SRC_PORT:
    case CTA_PROTO_DST_PORT:
    case CTA_PROTO_ICMP_ID:
        rc = mnl_attr_validate(attr, MNL_TYPE_U16);
        if (rc < 0) return MNL_CB_ERROR;
        break;
    }

    tb[type] = attr;

    return MNL_CB_OK;
}


/**
 * @brief validates and stores conntrack tuple info
 *
 * @param attr the netlink attribute
 * @param data the table of <attribute, value>
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static int
parse_tuple_cb(const struct nlattr *attr, void *data)
{
    const struct nlattr **tb;
    int type;
    int rc;

    tb = data;
    type = mnl_attr_get_type(attr);

    rc = mnl_attr_type_valid(attr, CTA_TUPLE_MAX);
    if (rc < 0)
        return MNL_CB_OK;

    switch (type)
    {
    case CTA_TUPLE_IP:
        rc = mnl_attr_validate(attr, MNL_TYPE_NESTED);
        if (rc < 0) return MNL_CB_ERROR;
        break;

    case CTA_TUPLE_PROTO:
        rc = mnl_attr_validate(attr, MNL_TYPE_NESTED);
        if (rc < 0) return MNL_CB_ERROR;
        break;

    case CTA_TUPLE_ZONE:
        rc = mnl_attr_validate(attr, MNL_TYPE_U16);
        if (rc < 0) return MNL_CB_ERROR;
        break;
    }

    tb[type] = attr;

    return MNL_CB_OK;
}


/**
 * @brief translates conntrack tuple to ct_stats flow
 *
 * @param nest the netlink attribute
 * @param flow the ct_stats flow to update
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static int
get_tuple(const struct nlattr *nest, ct_flow_t *flow)
{
    struct nlattr *proto_tb[CTA_PROTO_MAX+1];
    struct nlattr *tb[CTA_TUPLE_MAX+1];
    struct nlattr *ip_tb[CTA_IP_MAX+1];
    struct in6_addr *in6;
    struct in_addr *in;
    int rc;

    memset(tb, 0, (CTA_TUPLE_MAX+1) * sizeof(tb[0]));

    rc = mnl_attr_parse_nested(nest, parse_tuple_cb, tb);
    if (rc < 0) return MNL_CB_ERROR;

    if (tb[CTA_TUPLE_IP] != NULL)
    {
        memset(ip_tb, 0, (CTA_IP_MAX+1) * sizeof(ip_tb[0]));

        rc = mnl_attr_parse_nested(tb[CTA_TUPLE_IP], parse_ip_cb, ip_tb);
        if (rc < 0) return MNL_CB_ERROR;

        if (ip_tb[CTA_IP_V4_SRC] != NULL)
        {
            in = mnl_attr_get_payload(ip_tb[CTA_IP_V4_SRC]);
            ct_stats_populate_sockaddr(AF_INET, &in->s_addr,
                                       &flow->layer3_info.src_ip);
        }

        if (ip_tb[CTA_IP_V4_DST] != NULL)
        {
            in = mnl_attr_get_payload(ip_tb[CTA_IP_V4_DST]);
            ct_stats_populate_sockaddr(AF_INET, &in->s_addr,
                                       &flow->layer3_info.dst_ip);
        }

        if (ip_tb[CTA_IP_V6_SRC] != NULL)
        {
            in6 = mnl_attr_get_payload(ip_tb[CTA_IP_V6_SRC]);
            ct_stats_populate_sockaddr(AF_INET6, in6->s6_addr,
                                       &flow->layer3_info.src_ip);
        }
        if (ip_tb[CTA_IP_V6_DST] != NULL)
        {
           in6 = mnl_attr_get_payload(ip_tb[CTA_IP_V6_DST]);
           ct_stats_populate_sockaddr(AF_INET6, in6->s6_addr,
                                      &flow->layer3_info.dst_ip);
        }
    }

    if (tb[CTA_TUPLE_PROTO] != NULL)
    {
        uint16_t val16;
        uint8_t val8;

        memset(proto_tb, 0, (CTA_PROTO_MAX+1) * sizeof(proto_tb[0]));

        rc = mnl_attr_parse_nested(tb[CTA_TUPLE_PROTO],
                                   parse_proto_cb, proto_tb);
        if (rc < 0) return MNL_CB_ERROR;

        if (proto_tb[CTA_PROTO_NUM] != NULL)
        {
            val8 = mnl_attr_get_u8(proto_tb[CTA_PROTO_NUM]);
            flow->layer3_info.proto_type = val8;
        }

        if (proto_tb[CTA_PROTO_SRC_PORT] != NULL)
        {
            val16 = mnl_attr_get_u16(proto_tb[CTA_PROTO_SRC_PORT]);
            flow->layer3_info.src_port = val16;
        }

        if (proto_tb[CTA_PROTO_DST_PORT] != NULL)
        {
            val16 = mnl_attr_get_u16(proto_tb[CTA_PROTO_DST_PORT]);
            flow->layer3_info.dst_port = val16;
        }

#ifdef CT_ICMP_SUPPORT
        if (proto_tb[CTA_PROTO_ICMP_ID] != NULL)
        {
            val16 = mnl_attr_get_u16(proto_tb[CTA_PROTO_ICMP_ID]);
            LOGT("%s: id=%u ", __func__, val16);
        }

        if (proto_tb[CTA_PROTO_ICMP_TYPE] != NULL)
        {
            val8 = mnl_attr_get_u8(proto_tb[CTA_PROTO_ICMP_TYPE]);
            LOGT("%s: type=%u ", __func__, val8);
        }

        if (proto_tb[CTA_PROTO_ICMP_CODE] != NULL)
        {
            val8 = mnl_attr_get_u8(proto_tb[CTA_PROTO_ICMP_CODE]);
            LOGT("%s: type=%u ", __func__, val8);
        }
#endif
    }
    if (tb[CTA_TUPLE_ZONE] != NULL)
    {
        flow->ct_zone = mnl_attr_get_u16(tb[CTA_TUPLE_ZONE]);
        LOGD("%s: Tuple ct_zone: %d", __func__, ntohs(flow->ct_zone));
    }
    return MNL_CB_OK;
}


/**
 * @brief validates and stores conntrack proto info
 *
 * @param attr the netlink attribute
 * @param data the table of <attribute, value>
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static int
parse_protoinfo(const struct nlattr *attr, void *data)
{
    const struct nlattr **tb;
    int type;
    int rc;

    tb = data;
    type = mnl_attr_get_type(attr);

    rc = mnl_attr_type_valid(attr, CTA_PROTOINFO_MAX);
    if (rc < 0) return MNL_CB_OK;

    switch(type)
    {
    case CTA_PROTOINFO_TCP:
        rc = mnl_attr_validate(attr, MNL_TYPE_NESTED);
        if (rc < 0) return MNL_CB_ERROR;
        break;
    }

    tb[type] = attr;

    return MNL_CB_OK;
}


/**
 * @brief validates and stores conntrack tcp protoinfo
 *
 * @param attr the netlink attribute
 * @param data the table of <attribute, value>
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static int
parse_tcp_protoinfo(const struct nlattr *attr, void *data)
{
    const struct nlattr **tb;
    int type;
    int rc;

    tb = data;
    type = mnl_attr_get_type(attr);

    rc = mnl_attr_type_valid(attr, CTA_PROTOINFO_TCP_MAX);
    if (rc < 0) return MNL_CB_OK;

    switch (type)
    {
    case CTA_PROTOINFO_TCP_STATE:
        rc = mnl_attr_validate(attr, MNL_TYPE_U8);
        if (rc < 0) return MNL_CB_ERROR;
        break;
    }

    tb[type] = attr;

    return MNL_CB_OK;
}


/**
 * @brief translates conntrack protoinfo to ct_stats flow
 *
 * @param nest the netlink attribute
 * @param flow the ct_stats flow to update
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static int
get_protoinfo(const struct nlattr *nest, ct_flow_t *flow)
{
    struct nlattr *tb[CTA_PROTOINFO_MAX + 1];
    int rc;

    memset(tb, 0 , (CTA_PROTOINFO_MAX + 1) * sizeof(tb[0]));
    rc = mnl_attr_parse_nested(nest, parse_protoinfo, tb);
    if (rc < 0) return MNL_CB_ERROR;

    if (tb[CTA_PROTOINFO_TCP] != NULL)
    {
        struct nlattr *tcp_tb[CTA_PROTOINFO_TCP_MAX + 1];

        memset(tcp_tb, 0 , (CTA_PROTOINFO_TCP_MAX + 1) * sizeof(tcp_tb[0]));
        rc = mnl_attr_parse_nested(tb[CTA_PROTOINFO_TCP],
                                   parse_tcp_protoinfo, tcp_tb);
        if (rc < 0) return MNL_CB_ERROR;

        if (tcp_tb[CTA_PROTOINFO_TCP_STATE] != NULL)
        {
            uint8_t state;

            state = mnl_attr_get_u8(tcp_tb[CTA_PROTOINFO_TCP_STATE]);
            switch (state)
            {
            case TCP_CONNTRACK_SYN_SENT:
            case TCP_CONNTRACK_SYN_RECV:
            case TCP_CONNTRACK_ESTABLISHED:
                flow->start = true;
                LOGD("%s: TCP Flow started", __func__);
                break;

            case TCP_CONNTRACK_FIN_WAIT:
            case TCP_CONNTRACK_CLOSE_WAIT:
            case TCP_CONNTRACK_LAST_ACK:
            case TCP_CONNTRACK_TIME_WAIT:
            case TCP_CONNTRACK_CLOSE:
            case TCP_CONNTRACK_TIMEOUT_MAX:
                flow->end = true;
                LOGD("%s: TCP Flow ended", __func__);
                break;

            default:
                break;
            }
        }
    }

    return MNL_CB_OK;
}


/**
 * @brief validates conntrack atttributes
 *
 * @param attr the netlink attribute
 * @param data the table of <attribute, value>
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static int
data_attr_cb(const struct nlattr *attr, void *data)
{
    const struct nlattr **tb;
    int type;
    int rc;

    tb = data;
    type = mnl_attr_get_type(attr);

    rc = mnl_attr_type_valid(attr, CTA_MAX);
    if (rc < 0) return MNL_CB_OK;

    switch (type)
    {
    case CTA_TUPLE_ORIG:
    case CTA_COUNTERS_ORIG:
    case CTA_COUNTERS_REPLY:
        rc = mnl_attr_validate(attr, MNL_TYPE_NESTED);
        if (rc < 0) return MNL_CB_ERROR;
        break;

    case CTA_TIMEOUT:
    case CTA_MARK:
    case CTA_SECMARK:
        rc =  mnl_attr_validate(attr, MNL_TYPE_U32);
        if (rc < 0) return MNL_CB_ERROR;
        break;

    case CTA_ZONE:
        rc = mnl_attr_validate(attr, MNL_TYPE_U16);
        if (rc < 0) return MNL_CB_ERROR;
        break;
    }

    tb[type] = attr;

    return MNL_CB_OK;
}


/**
 * @brief translates conntrack counters to ct_stats flow
 *
 * @param nest the netlink attribute
 * @param flow the ct_stats flow to update
 * @return MNL_CB_OK when successful, -1 otherwise
 */
static int
get_counter(const struct nlattr *nest, ct_flow_t *_flow)
{
    struct nlattr *count_tb[CTA_COUNTERS_MAX+1];
    uint64_t val64;
    uint32_t val32;
    int rc;

    memset(count_tb, 0, (CTA_COUNTERS_MAX+1) * sizeof(count_tb[0]));
    rc = mnl_attr_parse_nested(nest, parse_counters_cb, count_tb);
    if (rc < 0) return MNL_CB_ERROR;

    if (count_tb[CTA_COUNTERS32_PACKETS] != NULL)
    {
        val32 = ntohl(mnl_attr_get_u32(count_tb[CTA_COUNTERS32_PACKETS]));
        _flow->pkt_info.pkt_cnt = val32;
    }

    if (count_tb[CTA_COUNTERS_PACKETS] != NULL)
    {
        val64 = be64toh(mnl_attr_get_u64(count_tb[CTA_COUNTERS_PACKETS]));
        _flow->pkt_info.pkt_cnt = val64;
    }

    if (count_tb[CTA_COUNTERS32_BYTES] != NULL)
    {
        val32 = ntohl(mnl_attr_get_u32(count_tb[CTA_COUNTERS32_BYTES]));
        _flow->pkt_info.bytes = val32;
    }

    if (count_tb[CTA_COUNTERS_BYTES] != NULL)
    {
        val64 = be64toh(mnl_attr_get_u64(count_tb[CTA_COUNTERS_BYTES]));
        _flow->pkt_info.bytes = val64;
    }

    return MNL_CB_OK;
}


/**
 * @brief callback parsing the content of a netlink message
 *
 * @param nhl the netlink header message
 * @param data the opaque context passed to mnl processing
 * @return MNL_CB_OK when successful, -1 otherwise
 */
int
data_cb(const struct nlmsghdr *nlh, void *data)
{
    struct nlattr *tb[CTA_MAX+1];
    ctflow_info_t *flow_info_1;
    ctflow_info_t *flow_info;
    flow_stats_t *ct_stats;
    struct nfgenmsg *nfg;
    ct_flow_t *flow_1;
    uint16_t ct_zone;
    ct_flow_t *flow;
    int reply_flag;
    int rc;
    int af;

    memset(tb, 0, (CTA_MAX+1) * sizeof(tb[0]));
    ct_stats = (flow_stats_t *)data;
    nfg = mnl_nlmsg_get_payload(nlh);
    reply_flag = 1;

    rc = mnl_attr_parse(nlh, sizeof(*nfg), data_attr_cb, tb);
    if (rc < 0) return MNL_CB_ERROR;

    if (tb[CTA_ZONE] != NULL)
    {
        ct_zone = ntohs(mnl_attr_get_u16(tb[CTA_ZONE]));
    }
    else
    {
        ct_zone = 0; /* Zone = 0 flows will not have CTA_ZONE */
    }

    if (ct_zone != g_ct_stats.ct_zone) return MNL_CB_OK;

    LOGT("%s: Included IP flow for ct_zone: %d", __func__,
         g_ct_stats.ct_zone);

    flow_info = calloc(1, sizeof(struct ctflow_info));
    if(flow_info == NULL) return MNL_CB_OK;
    flow =  &flow_info->flow;

    flow_info_1 = calloc(1, sizeof(struct ctflow_info));
    if(flow_info_1 == NULL) goto flow_info_free;
    flow_1 =  &flow_info_1->flow;

    if (tb[CTA_TUPLE_ORIG] == NULL) goto flow_info_free;

    rc = get_tuple(tb[CTA_TUPLE_ORIG], flow);
    if (rc < 0) goto flow_info_1_free;

    if (tb[CTA_TUPLE_REPLY] == NULL) goto flow_info_1_free;

    rc = get_tuple(tb[CTA_TUPLE_REPLY], flow_1);
    if (rc < 0) goto flow_info_1_free;

    af = flow_1->layer3_info.src_ip.ss_family;
    if (af == AF_INET)
    {
        struct sockaddr_in *ssrc;

        ssrc = (struct sockaddr_in *)&flow->layer3_info.src_ip;

        reply_flag = ((ssrc->sin_addr.s_addr & 0XFF000000) == 0XFF000000);
        if (reply_flag == 0)
        {
            flow->layer3_info.dst_ip = flow_1->layer3_info.src_ip;
            flow_1->layer3_info.dst_ip = flow->layer3_info.src_ip;
        }
    }

    if (flow->layer3_info.proto_type != 17  &&
        tb[CTA_PROTOINFO] == NULL)
    {
        LOGT("%s: Missing protocol info.Dropping the ct_flow", __func__);
        goto flow_info_1_free;
    }

    if (flow->layer3_info.proto_type != 17)
    {
        rc = get_protoinfo(tb[CTA_PROTOINFO], flow);
        if (rc < 0) goto flow_info_1_free;
    }

    if (tb[CTA_COUNTERS_ORIG] == NULL) goto flow_info_1_free;

    rc = get_counter(tb[CTA_COUNTERS_ORIG], flow);
    if (rc < 0) goto flow_info_1_free;

    ds_dlist_insert_tail(&ct_stats->ctflow_list, flow_info);
    ct_stats->node_count++;
    if (af == AF_INET && reply_flag != 0) goto reply_flag_free;

    if (tb[CTA_COUNTERS_REPLY] == NULL) goto reply_flag_free;

    rc = get_counter(tb[CTA_COUNTERS_REPLY], flow_1);
    if (rc < 0) goto reply_flag_free;

    ds_dlist_insert_tail(&ct_stats->ctflow_list, flow_info_1);
    ct_stats->node_count++;

    return MNL_CB_OK;

flow_info_1_free:
    free(flow_info_1);
flow_info_free:
    free(flow_info);
    return MNL_CB_OK;

reply_flag_free:
    free(flow_info_1);
    return MNL_CB_OK;
}


/**
 * @brief probes conntrack info for the requested inet family
 *
 * @param af the inet family targeted by the conntrack probe
 * @return MNL_CB_OK when successful, -1 otherwise
 */
int
ct_stats_get_ct_flow(int af_family)
{
    char buf[MNL_SOCKET_BUFFER_SIZE];
    struct mnl_socket *nl;
    struct nlmsghdr *nlh;
    struct nfgenmsg *nfh;
    uint32_t seq, portid;
    int ret;
    int rc;

    nl = mnl_socket_open(NETLINK_NETFILTER);
    if (nl == NULL)
    {
        LOGE("%s: mnl_socket_open fail: %s", __func__, strerror(errno));
        return -1;
    }

    rc = mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID);
    if (rc < 0)
    {
        rc = errno;
        LOGE("%s: mnl_socket_bind fail: %s", __func__, strerror(errno));
        return -1;
    }

    nlh = mnl_nlmsg_put_header(buf);
    nlh->nlmsg_type = (NFNL_SUBSYS_CTNETLINK << 8) | IPCTNL_MSG_CT_GET;
    nlh->nlmsg_flags = NLM_F_REQUEST|NLM_F_DUMP;
    nlh->nlmsg_seq = seq = time(NULL);

    nfh = mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg));
    nfh->nfgen_family = af_family;
    nfh->version = 0;
    nfh->res_id = 0;

    ret = mnl_socket_sendto(nl, nlh, nlh->nlmsg_len);
    if (ret == -1)
    {
        LOGE("%s: mnl_socket_sendto", __func__);
        return -1;
    }

    portid = mnl_socket_get_portid(nl);

    while (1)
    {
        ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
        if (ret == -1)
        {
            ret = errno;
            LOGE("%s: mnl_socket_recvfrom failed: %s", __func__, strerror(ret));
            return 1;
        }

        ret = mnl_cb_run(buf, ret, seq, portid, data_cb, &g_ct_stats);
        if (ret == -1)
        {
            ret = errno;
            LOGE("%s: mnl_cb_run failed: %s", __func__, strerror(ret));
            return -1;
        }
        else if (ret <= MNL_CB_STOP) break;
    }

    mnl_socket_close(nl);

#ifdef CT_DEBUG_PRINT
    ctflow_info_t *flow_info = NULL;
    ds_dlist_foreach(&g_ct_stats.ctflow_list, flow_info)
    {
        ct_stats_print_contrack(&flow_info->flow);
    }
#endif
    if (LOG_SEVERITY_ENABLED(LOG_SEVERITY_TRACE))
        LOGT("%s: total ct flow %d", __func__, g_ct_stats.node_count);

    return 0;
}


/**
 * @brief frees the temporary list of parsed flows
 *
 * @param ct_stats the container of the list
 */
static void
free_ct_flow_list(flow_stats_t *ct_stats)
{
    ct_flow_t *flow;
    int del_count;

    del_count = 0;
    while (!ds_dlist_is_empty(&ct_stats->ctflow_list))
    {
        flow = ds_dlist_remove_tail(&ct_stats->ctflow_list);
        free(flow);
        ct_stats->node_count--;
        del_count++;
    }

    if (LOG_SEVERITY_ENABLED(LOG_SEVERITY_TRACE))
    {
        LOGT("%s: del_count %d node_count %d", __func__,
             del_count, ct_stats->node_count);
    }
    ct_stats->node_count = 0;
}


/**
 * @brief logs a flow content for debug purposes
 *
 * @param flow the flow to log
 */
void
ct_stats_print_contrack(ct_flow_t *flow)
{
    char src[INET6_ADDRSTRLEN];
    char dst[INET6_ADDRSTRLEN];

    if (flow == NULL) return;

    memset(src, 0, sizeof(src));
    memset(dst, 0, sizeof(dst));

    getnameinfo((struct sockaddr *)&flow->layer3_info.src_ip,
                sizeof(struct sockaddr_storage), src, sizeof(src),
                0, 0, NI_NUMERICHOST);
    getnameinfo((struct sockaddr *)&flow->layer3_info.dst_ip,
                sizeof(struct sockaddr_storage), dst, sizeof(dst),
                0, 0, NI_NUMERICHOST);
    LOGI("%s: [ proto=%d tx src=%s dst=%s] ", __func__,
         flow->layer3_info.proto_type, src, dst);

    LOGI("%s: [src port=%d dst port=%d] "
         "[packets=%" PRIu64 "  bytes=%" PRIu64 "]", __func__,
        ntohs(flow->layer3_info.src_port),
        ntohs(flow->layer3_info.dst_port),
        flow->pkt_info.pkt_cnt, flow->pkt_info.bytes);
}


/**
 * @brief apply the named filter to the given flow
 *
 * @param filter_name the filter name
 * @param mac_filter the mac filter
 * @param flow the flow to filter
 */
static bool
apply_filter(char *filter_name, fcm_filter_l2_info_t *mac_filter,
             ct_flow_t *flow)
{
    fcm_filter_l3_info_t filter;
    fcm_filter_stats_t pkt;
    bool action;
    int rc;

    if (filter_name == NULL) return true;

    rc = getnameinfo((struct sockaddr *)&flow->layer3_info.src_ip,
                     sizeof(struct sockaddr_storage),
                     filter.src_ip, sizeof(filter.src_ip),
                     0, 0, NI_NUMERICHOST);
    if (rc < 0) return false;

    rc = getnameinfo((struct sockaddr *)&flow->layer3_info.dst_ip,
                     sizeof(struct sockaddr_storage),
                     filter.dst_ip, sizeof(filter.dst_ip),
                     0, 0, NI_NUMERICHOST);
    if (rc < 0) return false;

    filter.sport = ntohs(flow->layer3_info.src_port);
    filter.dport = ntohs(flow->layer3_info.dst_port);
    filter.l4_proto = flow->layer3_info.proto_type;
    filter.ip_type = flow->layer3_info.family_type;

    pkt.pkt_cnt = flow->pkt_info.pkt_cnt;
    pkt.bytes = flow->pkt_info.bytes;

    fcm_filter_7tuple_apply(filter_name, mac_filter, &filter, &pkt, NULL, &action);

    return action;
}

/**
 * @brief adds collected conntrack info to the plugin aggregator
 *
 * @param ct_stats the aggregator container
 */
void
ct_flow_add_sample(flow_stats_t *ct_stats)
{
    struct net_md_aggregator *aggr;
    struct flow_counters pkts_ct;
    struct net_md_flow_key key;
    ctflow_info_t *flow_info;
    int sample_count;
    ct_flow_t *flow;
    bool ret;

    aggr = ct_stats->aggr;
    sample_count = 0;

    ds_dlist_foreach(&ct_stats->ctflow_list, flow_info)
    {
        bool                     smac_lookup;
        bool                     dmac_lookup;
        fcm_filter_l2_info_t     mac_filter;
        struct sockaddr_storage *ssrc;
        struct sockaddr_storage *sdst;
        os_macaddr_t             smac;
        os_macaddr_t             dmac;
        int                      af;

        memset(&smac, 0, sizeof(os_macaddr_t));
        memset(&dmac, 0, sizeof(os_macaddr_t));

        flow = &flow_info->flow;
        af = flow->layer3_info.src_ip.ss_family;

        ssrc = &flow->layer3_info.src_ip;
        sdst = &flow->layer3_info.dst_ip;

        // Lookup source ip.
        smac_lookup = neigh_table_lookup(ssrc, &smac);
        if (!smac_lookup)
        {
            LOGD("ct_stats: Failed to get mac for src ip of the flow.");
        }

        dmac_lookup = neigh_table_lookup(sdst, &dmac);
        // Lookup dest ip.
        if (!dmac_lookup)
        {
            LOGD("ct_stats: Failed to get mac for dst ip of the flow.");
        }


        snprintf(mac_filter.src_mac, sizeof(mac_filter.src_mac),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 smac.addr[0], smac.addr[1],
                 smac.addr[2], smac.addr[3],
                 smac.addr[4], smac.addr[5]);
        snprintf(mac_filter.dst_mac, sizeof(mac_filter.src_mac),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 dmac.addr[0], dmac.addr[1],
                 dmac.addr[2], dmac.addr[3],
                 dmac.addr[4], dmac.addr[5]);

        if (apply_filter(ct_stats->collect_filter, &mac_filter, flow))
        {
            memset(&key, 0, sizeof(struct net_md_flow_key));
            memset(&pkts_ct, 0, sizeof(struct flow_counters));
            if (smac_lookup) key.smac = &smac;
            if (dmac_lookup) key.dmac = &dmac;

            key.ip_version = (af == AF_INET ? 4 : 6);
            if (af == AF_INET)
            {
                struct sockaddr_in *ssrc;
                struct sockaddr_in *sdst;

                ssrc = (struct sockaddr_in *)&flow->layer3_info.src_ip;
                sdst = (struct sockaddr_in *)&flow->layer3_info.dst_ip;
                key.src_ip = (uint8_t *)&ssrc->sin_addr.s_addr;
                key.dst_ip = (uint8_t *)&sdst->sin_addr.s_addr;
            }
            else if (af == AF_INET6)
            {
                struct sockaddr_in6 *ssrc;
                struct sockaddr_in6 *sdst;

                ssrc = (struct sockaddr_in6 *)&flow->layer3_info.src_ip;
                sdst = (struct sockaddr_in6 *)&flow->layer3_info.dst_ip;
                key.src_ip = ssrc->sin6_addr.s6_addr;
                key.dst_ip = sdst->sin6_addr.s6_addr;
            }
            key.ipprotocol = flow->layer3_info.proto_type;
            key.sport = flow->layer3_info.src_port;
            key.dport = flow->layer3_info.dst_port;
            pkts_ct.packets_count = flow->pkt_info.pkt_cnt;
            pkts_ct.bytes_count = flow->pkt_info.bytes;
            if (flow->start) key.fstart = true;
            if (flow->end) key.fend = true;
            sample_count++;

            ret =  net_md_add_sample(aggr, &key, &pkts_ct);
            if (!ret)
            {
                LOGW("%s: some error with net_md_add_sample", __func__);
                break;
            }
        }
    }
    if (LOG_SEVERITY_ENABLED(LOG_SEVERITY_TRACE))
    {
        LOGT("%s: sample add %d count %d", __func__,
             sample_count, ct_stats->node_count);
    }
    free_ct_flow_list(ct_stats);
}

/**
 * @brief allocates a flow aggregator
 *
 * @param the collector info passed by fcm
 * @return 0 if successful, -1 otherwise
 */
static int
alloc_aggr(fcm_collect_plugin_t *collector)
{
    struct net_md_aggregator_set aggr_set;
    struct net_md_aggregator *aggr;
    struct node_info node_info;
    int report_type;

    memset(&aggr_set, 0, sizeof(aggr_set));
    node_info.node_id = collector->get_mqtt_hdr_node_id();
    node_info.location_id = collector->get_mqtt_hdr_loc_id();
    aggr_set.info = &node_info;
    if (collector->fmt == FCM_RPT_FMT_CUMUL)
    {
        report_type = NET_MD_REPORT_ABSOLUTE;
    }
    else if (collector->fmt == FCM_RPT_FMT_DELTA)
    {
        report_type = NET_MD_REPORT_RELATIVE;
    }
    else
    {
        LOGE("%s: unknown report type request ed: %d", __func__,
             collector->fmt);
        return -1;
    }

    aggr_set.report_type = report_type;
    aggr_set.num_windows = 1;;
    aggr_set.acc_ttl = (2 * collector->report_interval);
    aggr_set.report_filter = fcm_filter_nmd_callback;
    aggr_set.neigh_lookup = neigh_table_lookup;
    aggr = net_md_allocate_aggregator(&aggr_set);
    if (aggr == NULL)
    {
        LOGD("%s: Aggregator allocation failed", __func__);
        return -1;
    }
    g_ct_stats.aggr = aggr;
    collector->plugin_ctx = aggr;

    return 0;
}

/**
 * @brief activates the flow aggregator window
 *
 * @param the collector info passed by fcm
 * @return 0 if successful, -1 otherwise
 */
int
ct_stats_activate_window(fcm_collect_plugin_t *collector)
{
    struct net_md_aggregator *aggr;
    bool ret;

    aggr = collector->plugin_ctx;
    if (aggr == NULL)
    {
        LOGD("%s: Aggregator is empty", __func__);
        return -1;
    }

    ret = net_md_activate_window(aggr);
    if (ret == false)
    {
        LOGD("%s: Aggregator window activation failed", __func__);
        return -1;
    }

    return 0;
}


/**
 * @brief closes the flow aggregator window
 *
 * @param the collector info passed by fcm
 * @return 0 if successful, -1 otherwise
 */
void
ct_stats_close_window(fcm_collect_plugin_t *collector)
{
    struct net_md_aggregator *aggr;
    bool ret;

    aggr = collector->plugin_ctx;

    if (aggr == NULL) return;

    ret = net_md_close_active_window(aggr);

    if (!ret)
    {
        LOGD("%s: Aggregator close window failed", __func__);
        return;
    }
}


/**
 * @brief send flow aggregator report
 *
 * @param the collector info passed by fcm
 * @return 0 if successful, -1 otherwise
 */
void
ct_stats_send_aggr_report(fcm_collect_plugin_t *collector)
{
    struct net_md_aggregator *aggr;
    size_t n_flows;
    bool ret;

    aggr = collector->plugin_ctx;
    if (aggr == NULL) return;

    n_flows = net_md_get_total_flows(aggr);
    if (n_flows <= 0)
    {
        net_md_reset_aggregator(aggr);
        return;
    }

    ret = aggr->send_report(aggr, collector->mqtt_topic);
    if (ret == false)
    {
        LOGD("%s: Aggregator send report failed", __func__);
        return;
    }
}


/**
 * @brief triggers conntrack records collection
 *
 * @param the collector info passed by fcm
 */
void
ct_stats_collect_cb(fcm_collect_plugin_t *collector)
{
    int rc;

    if (collector == NULL) return;

    rc = ct_stats_get_ct_flow(AF_INET);
    if (rc == -1)
    {
        LOGE("%s: conntrack flow collection error", __func__);
        return;
    }

    rc = ct_stats_get_ct_flow(AF_INET6);
    if (rc == -1)
    {
        LOGE("%s: conntrack flow collection error", __func__);
        return;
    }
    g_ct_stats.collect_filter = collector->filters.collect;
    ct_flow_add_sample(&g_ct_stats);
}


/**
 * @brief triggers conntrack records report
 *
 * @param the collector info passed by fcm
 */
void
ct_stats_report_cb(fcm_collect_plugin_t *collector)
{
    uint16_t tmp_zone;
    char *ct_zone;


    if (collector == NULL) return;
    if (collector->mqtt_topic == NULL) return;

    fcm_filter_context_init(collector);
    ct_stats_close_window(collector);
    ct_stats_send_aggr_report(collector);
    ct_stats_activate_window(collector);
    /* Accept zone change after reporting */
    ct_zone = collector->get_other_config(collector, "ct_zone");

    tmp_zone = 0;
    if (ct_zone) tmp_zone = atoi(ct_zone);

    if (g_ct_stats.ct_zone != tmp_zone)
    {
        g_ct_stats.ct_zone = tmp_zone;
        LOGD("%s: updated zone: %d", __func__, g_ct_stats.ct_zone);
    }
}


/**
 * @brief releases ct_stats plugin resources
 *
 * @param the collector info passed by fcm
 */
void
ct_stats_plugin_close_cb(fcm_collect_plugin_t *collector)
{
    struct net_md_aggregator *aggr;
    struct imc_context *server;

    LOGD("%s: CT stats plugin stopped", __func__);
    aggr = collector->plugin_ctx;

    if (aggr == NULL)
    {
        LOGD("%s: Aggregator is empty\n", __func__);
        return;
    }

    ct_stats_close_window(collector);
    net_md_free_aggregator(aggr);

    server = &g_imc_server;
    ct_stats_terminate_server(server);
}


/**
 * @brief imc callobak processing the protobuf receivd from fsm
 *
 * @param data a pointer to the protobuf
 * @param len the protobuf length
 */
static void
test_recv_cb(void *data, size_t len)
{
    struct net_md_aggregator *aggr;
    struct packed_buffer recv_pb;

    aggr = g_ct_stats.aggr;
    recv_pb.buf = data;
    recv_pb.len = len;
    net_md_update_aggr(aggr, &recv_pb);
}


/**
 * @brief starts the imc server receivng flow info from fsm
 */
int
ct_stats_imc_init(void)
{
    struct imc_context *server;
    struct ev_loop *loop;
    bool ret;
    int rc;

    ret = ct_stats_load_imc();
    if (!ret) goto err_init_imc;

    server = &g_imc_server;

    /* Start the server */
    server->ztype = IMC_PULL;
    loop = g_ct_stats.loop;
    rc = ct_stats_init_server(server, loop, test_recv_cb);
    if (rc != 0) goto err_init_imc;

    server->initialized = true;
    return 0;

err_init_imc:
    return -1;
}

/**
 * @brief initializes the ct_stats collector
 *
 * @param collector ct_stats object provided by fcm
 */
int
ct_stats_plugin_init(fcm_collect_plugin_t *collector)
{
    char *ct_zone;
    int rc;

    g_ct_stats.node_count = 0;

    collector->collect_periodic = ct_stats_collect_cb;
    collector->send_report = ct_stats_report_cb;
    collector->close_plugin = ct_stats_plugin_close_cb;

    fcm_filter_context_init(collector);

    ds_dlist_init(&g_ct_stats.ctflow_list, ctflow_info_t, dl_node);

    ct_zone = collector->get_other_config(collector, "ct_zone");
    if (ct_zone) g_ct_stats.ct_zone = atoi(ct_zone);
    else g_ct_stats.ct_zone = 0;
    LOGD("%s: configured zone: %d", __func__, g_ct_stats.ct_zone);

    g_ct_stats.loop = collector->loop;

    rc = alloc_aggr(collector);
    if (rc != 0) return -1;

    rc = ct_stats_activate_window(collector);
    if (rc != 0) goto err;

    rc = ct_stats_imc_init();
    if (rc != 0) goto err;

    return 0;

err:
    net_md_free_aggregator(g_ct_stats.aggr);
    collector->plugin_ctx = NULL;
    g_ct_stats.aggr = NULL;

    return -1;
}
