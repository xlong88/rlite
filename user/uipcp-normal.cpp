#include <vector>
#include <list>
#include <map>
#include <string>
#include <iostream>
#include <fstream>
#include <cstring>
#include <sstream>
#include <unistd.h>
#include <stdint.h>
#include <cstdlib>
#include <cassert>
#include <pthread.h>

#include "uipcp-normal.hpp"

using namespace std;


namespace obj_class {
    string adata = "a_data";
    string dft = "dft";
    string neighbors = "neighbors";
    string enrollment = "enrollment";
    string status = "operational_status";
    string address = "address";
    string lfdb = "fsodb"; /* Lower Flow DB */
    string flows = "flows"; /* Supported flows */
    string flow = "flow";
};

namespace obj_name {
    string adata = "a_data";
    string dft = "/dif/mgmt/fa/" + obj_class::dft;
    string neighbors = "/daf/mgmt/" + obj_class::neighbors;
    string enrollment = "/def/mgmt/" + obj_class::enrollment;
    string status = "/daf/mgmt/" + obj_class::status;
    string address = "/daf/mgmt/naming" + obj_class::address;
    string lfdb = "/dif/mgmt/pduft/linkstate/" + obj_class::lfdb;
    string whatevercast = "/daf/mgmt/naming/whatevercast";
    string flows = "/dif/ra/fa/" + obj_class::flows;
};

#define MGMTBUF_SIZE_MAX 4096

static int
mgmt_write(struct uipcp *uipcp, const struct rina_mgmt_hdr *mhdr,
           void *buf, size_t buflen)
{
    uipcp_rib *rib = UIPCP_RIB(uipcp);
    char *mgmtbuf;
    int n;
    int ret = 0;

    if (buflen > MGMTBUF_SIZE_MAX) {
        PE("Dropping oversized mgmt message %d/%d\n",
            (int)buflen, MGMTBUF_SIZE_MAX);
    }

    mgmtbuf = (char *)malloc(sizeof(*mhdr) + buflen);
    if (!mgmtbuf) {
        PE("Out of memory\n");
        return -1;
    }

    memcpy(mgmtbuf, mhdr, sizeof(*mhdr));
    memcpy(mgmtbuf + sizeof(*mhdr), buf, buflen);
    buflen += sizeof(*mhdr);

    n = write(rib->mgmtfd, mgmtbuf, buflen);
    if (n < 0) {
        PE("write(): %d\n", n);
        ret = n;
    } else if (n != (int)buflen) {
        PE("partial write %d/%d\n", n, (int)buflen);
        ret = -1;
    }

    free(mgmtbuf);

    return ret;
}

int
mgmt_write_to_local_port(struct uipcp *uipcp, uint32_t local_port,
                         void *buf, size_t buflen)
{
    struct rina_mgmt_hdr mhdr;

    memset(&mhdr, 0, sizeof(mhdr));
    mhdr.type = RINA_MGMT_HDR_T_OUT_LOCAL_PORT;
    mhdr.local_port = local_port;

    return mgmt_write(uipcp, &mhdr, buf, buflen);
}

int
mgmt_write_to_dst_addr(struct uipcp *uipcp, uint64_t dst_addr,
                       void *buf, size_t buflen)
{
    struct rina_mgmt_hdr mhdr;

    memset(&mhdr, 0, sizeof(mhdr));
    mhdr.type = RINA_MGMT_HDR_T_OUT_DST_ADDR;
    mhdr.remote_addr = dst_addr;

    return mgmt_write(uipcp, &mhdr, buf, buflen);
}

