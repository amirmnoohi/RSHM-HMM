#ifndef __NMSG_RDMA_H__
#define __NMSG_RDMA_H__

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "nmsg.h"


/* ==================COMMON CODE=====================*/

/*
 * This structure is used to handle the nmsg data
 */
struct nmsg_data
{
    uint64_t sync_t;
    uint64_t local_port_t;
    uint64_t remote_port_t;
    uint64_t id;
    uint64_t buf;
    uint32_t rkey;
    uint32_t size;
    uint32_t type;
};

/*
 * This enum is to handle rdma connection state in both client/server
 */
enum nmsg_state
{
    IDLE = 1,
    CONNECT_REQUEST, //2
    ADDR_RESOLVED, //3
    ROUTE_RESOLVED, //3
    CONNECTED, //4
    RDMA_READ_ADV, //5
    RDMA_READ_COMPLETE, //6
    RDMA_WRITE_ADV, //7
    RDMA_WRITE_COMPLETE, //8
    ERROR //9
};
/* ==================COMMON CODE=====================*/

/* ==================SERVER CODE=====================*/
struct nmsg_server_handler
{
    /*
     * All the following variables are used for the configuration
     */
    char *addr_str;                          /* dst addr in string format */
    u8 addr[16];                             /* dst addr in NBO */
    uint8_t addr_type;                       /* ADDR_FAMILY - IPv4/V6 */
    uint16_t port;                           /* dst port in NBO */
    int message_size;                        /* size of the message */
    enum nmsg_state state;                   /* state of the connection */
    struct nmsg_stats stats;                 /* statistics */
    wait_queue_head_t sem;                   /* wait queue for the connection */

    struct task_struct *thread;               /* thread for server to run forever*/
    /*
     * All the following variables are used for RECV operation
     */
    struct ib_recv_wr recv_wr;               /* receive work request */
    struct ib_sge recv_sge;                  /* receive scatter/gather element */
    struct nmsg_data recv_buf __aligned(16); /* data to receive */
    u64 recv_dma_addr;                       /* dma address of the receive buffer */

    /*
     * All the following variables are used for SEND operation
     */
    struct ib_send_wr send_wr;               /* send work request */
    struct ib_sge send_sge;                  /* send scatter/gather element */
    struct nmsg_data send_buf __aligned(16); /* data to send */
    u64 send_dma_addr;                       /* dma address of the send buffer */
    /*
     * All the following variables are used for RDMA operation
     */
    struct ib_rdma_wr rdma_wr;               /* rdma work request */
    char *rdma_buf;                          /* rdma buffer */
    struct ib_sge rdma_sge;                  /* rdma scatter/gather element */
    u64 rdma_dma_addr;                       /* dma address of the rdma buffer */
    /*
     * All the following variables are used for remote RDMA operation
     */
    uint32_t remote_rkey;                    /* remote  RKEY */
    uint64_t remote_addr;                    /* remote TO */
    uint32_t remote_len;                     /* remote LEN */

    int page_list_len;                       /* length page list */
    struct ib_reg_wr reg_mr_wr;              /* register memory region work request */
    struct ib_mr *reg_mr;                    /* memory region for the register memory region */
    u8 key;                                  /* key for the memory region */

    struct ib_cq *cq;                        /* completion queue */
    struct ib_pd *pd;                        /* protection domain */
    struct ib_qp *qp;                        /* queue pair */
    struct rdma_cm_id *cm_id;                /* connection on client side, listener on server side. */
    struct rdma_cm_id *child_cm_id;          /* connection on server side */
}; /* object of nmsg server handler */

int __nmsg_init_server(void *);
int nmsg_start_server(const char *, const int);
void nmsg_process_message(char *);
void nmsg_stop_server(void);
/* ==================SERVER CODE=====================*/

/* ==================CLIENT CODE=====================*/

/*
 * Invoke on client side is like this:
 *
 * echo "message" > /proc/nmsg_client
 *
 * nmsg send/receive loop:
 * 	client sends source rkey/addr/len
 *	server receives source rkey/add/len
 *	server rdma reads "message" data from source
 * 	server sends "go ahead" on rdma write completion
 * 	<repeat loop on client call>
 */

struct nmsg_client_handler {
    /*
     * All the following variables are used for the configuration
     */
    char *addr_str;                           /* dst addr in string format */
    u8 addr[16];                              /* dst addr in NBO */
    uint8_t addr_type;                        /* ADDR_FAMILY - IPv4/V6 */
    uint16_t port;                            /* dst port in NBO */
    char *message;                            /* message to send */
    int message_size;                         /* size of the message */
    enum nmsg_state state;                    /* state of the connection */
    struct nmsg_stats stats;                  /* statistics */
    wait_queue_head_t sem;                    /* wait queue for the connection */

    /*
     * All the following variables are used for RECV operation
     */
    struct ib_recv_wr recv_wr;               /* receive work request */
    struct ib_sge recv_sge;                  /* receive scatter/gather element */
    struct nmsg_data recv_buf __aligned(16); /* data to receive */
    u64 recv_dma_addr;                       /* dma address of the receive buffer */

    /*
     * All the following variables are used for SEND operation
     */
    struct ib_send_wr send_wr;                /* send work request */
    struct ib_sge send_sge;                   /* send scatter/gather element */
    struct nmsg_data send_buf __aligned(16);  /* data to send */
    u64 send_dma_addr;                        /* dma address of the send buffer */

    int page_list_len;                        /* length page list */
    struct ib_reg_wr reg_mr_wr;               /* register memory region work request */
    struct ib_mr *reg_mr;                     /* memory region for the register memory region */
    u8 key;                                   /* key for the memory region */


    char *start_buf;                          /* rdma read src */
    u64 start_dma_addr;

    struct ib_cq *cq;                         /* completion queue */
    struct ib_pd *pd;                         /* protection domain */
    struct ib_qp *qp;                         /* queue pair */
    struct rdma_cm_id *cm_id;                 /* connection manager */
}; /* object of nmsg client handler */

int nmsg_init_client(const char *, const int);
int nmsg_send_message(char *);
void nmsg_destroy_client(void);
/* ==================CLIENT CODE=====================*/
#endif
