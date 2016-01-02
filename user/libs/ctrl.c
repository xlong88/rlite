#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <assert.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include "rlite/kernel-msg.h"
#include "rlite/conf-msg.h"
#include "rlite/utils.h"
#include "rlite/rlite.h"

#include "ctrl-utils.h"


static void
rl_ipcps_purge(struct list_head *ipcps)
{
    struct rlite_ipcp *rlite_ipcp, *tmp;

    /* Purge the IPCPs list. */

    list_for_each_entry_safe(rlite_ipcp, tmp, ipcps, node) {
        if (rlite_ipcp->dif_type) {
            free(rlite_ipcp->dif_type);
        }
        rina_name_free(&rlite_ipcp->ipcp_name);
        free(rlite_ipcp->dif_name);
        list_del(&rlite_ipcp->node);
        free(rlite_ipcp);
    }
}

int
rl_ctrl_ipcp_update(struct rlite_ctrl *ctrl,
                    const struct rl_kmsg_ipcp_update *upd)
{
    struct rlite_ipcp *rlite_ipcp = NULL;
    struct rlite_ipcp *cur;

    NPD("UPDATE IPCP update_type=%d, id=%u, addr=%lu, depth=%u, dif_name=%s "
       "dif_type=%s\n",
        upd->update_type, upd->ipcp_id, upd->ipcp_addr, upd->depth,
        upd->dif_name, upd->dif_type);

    pthread_mutex_lock(&ctrl->lock);

    list_for_each_entry(cur, &ctrl->ipcps, node) {
        if (cur->ipcp_id == upd->ipcp_id) {
            rlite_ipcp = cur;
            break;
        }
    }

    switch (upd->update_type) {
        case RLITE_UPDATE_ADD:
            if (rlite_ipcp) {
                PE("UPDATE IPCP [ADD]: ipcp %u already exists\n", upd->ipcp_id);
                goto out;
            }
            break;

        case RLITE_UPDATE_UPD:
        case RLITE_UPDATE_DEL:
            if (!rlite_ipcp) {
                PE("UPDATE IPCP [UPD/DEL]: ipcp %u does not exists\n", upd->ipcp_id);
                goto out;
            }
            break;

        default:
            PE("Invalid update type %u\n", upd->update_type);
            goto out;
    }

    if (upd->update_type == RLITE_UPDATE_UPD ||
            upd->update_type == RLITE_UPDATE_DEL) {
        /* Free the entry. */
        if (rlite_ipcp->dif_type) {
            free(rlite_ipcp->dif_type);
        }
        rina_name_free(&rlite_ipcp->ipcp_name);
        free(rlite_ipcp->dif_name);
        list_del(&rlite_ipcp->node);
        free(rlite_ipcp);
    }

    if (upd->update_type == RLITE_UPDATE_ADD ||
            upd->update_type == RLITE_UPDATE_UPD) {
        /* Create a new entry. */
        rlite_ipcp = malloc(sizeof(*rlite_ipcp));
        if (!rlite_ipcp) {
            PE("Out of memory\n");
            goto out;
        }

        rlite_ipcp->ipcp_id = upd->ipcp_id;
        rlite_ipcp->dif_type = strdup(upd->dif_type);
        rlite_ipcp->ipcp_addr = upd->ipcp_addr;
        rlite_ipcp->depth = upd->depth;
        rina_name_copy(&rlite_ipcp->ipcp_name, &upd->ipcp_name);
        rlite_ipcp->dif_name = strdup(upd->dif_name);

        list_add_tail(&rlite_ipcp->node, &ctrl->ipcps);
    }

out:
    pthread_mutex_unlock(&ctrl->lock);

    return 0;
}
uint32_t
rl_ctrl_get_id(struct rlite_ctrl *ctrl)
{
    uint32_t ret;

    pthread_mutex_lock(&ctrl->lock);
    if (++ctrl->event_id_counter == (1 << 30)) {
        ctrl->event_id_counter = 1;
    }
    ret = ctrl->event_id_counter;
    pthread_mutex_unlock(&ctrl->lock);

    return ret;
}