static int
rib_msg_rcvd(struct uipcp_rib *rib, struct rina_mgmt_hdr *mhdr,
             char *serbuf, int serlen)
{
    map<string, Neighbor>::iterator neigh;
    CDAPMessage *m = NULL;
    int ret;

    try {
        m = msg_deser_stateless(serbuf, serlen);

        if (m->obj_class == obj_class::adata &&
                    m->obj_name == obj_name::adata) {
            /* A-DATA message, does not belong to any CDAP
             * session. */
            const char *objbuf;
            size_t objlen;

            m->get_obj_value(objbuf, objlen);
            if (!objbuf) {
                PE("CDAP message does not contain a nested message\n");

                delete m;
                return 0;
            }

            AData adata(objbuf, objlen);

            if (!adata.cdap) {
                PE("A_DATA does not contain an encapsulated CDAP message\n");

                delete m;
                return 0;
            }

            ScopeLock(rib->lock);

            rib->cdap_dispatch(adata.cdap, NULL);

            delete m;
            return 0;
        }

        /* This is not an A-DATA message, so we try to match it
         * against existing CDAP connections.
         */

        /* Easy and inefficient solution for now. We delete the
         * already parsed CDAP message and call msg_deser() on
         * the matching connection (if found) --> This causes the
         * same message to be deserialized twice. The second
         * deserialization can be avoided extending the CDAP
         * library with a sort of CDAPConn::msg_rcv_feed_fsm(). */
        delete m;
        m = NULL;

        ScopeLock(rib->lock);

        /* Lookup neighbor by port id. */
        neigh = rib->lookup_neigh_by_port_id(mhdr->local_port);
        if (neigh == rib->neighbors.end()) {
            PE("Received message from unknown port id %d\n",
                    mhdr->local_port);
            return -1;
        }

        if (!neigh->second.conn) {
            neigh->second.conn = new CDAPConn(neigh->second.flow_fd, 1);
        }

        /* Deserialize the received CDAP message. */
        m = neigh->second.conn->msg_deser(serbuf, serlen);
        if (!m) {
            PE("msg_deser() failed\n");
            return -1;
        }

        /* Feed the enrollment state machine. */
        ret = neigh->second.enroll_fsm_run(m);

    } catch (std::bad_alloc) {
        PE("Out of memory\n");
    }

    delete m;

    return ret;
}

static void
mgmt_fd_ready(struct rlite_evloop *loop, int fd)
{
    struct rlite_appl *appl = container_of(loop, struct rlite_appl, loop);
    struct uipcp *uipcp = container_of(appl, struct uipcp, appl);
    uipcp_rib *rib = UIPCP_RIB(uipcp);
    char mgmtbuf[MGMTBUF_SIZE_MAX];
    struct rina_mgmt_hdr *mhdr;
    int n;

    assert(fd == rib->mgmtfd);

    /* Read a buffer that contains a management header followed by
     * a management SDU. */
    n = read(fd, mgmtbuf, sizeof(mgmtbuf));
    if (n < 0) {
        PE("Error: read() failed [%d]\n", n);
        return;

    } else if (n < (int)sizeof(*mhdr)) {
        PE("Error: read() does not contain mgmt header, %d<%d\n",
                n, (int)sizeof(*mhdr));
        return;
    }

    /* Grab the management header. */
    mhdr = (struct rina_mgmt_hdr *)mgmtbuf;
    assert(mhdr->type == RINA_MGMT_HDR_T_IN);

    /* Hand off the message to the RIB. */
    rib_msg_rcvd(rib, mhdr, ((char *)(mhdr + 1)),
                 n - sizeof(*mhdr));
}

uipcp_rib::uipcp_rib(struct uipcp *_u) : uipcp(_u)
{
    int ret;

    pthread_mutex_init(&lock, NULL);

    mgmtfd = rlite_open_mgmt_port(uipcp->ipcp_id);
    if (mgmtfd < 0) {
        ret = mgmtfd;
        throw std::exception();
    }

    ret = rlite_evloop_fdcb_add(&uipcp->appl.loop, mgmtfd, mgmt_fd_ready);
    if (ret) {
        close(mgmtfd);
        throw std::exception();
    }

    if (load_qos_cubes("/etc/rlite/uipcp-qoscubes.qos")) {
        close(mgmtfd);
        throw std::exception();
    }

    /* Insert the handlers for the RIB objects. */
    handlers.insert(make_pair(obj_name::dft, &uipcp_rib::dft_handler));
    handlers.insert(make_pair(obj_name::neighbors, &uipcp_rib::neighbors_handler));
    handlers.insert(make_pair(obj_name::lfdb, &uipcp_rib::lfdb_handler));
    handlers.insert(make_pair(obj_name::flows, &uipcp_rib::flows_handler));

}

uipcp_rib::~uipcp_rib()
{
    rlite_evloop_fdcb_del(&uipcp->appl.loop, mgmtfd);
    close(mgmtfd);
    pthread_mutex_destroy(&lock);
}

