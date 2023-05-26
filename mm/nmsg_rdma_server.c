#include <linux/inet.h>
#include "nmsg_rdma.h"

struct nmsg_server_handler* cb;

/* CM event handler */
static int nmsg_cma_event_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event)
{
    int ret;
    cb = cma_id->context;

    DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id, (cma_id == cb->cm_id) ? "parent" : "child");

    DEBUG_LOG("cb->state: %d", cb->state);
    
    switch (event->event)
    {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
        DEBUG_LOG("SERVER CMA: state changed from %d to %d\n", cb->state, RDMA_CM_EVENT_ADDR_RESOLVED);
        DEBUG_LOG("RDMA_CM_EVENT_ADDR_RESOLVED");
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			DEBUG_LOG(KERN_ERR PRE "rdma_resolve_route error %d\n", ret);
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
        DEBUG_LOG("SERVER CMA: state changed from %d to %d\n", cb->state, RDMA_CM_EVENT_ROUTE_RESOLVED);
        DEBUG_LOG("RDMA_CM_EVENT_ROUTE_RESOLVED");
		cb->state = ROUTE_RESOLVED;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
        DEBUG_LOG("SERVER CMA: state changed from %d to %d\n", cb->state, CONNECT_REQUEST);
        DEBUG_LOG("RDMA_CM_EVENT_CONNECT_REQUEST");
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
        DEBUG_LOG("SERVER CMA: state changed from %d to %d\n", cb->state, CONNECTED);
		DEBUG_LOG("ESTABLISHED\n");
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		DEBUG_LOG(KERN_ERR PRE "cma event %d, error %d\n", event->event, event->status);
        DEBUG_LOG("SERVER CMA: state changed from %d to %d\n", cb->state, ERROR);
        DEBUG_LOG("change state to error\n");
		cb->state = ERROR;
        DEBUG_LOG("before wakeup\n");
		wake_up_interruptible(&cb->sem);
        DEBUG_LOG("after wakeup\n");
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
        DEBUG_LOG("SERVER CMA: state changed from %d to %d\n", cb->state, ERROR);
		DEBUG_LOG(KERN_ERR PRE "DISCONNECT EVENT...\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
        DEBUG_LOG("SERVER CMA: state changed from %d to %d\n", cb->state, ERROR);
		DEBUG_LOG(KERN_ERR PRE "cma detected device removal!!!!\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	default:
        DEBUG_LOG("SERVER CMA: state changed from %d to default error\n", cb->state);
		DEBUG_LOG(KERN_ERR PRE "oof bad type!\n");
		wake_up_interruptible(&cb->sem);
		break;
    }

    return 0;
}


static int server_recv(struct ib_wc *wc)
{
    DEBUG_LOG("server_recv called\n");
	if (wc->byte_len != sizeof(cb->recv_buf)) {        // Check if the received data is of expected size
		DEBUG_LOG(KERN_ERR PRE "Received bogus data, size %d\n", wc->byte_len);  // Log an error message
		return -1;                                    // Return an error code
	}

	cb->remote_rkey = ntohl(cb->recv_buf.rkey);       // Convert the received rkey to host byte order and store it in the context
	cb->remote_addr = ntohll(cb->recv_buf.buf);       // Convert the received buffer address to host byte order and store it in the context
	cb->remote_len  = ntohl(cb->recv_buf.size);       // Convert the received buffer length to host byte order and store it in the context
	DEBUG_LOG("Received rkey %x addr %llx len %d from peer\n", 
        cb->remote_rkey, 
        (unsigned long long)cb->remote_addr, 
        cb->remote_len);  // Log a debug message with the received parameters

	if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE)  // Check if context state is CONNECTED or RDMA_WRITE_COMPLETE
		cb->state = RDMA_READ_ADV;                 // Set context state to RDMA_READ_ADV
	else
		cb->state = RDMA_WRITE_ADV;                // Set context state to RDMA_WRITE_ADV

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
		DEBUG_LOG(KERN_ERR PRE "SERVER cq completion in ERROR state\n");  // Log error message
		return;                    // Return from the function
	}

	ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP); // Request notification of next completion event on the CQ

	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) { // Poll the CQ for completion events until there are no more events
		if (wc.status) {           // Check if the completion event was successful
			if (wc.status == IB_WC_WR_FLUSH_ERR) {  // Check if the event was a WR flush error
				DEBUG_LOG("SERVER cq flushed in handler\n");  // Log that the CQ was flushed
				continue;             // Continue to the next iteration of the loop
			} else {
				DEBUG_LOG(KERN_ERR PRE "SERVER cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);  // Log the error message
				goto error;           // Jump to the error handling code
			}
		}

		switch (wc.opcode) {        // Check the opcode of the completion event
		case IB_WC_SEND:            // Handle send completion event
			DEBUG_LOG("SERVER CQ: rdma send completion\n");  // Log send completion
            cb->stats.send_bytes += cb->send_sge.length;
			cb->stats.send_msgs++;
			break;

		case IB_WC_RDMA_READ:       // Handle RDMA read completion event
			DEBUG_LOG("SERVER CQ: rdma read completion\n");  // Log RDMA read completion
            cb->stats.recv_bytes += sizeof(cb->recv_buf);
			cb->stats.recv_msgs++;
			cb->state = RDMA_READ_COMPLETE;       // Set context state to RDMA_READ_COMPLETE
			wake_up_interruptible(&cb->sem);      // Wake up any waiting threads
			break;

		case IB_WC_RECV:            // Handle receive completion event
			DEBUG_LOG("SERVER CQ: rdma recv completion\n");      // Log receive completion
			ret = server_recv(&wc);  // Call server or client receive function based on the context
			if (ret) {
				DEBUG_LOG(KERN_ERR PRE "SERVER recv wc error: %d\n", ret);  // Log receive error message
				goto error;           // Jump to the error handling code
			}

			ret = ib_post_recv(cb->qp, &cb->recv_wr, &bad_wr);  // Post a new receive request
			if (ret) {
				DEBUG_LOG(KERN_ERR PRE "SERVER post recv error: %d\n", ret); // Log error message
				goto error; // Jump to the error handling code
			}
			wake_up_interruptible(&cb->sem);   // Wake up any waiting threads
			break;

		default:  // Handle unexpected opcode
			DEBUG_LOG(KERN_ERR PRE "SERVER CQ: %s:%d Unexpected opcode %d, Shutting down\n", __func__, __LINE__, wc.opcode); // Log error message
			goto error;  // Jump to the error handling code
		}
	}
	if (ret) {                     // Handle poll error
		DEBUG_LOG(KERN_ERR PRE "SERVER CQ: poll error %d\n", ret);  // Log poll error message
		goto error;           // Jump to the error handling code
	}

	return;

error:                             // Error handling code
	cb->state = ERROR;              // Set context state to ERROR
	wake_up_interruptible(&cb->sem);  // Wake up any waiting threads
}