int
rl_ctrl_ipcps_print(struct rlite_ctrl *ctrl)
{
    struct rlite_ipcp *rlite_ipcp;

    pthread_mutex_lock(&ctrl->lock);

    PI_S("IPC Processes table:\n");
    list_for_each_entry(rlite_ipcp, &ctrl->ipcps, node) {
            char *ipcp_name_s = NULL;

            ipcp_name_s = rina_name_to_string(&rlite_ipcp->ipcp_name);
            PI_S("    id = %d, name = '%s', dif_type ='%s', dif_name = '%s',"
                    " address = %llu, depth = %u\n",
                        rlite_ipcp->ipcp_id, ipcp_name_s, rlite_ipcp->dif_type,
                        rlite_ipcp->dif_name,
                        (long long unsigned int)rlite_ipcp->ipcp_addr,
                        rlite_ipcp->depth);

            if (ipcp_name_s) {
                    free(ipcp_name_s);
            }
    }

    pthread_mutex_unlock(&ctrl->lock);

    return 0;
}

struct rlite_msg_base *
read_next_msg(int rfd)
{
    unsigned int max_resp_size = rlite_numtables_max_size(
                rlite_ker_numtables,
                sizeof(rlite_ker_numtables)/sizeof(struct rlite_msg_layout));
    struct rlite_msg_base *resp;
    char serbuf[4096];
    int ret;

    ret = read(rfd, serbuf, sizeof(serbuf));
    if (ret < 0) {
        perror("read(rfd)");
        return NULL;
    }

    /* Here we can malloc the maximum kernel message size. */
    resp = RLITE_MB(malloc(max_resp_size));
    if (!resp) {
        PE("Out of memory\n");
        return NULL;
    }

    /* Deserialize the message from serbuf into resp. */
    ret = deserialize_rlite_msg(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                                serbuf, ret, (void *)resp, max_resp_size);
    if (ret) {
        PE("Problems during deserialization [%d]\n", ret);
        free(resp);
        return NULL;
    }

    return resp;
}

int
rl_write_msg(int rfd, struct rlite_msg_base *msg)
{
    char serbuf[4096];
    unsigned int serlen;
    int ret;

    /* Serialize the message. */
    serlen = rlite_msg_serlen(rlite_ker_numtables, RLITE_KER_MSG_MAX, msg);
    if (serlen > sizeof(serbuf)) {
        PE("Serialized message would be too long [%u]\n",
                    serlen);
        return -1;
    }
    serlen = serialize_rlite_msg(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                                 serbuf, msg);

    ret = write(rfd, serbuf, serlen);
    if (ret < 0) {
        perror("write(rfd)");

    } else if (ret != serlen) {
        /* This should never happen if kernel code is correct. */
        PE("Error: partial write [%d/%u]\n",
                ret, serlen);
        ret = -1;

    } else {
        ret = 0;
    }

    return ret;
}

struct rlite_ipcp *
rl_ctrl_select_ipcp_by_dif(struct rlite_ctrl *ctrl,
                           const char *dif_name)
{
    struct rlite_ipcp *cur;

    pthread_mutex_lock(&ctrl->lock);

    if (dif_name) {
        /* The request specifies a DIF: lookup that. */
        list_for_each_entry(cur, &ctrl->ipcps, node) {
            if (strcmp(cur->dif_name, dif_name) == 0) {
                pthread_mutex_unlock(&ctrl->lock);
                return cur;
            }
        }

    } else {
        struct rlite_ipcp *rlite_ipcp = NULL;

        /* The request does not specify a DIF: select any DIF,
         * giving priority to normal DIFs. */
        list_for_each_entry(cur, &ctrl->ipcps, node) {
            if ((strcmp(cur->dif_type, "normal") == 0 ||
                        !rlite_ipcp)) {
                rlite_ipcp = cur;
            }
        }

        pthread_mutex_unlock(&ctrl->lock);

        return rlite_ipcp;
    }

    pthread_mutex_unlock(&ctrl->lock);

    return NULL;
}

struct rlite_ipcp *
rl_ctrl_lookup_ipcp_by_name(struct rlite_ctrl *ctrl,
                            const struct rina_name *name)
{
    struct rlite_ipcp *ipcp;

    pthread_mutex_lock(&ctrl->lock);

    if (rina_name_valid(name)) {
        list_for_each_entry(ipcp, &ctrl->ipcps, node) {
            if (rina_name_valid(&ipcp->ipcp_name)
                    && rina_name_cmp(&ipcp->ipcp_name, name) == 0) {
                pthread_mutex_unlock(&ctrl->lock);
                return ipcp;
            }
        }
    }

    pthread_mutex_unlock(&ctrl->lock);

    return NULL;
}

