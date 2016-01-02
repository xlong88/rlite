#ifndef __RINALITE_CDAP_H__
#define __RINALITE_CDAP_H__

#include <string>
#include <set>

#include "rinalite/rinalite-common.h"
#include "CDAP.pb.h"


struct CDAPAuthValue {
    std::string name;
    std::string password;
    std::string other;
};

class CDAPConn {
    std::set<int> pending_invoke_ids;
    int invoke_id_next;
    int max_pending_ops;
    std::set<int> pending_invoke_ids_remote;

    enum {
        NONE = 1,
        AWAITCON,
        CONNECTED,
        AWAITCLOSE,
    } state;

    int __put_invoke_id(std::set<int> &pending, int invoke_id);
    int get_invoke_id();
    int put_invoke_id(int invoke_id);
    int get_invoke_id_remote(int invoke_id);
    int put_invoke_id_remote(int invoke_id);

    int conn_fsm_run(struct CDAPMessage *m, bool sender);

public:
    CDAPConn(int fd);

    /* @invoke_id is not meaningful for request messages. */
    int msg_send(struct CDAPMessage *m, int invoke_id);

    struct CDAPMessage * msg_recv();

    int m_connect_send(int *invoke_id, gpb::authTypes_t auth_mech,
                        const struct CDAPAuthValue *auth_value,
                        const struct rina_name *local_appl,
                        const struct rina_name *remote_appl);

    int m_connect_r_send(const struct CDAPMessage *req, int result,
                         const std::string& result_reason);

    int m_release_send(int *invoke_id, gpb::flagValues_t flags);

    int m_release_r_send(const struct CDAPMessage *req,
                         gpb::flagValues_t flags, int result,
                         const std::string& result_reason);

    int m_create_send(int *invoke_id, gpb::flagValues_t flags,
                      const std::string& obj_class,
                      const std::string& obj_name, long obj_inst,
                      int scope, const std::string& filter);

    int m_create_r_send(const struct CDAPMessage *req,
                        gpb::flagValues_t flags, const std::string& obj_class,
                        const std::string& obj_name, long obj_inst,
                        int result, const std::string& result_reason);

    struct rina_name local_appl;
    struct rina_name remote_appl;
    int fd;
};

/* Internal representation of a CDAP message. */
struct CDAPMessage {
    int                 abs_syntax;
    gpb::authTypes_t    auth_mech;
    CDAPAuthValue       auth_value;
    struct rina_name    src_appl;
    struct rina_name    dst_appl;
    std::string         filter;
    gpb::flagValues_t   flags;
    int                 invoke_id;
    std::string         obj_class;
    long                obj_inst;
    std::string         obj_name;
    gpb::opCode_t       op_code;
    int                 result;
    std::string         result_reason;
    int                 scope;
    long                version;

    enum obj_value_t {
        NONE,
        I32,
        I64,
        BYTES,
        FLOAT,
        DOUBLE,
        BOOL,
        STRING,
    };

    bool is(obj_value_t tt) const { return obj_value.ty == tt; }
    bool is_request() const { return !is_response(); }
    bool is_response() const { return op_code & 0x1; }

    void print() const;

    CDAPMessage(gpb::opCode_t);
    ~CDAPMessage();

    CDAPMessage(const gpb::CDAPMessage& gm);
    operator gpb::CDAPMessage() const;

private:
    /* Representation of the object value. */
    struct {
        obj_value_t         ty;
        union {
            int32_t         i32; /* intval and sintval */
            int64_t         i64; /* int64val and sint64val */
            float           fp_single;
            double          fp_double;
            bool            boolean;
        } u;
        std::string              str; /* strval and byteval */
    }                   obj_value;

    CDAPMessage() { } /* This cannot be called. */
};

#endif /* __RINALITE_CDAP_H__ */
