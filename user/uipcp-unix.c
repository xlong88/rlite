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
#include <endian.h>
#include "rinalite/rina-kernel-msg.h"
#include "rinalite/rina-conf-msg.h"
#include "rinalite/rinalite-utils.h"

#include "rinalite_list.h"
#include "helpers.h"
#include "rinalite_evloop.h"
#include "rinalite_appl.h"
#include "uipcp-container.h"


struct registered_ipcp {
    struct rina_name dif_name;
    unsigned int ipcp_id;
    struct rina_name ipcp_name;

    struct list_head node;
};

/* Global variable containing the main struct of the uipcps. This variable
 * should be accessed directly only by signal handlers (because I don't know
 * how to do it differently). The rest of the program should access it
 * through pointers.
 */
static struct uipcps guipcps;

static int
rina_conf_response(int sfd, struct rina_msg_base *req,
                   struct rina_msg_base_resp *resp)
{
    resp->msg_type = RINA_CONF_BASE_RESP;
    resp->event_id = req->event_id;

    return rina_msg_write_fd(sfd, RINALITE_RMB(resp));
}

static void
track_ipcp_registration(struct uipcps *uipcps, int reg,
                        const struct rina_name *dif_name,
                        unsigned int ipcp_id,
                        const struct rina_name *ipcp_name)
{
    struct registered_ipcp *ripcp;

    if (reg) {
        int found = 0;

        /* Append a successful registration to the persistent
         * registration list. */
        list_for_each_entry(ripcp, &uipcps->ipcps_registrations, node) {
            if (rina_name_cmp(&ripcp->dif_name, dif_name) == 0 &&
                    rina_name_cmp(&ripcp->ipcp_name, ipcp_name) == 0) {
                found = 1;
                break;
            }
        }

        if (!found) {
            ripcp = malloc(sizeof(*ripcp));
            if (!ripcp) {
                PE("%s: ripcp allocation failed\n", __func__);
            } else {
                memset(ripcp, 0, sizeof(*ripcp));
                rina_name_copy(&ripcp->dif_name, dif_name);
                ripcp->ipcp_id = ipcp_id;
                rina_name_copy(&ripcp->ipcp_name, ipcp_name);
                list_add_tail(&ripcp->node, &uipcps->ipcps_registrations);
            }
        }
    } else {
        /* Try to remove a registration element from the persistent
         * registration list. If 'dif_name' and 'ipcp_name' are specified,
         * match the corresponding tuple fields. Otherwise match the
         * by IPCP id. */
        list_for_each_entry(ripcp, &uipcps->ipcps_registrations, node) {
            if ((dif_name && ipcp_name &&
                    rina_name_cmp(&ripcp->dif_name, dif_name) == 0 &&
                    rina_name_cmp(&ripcp->ipcp_name, ipcp_name) == 0) ||
                    (!dif_name && !ipcp_name && ripcp->ipcp_id == ipcp_id)) {
                list_del(&ripcp->node);
                free(ripcp);
                break;
            }
        }
    }
}

static uint8_t
rina_ipcp_register(struct uipcps *uipcps, int reg,
                   const struct rina_name *dif_name,
                   unsigned int ipcp_id,
                   const struct rina_name *ipcp_name)
{
    struct uipcp *uipcp;
    uint8_t result;

    /* Grab the corresponding userspace IPCP. */
    uipcp = uipcp_lookup(uipcps, ipcp_id);
    if (!uipcp) {
        PE("%s: No such uipcp [%u]\n", __func__, ipcp_id);
        return -1;
    }

    /* Perform a fetch to find out if shim IPCPs have been created,
     * deleted or configured from the last fetch. */
    rinalite_ipcps_fetch(&uipcp->appl.loop);

    /* Perform the registration. */
    result = rinalite_appl_register(&uipcp->appl, reg, dif_name,
                                  0, NULL, ipcp_name);

    if (result == 0) {
        /* Track the (un)registration in the persistent registration list. */
        track_ipcp_registration(uipcps, reg, dif_name, ipcp_id, ipcp_name);
    }

    return result;
}

static int
rina_conf_ipcp_register(struct uipcps *uipcps, int sfd,
                       const struct rina_msg_base *b_req)
{
    struct rina_cmsg_ipcp_register *req = (struct rina_cmsg_ipcp_register *)b_req;
    struct rina_msg_base_resp resp;

    resp.result = rina_ipcp_register(uipcps, req->reg, &req->dif_name,
                                     req->ipcp_id, &req->ipcp_name);

    return rina_conf_response(sfd, RINALITE_RMB(req), &resp);
}