int
rl_ctrl_lookup_ipcp_addr_by_id(struct rlite_ctrl *ctrl, unsigned int id,
                               uint64_t *addr)
{
    struct rlite_ipcp *ipcp;

    pthread_mutex_lock(&ctrl->lock);

    list_for_each_entry(ipcp, &ctrl->ipcps, node) {
        if (ipcp->ipcp_id == id) {
            *addr = ipcp->ipcp_addr;
            pthread_mutex_unlock(&ctrl->lock);
            return 0;
        }
    }

    pthread_mutex_unlock(&ctrl->lock);

    return -1;
}

struct rlite_ipcp *
rl_ctrl_lookup_ipcp_by_id(struct rlite_ctrl *ctrl, unsigned int id)
{
    struct rlite_ipcp *ipcp;

    pthread_mutex_lock(&ctrl->lock);

    list_for_each_entry(ipcp, &ctrl->ipcps, node) {
        if (rina_name_valid(&ipcp->ipcp_name) && ipcp->ipcp_id == id) {
            pthread_mutex_unlock(&ctrl->lock);
            return ipcp;
        }
    }

    pthread_mutex_unlock(&ctrl->lock);

    return NULL;
}

void
rl_flow_spec_default(struct rlite_flow_spec *spec)
{
    memset(spec, 0, sizeof(*spec));
    strncpy(spec->cubename, "unrel", sizeof(spec->cubename));
}

/* This is used by uipcp, not by applications. */
void
rl_flow_cfg_default(struct rlite_flow_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->partial_delivery = 0;
    cfg->incomplete_delivery = 0;
    cfg->in_order_delivery = 0;
    cfg->max_sdu_gap = (uint64_t)-1;
    cfg->dtcp_present = 0;
    cfg->dtcp.fc.fc_type = RLITE_FC_T_NONE;
}

static int
open_port_common(uint32_t port_id, unsigned int mode, uint32_t ipcp_id)
{
    struct rlite_ioctl_info info;
    int fd;
    int ret;

    fd = open("/dev/rlite-io", O_RDWR);
    if (fd < 0) {
        perror("open(/dev/rlite-io)");
        return -1;
    }

    info.port_id = port_id;
    info.ipcp_id = ipcp_id;
    info.mode = mode;

    ret = ioctl(fd, 73, &info);
    if (ret) {
        perror("ioctl(/dev/rlite-io)");
        return -1;
    }

    return fd;
}

int
rl_open_appl_port(uint32_t port_id)
{
    return open_port_common(port_id, RLITE_IO_MODE_APPL_BIND, 0);
}

int rl_open_mgmt_port(uint16_t ipcp_id)
{
    /* The port_id argument is not valid in this call, it will not
     * be considered by the kernel. */
    return open_port_common(~0U, RLITE_IO_MODE_IPCP_MGMT, ipcp_id);
}

struct pending_entry *
pending_queue_remove_by_event_id(struct list_head *list, uint32_t event_id)
{
    struct pending_entry *cur;
    struct pending_entry *found = NULL;

    list_for_each_entry(cur, list, node) {
        if (cur->msg->event_id == event_id) {
            found = cur;
            break;
        }
    }

    if (found) {
        list_del(&found->node);
    }

    return found;
}

struct pending_entry *
pending_queue_remove_by_msg_type(struct list_head *list, unsigned int msg_type)
{
    struct pending_entry *cur;
    struct pending_entry *found = NULL;

    list_for_each_entry(cur, list, node) {
        if (cur->msg->msg_type == msg_type) {
            found = cur;
            break;
        }
    }

    if (found) {
        list_del(&found->node);
    }

    return found;
}

void
pending_queue_fini(struct list_head *list)
{
    struct pending_entry *e, *tmp;

    list_for_each_entry_safe(e, tmp, list, node) {
        list_del(&e->node);
        if (e->msg) {
            free(e->msg);
        }
        if (e->resp) {
            free(e->resp);
        }
        free(e);
    }
}
int
rl_register_req_fill(struct rl_kmsg_appl_register *req, uint32_t event_id,
                     unsigned int ipcp_id, int reg,
                     const struct rina_name *appl_name)
{
    memset(req, 0, sizeof(*req));
    req->msg_type = RLITE_KER_APPL_REGISTER;
    req->event_id = event_id;
    req->ipcp_id = ipcp_id;
    req->reg = reg;
    rina_name_copy(&req->appl_name, appl_name);

    return 0;
}

