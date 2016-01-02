#include <iostream>
#include <string>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include "rinalite/rinalite-common.h"
#include "rinalite/rinalite-utils.h"
#include "cdap.hpp"

using namespace std;


CDAPMessage::CDAPMessage(gpb::opCode_t op_code_arg)
{
    op_code = op_code_arg;
    obj_value.ty = NONE;
    flags = gpb::F_NO_FLAGS;
    memset(&src_appl, 0, sizeof(src_appl));
    memset(&dst_appl, 0, sizeof(dst_appl));
}

CDAPMessage::CDAPMessage(const gpb::CDAPMessage& gm)
{
    const char *apn, *api, *aen, *aei;
    gpb::objVal_t objvalue = gm.objvalue();
    int ret;

    abs_syntax = gm.abssyntax();
    op_code = gm.opcode();
    invoke_id = gm.invokeid();
    flags = gm.flags();
    obj_class = gm.objclass();
    obj_name = gm.objname();
    obj_inst = gm.objinst();

    /* Convert object value. */
    if (objvalue.has_intval()) {
        obj_value.u.i32 = objvalue.intval();
        obj_value.ty = I32;

    } else if (objvalue.has_sintval()) {
        obj_value.u.i32 = objvalue.sintval();
        obj_value.ty = I32;

    } else if (objvalue.has_int64val()) {
        obj_value.u.i64 = objvalue.int64val();
        obj_value.ty = I64;

    } else if (objvalue.has_sint64val()) {
        obj_value.u.i64 = objvalue.sint64val();
        obj_value.ty = I64;

    } else if (objvalue.has_strval()) {
        obj_value.str = objvalue.strval();
        obj_value.ty = STRING;

    } else if (objvalue.has_floatval()) {
        obj_value.u.fp_single = objvalue.floatval();
        obj_value.ty = FLOAT;

    } else if (objvalue.has_doubleval()) {
        obj_value.u.fp_double = objvalue.doubleval();
        obj_value.ty = DOUBLE;

    } else if (objvalue.has_boolval()) {
        obj_value.u.boolean = objvalue.boolval();
        obj_value.ty = BOOL;

    } else if (objvalue.has_byteval()) {
        obj_value.str = objvalue.byteval();
        obj_value.ty = BYTES;

    } else {
        obj_value.ty = NONE;
    }

    result = gm.result();
    scope = gm.scope();
    filter = gm.filter();

    auth_mech = gm.authmech();
    auth_value.name = gm.authvalue().authname();
    auth_value.password = gm.authvalue().authpassword();
    auth_value.other = gm.authvalue().authother();

    apn = gm.has_destapname() ? gm.destapname().c_str() : NULL;
    api = gm.has_destapinst() ? gm.destapinst().c_str() : NULL;
    aen = gm.has_destaename() ? gm.destaename().c_str() : NULL;
    aei = gm.has_destaeinst() ? gm.destaeinst().c_str() : NULL;
    rina_name_fill(&dst_appl, apn, api, aen, aei);

    apn = gm.has_srcapname() ? gm.srcapname().c_str() : NULL;
    api = gm.has_srcapinst() ? gm.srcapinst().c_str() : NULL;
    aen = gm.has_srcaename() ? gm.srcaename().c_str() : NULL;
    aei = gm.has_srcaeinst() ? gm.srcaeinst().c_str() : NULL;
    rina_name_fill(&src_appl, apn, api, aen, aei);

    result_reason = gm.resultreason();
    version = gm.version();
}

#define safe_c_string(_s) ((_s) ? (_s) : "")