static int
rina_conf_ipcp_enroll(struct uipcps *uipcps, int sfd,
                      const struct rina_msg_base *b_req)
{
    struct rina_cmsg_ipcp_enroll *req = (struct rina_cmsg_ipcp_enroll *)b_req;
    struct rina_msg_base_resp resp;
    struct uipcp *uipcp;
    int ret;

    resp.result = 1; /* Report failure by default. */

    /* Find the userspace part of the enrolling IPCP. */
    uipcp = uipcp_lookup(uipcps, req->ipcp_id);
    if (!uipcp) {
        PE("%s: Could not find userspace IPC process %u\n",
            __func__, req->ipcp_id);
        goto out;
    }

    /* Perform a fetch to find out if shim IPCPs have been created,
     * deleted or configured from the last fetch. */
    rinalite_ipcps_fetch(&uipcp->appl.loop);

    /* Perform enrollment in userspace. */
    ret = uipcp_enroll(uipcp, req);
    if (ret) {
        goto out;
    }

    resp.result = 0;

out:
    return rina_conf_response(sfd, RINALITE_RMB(req), &resp);
}

static int
rina_conf_ipcp_dft_set(struct uipcps *uipcps, int sfd,
                       const struct rina_msg_base *b_req)
{
    struct rina_cmsg_ipcp_dft_set *req = (struct rina_cmsg_ipcp_dft_set *)b_req;
    struct rina_msg_base_resp resp;
    struct uipcp *uipcp;
    int ret;

    resp.result = 1; /* Report failure by default. */

    uipcp = uipcp_lookup(uipcps, req->ipcp_id);
    if (!uipcp) {
        PE("%s: Could not find uipcp for IPC process %u\n",
            __func__, req->ipcp_id);
        goto out;
    }

    ret = uipcp_dft_set(uipcp, &req->appl_name, req->remote_addr);
    if (ret) {
        goto out;
    }

    resp.result = 0;

out:
    return rina_conf_response(sfd, RINALITE_RMB(req), &resp);
}

static int
rina_conf_uipcp_update(struct uipcps *uipcps, int sfd,
                       const struct rina_msg_base *b_req)
{
    struct rina_cmsg_uipcp_update *req = (struct rina_cmsg_uipcp_update *)b_req;
    struct rina_msg_base_resp resp;
    struct uipcp *uipcp;

    resp.result = 1; /* Report failure by default. */

    if (req->msg_type == RINA_CONF_UIPCP_CREATE) {
        resp.result = uipcp_add(uipcps, req->ipcp_id);

    } else if (req->msg_type == RINA_CONF_UIPCP_DESTROY) {
        /* Track all the unregistrations of the destroyed IPCP in
         * the persistent registrations list. */
        track_ipcp_registration(uipcps, 0, NULL, req->ipcp_id, NULL);

        uipcp = uipcp_lookup(uipcps, req->ipcp_id);
        if (!uipcp) {
            PE("%s: Could not find uipcp for IPC process %u\n",
                __func__, req->ipcp_id);
            goto out;
        }

        resp.result = uipcp_del(uipcps, req->ipcp_id);
    }

    uipcps_fetch(uipcps);

    resp.result = 0;

out:
    return rina_conf_response(sfd, RINALITE_RMB(req), &resp);
}

typedef int (*rina_req_handler_t)(struct uipcps *uipcps, int sfd,
                                   const struct rina_msg_base * b_req);

/* The table containing all application request handlers. */
static rina_req_handler_t rina_config_handlers[] = {
    [RINA_CONF_IPCP_REGISTER] = rina_conf_ipcp_register,
    [RINA_CONF_IPCP_ENROLL] = rina_conf_ipcp_enroll,
    [RINA_CONF_IPCP_DFT_SET] = rina_conf_ipcp_dft_set,
    [RINA_CONF_UIPCP_CREATE] = rina_conf_uipcp_update,
    [RINA_CONF_UIPCP_DESTROY] = rina_conf_uipcp_update,
    [RINA_CONF_UIPCP_UPDATE] = rina_conf_uipcp_update,
    [RINA_CONF_MSG_MAX] = NULL,
};