struct rlite_ipcp *
uipcp_rib::ipcp_info() const
{
    struct rlite_ipcp *ipcp;

    ipcp = rlite_lookup_ipcp_by_id(&uipcp->appl.loop, uipcp->ipcp_id);
    assert(ipcp);

    return ipcp;
}

static inline string
u82boolstr(uint8_t v) {
    return v != 0 ? string("true") : string("false");
}

char *
uipcp_rib::dump() const
{
    stringstream ss;
    struct rlite_ipcp *ipcp = ipcp_info();

    ss << "QoS cubes" << endl;
    for (map<string, struct rina_flow_config>::const_iterator
                    i = qos_cubes.begin(); i != qos_cubes.end(); i++) {
            const struct rina_flow_config& c = i->second;

            ss << i->first.c_str() << ": {" << endl;
            ss << "   partial_delivery=" << u82boolstr(c.partial_delivery)
                << endl << "   incomplete_delivery=" <<
                u82boolstr(c.incomplete_delivery) << endl <<
                "   in_order_delivery=" << u82boolstr(c.in_order_delivery)
                << endl << "   max_sdu_gap=" <<
                static_cast<unsigned long long>(c.max_sdu_gap) << endl
                << "   dtcp_present=" << u82boolstr(c.dtcp_present) << endl
                << "   dtcp.initial_a=" <<
                static_cast<unsigned int>(c.dtcp.initial_a) << endl
                << "   dtcp.flow_control=" << u82boolstr(c.dtcp.flow_control)
                << endl << "   dtcp.rtx_control=" <<
                u82boolstr(c.dtcp.rtx_control) << endl;

            if (c.dtcp.fc.fc_type == RINA_FC_T_WIN) {
                ss << "   dtcp.fc.max_cwq_len=" <<
                    static_cast<unsigned int>(c.dtcp.fc.cfg.w.max_cwq_len)
                    << endl << "   dtcp.fc.initial_credit=" <<
                    static_cast<unsigned int>(c.dtcp.fc.cfg.w.initial_credit)
                    << endl;
            } else if (c.dtcp.fc.fc_type == RINA_FC_T_RATE) {
                ss << "   dtcp.fc.sending_rate=" <<
                    static_cast<unsigned int>(c.dtcp.fc.cfg.r.sending_rate)
                    << endl << "   dtcp.fc.time_period=" <<
                    static_cast<unsigned int>(c.dtcp.fc.cfg.r.time_period)
                    << endl;
            }

            ss << "   dtcp.rtx.max_time_to_retry=" <<
                static_cast<unsigned int>(c.dtcp.rtx.max_time_to_retry)
                << endl << "   dtcp.rtx.data_rxms_max=" <<
                static_cast<unsigned int>(c.dtcp.rtx.data_rxms_max) << endl <<
                "   dtcp.rtx.initial_tr=" <<
                static_cast<unsigned int>(c.dtcp.rtx.initial_tr) << endl;
            ss << "}" << endl;
    }

    ss << "Address: " << ipcp->ipcp_addr << endl << endl;

    ss << "LowerDIFs: {";
    for (list<string>::const_iterator lit = lower_difs.begin();
                            lit != lower_difs.end(); lit++) {
            ss << *lit << ", ";
    }
    ss << "}" << endl << endl;

    ss << "Candidate Neighbors:" << endl;
    for (map<string, NeighborCandidate>::const_iterator
            mit = cand_neighbors.begin();
                mit != cand_neighbors.end(); mit++) {
        const NeighborCandidate& cand = mit->second;

        ss << "    Name: " << cand.apn << "/" << cand.api
            << ", Address: " << cand.address << ", Lower DIFs: {";

        for (list<string>::const_iterator lit = cand.lower_difs.begin();
                    lit != cand.lower_difs.end(); lit++) {
            ss << *lit << ", ";
        }
        ss << "}" << endl;
    }

    ss << endl;

    ss << "Directory Forwarding Table:" << endl;
    for (map<string, DFTEntry>::const_iterator
            mit = dft.begin(); mit != dft.end(); mit++) {
        const DFTEntry& entry = mit->second;

        ss << "    Application: " << static_cast<string>(entry.appl_name)
            << ", Address: " << entry.address << ", Timestamp: "
                << entry.timestamp << endl;
    }

    ss << endl;

    ss << "Lower Flow Database:" << endl;
    for (map<string, LowerFlow>::const_iterator
            mit = lfdb.begin(); mit != lfdb.end(); mit++) {
        const LowerFlow& flow = mit->second;

        ss << "    LocalAddr: " << flow.local_addr << ", RemoteAddr: "
            << flow.remote_addr << ", Cost: " << flow.cost <<
                ", Seqnum: " << flow.seqnum << ", State: " << flow.state
                    << ", Age: " << flow.age << endl;
    }

    ss << endl;

    ss << "Supported flows:" << endl;
    for (map<string, FlowRequest>::const_iterator
            mit = flow_reqs.begin(); mit != flow_reqs.end(); mit++) {
        const FlowRequest& freq = mit->second;

        ss << "    SrcAppl: " << static_cast<string>(freq.src_app) <<
                ", DstAppl: " << static_cast<string>(freq.dst_app) <<
                ", SrcAddr: " << freq.src_addr << ", SrcPort: " <<
                freq.src_port << ", DstAddr: " << freq.dst_addr <<
                ", DstPort: " << freq.dst_port << endl;
    }

    return strdup(ss.str().c_str());
}

