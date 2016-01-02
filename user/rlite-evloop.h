#ifndef __RINALITE_EVLOOP_H__
#define __RINALITE_EVLOOP_H__

#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <rlite/kernel-msg.h>
#include <rlite/conf-msg.h>
#include <rlite/utils.h>

#include "rlite-list.h"


#ifdef __cplusplus
extern "C" {
#endif

struct rlite_ipcp {
    /* IPCP attributes. */
    unsigned int ipcp_id;
    struct rina_name ipcp_name;
    uint64_t ipcp_addr;
    char *dif_type;
    struct rina_name dif_name;

    struct list_head node;
};

/* Some useful macros for casting. */
#define RINALITE_RMB(m) (struct rina_msg_base *)(m)
#define RINALITE_RMBR(m) (struct rina_msg_base_resp *)(m)

struct rlite_evloop;

/* The signature of a response handler. */
typedef int (*rina_resp_handler_t)(struct rlite_evloop *loop,
                                   const struct rina_msg_base_resp *b_resp,
                                   const struct rina_msg_base *b_req);

/* The signature of file descriptor callback. */
typedef void (*rlite_evloop_fdcb_t)(struct rlite_evloop *loop, int fd);

/* The signature of timer callback. */
typedef void (*rlite_tmr_cb_t)(struct rlite_evloop *loop, void *arg);

struct rlite_evloop_fdcb {
    int fd;
    rlite_evloop_fdcb_t cb;

    struct list_head node;
};

struct rlite_evloop {
    /* Handler for the event loop thread. */
    pthread_t evloop_th;

    /* Table containing the kernel handlers. */
    rina_resp_handler_t handlers[RINA_KERN_MSG_MAX+1];

    /* File descriptor for the RINA control device ("/dev/rlite") */
    int rfd;

    /* A FIFO queue that stores pending RINA events. */
    struct list_head pqueue;

    /* What event-id to use for the next request issued to the kernel. */
    uint32_t event_id_counter;

    /* Synchronization variables used to implement mutual exclusion between the
     * event-loop thread and the script thead. */
    pthread_mutex_t lock;

    /* Used to stop the event-loop. */
    int eventfd;

    /* Used to store the list of file descriptor callbacks registered within
     * the event-loop. */
    struct list_head fdcbs;

    struct list_head timer_events;
    pthread_mutex_t timer_lock;
    int timer_events_cnt;
    int timer_next_id;

    /* Used to store the list of ipcp entries fetched from kernel. */
    struct list_head ipcps;
};

/* Issue a request message to the kernel. Takes the ownership of
 * @msg. */
struct rina_msg_base *
rlite_issue_request(struct rlite_evloop *loop, struct rina_msg_base *msg,
                       size_t msg_len, int has_response,
                       unsigned int wait_for_completion, int *result);

int
rlite_evloop_stop(struct rlite_evloop *loop);

int
rlite_evloop_init(struct rlite_evloop *loop, const char *dev,
                     rina_resp_handler_t *handlers);

int
rlite_evloop_fini(struct rlite_evloop *loop);

int
rlite_evloop_set_handler(struct rlite_evloop *loop, unsigned int index,
                            rina_resp_handler_t handler);

int
rlite_evloop_fdcb_add(struct rlite_evloop *loop, int fd,
                         rlite_evloop_fdcb_t cb);

int
rlite_evloop_fdcb_del(struct rlite_evloop *loop, int fd);

int
rlite_evloop_schedule(struct rlite_evloop *loop, unsigned long delta_ms,
                      rlite_tmr_cb_t cb, void *arg);

int
rlite_evloop_schedule_canc(struct rlite_evloop *loop, int id);

int
rlite_ipcps_print(struct rlite_evloop *loop);

/* Fetch information about all IPC processes. */
int
rlite_ipcps_fetch(struct rlite_evloop *loop);

struct rlite_ipcp *
rlite_select_ipcp_by_dif(struct rlite_evloop *loop,
                            const struct rina_name *dif_name,
                            int fallback);

struct rlite_ipcp *
rlite_lookup_ipcp_by_name(struct rlite_evloop *loop,
                             const struct rina_name *name);

int
rlite_lookup_ipcp_addr_by_id(struct rlite_evloop *loop, unsigned int id,
                                uint64_t *addr);

struct rlite_ipcp *
rlite_lookup_ipcp_by_id(struct rlite_evloop *loop, unsigned int id);

#ifdef __cplusplus
}
#endif

#endif  /* __RINALITE_EVLOOP_H__ */