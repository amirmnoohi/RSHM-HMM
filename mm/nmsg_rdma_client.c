#include <linux/inet.h>
#include "nmsg_rdma.h"

struct nmsg_client_handler* cb;

/* CM event handler */
static int nmsg_cma_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event)
{
    int ret;
    cb = cma_id->context;

    DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id, (cma_id == cb->cm_id) ? "parent" : "child");
    DEBUG_LOG("cb->state: %d", cb->state);
    
    switch (event->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            DEBUG_LOG("CLIENT CMA: state changed from %d to %d\n", cb->state, RDMA_CM_EVENT_ADDR_RESOLVED);
            DEBUG_LOG("RDMA_CM_EVENT_ADDR_RESOLVED");
            cb->state = ADDR_RESOLVED;
            ret = rdma_resolve_route(cma_id, 2000);
            if (ret) {
                DEBUG_LOG(KERN_ERR PRE "rdma_resolve_route error %d\n", ret);
                wake_up_interruptible(&cb->sem);
            }
            break;
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
            DEBUG_LOG("CLIENT CMA: state changed from %d to %d\n", cb->state, RDMA_CM_EVENT_ROUTE_RESOLVED);
            DEBUG_LOG("RDMA_CM_EVENT_ROUTE_RESOLVED");
            cb->state = ROUTE_RESOLVED;
            wake_up_interruptible(&cb->sem);
            break;
        case RDMA_CM_EVENT_CONNECT_REQUEST:
            DEBUG_LOG("CLIENT CMA: state changed from %d to %d\n", cb->state, CONNECT_REQUEST);
            DEBUG_LOG("RDMA_CM_EVENT_CONNECT_REQUEST");
            cb->state = CONNECT_REQUEST;
            wake_up_interruptible(&cb->sem);
            break;
        case RDMA_CM_EVENT_ESTABLISHED:
            DEBUG_LOG("CLIENT CMA: state changed from %d to %d\n", cb->state, CONNECTED);
            DEBUG_LOG("ESTABLISHED\n");
            cb->state = CONNECTED;
            wake_up_interruptible(&cb->sem);
            break;
        case RDMA_CM_EVENT_ADDR_ERROR:
        case RDMA_CM_EVENT_ROUTE_ERROR:
        case RDMA_CM_EVENT_CONNECT_ERROR:
        case RDMA_CM_EVENT_UNREACHABLE:
        case RDMA_CM_EVENT_REJECTED:
            DEBUG_LOG(KERN_ERR PRE "cma event %d, error %d\n", event->event, event->status);
            // state changed from to
            DEBUG_LOG("CLIENT CMA: state changed from %d to %d\n", cb->state, ERROR);
            DEBUG_LOG("change state to error\n");
            cb->state = ERROR;
            DEBUG_LOG("before wakeup\n");
            wake_up_interruptible(&cb->sem);
            DEBUG_LOG("after wakeup\n");
            break;
        case RDMA_CM_EVENT_DISCONNECTED:
            DEBUG_LOG("CLIENT CMA: state changed from %d to %d\n", cb->state, ERROR);
            DEBUG_LOG(KERN_ERR PRE "DISCONNECT EVENT...\n");
            cb->state = ERROR;
            wake_up_interruptible(&cb->sem);
            break;
        case RDMA_CM_EVENT_DEVICE_REMOVAL:
            DEBUG_LOG("CLIENT CMA: state changed from %d to %d\n", cb->state, ERROR);
            DEBUG_LOG(KERN_ERR PRE "cma detected device removal!!!!\n");
            cb->state = ERROR;
            wake_up_interruptible(&cb->sem);
            break;
        default:
            DEBUG_LOG("CLIENT CMA: state changed from %d to default error\n", cb->state);
            DEBUG_LOG(KERN_ERR PRE "oof bad type!\n");
            wake_up_interruptible(&cb->sem);
            break;
    }

    return 0;
}

