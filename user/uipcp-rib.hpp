#ifndef __UIPCP_RIB_H__
#define __UIPCP_RIB_H__

#include <string>
#include <map>
#include <list>

#include "rinalite/common.h"
#include "rinalite/utils.h"
#include "rinalite/conf-msg.h"
#include "rinalite-appl.h"
#include "rinalite-conf.h"

#include "uipcp-codecs.hpp"
#include "uipcp-container.h"
#include "cdap.hpp"

namespace obj_class {
    extern std::string adata;
    extern std::string dft;
    extern std::string neighbors;
    extern std::string enrollment;
    extern std::string status;
    extern std::string address;
    extern std::string lfdb; /* Lower Flow DB */
    extern std::string flows; /* Supported flows */
    extern std::string flow;
};

namespace obj_name {
    extern std::string adata;
    extern std::string dft;
    extern std::string neighbors;
    extern std::string enrollment;
    extern std::string status;
    extern std::string address;
    extern std::string lfdb;
    extern std::string whatevercast;
    extern std::string flows;
};

struct Neighbor {
    RinaName ipcp_name;
    int flow_fd;
    unsigned int port_id;
    CDAPConn *conn;
    struct uipcp_rib *rib;

    enum state_t {
        NONE = 0,
        I_WAIT_CONNECT_R,
        S_WAIT_START,
        I_WAIT_START_R,
        S_WAIT_STOP_R,
        I_WAIT_STOP,
        I_WAIT_START,
        ENROLLED,
        ENROLLMENT_STATE_LAST,
    } enrollment_state;

    typedef int (Neighbor::*enroll_fsm_handler_t)(const CDAPMessage *rm);
    enroll_fsm_handler_t enroll_fsm_handlers[ENROLLMENT_STATE_LAST];

    Neighbor() : conn(NULL), rib(NULL) { } /* Required to use the map. */
    Neighbor(struct uipcp_rib *rib, const struct rina_name *name,
             int fd, unsigned int port_id);
    Neighbor(const Neighbor& other);
    bool operator==(const Neighbor& other) const { return port_id == other.port_id; }
    bool operator!=(const Neighbor& other) const { return !(*this == other); }
    ~Neighbor();

    const char *enrollment_state_repr(state_t s) const;

    int send_to_port_id(CDAPMessage *m, int invoke_id,
                        const UipcpObject *obj) const;
    int enroll_fsm_run(const CDAPMessage *rm);

    /* Enrollment state machine handlers. */
    int none(const CDAPMessage *rm);
    int i_wait_connect_r(const CDAPMessage *rm);
    int s_wait_start(const CDAPMessage *rm);
    int i_wait_start_r(const CDAPMessage *rm);
    int i_wait_stop(const CDAPMessage *rm);
    int s_wait_stop_r(const CDAPMessage *rm);
    int i_wait_start(const CDAPMessage *rm);
    int enrolled(const CDAPMessage *rm);

    void abort();
};

/* Shortest Path algorithm. */
class SPEngine {
public:
    SPEngine() {};
    int run(uint64_t, const std::map<std::string, LowerFlow >& db);

    /* The routing table computed by run(). */
    std::map<uint64_t, uint64_t> next_hops;

private:
    struct Edge {
        uint64_t to;
        unsigned int cost;

        Edge(uint64_t to_, unsigned int cost_) :
                            to(to_), cost(cost_) { }
    };

    struct Info {
        unsigned int dist;
        bool visited;
    };

    std::map<uint64_t, std::list<Edge> > graph;
    std::map<uint64_t, Info> info;
};

struct uipcp_rib {
    /* Backpointer to parent data structure. */
    struct uipcp *uipcp;

    typedef int (uipcp_rib::*rib_handler_t)(const CDAPMessage *rm,
                                            Neighbor *neigh);
    std::map< std::string, rib_handler_t > handlers;

    /* Lower DIFs. */
    std::list< std::string > lower_difs;

    /* Neighbors. */
    std::map< std::string, Neighbor > neighbors;
    std::map< std::string, NeighborCandidate > cand_neighbors;

    /* Directory Forwarding Table. */
    std::map< std::string, DFTEntry > dft;

    /* Lower Flow Database. */
    std::map< std::string, LowerFlow > lfdb;

    SPEngine spe;

    /* For A-DATA messages. */
    InvokeIdMgr invoke_id_mgr;

    /* Supported flows. */
    std::map< std::string, FlowRequest> flow_reqs;

    uipcp_rib(struct uipcp *_u);

    struct rinalite_ipcp *ipcp_info() const;
    char *dump() const;

    int set_address(uint64_t address);
    int add_neighbor(const struct rina_name *neigh_name, int neigh_flow_fd,
                     unsigned int neigh_port_id, bool start_enrollment);
    uint64_t dft_lookup(const RinaName& appl_name) const;
    int dft_set(const RinaName& appl_name, uint64_t remote_addr);
    int ipcp_register(int reg, std::string lower_dif);
    int application_register(int reg, const RinaName& appl_name);
    int flow_deallocated(struct rina_kmsg_flow_deallocated *req);
    uint64_t lookup_neighbor_address(const RinaName& neigh_name) const;
    RinaName lookup_neighbor_by_address(uint64_t address);
    int add_lower_flow(uint64_t local_addr, const Neighbor& neigh);
    int fa_req(struct rina_kmsg_fa_req *req);
    int fa_resp(struct rina_kmsg_fa_resp *resp);
    int pduft_sync();
    std::map<std::string, Neighbor>::iterator lookup_neigh_by_port_id(unsigned int
                                                            port_id);
    uint64_t address_allocate() const;

    int send_to_dst_addr(CDAPMessage& m, uint64_t dst_addr,
                         const UipcpObject& obj);

    /* Synchronize neighbors. */
    int remote_sync_neigh(const Neighbor& neigh, bool create,
                          const std::string& obj_class, const std::string& obj_name,
                          const UipcpObject *obj_value) const;
    int remote_sync_excluding(const Neighbor *exclude, bool create,
                              const std::string& obj_class,
                              const std::string& obj_name,
                              const UipcpObject *obj_value) const;
    int remote_sync_all(bool create, const std::string& obj_class,
                        const std::string& obj_name,
                        const UipcpObject *obj_value) const;

    /* Receive info from neighbors. */
    int cdap_dispatch(const CDAPMessage *rm, Neighbor *neigh);

    /* RIB handlers for received CDAP messages. */
    int dft_handler(const CDAPMessage *rm, Neighbor *neigh);
    int neighbors_handler(const CDAPMessage *rm, Neighbor *neigh);
    int lfdb_handler(const CDAPMessage *rm, Neighbor *neigh);
    int flows_handler(const CDAPMessage *rm, Neighbor *neigh);

    int flows_handler_create(const CDAPMessage *rm, Neighbor *neigh);
    int flows_handler_create_r(const CDAPMessage *rm, Neighbor *neigh);
};

#endif  /* __UIPCP_RIB_H__ */