int
rl_fa_req_fill(struct rl_kmsg_fa_req *req,
               uint32_t event_id, unsigned int ipcp_id,
               const char *dif_name,
               const struct rina_name *ipcp_name,
               const struct rina_name *local_appl,
               const struct rina_name *remote_appl,
               const struct rlite_flow_spec *flowspec,
               uint16_t upper_ipcp_id)
{
    memset(req, 0, sizeof(*req));
    req->msg_type = RLITE_KER_FA_REQ;
    req->event_id = event_id;
    req->ipcp_id = ipcp_id;
    req->upper_ipcp_id = upper_ipcp_id;
    if (flowspec) {
        memcpy(&req->flowspec, flowspec, sizeof(*flowspec));
    } else {
        rl_flow_spec_default(&req->flowspec);
    }
    rina_name_copy(&req->local_appl, local_appl);
    rina_name_copy(&req->remote_appl, remote_appl);

    return 0;
}

int
rl_fa_resp_fill(struct rl_kmsg_fa_resp *resp, uint32_t kevent_id,
                uint16_t ipcp_id, uint16_t upper_ipcp_id,
                uint32_t port_id, uint8_t response)
{
    memset(resp, 0, sizeof(*resp));

    resp->msg_type = RLITE_KER_FA_RESP;
    resp->event_id = 1;
    resp->kevent_id = kevent_id;
    resp->ipcp_id = ipcp_id;  /* Currently unused by the kernel. */
    resp->upper_ipcp_id = upper_ipcp_id;
    resp->port_id = port_id;
    resp->response = response;

    return 0;
}

int
rl_ipcp_config_fill(struct rl_kmsg_ipcp_config *req, uint16_t ipcp_id,
                    const char *param_name, const char *param_value)
{
    memset(req, 0, sizeof(*req));
    req->msg_type = RLITE_KER_IPCP_CONFIG;
    req->event_id = 1;
    req->ipcp_id = ipcp_id;
    req->name = strdup(param_name);
    req->value = strdup(param_value);

    return 0;
}

static int
rl_ctrl_barrier(struct rlite_ctrl *ctrl)
{
    struct rlite_msg_base req;
    struct rlite_msg_base *resp;
    int ret;

    req.msg_type = RLITE_KER_BARRIER;
    req.event_id = rl_ctrl_get_id(ctrl);

    ret = rl_write_msg(ctrl->rfd, &req);
    if (ret < 0) {
        return -1;
    }

    resp = RLITE_MB(rl_ctrl_wait(ctrl, req.event_id));
    if (!resp) {
        return -1;
    }

    free(resp);

    return 0;
}

int
rl_ctrl_init(struct rlite_ctrl *ctrl, const char *dev)
{
    int ret;

    if (!dev) {
        dev = "/dev/rlite";
    }

    list_init(&ctrl->pqueue);
    list_init(&ctrl->ipcps);
    pthread_mutex_init(&ctrl->lock, NULL);
    ctrl->event_id_counter = 1;

    /* Open the RLITE control device. */
    ctrl->rfd = open(dev, O_RDWR);
    if (ctrl->rfd < 0) {
        PE("Cannot open '%s'\n", dev);
        perror("open(ctrldev)");
        return ctrl->rfd;
    }

    /* Set non-blocking operation for the RLITE control device, so that
     * we can synchronize with the kernel through select(). */
    ret = fcntl(ctrl->rfd, F_SETFL, O_NONBLOCK);
    if (ret) {
        perror("fcntl(O_NONBLOCK)");
        return ret;
    }

    /* Issue a barrier operation, in order to process all
     * pending RLITE_KER_IPCP_UPDATE messages before returing to the caller. */
    ret = rl_ctrl_barrier(ctrl);
    if (ret) {
        PE("rl_ctrl_barrier() failed\n");
        return ret;
    }

    return 0;
}

