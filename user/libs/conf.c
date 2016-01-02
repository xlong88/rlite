#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include "rlite/conf.h"
#include "ctrl-utils.h"


/* Create an IPC process. */
long int
rl_conf_ipcp_create(struct rlite_ctrl *ctrl,
                    const struct rina_name *name, const char *dif_type,
                    const char *dif_name)
{
    struct rl_kmsg_ipcp_create msg;
    struct rl_kmsg_ipcp_create_resp *resp;
    long int ret;

    memset(&msg, 0, sizeof(msg));
    msg.msg_type = RLITE_KER_IPCP_CREATE;
    msg.event_id = rl_ctrl_get_id(ctrl);
    rina_name_copy(&msg.name, name);
    msg.dif_type = strdup(dif_type);
    msg.dif_name = strdup(dif_name);

    ret = rl_write_msg(ctrl->rfd, RLITE_MB(&msg));
    if (ret < 0) {
        PE("Failed to issue request to the kernel\n");
        rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                       RLITE_MB(&msg));
        return -1;
    }

    resp = (struct rl_kmsg_ipcp_create_resp *)rl_ctrl_wait(ctrl, msg.event_id);
    if (!resp) {
        return -1;
    }

    ret = resp->ipcp_id;

    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(&msg));
    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(resp));
    free(resp);

    return ret;
}

/* Destroy an IPC process. */
int
rl_conf_ipcp_destroy(struct rlite_ctrl *ctrl, unsigned int ipcp_id,
                     const char *dif_type)
{
    struct rl_kmsg_ipcp_destroy msg;
    int ret;

    memset(&msg, 0, sizeof(msg));
    msg.msg_type = RLITE_KER_IPCP_DESTROY;
    msg.event_id = 1;
    msg.ipcp_id = ipcp_id;

    ret = rl_write_msg(ctrl->rfd, RLITE_MB(&msg));
    if (ret < 0) {
        PE("Failed to issue request to the kernel\n");
    }

    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(&msg));

    return ret;
}

/* Configure an IPC process. */
int
rl_conf_ipcp_config(struct rlite_ctrl *ctrl, unsigned int ipcp_id,
                    const char *param_name, const char *param_value)
{
    struct rl_kmsg_ipcp_config msg;
    int ret;

    rl_ipcp_config_fill(&msg, ipcp_id, param_name, param_value);

    ret = rl_write_msg(ctrl->rfd, RLITE_MB(&msg));
    if (ret < 0) {
        PE("Failed to issue request to the kernel\n");
    }

    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(&msg));

    return ret;
}

static int
flow_fetch_resp(struct list_head *flows,
                const struct rl_kmsg_flow_fetch_resp *resp)
{
    struct rlite_flow *rlite_flow;

    if (resp->end) {
        /* This response is just to say there are no
         * more flows. */

        return 0;
    }

    rlite_flow = malloc(sizeof(*rlite_flow));
    if (!rlite_flow) {
        PE("Out of memory\n");
        return 0;
    }

    rlite_flow->ipcp_id = resp->ipcp_id;
    rlite_flow->local_port = resp->local_port;
    rlite_flow->remote_port = resp->remote_port;
    rlite_flow->local_addr = resp->local_addr;
    rlite_flow->remote_addr = resp->remote_addr;

    list_add_tail(&rlite_flow->node, flows);

    return 0;
}

int
rl_conf_flows_fetch(struct rlite_ctrl *ctrl, struct list_head *flows)
{
    struct rl_kmsg_flow_fetch_resp *resp;
    struct rlite_msg_base msg;
    int end = 0;

    memset(&msg, 0, sizeof(msg));
    msg.msg_type = RLITE_KER_FLOW_FETCH;

    /* Fill the flows list. */

    while (!end) {
        /* Fetch information about a single IPC process. */
        int ret;

        msg.event_id = rl_ctrl_get_id(ctrl);

        ret = rl_write_msg(ctrl->rfd, RLITE_MB(&msg));
        if (ret < 0) {
            PE("Failed to issue request to the kernel\n");
        }

        resp = (struct rl_kmsg_flow_fetch_resp *)
               rl_ctrl_wait(ctrl, msg.event_id);
        if (!resp) {
            end = 1;

        } else {
            /* Consume and free the response. */
            flow_fetch_resp(flows, resp);

            end = resp->end;
            rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                           RLITE_MB(resp));
            free(resp);
        }
    }

    rlite_msg_free(rlite_ker_numtables, RLITE_KER_MSG_MAX,
                   RLITE_MB(&msg));

    return 0;
}

void
rl_conf_flows_purge(struct list_head *flows)
{
    struct rlite_flow *rlite_flow, *tmp;

    /* Purge the flows list. */
    list_for_each_entry_safe(rlite_flow, tmp, flows, node) {
        list_del(&rlite_flow->node);
        free(rlite_flow);
    }
}

int
rl_conf_flows_print(struct list_head *flows)
{
    struct rlite_flow *rlite_flow;

    PI_S("Flows table:\n");
    list_for_each_entry(rlite_flow, flows, node) {
            PI_S("    ipcp_id = %u, local_port = %u, remote_port = %u, "
                    "local_addr = %llu, remote_addr = %llu\n",
                        rlite_flow->ipcp_id, rlite_flow->local_port,
                        rlite_flow->remote_port,
                        (long long unsigned int)rlite_flow->local_addr,
                        (long long unsigned int)rlite_flow->remote_addr);
    }

    return 0;
}