/* Unix server thread to manage configuration requests. */
static void *
unix_server(void *arg)
{
    struct uipcps *uipcps = arg;
    char serbuf[4096];
    char msgbuf[4096];

    for (;;) {
        struct sockaddr_un client_address;
        socklen_t client_address_len = sizeof(client_address);
        struct rina_msg_base *req;
        int cfd;
        int ret;
        int n;

        /* Accept a new client. */
        cfd = accept(uipcps->lfd, (struct sockaddr *)&client_address,
                     &client_address_len);

        /* Read the request message in serialized form. */
        n = read(cfd, serbuf, sizeof(serbuf));
        if (n < 0) {
            PE("%s: read() error [%d]\n", __func__, n);
        }

        /* Deserialize into a formatted message. */
        ret = deserialize_rina_msg(rina_conf_numtables, serbuf, n,
                                        msgbuf, sizeof(msgbuf));
        if (ret) {
            PE("%s: deserialization error [%d]\n", __func__, ret);
        }

        /* Lookup the message type. */
        req = RINALITE_RMB(msgbuf);
        if (rina_config_handlers[req->msg_type] == NULL) {
            struct rina_msg_base_resp resp;

            PE("%s: Invalid message received [type=%d]\n", __func__,
                    req->msg_type);
            resp.msg_type = RINA_CONF_BASE_RESP;
            resp.event_id = req->event_id;
            resp.result = 1;
            rina_msg_write_fd(cfd, (struct rina_msg_base *)&resp);
        } else {
            /* Valid message type: handle the request. */
            ret = rina_config_handlers[req->msg_type](uipcps, cfd, req);
            if (ret) {
                PE("%s: Error while handling message type [%d]\n",
                        __func__, req->msg_type);
            }
        }

        /* Close the connection. */
	close(cfd);

        fflush(stdout);
    }

    return NULL;
}

/* Dump the ipcps_registrations list to a file, so that
 * subsequent uipcps invocations can redo the registrations. */
static void
persistent_ipcp_reg_dump(struct uipcps *uipcps)
{
    FILE *fpreg = fopen(RINA_PERSISTENT_REG_FILE, "w");

    if (!fpreg) {
        PE("%s: Cannot open persistent register file (%s)\n",
                __func__, RINA_PERSISTENT_REG_FILE);
    } else {
        char *dif_s, *ipcp_s;
        struct registered_ipcp *ripcp;

        list_for_each_entry(ripcp, &uipcps->ipcps_registrations, node) {
            dif_s = rina_name_to_string(&ripcp->dif_name);
            ipcp_s = rina_name_to_string(&ripcp->ipcp_name);
            if (dif_s && ipcp_s) {
                fprintf(fpreg, "%s %u %s\n", dif_s, ripcp->ipcp_id, ipcp_s);
            } else {
                PE("%s: Error in rina_name_to_string()\n", __func__);
            }
            if (dif_s) free(dif_s);
            if (ipcp_s) free(ipcp_s);
        }
        fclose(fpreg);
    }
}

static int
uipcps_update(struct uipcps *uipcps)
{
    struct rinalite_evloop loop;
    struct rinalite_ipcp *rinalite_ipcp;
    int ret = 0;

    ret = rinalite_evloop_init(&loop, "/dev/rina-ctrl", NULL);
    if (ret) {
        return ret;
    }

    rinalite_ipcps_fetch(&loop);

    rinalite_ipcps_print(&loop);
    fflush(stdout);

    /* Create an userspace IPCP for each existing IPCP. */
    list_for_each_entry(rinalite_ipcp, &loop.ipcps, node) {
        if (rinalite_ipcp->dif_type == DIF_TYPE_NORMAL) {
            ret = uipcp_add(uipcps, rinalite_ipcp->ipcp_id);
            if (ret) {
                return ret;
            }
        }
    }

    rinalite_evloop_stop(&loop);
    rinalite_evloop_fini(&loop);

    /* Perform a fetch operation on the evloops of
     * all the userspace IPCPs. */
    uipcps_fetch(uipcps);

    if (1) {
        /* Read the persistent IPCP registration file into
         * the ipcps_registrations list. */
        FILE *fpreg = fopen(RINA_PERSISTENT_REG_FILE, "r");
        char line[4096];

        if (fpreg) {
            while (fgets(line, sizeof(line), fpreg)) {
                char *s1 = NULL;
                char *s2 = NULL;
                char *s3 = NULL;
                struct rina_name dif_name;
                struct rina_name ipcp_name;
                unsigned int ipcp_id;
                uint8_t reg_result;

                s1 = strchr(line, '\n');
                if (s1) {
                    *s1 = '\0';
                }

                s1 = strtok(line, " ");
                s2 = strtok(0, " ");
                s3 = strtok(0, " ");

                if (s1 && s2 && s3 && rina_name_from_string(s1, &dif_name) == 0
                        && rina_name_from_string(s3, &ipcp_name) == 0) {
                    ipcp_id = atoi(s2);
                    reg_result = rina_ipcp_register(uipcps, 1, &dif_name,
                                                    ipcp_id, &ipcp_name);
                    PI("%s: Automatic re-registration for %s --> %s\n",
                        __func__, s3, (reg_result == 0) ? "DONE" : "FAILED");
                }
            }

            fclose(fpreg);
        }
    }

    return 0;
}