int
rl_ctrl_fini(struct rlite_ctrl *ctrl)
{
    pthread_mutex_lock(&ctrl->lock);
    pending_queue_fini(&ctrl->pqueue);
    rl_ipcps_purge(&ctrl->ipcps);
    pthread_mutex_unlock(&ctrl->lock);

    if (ctrl->rfd >= 0) {
        close(ctrl->rfd);
    }

    return 0;
}

uint32_t
rl_ctrl_fa_req(struct rlite_ctrl *ctrl, const char *dif_name,
               const struct rina_name *ipcp_name,
               const struct rina_name *local_appl,
               const struct rina_name *remote_appl,
               const struct rlite_flow_spec *flowspec)
{
    struct rl_kmsg_fa_req req;
    struct rlite_ipcp *rlite_ipcp;
    uint32_t event_id;
    int ret;

    rlite_ipcp = rl_ctrl_lookup_ipcp_by_name(ctrl, ipcp_name);
    if (!rlite_ipcp) {
        rlite_ipcp = rl_ctrl_select_ipcp_by_dif(ctrl, dif_name);
    }
    if (!rlite_ipcp) {
        PE("No suitable IPCP found\n");
        return 0;
    }

    event_id = rl_ctrl_get_id(ctrl);

    ret = rl_fa_req_fill(&req, event_id, rlite_ipcp->ipcp_id,
                         dif_name, ipcp_name, local_appl, remote_appl,
                         flowspec, 0xffff);
    if (ret) {
        PE("Failed to fill flow allocation request\n");
        return 0;
    }

    ret = rl_write_msg(ctrl->rfd, RLITE_MB(&req));
    if (ret < 0) {
        PE("Failed to issue request to the kernel\n");
        event_id = 0;
    }

    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(&req));

    return event_id;
}

uint32_t
rl_ctrl_reg_req(struct rlite_ctrl *ctrl, int reg,
                const char *dif_name,
                const struct rina_name *ipcp_name,
                const struct rina_name *appl_name)
{
    struct rl_kmsg_appl_register req;
    struct rlite_ipcp *rlite_ipcp;
    uint32_t event_id;
    int ret;

    rlite_ipcp = rl_ctrl_lookup_ipcp_by_name(ctrl, ipcp_name);
    if (!rlite_ipcp) {
        rlite_ipcp = rl_ctrl_select_ipcp_by_dif(ctrl, dif_name);
    }
    if (!rlite_ipcp) {
        PE("Could not find a suitable IPC process\n");
        return 0;
    }

    event_id = rl_ctrl_get_id(ctrl);

    ret = rl_register_req_fill(&req, event_id, rlite_ipcp->ipcp_id,
                               reg, appl_name);
    if (ret) {
        PE("Failed to fill (un)register request\n");
        return 0;
    }

    ret = rl_write_msg(ctrl->rfd, RLITE_MB(&req));
    if (ret < 0) {
        PE("Failed to issue request to the kernel\n");
        event_id = 0;
    }

    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(&req));

    return event_id;
}

static struct rlite_msg_base *
rl_ctrl_wait_common(struct rlite_ctrl *ctrl, unsigned int msg_type,
                    uint32_t event_id)
{
    struct rlite_msg_base *resp;
    struct pending_entry *entry;
    fd_set rdfs;
    int ret;

    /* Try to match the msg_type or the event_id against a response that has
     * already been read. */
    pthread_mutex_lock(&ctrl->lock);
    if (msg_type) {
        entry = pending_queue_remove_by_msg_type(&ctrl->pqueue, msg_type);
    } else {
        entry = pending_queue_remove_by_event_id(&ctrl->pqueue, event_id);
    }
    pthread_mutex_unlock(&ctrl->lock);

    if (entry) {
        resp = RLITE_MB(entry->msg);
        free(entry);

        return resp;
    }

    for (;;) {
        FD_ZERO(&rdfs);
        FD_SET(ctrl->rfd, &rdfs);

        ret = select(ctrl->rfd + 1, &rdfs, NULL, NULL, NULL);

        if (ret == -1) {
            /* Error. */
            perror("select()");
            break;

        } else if (ret == 0) {
            /* Timeout */
            PE("Unexpected timeout\n");
            break;
        }

        /* Read the next message posted by the kernel. */
        resp = read_next_msg(ctrl->rfd);
        if (!resp) {
            continue;
        }

        if (msg_type && resp->msg_type == msg_type) {
            /* We found the requested match against msg_type. */
            return resp;
        }

        if (resp->event_id == event_id) {
            /* We found the requested match against event_id. */
            return resp;
        }

        /* Filter out and process certain types of message. */
        switch (resp->msg_type) {
            case RLITE_KER_IPCP_UPDATE:
                rl_ctrl_ipcp_update(ctrl, (struct rl_kmsg_ipcp_update *)resp);
                rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                               RLITE_MB(resp));
                free(resp);
                continue;

            default:
                break;
        }

        /* Store the message for subsequent use. */
        entry = malloc(sizeof(*entry));
        if (!entry) {
            PE("Out of memory\n");
            free(resp);

            return NULL;
        }
        memset(entry, 0, sizeof(*entry));
        entry->msg = RLITE_MB(resp);
        pthread_mutex_lock(&ctrl->lock);
        list_add_tail(&entry->node, &ctrl->pqueue);
        pthread_mutex_unlock(&ctrl->lock);
    }

    return NULL;
}

