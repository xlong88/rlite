#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include "rlite/conf.h"


int
rlite_ipcp_config(struct rlite_evloop *loop, uint16_t ipcp_id,
                 const char *param_name, const char *param_value)
{
    struct rl_kmsg_ipcp_config *req;
    struct rlite_msg_base *resp;
    int result;

    /* Allocate and create a request message. */
    req = malloc(sizeof(*req));
    if (!req) {
        PE("Out of memory\n");
        return ENOMEM;
    }

    memset(req, 0, sizeof(*req));
    req->msg_type = RLITE_KER_IPCP_CONFIG;
    req->event_id = 1;
    req->ipcp_id = ipcp_id;
    req->name = strdup(param_name);
    req->value = strdup(param_value);

    PD("Requesting IPCP config...\n");

    resp = rlite_issue_request(loop, RLITE_MB(req), sizeof(*req),
                         0, 0, &result);
    assert(!resp);
    PD("result: %d\n", result);

    return result;
}