int
uipcp_rib::set_address(uint64_t address)
{
    stringstream addr_ss;

    addr_ss << address;
    return rlite_ipcp_config(&uipcp->appl.loop, uipcp->ipcp_id,
                                "address", addr_ss.str().c_str());
}

int
uipcp_rib::register_to_lower(int reg, string lower_dif)
{
    list<string>::iterator lit;

    for (lit = lower_difs.begin(); lit != lower_difs.end(); lit++) {
        if (*lit == lower_dif) {
            break;
        }
    }

    if (reg) {
        if (lit != lower_difs.end()) {
            PE("DIF %s already registered\n", lower_dif.c_str());
            return -1;
        }

        lower_difs.push_back(lower_dif);

    } else {
        if (lit == lower_difs.end()) {
            PE("DIF %s not registered\n", lower_dif.c_str());
            return -1;
        }
        lower_difs.erase(lit);
    }

    return 0;
}

int
uipcp_rib::send_to_dst_addr(CDAPMessage& m, uint64_t dst_addr,
                            const UipcpObject& obj)
{
    struct rlite_ipcp *ipcp;
    AData adata;
    CDAPMessage am;
    char objbuf[4096];
    char aobjbuf[4096];
    char *serbuf;
    int objlen;
    int aobjlen;
    size_t serlen;
    int ret;

    ipcp = ipcp_info();

    objlen = obj.serialize(objbuf, sizeof(objbuf));
    if (objlen < 0) {
        PE("serialization failed\n");
        return -1;
    }

    m.set_obj_value(objbuf, objlen);

    m.invoke_id = invoke_id_mgr.get_invoke_id();

    adata.src_addr = ipcp->ipcp_addr;
    adata.dst_addr = dst_addr;
    adata.cdap = &m;

    am.m_write(gpb::F_NO_FLAGS, obj_class::adata, obj_name::adata,
               0, 0, string());

    aobjlen = adata.serialize(aobjbuf, sizeof(aobjbuf));
    if (aobjlen < 0) {
        invoke_id_mgr.put_invoke_id(m.invoke_id);
        PE("serialization failed\n");
        return -1;
    }

    am.set_obj_value(aobjbuf, aobjlen);

    try {
        ret = msg_ser_stateless(&am, &serbuf, &serlen);
    } catch (std::bad_alloc) {
        ret = -1;
    }

    if (ret) {
        PE("message serialization failed\n");
        invoke_id_mgr.put_invoke_id(m.invoke_id);
        delete serbuf;
        return -1;
    }

    return mgmt_write_to_dst_addr(uipcp, dst_addr, serbuf, serlen);
}