CDAPMessage::operator gpb::CDAPMessage() const
{
    gpb::CDAPMessage gm;
    gpb::objVal_t *objvalue = new gpb::objVal_t();
    gpb::authValue_t *authvalue = new gpb::authValue_t();

    if (!objvalue || !authvalue) {
        PE("%s: Out of memory\n", __func__);
        if (objvalue) delete objvalue;
        if (authvalue) delete authvalue;

        return gpb::CDAPMessage();
    }

    gm.set_abssyntax(abs_syntax);
    gm.set_opcode(op_code);
    gm.set_invokeid(invoke_id);
    gm.set_flags(flags);
    gm.set_objclass(obj_class);
    gm.set_objname(obj_name);
    gm.set_objinst(obj_inst);

    /* Convert object value. */
    switch (obj_value.ty) {
        case I32:
            objvalue->set_intval(obj_value.u.i32);
            break;

        case I64:
            objvalue->set_int64val(obj_value.u.i64);
            break;

        case STRING:
            objvalue->set_strval(obj_value.str);
            break;

        case FLOAT:
            objvalue->set_floatval(obj_value.u.fp_single);
            break;

        case DOUBLE:
            objvalue->set_doubleval(obj_value.u.fp_double);
            break;

        case BOOL:
            objvalue->set_boolval(obj_value.u.boolean);
            break;

        case BYTES:
            objvalue->set_byteval(obj_value.str);
            break;
    }

    if (obj_value.ty != NONE) {
        gm.set_allocated_objvalue(objvalue);
    }

    gm.set_result(result);
    gm.set_scope(scope);
    gm.set_filter(filter);
    gm.set_authmech(auth_mech);

    authvalue->set_authname(auth_value.name);
    authvalue->set_authpassword(auth_value.password);
    authvalue->set_authother(auth_value.other);
    gm.set_allocated_authvalue(authvalue);

    gm.set_destapname(std::string(safe_c_string(dst_appl.apn)));
    gm.set_destapinst(std::string(safe_c_string(dst_appl.api)));
    gm.set_destaename(std::string(safe_c_string(dst_appl.aen)));
    gm.set_destaeinst(std::string(safe_c_string(dst_appl.aei)));

    gm.set_srcapname(std::string(safe_c_string(src_appl.apn)));
    gm.set_srcapinst(std::string(safe_c_string(src_appl.api)));
    gm.set_srcaename(std::string(safe_c_string(src_appl.aen)));
    gm.set_srcaeinst(std::string(safe_c_string(src_appl.aei)));

    gm.set_resultreason(result_reason);
    gm.set_version(version);

    return gm;
}

void
CDAPMessage::print() const
{
    char *name;

    PD("CDAP Message {\n");
    PD("    abs_syntax: %d\n", abs_syntax);
    PD("    op_code: %d\n", op_code);
    PD("    invoke_id: %d\n", invoke_id);
    PD("    flags: %04x\n", flags);
    PD("    obj_class: %s\n", obj_class.c_str());
    PD("    obj_name: %s\n", obj_name.c_str());
    PD("    obj_inst: %ld\n", obj_inst);

    /* Print object value. */
    switch (obj_value.ty) {
        case I32:
            PD("    obj_value: %d\n", obj_value.u.i32);
            break;

        case I64:
            PD("    obj_value: %lld\n", (long long)obj_value.u.i64);
            break;

        case STRING:
            PD("    obj_value: %s\n", obj_value.str.c_str());
            break;

        case FLOAT:
            PD("    obj_value: %f\n", obj_value.u.fp_single);
            break;

        case DOUBLE:
            PD("    obj_value: %f\n", obj_value.u.fp_double);
            break;

        case BOOL:
            PD("    obj_value: %s\n", (obj_value.u.boolean ? "true" : "false"));
            break;

        case BYTES:
            PD("    obj_value: %s\n", obj_value.str.c_str());
            break;
    }


    PD("    result: %d\n", result);
    PD("    scope: %d\n", scope);
    PD("    filter: %s\n", filter.c_str());

    PD("    auth_mech: %d\n", auth_mech);

    PD("    auth_value: name='%s' pwd='%s' other='%s'\n",
                auth_value.name.c_str(), auth_value.password.c_str(),
                auth_value.other.c_str());

    name = rina_name_to_string(&dst_appl);
    PD("    dst_appl: %s\n", name);
    if (name) {
        free(name);
    }

    name = rina_name_to_string(&src_appl);
    PD("    src_appl: %s\n", name);
    if (name) {
        free(name);
    }

    PD("    result_reason: %s\n", result_reason.c_str());
    PD("    version: %ld\n", version);
}

int
cdap_msg_send(const struct CDAPMessage *m, int fd)
{
    gpb::CDAPMessage gm = static_cast<gpb::CDAPMessage>(*m);
    size_t serlen = gm.ByteSize();
    char *serbuf = (char *)malloc(serlen);
    int n;

    if (!serbuf) {
        return -ENOMEM;
    }

    gm.SerializeToArray(serbuf, serlen);

    n = write(fd, serbuf, serlen);
    if (n != serlen) {
        if (n < 0) {
            perror("write(cdap_msg)");
        } else {
            PE("%s: Partial write %d/%d\n", __func__, n, serlen);
        }
        return -1;
    }

    return 0;
}

struct CDAPMessage *
cdap_msg_recv(int fd)
{
    struct CDAPMessage *m;
    gpb::CDAPMessage gm;
    char serbuf[4096];
    int n;

    n = read(fd, serbuf, sizeof(serbuf));
    if (n < 0) {
        perror("read(cdap_msg)");
        return NULL;
    }

    gm.ParseFromArray(serbuf, n);

    m = new CDAPMessage(gm);
    if (!m) {
        PE("%s: Out of memory\n", __func__);
    }

    return m;
}