struct rlite_msg_base *
rl_ctrl_wait(struct rlite_ctrl *ctrl, uint32_t event_id)
{
    return rl_ctrl_wait_common(ctrl, 0, event_id);
}

struct rlite_msg_base *
rl_ctrl_wait_any(struct rlite_ctrl *ctrl, unsigned int msg_type)
{
    return rl_ctrl_wait_common(ctrl, msg_type, 0);
}

int
rl_ctrl_flow_alloc(struct rlite_ctrl *ctrl, const char *dif_name,
                   const struct rina_name *ipcp_name,
                   const struct rina_name *local_appl,
                   const struct rina_name *remote_appl,
                   const struct rlite_flow_spec *flowspec)
{
    struct rl_kmsg_fa_resp_arrived *resp;
    uint32_t event_id;
    int fd;

    event_id = rl_ctrl_fa_req(ctrl, dif_name, ipcp_name, local_appl,
                              remote_appl, flowspec);

    if (!event_id) {
        return -1;
    }

    resp = (struct rl_kmsg_fa_resp_arrived *)rl_ctrl_wait(ctrl, event_id);
    if (!resp) {
        return -1;
    }


    if (resp->response) {
        PE("Flow allocation request denied\n");
        fd = -1;
    } else {
        fd = rl_open_appl_port(resp->port_id);
    }

    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(resp));
    free(resp);

    return fd;
}

int
rl_ctrl_register(struct rlite_ctrl *ctrl, int reg,
                 const char *dif_name,
                 const struct rina_name *ipcp_name,
                 const struct rina_name *appl_name)
{
    struct rl_kmsg_appl_register_resp *resp;
    uint32_t event_id;
    int ret;

    event_id = rl_ctrl_reg_req(ctrl, reg, dif_name, ipcp_name, appl_name);

    if (!event_id) {
        return -1;
    }

    resp = (struct rl_kmsg_appl_register_resp *)rl_ctrl_wait(ctrl, event_id);
    if (!resp) {
        return -1;
    }

    if (resp->response) {
        PE("Registration request denied\n");
        ret = -1;
    } else {
        ret = 0;
    }

    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(resp));
    free(resp);

    return ret;
}

int
rl_ctrl_flow_accept(struct rlite_ctrl *ctrl)
{
    struct rl_kmsg_fa_req_arrived *req;
    struct rl_kmsg_fa_resp resp;
    int ret;

    req = (struct rl_kmsg_fa_req_arrived *)
          rl_ctrl_wait_any(ctrl, RLITE_KER_FA_REQ_ARRIVED);

    if (!req) {
        return -1;
    }

    ret = rl_fa_resp_fill(&resp, req->kevent_id, req->ipcp_id, 0xffff,
                          req->port_id, RLITE_SUCC);
    if (ret) {
        PE("Failed to fill flow allocation response\n");
        goto out;
    }

    ret = rl_write_msg(ctrl->rfd, RLITE_MB(&resp));
    if (ret < 0) {
        PE("Failed to issue request to the kernel\n");
        goto out;
    }

    ret = rl_open_appl_port(req->port_id);

out:
    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(&resp));
    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(req));
    free(req);

    return ret;
}