/* To be called under RIB lock. */
int
uipcp_rib::cdap_dispatch(const CDAPMessage *rm, Neighbor *neigh)
{
    /* Dispatch depending on the obj_name specified in the request. */
    map< string, rib_handler_t >::iterator hi = handlers.find(rm->obj_name);

    if (hi == handlers.end()) {
        size_t pos = rm->obj_name.rfind("/");
        string container_obj_name;

        if (pos != string::npos) {
            container_obj_name = rm->obj_name.substr(0, pos);
            PD("Falling back to container object '%s'\n",
               container_obj_name.c_str());
            hi = handlers.find(container_obj_name);
        }
    }

    if (hi == handlers.end()) {
        PE("Unable to manage CDAP message\n");
        rm->print();
        return -1;
    }

    return (this->*(hi->second))(rm, neigh);
}

uint64_t
uipcp_rib::address_allocate() const
{
    return 0; // TODO
}

int
uipcp_rib::remote_sync_obj_excluding(const Neighbor *exclude,
                                 bool create, const string& obj_class,
                                 const string& obj_name,
                                 const UipcpObject *obj_value) const
{
    for (map<string, Neighbor>::const_iterator neigh = neighbors.begin();
                        neigh != neighbors.end(); neigh++) {
        if (exclude && neigh->second == *exclude) {
            continue;
        }

        if (neigh->second.enrollment_state != Neighbor::ENROLLED) {
            /* Skip this one since it's not enrolled yet. */
            continue;
        }

        neigh->second.remote_sync_obj(create, obj_class, obj_name, obj_value);
    }

    return 0;
}

int
uipcp_rib::remote_sync_obj_all(bool create, const string& obj_class,
                           const string& obj_name,
                           const UipcpObject *obj_value) const
{
    return remote_sync_obj_excluding(NULL, create, obj_class, obj_name, obj_value);
}

static int
normal_appl_register(struct rlite_evloop *loop,
                     const struct rina_msg_base_resp *b_resp,
                     const struct rina_msg_base *b_req)
{
    struct rlite_appl *appl = container_of(loop, struct rlite_appl,
                                                   loop);
    struct uipcp *uipcp = container_of(appl, struct uipcp, appl);
    struct rina_kmsg_appl_register *req =
                (struct rina_kmsg_appl_register *)b_resp;
    uipcp_rib *rib = UIPCP_RIB(uipcp);
    ScopeLock(rib->lock);

    rib->appl_register(req);

    return 0;
}

static int
normal_fa_req(struct rlite_evloop *loop,
             const struct rina_msg_base_resp *b_resp,
             const struct rina_msg_base *b_req)
{
    struct rlite_appl *appl = container_of(loop, struct rlite_appl,
                                                   loop);
    struct uipcp *uipcp = container_of(appl, struct uipcp, appl);
    struct rina_kmsg_fa_req *req = (struct rina_kmsg_fa_req *)b_resp;
    uipcp_rib *rib = UIPCP_RIB(uipcp);

    PD("[uipcp %u] Got reflected message\n", uipcp->ipcp_id);

    assert(b_req == NULL);

    ScopeLock(rib->lock);

    return rib->fa_req(req);
}

static int
neigh_fa_req_arrived(struct rlite_evloop *loop,
                     const struct rina_msg_base_resp *b_resp,
                     const struct rina_msg_base *b_req)
{
    struct rlite_appl *appl = container_of(loop, struct rlite_appl,
                                                   loop);
    struct uipcp *uipcp = container_of(appl, struct uipcp, appl);
    struct rina_kmsg_fa_req_arrived *req =
                    (struct rina_kmsg_fa_req_arrived *)b_resp;
    uipcp_rib *rib = UIPCP_RIB(uipcp);
    int flow_fd;
    int result = 0;
    int ret;

    assert(b_req == NULL);

    PD("flow request arrived: [ipcp_id = %u, data_port_id = %u]\n",
            req->ipcp_id, req->port_id);

    /* First of all we update the neighbors in the RIB. This
     * must be done before invoking rlite_flow_allocate_resp,
     * otherwise a race condition would exist (us receiving
     * an M_CONNECT from the neighbor before having the
     * chance to call rib_neigh_set_port_id()). */
    ret = rib_neigh_set_port_id(rib, &req->remote_appl,
                                req->port_id);
    if (ret) {
        PE("rib_neigh_set_port_id() failed\n");
        result = 1;
    }

    ret = rlite_flow_allocate_resp(&uipcp->appl, req->kevent_id, req->ipcp_id,
                                   uipcp->ipcp_id, req->port_id, result);

    if (ret || result) {
        PE("rlite_flow_allocate_resp() failed\n");
        goto err;
    }

    flow_fd = rlite_open_appl_port(req->port_id);
    if (flow_fd < 0) {
        goto err;
    }

    ret = rib_neigh_set_flow_fd(rib, &req->remote_appl, flow_fd);
    if (ret) {
        goto err;
    }

    return 0;

err:
    rib->del_neighbor(RinaName(&req->remote_appl));

    return 0;
}