static int client_recv(struct ib_wc *wc)
{
    DEBUG_LOG("client_recv called\n");
    
	if (wc->byte_len != sizeof(cb->recv_buf)) {    // Check if the received data is of expected size
		DEBUG_LOG(KERN_ERR PRE "Received bogus data, size %d\n", 
		       wc->byte_len);                       // Log an error message
		return -1;                                  // Return an error code
	}

	if (cb->state == RDMA_READ_ADV)                  // Check if context state is RDMA_READ_ADV
		cb->state = RDMA_WRITE_ADV;                 // Set context state to RDMA_WRITE_ADV
	else
		cb->state = RDMA_WRITE_COMPLETE;            // Set context state to RDMA_WRITE_COMPLETE

	return 0;                                        // Return success
}

/* CQ event handler */
static void nmsg_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct ib_wc wc;               // Create a work completion object to store event details
	const struct ib_recv_wr *bad_wr;  // Pointer to the first failed WR in an SGL or chain of WRs 
	int ret;                       // Return value from functions
    cb = ctx;                      // Set user-defined context to cb variable 

	BUG_ON(cb->cq != cq);          // Check that the CQ in the context matches the passed in CQ, panic if they don't
	if (cb->state == ERROR) {      // Check if the context is in an error state
		DEBUG_LOG(KERN_ERR PRE "CLIENT cq completion in ERROR state\n");  // Log error message
		return;                    // Return from the function
	}

	ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP); // Request notification of next completion event on the CQ

	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) { // Poll the CQ for completion events until there are no more events
		if (wc.status) {           // Check if the completion event was successful
			if (wc.status == IB_WC_WR_FLUSH_ERR) {  // Check if the event was a WR flush error
				DEBUG_LOG("CLIENT cq flushed in handler\n");  // Log that the CQ was flushed
				continue;             // Continue to the next iteration of the loop
			} else {
				DEBUG_LOG(KERN_ERR PRE "CLIENT cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);  // Log the error message
				goto error;           // Jump to the error handling code
			}
		}

		switch (wc.opcode) {        // Check the opcode of the completion event
            case IB_WC_SEND:            // Handle send completion event
                DEBUG_LOG("CLIENT CQ: rdma send completion\n");  // Log send completion
                cb->stats.send_bytes += cb->send_sge.length;
                cb->stats.send_msgs++;
                break;
            case IB_WC_RECV:            // Handle receive completion event
                DEBUG_LOG("CLIENT CQ: rdma recv completion\n");      // Log receive completion
                ret = client_recv(&wc);  // Call server or client receive function based on the context
                if (ret) {
                    DEBUG_LOG(KERN_ERR PRE "CLIENT recv wc error: %d\n", ret);  // Log receive error message
                    goto error;           // Jump to the error handling code
                }
                ret = ib_post_recv(cb->qp, &cb->recv_wr, &bad_wr);  // Post a new receive request
                if (ret) {
                    DEBUG_LOG(KERN_ERR PRE "CLIENT post recv error: %d\n", ret); // Log error message
                    goto error; // Jump to the error handling code
                }
                wake_up_interruptible(&cb->sem);   // Wake up any waiting threads
                break;
            default:  // Handle unexpected opcode
                DEBUG_LOG(KERN_ERR PRE "CLIENT CQ: %s:%d Unexpected opcode %d, Shutting down\n", __func__, __LINE__, wc.opcode); // Log error message
                goto error;  // Jump to the error handling code
		}
	}
	if (ret) {                     // Handle poll error
		DEBUG_LOG(KERN_ERR PRE "CQ: poll error %d\n", ret);  // Log poll error message
		goto error;           // Jump to the error handling code
	}

	return;

error:                             // Error handling code
	cb->state = ERROR;              // Set context state to ERROR
	wake_up_interruptible(&cb->sem);  // Wake up any waiting threads
}

/* 
 * Fill in the sockaddr struct for the global cb
 * This is a bit of a hack, but it works for now
 * TODO: make this more generic
 * TODO: make this work for IPv6 
 */
