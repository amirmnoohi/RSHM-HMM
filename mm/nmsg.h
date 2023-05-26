#ifndef __NMSG_H__
#define __NMSG_H__

#include <net/sock.h>
#include <linux/inet.h>
#include <linux/kthread.h>
#include <linux/sched/signal.h>

#include "mem.h"

/* ==================CLIENT CODE=====================*/
int nmsg_init_client(const char *, const int);
int nmsg_send_message(char *);
int nmsg_retrieve_pages(int);
void nmsg_destroy_client(void);
/* ==================CLIENT CODE=====================*/

/* ==================SERVER CODE=====================*/
int __nmsg_init_server(void *);
int nmsg_start_server(const char *, const int);
void nmsg_process_message(char *);
void nmsg_stop_server(void);
/* ==================SERVER CODE=====================*/

#endif