static void
sigint_handler(int signum)
{
    struct uipcps *uipcps = &guipcps;

    persistent_ipcp_reg_dump(uipcps);

    /* TODO Here we should free all the dynamically allocated memory
     * referenced by uipcps. */

    unlink(RINA_UIPCPS_UNIX_NAME);
    exit(EXIT_SUCCESS);
}

static void
sigpipe_handler(int signum)
{
    PI("SIGPIPE received\n");
}

int main(int argc, char **argv)
{
    struct uipcps *uipcps = &guipcps;
    pthread_t unix_th;
    struct sockaddr_un server_address;
    struct sigaction sa;
    int ret;

    /* Open a Unix domain socket to listen to. */
    uipcps->lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (uipcps->lfd < 0) {
        perror("socket(AF_UNIX)");
        exit(EXIT_FAILURE);
    }
    memset(&server_address, 0, sizeof(server_address));
    server_address.sun_family = AF_UNIX;
    strncpy(server_address.sun_path, RINA_UIPCPS_UNIX_NAME,
            sizeof(server_address.sun_path) - 1);
    if (unlink(RINA_UIPCPS_UNIX_NAME) == 0) {
        /* This should not happen if everything behaves correctly.
         * However, if something goes wrong, the Unix domain socket
         * could still exist and so the following bind() would fail.
         * This unlink() will clean up in this situation. */
        PI("info: cleaned up existing unix domain socket\n");
    }
    ret = bind(uipcps->lfd, (struct sockaddr *)&server_address,
                sizeof(server_address));
    if (ret) {
        perror("bind(AF_UNIX, path)");
        exit(EXIT_FAILURE);
    }
    ret = listen(uipcps->lfd, 50);
    if (ret) {
        perror("listen(AF_UNIX)");
        exit(EXIT_FAILURE);
    }

    list_init(&uipcps->uipcps);
    list_init(&uipcps->ipcps_registrations);

    /* Set an handler for SIGINT and SIGTERM so that we can remove
     * the Unix domain socket used to access the uipcp server. */
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ret = sigaction(SIGINT, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }
    ret = sigaction(SIGTERM, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGTERM)");
        exit(EXIT_FAILURE);
    }

    /* Handle the SIGPIPE signal, which is received when
     * trying to read/write from/to a Unix domain socket
     * that has been closed by the other end. */
    sa.sa_handler = sigpipe_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ret = sigaction(SIGPIPE, &sa, NULL);
    if (ret) {
        perror("sigaction(SIGPIPE)");
        exit(EXIT_FAILURE);
    }

    /* Fetch kernel state and create userspace IPCPs as needed. This
     * must be done before launching the unix server in order to
     * avoid race conditions between main thread fetching and unix
     * server thread serving a client. That is, a client could see
     * incomplete state and its operation may fail or behave
     * unexpectedly.*/
    ret = uipcps_update(uipcps);
    if (ret) {
        PE("Failed to load userspace ipcps\n");
        exit(EXIT_FAILURE);
    }

    /* Create and start the unix server thread. */
    ret = pthread_create(&unix_th, NULL, unix_server, uipcps);
    if (ret) {
        perror("pthread_create(unix)");
        exit(EXIT_FAILURE);
    }

    /* The following code should be never reached, since the unix
     * socket server should execute until a SIGINT signal comes. */
    ret = pthread_join(unix_th, NULL);
    if (ret < 0) {
        perror("pthread_join(unix)");
        exit(EXIT_FAILURE);
    }

    return 0;
}