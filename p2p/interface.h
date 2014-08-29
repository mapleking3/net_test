#ifndef __INTERFACE_H__
#define __INTERFACE_H__

#define SERVER_LISTEN_PORT ("28099")

#define SAFE_METHOD_FREE(method, p) \
    do {\
        if (NULL != p)\
        {\
            method(p);\
            p = NULL;\
        }\
    } while (0)

#define ASSERT_GOTO(cond, jump) \
{\
    if (!(cond)) {\
        printf("assert: %s failed[%m]!\n", #cond);\
        goto jump;\
    }\
}

#define ASSERT_RET(cond, ret) \
{\
    if (!(cond)) {\
        printf("assert: %s failed[%m]!\n", #cond);\
        return (ret);\
    }\
}

enum {
    CMD_HEARTBEAT_E     = 1001,
    CMD_TUNNEL_E        = 1002,
    CMD_TUNNEL_REQ_E    = 1003,
    CMD_TUNNEL_RSP_E    = 1004,
};

typedef struct {
    unsigned int    unit_no;
} HEARTBEAT_T;

typedef struct {
    unsigned short  tunnel_port;
    unsigned short  listen_port;
} TUNNEL_T;

typedef struct {
    int             unit_no;
    unsigned short  tunnel_port;
    char            res[2];
} TUNNEL_REQ_T;

#endif  ///< __INTERFACE_H_