static void fill_sockaddr(struct sockaddr_storage *sin)
{
    struct sockaddr_in *sin4;

    memset(sin, 0, sizeof(*sin));

    sin4 = (struct sockaddr_in *)sin;

    sin4->sin_family = cb->addr_type;

    memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
    
    sin4->sin_port = cb->port;
}

/*
 * Create a QP for the global cb
 */
static int nmsg_create_qp(void)
{
    struct ib_qp_init_attr init_attr;
    int ret;

    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.cap.max_send_wr = 64;
    init_attr.cap.max_recv_wr = 2;

    /* For flush_qp() */
    init_attr.cap.max_send_wr++;
    init_attr.cap.max_recv_wr++;

    init_attr.cap.max_recv_sge = 1;
    init_attr.cap.max_send_sge = 1;
    init_attr.qp_type = IB_QPT_RC;
    init_attr.send_cq = cb->cq;
    init_attr.recv_cq = cb->cq;
    init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

    ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
    if (!ret)
        cb->qp = cb->cm_id->qp;

    return ret;
}

static u32 nmsg_rdma_rkey(u64 buf)
{
    u32 rkey;
    const struct ib_send_wr *bad_wr;
    int ret;
    struct scatterlist sg = {0};

    /*
     * Update the reg key.
     */
    ib_update_fast_reg_key(cb->reg_mr, ++cb->key);
    cb->reg_mr_wr.key = cb->reg_mr->rkey;

    /*
     * Update the reg WR with new buf info.
     */
    cb->reg_mr_wr.access = IB_ACCESS_REMOTE_READ;
    
    sg_dma_address(&sg) = buf;
    sg_dma_len(&sg) = cb->message_size;

    ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, NULL, PAGE_SIZE);
    BUG_ON(ret <= 0 || ret > cb->page_list_len);

    DEBUG_LOG("reg_mr new rkey 0x%x pgsz %u len %lu iova_start %llx\n",
              cb->reg_mr_wr.key,
              cb->reg_mr->page_size,
              (unsigned long)cb->reg_mr->length,
              (unsigned long long)cb->reg_mr->iova);

    ret = ib_post_send(cb->qp, &cb->reg_mr_wr.wr, &bad_wr);
    if (ret)
    {
        printk(KERN_ERR PRE "post send error %d\n", ret);
        cb->state = ERROR;
    }
    rkey = cb->reg_mr->rkey;
    return rkey;
}

static void make_message(u64 buf)
{
    struct nmsg_data *info = &cb->send_buf;
    u32 rkey;
    
    rkey = nmsg_rdma_rkey(buf);
    info->buf = htonll(buf);
    info->rkey = htonl(rkey);
    info->size = htonl(cb->message_size);
    DEBUG_LOG("RDMA addr %llx rkey %x len %d\n", (unsigned long long)buf, rkey, cb->message_size);

}


/*
 * Create a CQ and PD for the global cb (setup for QP creation)
 * Called from nmsg_setup_qp()
 * Returns 0 on success, negative errno on failure
 * On failure, the global cb is not modified
 * On success, the global cb is modified
 */
static int nmsg_setup_qp(struct rdma_cm_id *cm_id)
{
    int ret;
    struct ib_cq_init_attr attr = {0};

    cb->pd = ib_alloc_pd(cm_id->device, 0);
    if (IS_ERR(cb->pd))
    {
        printk(KERN_ERR PRE "ib_alloc_pd failed\n");
        return PTR_ERR(cb->pd);
    }
    DEBUG_LOG("created pd %p\n", cb->pd);

    attr.cqe = 128;
    attr.comp_vector = 0;
    cb->cq = ib_create_cq(cm_id->device, nmsg_cq_event_handler, NULL, cb, &attr);
    if (IS_ERR(cb->cq))
    {
        printk(KERN_ERR PRE "ib_create_cq failed\n");
        ret = PTR_ERR(cb->cq);
        goto err1;
    }
    DEBUG_LOG("created cq %p\n", cb->cq);
    ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
    if (ret)
    {
        printk(KERN_ERR PRE "ib_create_cq failed\n");
        goto err2;
    }

    ret = nmsg_create_qp();
    if (ret)
    {
        printk(KERN_ERR PRE "nmsg_create_qp failed: %d\n", ret);
        goto err2;
    }
    DEBUG_LOG("created qp %p\n", cb->qp);
    return 0;
err2:
    ib_destroy_cq(cb->cq);
err1:
    ib_dealloc_pd(cb->pd);
    return ret;
}