static int
normal_fa_resp(struct rlite_evloop *loop,
              const struct rina_msg_base_resp *b_resp,
              const struct rina_msg_base *b_req)
{
    struct rlite_appl *appl = container_of(loop, struct rlite_appl,
                                                   loop);
    struct uipcp *uipcp = container_of(appl, struct uipcp, appl);
    struct rina_kmsg_fa_resp *resp =
                (struct rina_kmsg_fa_resp *)b_resp;
    uipcp_rib *rib = UIPCP_RIB(uipcp);

    PD("[uipcp %u] Got reflected message\n", uipcp->ipcp_id);

    assert(b_req == NULL);

    ScopeLock(rib->lock);

    return rib->fa_resp(resp);
}

static int
normal_flow_deallocated(struct rlite_evloop *loop,
                       const struct rina_msg_base_resp *b_resp,
                       const struct rina_msg_base *b_req)
{
    struct rlite_appl *appl = container_of(loop, struct rlite_appl,
                                                   loop);
    struct uipcp *uipcp = container_of(appl, struct uipcp, appl);
    struct rina_kmsg_flow_deallocated *req =
                (struct rina_kmsg_flow_deallocated *)b_resp;
    uipcp_rib *rib = UIPCP_RIB(uipcp);
    ScopeLock(rib->lock);

    rib->flow_deallocated(req);

    return 0;
}

static int
normal_init(struct uipcp *uipcp)
{
    try {
        uipcp->priv = new uipcp_rib(uipcp);

    } catch (std::bad_alloc) {
        PE("Out of memory\n");
        return -1;
    } catch (std::exception) {
        PE("RIB initialization failed\n");
        return -1;
    }

    return rlite_evloop_set_handler(&uipcp->appl.loop, RINA_KERN_FA_REQ_ARRIVED,
                                    neigh_fa_req_arrived);

    return 0;
}

static int
normal_fini(struct uipcp *uipcp)
{
    delete UIPCP_RIB(uipcp);

    return 0;
}

static int
normal_ipcp_register(struct uipcp *uipcp, int reg,
                     const struct rina_name *lower_dif,
                     unsigned int ipcp_id,
                     const struct rina_name *ipcp_name)
{
    uipcp_rib *rib = UIPCP_RIB(uipcp);
    string name;
    int result;

    /* Perform the registration. */
    result = rlite_appl_register_wait(&uipcp->appl, reg, lower_dif,
                                      0, NULL, ipcp_name);

    if (result) {
        return result;
    }

    if (!rina_name_valid(lower_dif)) {
        PE("lower_dif name is not valid\n");
        return -1;
    }

    name = string(lower_dif->apn);

    ScopeLock(rib->lock);

    result = rib->register_to_lower(reg, name);

    return result;
}

static int
normal_ipcp_dft_set(struct uipcp *uipcp, struct rina_cmsg_ipcp_dft_set *req)
{
    uipcp_rib *rib = UIPCP_RIB(uipcp);
    ScopeLock(rib->lock);

    return rib->dft_set(RinaName(&req->appl_name), req->remote_addr);
}

static char *
normal_ipcp_rib_show(struct uipcp *uipcp)
{
    uipcp_rib *rib = UIPCP_RIB(uipcp);
    ScopeLock(rib->lock);

    return rib->dump();
}

struct uipcp_ops normal_ops = {
    .init = normal_init,
    .fini = normal_fini,
    .register_to_lower = normal_ipcp_register,
    .enroll = normal_ipcp_enroll,
    .dft_set = normal_ipcp_dft_set,
    .rib_show = normal_ipcp_rib_show,
    .appl_register = normal_appl_register,
    .fa_req = normal_fa_req,
    .fa_resp = normal_fa_resp,
    .flow_deallocated = normal_flow_deallocated,
};