static int nmsg_accept(void)
{
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PRE "rdma_accept error: %d\n", ret);
		return ret;
	}


    wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
    if (cb->state == ERROR) {
        printk(KERN_ERR PRE "wait for CONNECTED state %d\n", 
            cb->state);
        return -1;
    }
	
    DEBUG_LOG("client connected\n");

	return 0;
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

    ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
    if (!ret)
        cb->qp = cb->child_cm_id->qp;

    return ret;
}

static u32 nmsg_rdma_rkey(u64 buf)
{
    u32 rkey;
    const struct ib_send_wr *bad_wr;
    int ret;
    struct scatterlist sg = {0};

    // cb->invalidate_wr.ex.invalidate_rkey = cb->reg_mr->rkey;

    /*
     * Update the reg key.
     */
    ib_update_fast_reg_key(cb->reg_mr, ++cb->key);
    cb->reg_mr_wr.key = cb->reg_mr->rkey;

    /*
     * Update the reg WR with new buf info.
     */
    cb->reg_mr_wr.access = IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE;
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

    cb->rdma_sge.addr = cb->rdma_dma_addr;
    cb->rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
    cb->rdma_wr.wr.sg_list = &cb->rdma_sge;
    cb->rdma_wr.wr.num_sge = 1;

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
 *
 */
static int nmsg_setup_buffers(void)
{
    int ret;

    // allocate the receive buffer
    cb->recv_dma_addr = ib_dma_map_single(cb->pd->device, &cb->recv_buf, sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);

    // allocate the send buffer
    cb->send_dma_addr = ib_dma_map_single(cb->pd->device, &cb->send_buf, sizeof(cb->send_buf), DMA_BIDIRECTIONAL);

    // allocate the rdma buffer (memory contiguous)
    cb->rdma_buf = dma_alloc_coherent(cb->pd->device->dma_device, cb->message_size, &cb->rdma_dma_addr, GFP_KERNEL);
    if (!cb->rdma_buf)
    {
        DEBUG_LOG(PRE "rdma_buf allocation failed\n");
        ret = -ENOMEM;
        goto bail;
    }
    
    cb->page_list_len = (((cb->message_size - 1) & PAGE_MASK) + PAGE_SIZE) >> PAGE_SHIFT;
    cb->reg_mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, cb->page_list_len);


    if (IS_ERR(cb->reg_mr))
    {
        ret = PTR_ERR(cb->reg_mr);
        DEBUG_LOG(PRE "recv_buf reg_mr failed %d\n", ret);
        goto bail;
    }
    DEBUG_LOG(PRE "reg rkey 0x%x page_list_len %u\n", cb->reg_mr->rkey, cb->page_list_len);

    nmsg_setup_wr();
    DEBUG_LOG(PRE "allocated & registered buffers...\n");
    return 0;
bail:
    if (cb->reg_mr && !IS_ERR(cb->reg_mr))
        ib_dereg_mr(cb->reg_mr);
    if (cb->rdma_buf)
        dma_free_coherent(cb->pd->device->dma_device, cb->message_size, cb->rdma_buf, cb->rdma_dma_addr);
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

    dma_free_coherent(cb->pd->device->dma_device, cb->message_size, cb->rdma_buf, cb->rdma_dma_addr);
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


static int nmsg_bind_server(void){
    struct sockaddr_storage sin;
    int ret;

    fill_sockaddr(&sin);

    ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
    if (ret) {
		printk(KERN_ERR PRE "rdma_bind_addr error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("rdma_bind_addr successful\n");

	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		printk(KERN_ERR PRE "rdma_listen failed: %d\n", ret);
		return ret;
	}
	DEBUG_LOG("rdma_listen successful\n");
	return 0;
}


static int nmsg_run_server(void)
{
	struct ib_send_wr inv;
	const struct ib_send_wr *bad_wr;
	int ret = 0;
    /* Wait for client's Start STAG/TO/Len */
    wait_event_interruptible(cb->sem, cb->state >= RDMA_READ_ADV);
    if (cb->state != RDMA_READ_ADV) {
        printk(KERN_ERR PRE "wait for RDMA_READ_ADV state %d\n",
            cb->state);
        return -1;
    }

    DEBUG_LOG("server received sink adv\n");

    cb->rdma_wr.rkey = cb->remote_rkey;
    cb->rdma_wr.remote_addr = cb->remote_addr;
    cb->rdma_wr.wr.sg_list->length = cb->remote_len;
    cb->rdma_sge.lkey = nmsg_rdma_rkey(cb->rdma_dma_addr);
    cb->rdma_wr.wr.next = NULL;

    /* Issue RDMA Read. */
    cb->rdma_wr.wr.opcode = IB_WR_RDMA_READ;
    /* 
    * Immediately follow the read with a 
    * fenced LOCAL_INV.
    */
    cb->rdma_wr.wr.next = &inv;
    memset(&inv, 0, sizeof inv);
    inv.opcode = IB_WR_LOCAL_INV;
    inv.ex.invalidate_rkey = cb->reg_mr->rkey;
    inv.send_flags = IB_SEND_FENCE;


    ret = ib_post_send(cb->qp, &cb->rdma_wr.wr, &bad_wr);
    if (ret) {
        printk(KERN_ERR PRE "post send error %d\n", ret);
        return ret;
    }
    cb->rdma_wr.wr.next = NULL;

    DEBUG_LOG("server posted rdma read req \n");

    /* Wait for read completion */
    wait_event_interruptible(cb->sem, cb->state >= RDMA_READ_COMPLETE);
    if (cb->state != RDMA_READ_COMPLETE) {
        printk(KERN_ERR PRE "wait for RDMA_READ_COMPLETE state %d\n", cb->state);
        return ret;
    }
    DEBUG_LOG("server received read complete\n");

    /* Display data in recv buf */
    printk(KERN_INFO PRE "server ping data (64B max): |%s|\n", cb->rdma_buf);

    cb->state = CONNECTED;
    cb->send_wr.ex.invalidate_rkey = cb->remote_rkey;
    cb->send_wr.opcode = IB_WR_SEND_WITH_INV;

    ret = ib_post_send(cb->qp, &cb->send_wr, &bad_wr);
    if (ret) {
        printk(KERN_ERR PRE "post send error %d\n", ret);
		return ret;
    }
	DEBUG_LOG("server posted go ahead\n");
    return 0;
}


int nmsg_connect_client(void)
{
    wait_event_interruptible(cb->sem, cb->state >= CONNECT_REQUEST);
	if (cb->state != CONNECT_REQUEST) {
		printk(KERN_ERR PRE "wait for CONNECT_REQUEST state %d\n",
			cb->state);
		return -1;
	}

	if (!reg_supported(cb->child_cm_id->device))
		return -EINVAL;
    
    DEBUG_LOG(PRE "Client connected\n");
    return 0;
}


int nmsg_server_setup(void)
{
    const struct ib_recv_wr *bad_wr;
	int ret;

	ret = nmsg_connect_client();    
    if (ret) {
        printk(PRE "Unable to connect client\n");
        return 0;
    }
    // setup qp
    ret = nmsg_setup_qp(cb->child_cm_id);
    if (ret)
    {
        printk(KERN_ERR PRE "setup_qp failed: %d\n", ret);
        goto err0;
    }

    DEBUG_LOG("setup_qp successful\n");

    // setup buffers
	ret = nmsg_setup_buffers();
	if (ret) {
		printk(KERN_ERR PRE "setup_buffers failed: %d\n", ret);
		goto err1;
	}

    DEBUG_LOG("setup_buffers successful\n");

    // post recv
    ret = ib_post_recv(cb->qp, &cb->recv_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PRE "ib_post_recv failed: %d\n", ret);
		goto err2;
	}
    DEBUG_LOG("ib_post_recv successful\n");

    return ret;
err2:
	nmsg_free_buffers();
err1:
	nmsg_free_qp();
err0:
	rdma_destroy_id(cb->child_cm_id);
    
    return ret;
}


int nmsg_start(void* cmd)
{
    int ret = 0;

    DEBUG_LOG(PRE "Started nmsg_start function in thread\n");

    ret = nmsg_server_setup();
    if (ret){
        printk(KERN_ERR PRE "nmsg_server_setup failed with code %d\n", ret);
        return ret;
    }

    // accept
    ret = nmsg_accept();
    if (ret) {
        printk(KERN_ERR PRE "connect error %d\n", ret);
        goto err;
    }
    DEBUG_LOG("nmsg_accept successful\n");

    while (!ret) {
        // start server
        ret = nmsg_run_server();
    }

err:
    nmsg_free_buffers();
    nmsg_free_qp();
    rdma_destroy_id(cb->child_cm_id);

    return ret;
}

/*
 * Read proc returns stats for each device.
 */
int nmsg_write_stats(struct seq_file *seq, void *v)
{
	// int num = 1;

	// DEBUG_LOG(KERN_INFO PRE "Writing Stats to /proc/nmsg\n");
    // if (cb->pd) {
    //     seq_printf(seq,
    //             "%d-%s %lld %lld %lld %lld %lld %lld %lld %lld\n",
    //             num++, cb->pd->device->name, cb->stats.send_bytes,
    //             cb->stats.send_msgs, cb->stats.recv_bytes,
    //             cb->stats.recv_msgs, cb->stats.write_bytes,
    //             cb->stats.write_msgs,
    //             cb->stats.read_bytes,
    //             cb->stats.read_msgs);
    // } else {
    //     seq_printf(seq, "%d listen\n", num++);
    // }
	return 0;
}

static void nmsg_pre_setup(int port, const char* addr)
{
    cb = kmalloc(sizeof(struct nmsg_server_handler), GFP_KERNEL);

    cb->addr_str = kmalloc(strlen(addr), GFP_KERNEL);
    strcpy(cb->addr_str, addr);
    in4_pton(addr, -1, cb->addr, -1, NULL);
    cb->addr_type = AF_INET;
            
    cb->state = IDLE;
    cb->message_size = 1024;
    init_waitqueue_head(&cb->sem);
    DEBUG_LOG(KERN_INFO PRE "init wait queue\n");

    cb->port = htons(port);
}

int nmsg_pre_start(int port, const char* addr)
{
    int ret = 0;

    nmsg_pre_setup(port, addr);

    cb->cm_id = rdma_create_id(&init_net, nmsg_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
    if (IS_ERR(cb->cm_id))
    {
        ret = PTR_ERR(cb->cm_id);
        printk(KERN_ERR PRE "rdma_create_id error %d\n", ret);
        return ret;
    }
    DEBUG_LOG("created cm_id %p\n", cb->cm_id);

    ret = nmsg_bind_server();
    if (ret)
        return ret;
    
    DEBUG_LOG("bind_server successful\n");

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


int __nmsg_init_server(void * data)
{
    return nmsg_start(data);
}

int nmsg_start_server(const char* addr, const int port)
{
    int ret = 0;
    ret = nmsg_pre_start(port, addr);
    if (ret){
        printk(KERN_ERR PRE "NMSG Failed to start with config: port=%d addr=%s", port, addr);
        return ret;
    }

    cb->thread = kthread_create(__nmsg_init_server, NULL, "nmsg_server");
    if (IS_ERR(cb->thread)){
        printk(KERN_ERR "Failed to start server\n");
        return PTR_ERR(cb->thread);
    }
    DEBUG_LOG(KERN_INFO PRE "Starting nmsg_start function...\n");

    wake_up_process(cb->thread);
    return ret;
}

void nmsg_process_message(char* message)
{
    
}

void nmsg_stop_server(void)
{
    if (cb->thread) {
        send_sig(SIGKILL, cb->thread, 1);
    }

    nmsg_free();
}