static int reg_supported(struct ib_device *dev)
{
	u64 needed_flags = IB_DEVICE_MEM_MGT_EXTENSIONS;

	if ((dev->attrs.device_cap_flags & needed_flags) != needed_flags) {
		DEBUG_LOG("Fastreg not supported - device_cap_flags 0x%llx\n", (unsigned long long)dev->attrs.device_cap_flags);
		return 0;
	}
	DEBUG_LOG("Fastreg supported - device_cap_flags 0x%llx\n", (unsigned long long)dev->attrs.device_cap_flags);
	return 1;
}

static void nmsg_setup_wr(void)
{
	cb->recv_sge.addr = cb->recv_dma_addr;
	cb->recv_sge.length = sizeof cb->recv_buf;
	cb->recv_sge.lkey = cb->pd->local_dma_lkey;
	cb->recv_wr.sg_list = &cb->recv_sge;
	cb->recv_wr.num_sge = 1;

	cb->send_sge.addr = cb->send_dma_addr;
	cb->send_sge.length = sizeof cb->send_buf;
	cb->send_sge.lkey = cb->pd->local_dma_lkey;
	cb->send_wr.sg_list = &cb->send_sge;
	cb->send_wr.num_sge = 1;

	cb->send_wr.opcode = IB_WR_SEND;
	cb->send_wr.send_flags = IB_SEND_SIGNALED;

	/* 
	 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
	 * both unsignaled.  The client uses them to reregister
	 * the rdma buffers with a new key each iteration.
	 */
	cb->reg_mr_wr.wr.opcode = IB_WR_REG_MR;
	cb->reg_mr_wr.mr = cb->reg_mr;
}


/*
 * Setup the buffers for the global cb
 * Called from nmsg_setup()
 * Returns 0 on success, non-zero on failure
 */
static int nmsg_setup_buffers(void)
{
    int ret;

    // allocate the receive buffer
    cb->recv_dma_addr = ib_dma_map_single(cb->pd->device, &cb->recv_buf, sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
    // allocate the send buffer
    cb->send_dma_addr = ib_dma_map_single(cb->pd->device, &cb->send_buf, sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
    
    cb->page_list_len = (((cb->message_size - 1) & PAGE_MASK) + PAGE_SIZE) >> PAGE_SHIFT;
    // allocate memmory region buffer
    cb->reg_mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, cb->page_list_len);


    if (IS_ERR(cb->reg_mr))
    {
        ret = PTR_ERR(cb->reg_mr);
        DEBUG_LOG(PRE "recv_buf reg_mr failed %d\n", ret);
        goto bail;
    }
    DEBUG_LOG(PRE "reg rkey 0x%x page_list_len %u\n", cb->reg_mr->rkey, cb->page_list_len);
    
    cb->start_buf = dma_alloc_coherent(cb->pd->device->dma_device, cb->message_size, &cb->start_dma_addr, GFP_KERNEL);
    if (!cb->start_buf)
    {
        DEBUG_LOG(PRE "start_buf malloc failed\n");
        ret = -ENOMEM;
        goto bail;
    }

    nmsg_setup_wr();
    DEBUG_LOG(PRE "allocated & registered buffers...\n");
    return 0;
bail:
    if (cb->reg_mr && !IS_ERR(cb->reg_mr))
        ib_dereg_mr(cb->reg_mr);
    if (cb->start_buf)
        dma_free_coherent(cb->pd->device->dma_device, cb->message_size, cb->start_buf, cb->start_dma_addr);
    return ret;
}

/*
 * Free the buffers for the global cb
 */
static void nmsg_free_buffers(void)
{
    DEBUG_LOG("nmsg_free_buffers called on cb %p\n", cb);
    
    if (cb->reg_mr)
        ib_dereg_mr(cb->reg_mr);

    ib_dma_unmap_single(cb->pd->device, cb->recv_dma_addr,
                     sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
    ib_dma_unmap_single(cb->pd->device, cb->send_dma_addr,
                     sizeof(cb->send_buf), DMA_BIDIRECTIONAL);

    if (cb->start_buf)
    {
        dma_free_coherent(cb->pd->device->dma_device, cb->message_size, cb->start_buf, cb->start_dma_addr);
    }
}

/*
 * Free the QP for the global cb
 */
static void nmsg_free_qp(void)
{
    ib_destroy_qp(cb->qp);
    ib_destroy_cq(cb->cq);
    ib_dealloc_pd(cb->pd);
}

/*
 * This function start writing data to the server memory as a client
 */
static int nmsg_start_client(void)
{
    int ret;
    const struct ib_send_wr *bad_wr;

    DEBUG_LOG("nmsg_start_client called successfully\n");

    cb->state = RDMA_READ_ADV;

    memcpy(cb->start_buf, cb->message, cb->message_size);

    make_message(cb->start_dma_addr);

    if (cb->state == ERROR)
    {
        printk(KERN_ERR PRE "nmsg_format_send failed\n");
        return -1;
    }
    DEBUG_LOG("make_message successful\n");

    ret = ib_post_send(cb->qp, &cb->send_wr, &bad_wr);
    if (ret)
    {
        printk(KERN_ERR PRE "post send error %d\n", ret);
        return ret;
    }

    DEBUG_LOG("ib_post_send successful\n");
    return 0;
}


/*
 * This function start the core of NMSG
 * @param cmd: a pointer to the command string from /proc/nmsg
 * @return: 0 on success
 */
int nmsg_start(void* cmd)
{
    char* command = (char*) cmd;
    cb->message = kmalloc(strlen(command), GFP_KERNEL);
    strcpy(cb->message, command);
    
    printk(KERN_INFO PRE "Client mode\n");
    return nmsg_start_client();
}

/*
 * connect to the server
 * This function is called on the client side
 * It waits for the connection to be established
 * It returns 0 on success, -1 on error
 */
static int nmsg_connect_client(void)
{
    struct rdma_conn_param conn_param;
    int ret;

    memset(&conn_param, 0, sizeof conn_param);
    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    conn_param.retry_count = 10;

    ret = rdma_connect(cb->cm_id, &conn_param);
    if (ret)
    {
        printk(KERN_ERR PRE "rdma_connect error %d\n", ret);
        return ret;
    }

    wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
    if (cb->state == ERROR)
    {
        printk(KERN_ERR PRE "wait for CONNECTED state %d\n", cb->state);
        return -1;
    }
    return 0;
}

/*
 * Bind the client to the server
 * Called from nmsg_run_client()
 * Returns 0 on success, negative errno on failure
 */
static int nmsg_bind_client(void)
{
    struct sockaddr_storage sin;
    int ret;
    DEBUG_LOG("nmsg_bind_client called\n");

    fill_sockaddr(&sin);

    DEBUG_LOG("socket address filled\n");

    // print cb->cm_id
    DEBUG_LOG("cb->cm_id = %p\n", cb->cm_id);
    ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
    if (ret)
    {
        printk(KERN_ERR PRE "rdma_resolve_addr error %d\n", ret);
        return ret;
    }
    DEBUG_LOG("rdma_resolve_addr successful\n");

    wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);

	if (cb->state != ROUTE_RESOLVED) {
		printk(KERN_ERR PRE "addr/route resolution did not resolve: state %d\n", cb->state);
		return -EINTR;
	}

	if (!reg_supported(cb->cm_id->device))
		return -EINVAL;

    return 0;
}

static int nmsg_run_client(void)
{
    
    const struct ib_recv_wr *bad_wr;
    int ret;

    DEBUG_LOG("nmsg_run_client called\n");

    ret = nmsg_bind_client();
    if (ret)
        return ret;

    DEBUG_LOG("nmsg_bind_client successful\n");


    ret = nmsg_setup_qp(cb->cm_id);
    if (ret)
    {
        printk(KERN_ERR PRE "setup_qp failed: %d\n", ret);
        return ret;
    }
    DEBUG_LOG("nmsg_setup_qp successful\n");


    ret = nmsg_setup_buffers();
    if (ret)
    {
        printk(KERN_ERR PRE "nmsg_setup_buffers failed: %d\n", ret);
        goto err1;
    }
    DEBUG_LOG("nmsg_setup_buffers successful\n");

    ret = ib_post_recv(cb->qp, &cb->recv_wr, &bad_wr);
    if (ret)
    {
        printk(KERN_ERR PRE "ib_post_recv failed: %d\n", ret);
        goto err2;
    }

    DEBUG_LOG("ib_post_recv successful\n");

    ret = nmsg_connect_client();
    if (ret)
    {
        printk(KERN_ERR PRE "connect error %d\n", ret);
        goto err2;
    }

    DEBUG_LOG("nmsg_connect_client successful\n");

    return 0;

err2:
    nmsg_free_buffers();
err1:
    nmsg_free_qp();

    return ret;
}

// /*
//  * Read proc returns stats for each device.
//  */
// int nmsg_write_stats(struct seq_file *seq, void *v)
// {
// 	int num = 1;

// 	DEBUG_LOG(KERN_INFO PRE "Writing Stats to /proc/nmsg\n");
//     if (cb->pd) {
//         seq_printf(seq,
//                 "%d-%s %lld %lld %lld %lld %lld %lld %lld %lld\n",
//                 num++, cb->pd->device->name, cb->stats.send_bytes,
//                 cb->stats.send_msgs, cb->stats.recv_bytes,
//                 cb->stats.recv_msgs, cb->stats.write_bytes,
//                 cb->stats.write_msgs,
//                 cb->stats.read_bytes,
//                 cb->stats.read_msgs);
//     } else {
//         seq_printf(seq, "%d listen\n", num++);
//     }
// 	return 0;
// }

int nmsg_pre_start(int port, const char* addr)
{
    int ret = 0;
    cb = kmalloc(sizeof(struct nmsg_client_handler), GFP_KERNEL);

    /*Init addr and ip port in general struct*/
    cb->addr_str = kmalloc(strlen(addr), GFP_KERNEL);
    strcpy(cb->addr_str, addr);
    in4_pton(addr, -1, cb->addr, -1, NULL);
    cb->addr_type = AF_INET;
    cb->port = htons(port);

    /*Init message and queue for sending and recieving message*/
    cb->state = IDLE;
    //TODO: change message size with message
    cb->message_size = 1024;
    init_waitqueue_head(&cb->sem);
    DEBUG_LOG(KERN_INFO PRE "init wait queue\n");

    /*Init rdma id*/
    cb->cm_id = rdma_create_id(&init_net, nmsg_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
    if (IS_ERR(cb->cm_id))
    {
        ret = PTR_ERR(cb->cm_id);
        printk(KERN_ERR PRE "rdma_create_id error %d\n", ret);
        kfree(cb);
        return ret;
    }
    DEBUG_LOG("NMSG CLIENT: created cm_id %p\n", cb->cm_id);

    printk(KERN_INFO PRE "CLIENT MODE\n");
    ret = nmsg_run_client();
    
    return ret;
}


void nmsg_free(void)
{

    DEBUG_LOG("destroy cm_id %p\n", cb->cm_id);
    rdma_disconnect(cb->cm_id);
    rdma_destroy_id(cb->cm_id);

    nmsg_free_buffers();
    nmsg_free_qp();

    kfree(cb);
}

void nmsg_destroy_client(void)
{
    nmsg_free();
}

int nmsg_init_client(const char* addr, int port)
{
    return nmsg_pre_start(port, addr);
}

int nmsg_send_message(char * message)
{
    return nmsg_start(message);
}