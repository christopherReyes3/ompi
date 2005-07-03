/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "ompi_config.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/errno.h>
#include "types.h"
#include "datatype/datatype.h"
#include "mca/pml/base/pml_base_sendreq.h"
#include "mca/pml/base/pml_base_recvreq.h"
#include "mca/ptl/base/ptl_base_recvfrag.h"
#include "ptl_elan.h"
#include "ptl_elan_peer.h"
#include "ptl_elan_proc.h"
#include "ptl_elan_frag.h"
#include "ptl_elan_priv.h"

static void
mca_ptl_elan_data_frag (struct mca_ptl_elan_module_t *ptl,
                        mca_ptl_base_header_t * header)
{
    /* Allocate a recv frag descriptor */
    mca_ptl_elan_recv_frag_t *recv_frag;
    opal_list_item_t *item;

    bool        matched; 
    int         rc = OMPI_SUCCESS;

    OMPI_FREE_LIST_GET (&mca_ptl_elan_component.elan_recv_frags_free, 
                        item, rc);

    while (OMPI_SUCCESS != rc) {
	/* FIXME: progress the recv state machine */
        ompi_output (0,
                     "[%s:%d] Retry to allocate a recv fragment",
                     __FILE__, __LINE__);
	OMPI_FREE_LIST_GET (&mca_ptl_elan_component.elan_recv_frags_free, 
                item, rc);
    } 

    recv_frag = (mca_ptl_elan_recv_frag_t *) item;
    recv_frag->frag_recv.frag_base.frag_owner = (mca_ptl_base_module_t *) ptl;
    /* XXX: 
     * Another problem caused by TCP oriented PML.
     * a) Since elan is not connection oriented, 
     *    No information about which peer until checking the header
     *    Somewhere after the frag is matched, this peer information needs
     *    to be filled in so that ACK can be sent out.
     * b) Possibly, another drawback of hooking the ack to the particular 
     *    recv fragment. If the ack fragment is not hooked this way,
     *    PML will provide the peer information when the ack is requested.
     * c) What if the recv request specifies MPI_ANY_SOURCE, then 
     *    for the handshaking to complete, peer should be fixed the
     *    handshaking. Then in this case, PML needs information from
     *    PTL to know about which peer this data is from.
     *    So PTL has to provide the peer information to PML.
     */
    recv_frag->frag_recv.frag_base.frag_peer = NULL;
    recv_frag->frag_recv.frag_request = NULL;
    recv_frag->frag_recv.frag_is_buffered = false;
    recv_frag->frag_hdr_cnt = 0;
    recv_frag->frag_msg_cnt = 0;
    recv_frag->frag_ack_pending = false;
    recv_frag->frag_progressed = 0;

    /* Copy the header, mca_ptl_base_match() does not do what it claims */
    recv_frag->frag_recv.frag_base.frag_header = *header;

    LOG_PRINT(PTL_ELAN_DEBUG_RECV,
	    "[recv header...] type %d src_ptr %p dst_ptr %p data %x\n", 
	    header->hdr_common.hdr_type,
	    header->hdr_frag.hdr_src_ptr.pval,
	    header->hdr_frag.hdr_dst_ptr.pval,
	    (int)((char *) header + sizeof (mca_ptl_base_header_t)));

    /* Taking the data starting point be default */
    recv_frag->frag_recv.frag_base.frag_addr = 
	(char *) header + sizeof (mca_ptl_base_header_t);
    recv_frag->frag_recv.frag_base.frag_size = header->hdr_frag.hdr_frag_length;

    /* match with preposted requests */
    matched = ptl->super.ptl_match(
	    recv_frag->frag_recv.frag_base.frag_owner,
	    &recv_frag->frag_recv,
	    &recv_frag->frag_recv.frag_base.frag_header.hdr_match); 
    
    if (!matched) {
	/* TODO: 
	 *   Next version need to do this only when it is 
	 *   blocking the recv queue */
        memcpy (recv_frag->unex_buff,
                (char *) header + sizeof (mca_ptl_base_header_t),
                header->hdr_frag.hdr_frag_length);
        recv_frag->frag_recv.frag_is_buffered = true;
        recv_frag->frag_recv.frag_base.frag_addr = recv_frag->unex_buff;
    } 
}

static void
mca_ptl_elan_last_frag (struct mca_ptl_elan_module_t *ptl,
                        mca_ptl_base_header_t * hdr)
{
    mca_pml_base_recv_request_t *request; 

    request = (mca_pml_base_recv_request_t*) hdr->hdr_frag.hdr_dst_ptr.pval; 
    /*pml_req->req_peer_match;*/

    LOG_PRINT(PTL_ELAN_DEBUG_ACK,
	    " local req %p addr %p, length %d\n", 
	    (void *)request, request->req_base.req_addr,
	    hdr->hdr_frag.hdr_frag_length);

    ptl->super.ptl_recv_progress ( 
	    (mca_ptl_base_module_t *) ptl, 
	    request, 
	    hdr->hdr_frag.hdr_frag_length,
	    hdr->hdr_frag.hdr_frag_length);

}

static void
mca_ptl_elan_last_frag_ack (struct mca_ptl_elan_module_t *ptl,
                        mca_ptl_base_header_t * header)
{
     mca_ptl_elan_send_frag_t* desc;
     mca_pml_base_send_request_t* req;

 
     LOG_PRINT(PTL_ELAN_DEBUG_ACK, "desc %p \n",
		header->hdr_ack.hdr_src_ptr.pval);

     /* XXX: Check if the sourc pointer points to the correct fragment */
     desc = (mca_ptl_elan_send_frag_t*) header->hdr_ack.hdr_src_ptr.pval;
     req  = (mca_pml_base_send_request_t *) desc->desc->req;

     req->req_peer_match = header->hdr_ack.hdr_dst_match;
     req->req_peer_addr  = header->hdr_ack.hdr_dst_addr;
     req->req_peer_size  = header->hdr_ack.hdr_dst_size;

     LOG_PRINT(PTL_ELAN_DEBUG_ACK, 
	     "desc %p remote req %p addr %p, len %d\n", 
	     header->hdr_ack.hdr_src_ptr.pval,
	     req->req_peer_match.pval,
	     req->req_peer_addr.pval,
	     req->req_peer_size);

    if(opal_atomic_fetch_and_set_int (&desc->frag_progressed, 1) == 0) {
	ptl->super.ptl_send_progress(
	       	(struct mca_ptl_base_module_t*) ptl,
		req, header->hdr_ack.hdr_dst_size);
    }

    /* Return a frag or if not cached, or it is a follow up */ 
    if ( (desc->desc->desc_status != MCA_PTL_ELAN_DESC_CACHED)){
	if (desc->desc->desc_type == MCA_PTL_ELAN_DESC_PUT) {
	    OMPI_FREE_LIST_RETURN (&ptl->putget->put_desc_free,
		    (opal_list_item_t *) desc);
	} else {
	    OMPI_FREE_LIST_RETURN (&ptl->queue->tx_desc_free,
		    (opal_list_item_t *) desc);
	}
    } else {
	LOG_PRINT(PTL_ELAN_DEBUG_ACK,
		"PML will return frag to list %p, length %d\n", 
		(void*)&ptl->queue->tx_desc_free,
		ptl->queue->tx_desc_free.super.opal_list_length);
    }

}

static void
mca_ptl_elan_ctrl_frag (struct mca_ptl_elan_module_t *ptl,
                        mca_ptl_base_header_t * header)
{
     /* TODO:
      * 0) First of all, no need to allocate recv frag descriptors, 
      *    since control packet does not contain data.
      * 1) Update the send request, which will start successive fragments 
      *    if it is an ACK and there are more data to go.
      * XXX: Resend if it is a NACK, which layer PML/PTL to handle that?
      */
     mca_ptl_elan_send_frag_t* desc;
     mca_pml_base_send_request_t* req;

     desc = (mca_ptl_elan_send_frag_t*) header->hdr_ack.hdr_src_ptr.pval;
     req  = (mca_pml_base_send_request_t *) desc->desc->req;

     req->req_peer_match = header->hdr_ack.hdr_dst_match;
     req->req_peer_addr  = header->hdr_ack.hdr_dst_addr;
     req->req_peer_size  = header->hdr_ack.hdr_dst_size;

     LOG_PRINT(PTL_ELAN_DEBUG_ACK, "desc %p remote req %p addr %p, len %d\n", 
	    (void*)desc,
	    req->req_peer_match.pval, 
	    (void*)req->req_peer_addr.pval, 
	    req->req_peer_size);

     /* XXX: This sort of synchronized fragment release will lead
      * to race conditions, also see the note insize the follwoing routine */
     mca_ptl_elan_send_desc_done (desc, req); 
}

static void
mca_ptl_elan_init_qdma_desc (struct mca_ptl_elan_send_frag_t *frag,
                             mca_ptl_elan_module_t * ptl,
			     struct mca_ptl_elan_peer_t *ptl_peer,
                             mca_pml_base_send_request_t *pml_req,
			     size_t offset,
			     size_t *size,
			     int flags)
{
    int         header_length;
    int         destvp;
    int         size_out;
    int         size_in;
    int         rc = OMPI_SUCCESS;
   
    mca_ptl_base_header_t *hdr;
    struct ompi_ptl_elan_qdma_desc_t * desc;
    ELAN4_CTX  *ctx;

    desc   = (ompi_ptl_elan_qdma_desc_t *)frag->desc;
    destvp = ptl_peer->peer_vp;
    size_in = *size;
    ctx = ptl->ptl_elan_ctx, 

    hdr = (mca_ptl_base_header_t *) & desc->buff[0];

    if(offset == 0) {
	hdr->hdr_common.hdr_type = MCA_PTL_HDR_TYPE_MATCH;
	hdr->hdr_common.hdr_flags = flags;
	hdr->hdr_common.hdr_size = sizeof (mca_ptl_base_match_header_t);
	hdr->hdr_frag.hdr_frag_offset = offset;
	hdr->hdr_frag.hdr_frag_seq = 0;
	/* Frag descriptor, so that incoming ack will locate it */
        hdr->hdr_frag.hdr_src_ptr.lval = 0;
	hdr->hdr_frag.hdr_src_ptr.pval = frag; 
	/* Stash local buffer address into the header, for ptl_elan_get */
	hdr->hdr_frag.hdr_dst_ptr.pval = 0;
	hdr->hdr_frag.hdr_dst_ptr.lval = elan4_main2elan(
	       	ctx, pml_req->req_base.req_addr);

	hdr->hdr_match.hdr_contextid = pml_req->req_base.req_comm->c_contextid;
	hdr->hdr_match.hdr_src = pml_req->req_base.req_comm->c_my_rank;
	hdr->hdr_match.hdr_dst = pml_req->req_base.req_peer;
	hdr->hdr_match.hdr_tag = pml_req->req_base.req_tag;
	hdr->hdr_match.hdr_msg_length = pml_req->req_bytes_packed;
	hdr->hdr_match.hdr_msg_seq = pml_req->req_base.req_sequence;
	header_length = sizeof (mca_ptl_base_match_header_t);
    } else {
	hdr->hdr_common.hdr_type = MCA_PTL_HDR_TYPE_FRAG;
	hdr->hdr_common.hdr_flags = flags;
	hdr->hdr_common.hdr_size = sizeof (mca_ptl_base_frag_header_t);
	hdr->hdr_frag.hdr_frag_offset = offset;
	hdr->hdr_frag.hdr_frag_seq = 0;
        hdr->hdr_frag.hdr_src_ptr.lval = 0;
	hdr->hdr_frag.hdr_src_ptr.pval = frag; /* Frag descriptor */
        hdr->hdr_frag.hdr_dst_ptr = pml_req->req_peer_match;
	header_length = sizeof (mca_ptl_base_frag_header_t);
    }

    LOG_PRINT(PTL_ELAN_DEBUG_SEND, 
	    "req %p frag %p dst_ptr %p addr %p offset %d \n",
	    (void*)pml_req,
	    (void*)hdr->hdr_frag.hdr_src_ptr.pval,
	    (void*)hdr->hdr_frag.hdr_dst_ptr.pval,
	    (void*)pml_req->req_base.req_addr, offset);

    /* initialize convertor */
    if(size_in > 0) {
#if !OMPI_PTL_ELAN_USE_DTP
	memcpy(&desc->buff[header_length], 
		pml_req->req_base.req_addr, size_in);
	size_out = size_in;
#else
	int iov_count = 1, max_data = size_in;
	int freeAfter = 0;
	struct iovec iov;
        ompi_convertor_t *convertor;

        convertor = &frag->frag_base.frag_convertor;
        ompi_convertor_copy(&pml_req->req_convertor, convertor);
        ompi_convertor_init_for_send(convertor,
				    0, 
				    pml_req->req_base.req_datatype,
				    pml_req->req_base.req_count,
				    pml_req->req_base.req_addr,
				    offset,
				    NULL);

	/* For now, eager sends are always packed into the descriptor
         * TODO: Inline up to 256 bytes (including the header), then
	 *       do a chained send for mesg < first_frag_size */
        iov.iov_base = &desc->buff[header_length];
        iov.iov_len  = size_in;
        if((rc = ompi_convertor_pack(convertor, 
			&iov, &iov_count, &max_data, &freeAfter)) < 0) {
	    ompi_output (0, "[%s:%d] Unable to pack data\n",
			 __FILE__, __LINE__);
            return;
	}
        size_out = max_data;
#endif
    } else {
	size_out = size_in;
    }

    *size = size_out;
    hdr->hdr_frag.hdr_frag_length = size_out;

    /* TODO: change this ugly fix for get */
    hdr->hdr_frag.hdr_dst_ptr.lval = elan4_main2elan(
	    ctx, (char *)pml_req->req_base.req_addr + size_out);

    /* TODO: 
     *     For now just save the information to the provided header 
     *     Later will use the inline header to report the progress */
    frag->frag_base.frag_header = *hdr; 

#if OMPI_PTL_ELAN_COMP_QUEUE
    /* XXX: The chain dma will go directly into a command stream
     * so we need addend the command queue control bits.
     * Allocate space from command queues hanged off the CTX.
     */
    PTL_ELAN4_GET_QBUFF (desc->comp_event->ev_Params[1], ctx, 8, CQ_Size8K);
    desc->comp_event->ev_CountAndType = E4_EVENT_INIT_VALUE(-32,
	    E4_EVENT_COPY, E4_EVENT_DTYPE_LONG, 8);
    desc->comp_dma.dma_cookie   = elan4_local_cookie(ptl->queue->tx_cpool,
	    E4_COOKIE_TYPE_LOCAL_DMA, ptl->elan_vp);

#if OMPI_PTL_ELAN_ONE_QUEUE
    frag->frag_base.frag_header.hdr_common.hdr_type += 8;
#endif

    desc->comp_dma.dma_srcAddr  = elan4_main2elan (ctx, 
	    (void *) &frag->frag_base.frag_header);
    memcpy ((void *)desc->comp_buff, (void *)&desc->comp_dma, 
	    sizeof (E4_DMA64));

    /* Initialize some of the dma structures 
     * XXX: Hardcoded DMA retry count */
    desc->main_dma.dma_typeSize = E4_DMA_TYPE_SIZE ((header_length +
                                                     size_out),
                                                    DMA_DataTypeByte,
                                                    DMA_QueueWrite, 16);
    desc->main_dma.dma_vproc = destvp;
    desc->main_dma.dma_cookie= elan4_local_cookie (ptl->queue->tx_cpool, 
	    E4_COOKIE_TYPE_LOCAL_DMA, destvp);
    desc->main_dma.dma_srcEvent= elan4_main2elan(ctx, desc->comp_event);
    desc->main_dma.dma_srcAddr = MAIN2ELAN (ctx, &desc->buff[0]);

    LOG_PRINT (PTL_ELAN_DEBUG_SEND,
	    " desc %p comp_buff %p comp_event %p "
	    "comp src_addr %x main dst_addr %x size %d\n",
	    desc, 
	    (void *)desc->comp_buff,
	    (void *)desc->comp_event,
	    (int)desc->comp_dma.dma_srcAddr, 
	    (int)desc->main_dma.dma_srcAddr, 
	    size_out);           
#else
    desc->main_dma.dma_srcAddr = MAIN2ELAN (ctx, &desc->buff[0]);
    /* XXX: Hardcoded DMA retry count */
    desc->main_dma.dma_typeSize = E4_DMA_TYPE_SIZE ((header_length +
                                                     size_out),
                                                    DMA_DataTypeByte,
                                                    DMA_QueueWrite, 16);
    desc->main_dma.dma_cookie = elan4_local_cookie (ptl->queue->tx_cpool, 
	    E4_COOKIE_TYPE_LOCAL_DMA, destvp);
    desc->main_dma.dma_vproc = destvp;
#endif

    LOG_PRINT (PTL_ELAN_DEBUG_SEND,
	    "dest events main %lx \n",
	    desc->main_dma.dma_dstEvent);

    /* Make main memory coherent with IO domain (IA64) */
    MEMBAR_VISIBLE ();
}

static void
mca_ptl_elan_init_put_desc (struct mca_ptl_elan_send_frag_t *frag,
                               mca_ptl_elan_module_t * ptl,
			       struct mca_ptl_elan_peer_t *ptl_peer,
                               mca_pml_base_send_request_t *pml_req,
			       size_t offset,
			       size_t *size,
			       int flags)
{
    int         destvp;
    int         size_out;
    int         size_in;
    int         rc = OMPI_SUCCESS;

    ELAN4_CTX   *ctx;
    struct ompi_ptl_elan_putget_desc_t * desc;
    mca_ptl_base_header_t *hdr;

    hdr = &frag->frag_base.frag_header;
    desc   = (ompi_ptl_elan_putget_desc_t *)frag->desc;
    destvp = ptl_peer->peer_vp;
    size_in = *size;
    ctx =  ptl->ptl_elan_ctx;

    hdr->hdr_common.hdr_type = MCA_PTL_HDR_TYPE_FIN;
    hdr->hdr_common.hdr_flags = flags;
    hdr->hdr_common.hdr_size = sizeof(mca_ptl_base_frag_header_t);
    hdr->hdr_frag.hdr_frag_offset = offset;
    hdr->hdr_frag.hdr_frag_seq = 0;
    hdr->hdr_frag.hdr_src_ptr.lval = 0; 
    hdr->hdr_frag.hdr_src_ptr.pval = frag; /* No need to hook a frag */
    hdr->hdr_frag.hdr_dst_ptr = pml_req->req_peer_match;
    hdr->hdr_frag.hdr_frag_length = size_in;

    /* FIXME: provide a fix according to data contiguity */
    desc->src_elan_addr = elan4_main2elan (ctx, 
	    ((char *)pml_req->req_base.req_addr + offset));
    desc->dst_elan_addr = (E4_Addr)pml_req->req_peer_addr.lval;
    desc->desc_buff = hdr;
    LOG_PRINT(PTL_ELAN_DEBUG_PUT, " remote req %p addr %lx, length %d\n", 
		pml_req->req_peer_match.pval, 
		(long)pml_req->req_peer_addr.lval, 
		pml_req->req_peer_size);

    /* FIXME: initialize convertor and get the fragment copied out */
#if OMPI_PTL_ELAN_USE_DTP 
    if(size_in > 0 && 0) {
	int iov_count = 1, max_data = size_in;
	int freeAfter = 0;
	struct iovec iov;
        ompi_convertor_t *convertor;

       convertor = &frag->frag_base.frag_convertor;
       ompi_convertor_copy(&pml_req->req_convertor, convertor);
       ompi_convertor_init_for_send(convertor,
				    0, 
				    pml_req->req_base.req_datatype,
				    pml_req->req_base.req_count,
				    pml_req->req_base.req_addr,
				    offset,
				    NULL);
	/* TODO: 
	 *   For now, eager sends are always packed into the descriptor
	 *   Inline up to 256 bytes (including the header), then
	 *   do a chained send for mesg < first_frag_size */

        iov.iov_base = NULL; //desc->desc_buff;
        iov.iov_len  = size_in;
        if((rc = ompi_convertor_pack(convertor, 
			&iov, &iov_count, &max_data, &freeAfter)) < 0) {
	    ompi_output (0, "[%s:%d] Unable to pack data\n",
			 __FILE__, __LINE__);
            return;
	}

	/* FIXME: please find if the new memory is allocated 
	   but this info needs to be obtained from convertor.
	   Not sure if convertor has a hook to do that.
	   Then, accordingly need to find out the elan address */
#if 0
	desc->src_elan_addr = elan4_main2elan(ctx, iov.iov_base);
	desc->desc_buff = iov.iov_base;
#endif
        size_out = iov.iov_len;
    } else
#endif
    {
	size_out = size_in;
    }

    *size = size_out;
    hdr->hdr_frag.hdr_frag_length = size_out;

    /* Setup the chain dma */
    desc->chain_dma.dma_typeSize = E4_DMA_TYPE_SIZE (
	    sizeof(mca_ptl_base_frag_header_t), 
	    DMA_DataTypeByte, DMA_QueueWrite, 8);
    desc->chain_dma.dma_cookie   = elan4_local_cookie(ptl->putget->pg_cpool,
	    E4_COOKIE_TYPE_LOCAL_DMA, destvp);
    desc->chain_dma.dma_vproc    = destvp;
    desc->chain_dma.dma_srcAddr  = elan4_main2elan (ctx, (void *) hdr);
    desc->chain_dma.dma_dstAddr  = 0x0ULL; 

    /* causes the inputter to redirect the dma to the inputq */
    desc->chain_dma.dma_dstEvent = elan4_main2elan (ctx, 
	    (void *) ptl->queue->input);

    LOG_PRINT (PTL_ELAN_DEBUG_PUT,
	    "dest events main %lx chain %lx comp %lx \n",
	    desc->main_dma.dma_dstEvent, 
	    desc->chain_dma.dma_dstEvent, 
	    desc->comp_dma.dma_dstEvent);

#if OMPI_PTL_ELAN_COMP_QUEUE
    /* XXX: Chain a QDMA to each queue and 
     * Have all the srcEvent fired to the Queue 
     *
     * XXX: The chain dma will go directly into a command stream
     * so we need addend the command queue control bits.
     * Allocate space from command queues hanged off the CTX.
     */
    desc->comp_event->ev_Params[0]    = elan4_main2elan (ctx, 
	    (void *)desc->comp_buff);
    PTL_ELAN4_GET_QBUFF (desc->comp_event->ev_Params[1], ctx, 8, CQ_Size8K);
    desc->comp_event->ev_CountAndType = E4_EVENT_INIT_VALUE(-32,
	    E4_EVENT_COPY, E4_EVENT_DTYPE_LONG, 8);

    desc->comp_dma.dma_cookie   = elan4_local_cookie(ptl->queue->tx_cpool,
	    E4_COOKIE_TYPE_LOCAL_DMA, ptl->elan_vp);

#if OMPI_PTL_ELAN_ONE_QUEUE
    *((mca_ptl_base_header_t *) desc->buff) = *hdr;
    ((mca_ptl_base_header_t *) desc->buff)->hdr_common.hdr_type += 8; 
    desc->comp_dma.dma_srcAddr  = elan4_main2elan (ctx, (void *)desc->buff);
#else
    desc->comp_dma.dma_srcAddr  = elan4_main2elan (ctx, 
	    (void *) &frag->frag_base.frag_header);
#endif
    memcpy ((void *)desc->comp_buff, (void *)&desc->comp_dma, 
	    sizeof (E4_DMA64));

    /* XXX: chain the comp event to the put_dma */
    desc->chain_dma.dma_srcEvent= elan4_main2elan(ctx, desc->comp_event);
#else
    desc->chain_dma.dma_srcEvent = elan4_main2elan (ctx, desc->elan_event);
    INITEVENT_WORD (ctx, (E4_Event *) desc->elan_event, &desc->main_doneWord);
    RESETEVENT_WORD (&desc->main_doneWord);
    PRIMEEVENT_WORD (ctx, (E4_Event *)desc->elan_event, 1);
#endif

    desc->chain_dma.dma_typeSize |= RUN_DMA_CMD;
    desc->chain_dma.dma_pad       = NOP_CMD;

    /* Copy down the chain dma to the chain buffer in elan sdram  */
    memcpy ((void *)desc->chain_buff, (void *)&desc->chain_dma, 
	    sizeof (E4_DMA64));

    desc->chain_event->ev_CountAndType = E4_EVENT_INIT_VALUE(-32,
	    E4_EVENT_COPY, E4_EVENT_DTYPE_LONG, 8);
    desc->chain_event->ev_Params[0]    = elan4_main2elan (ctx, 
	    (void *)desc->chain_buff);

    /* XXX: The chain dma will go directly into a command stream
     * so we need addend the command queue control bits.
     * Allocate space from command queues hanged off the CTX. */
    PTL_ELAN4_GET_QBUFF (desc->chain_event->ev_Params[1], ctx, 8, CQ_Size8K);

    /* Chain an event */
    desc->main_dma.dma_srcEvent= elan4_main2elan(ctx, desc->chain_event);

    desc->main_dma.dma_srcAddr = desc->src_elan_addr;
    desc->main_dma.dma_dstAddr = desc->dst_elan_addr;
    desc->main_dma.dma_dstEvent= 0x0ULL; /*disable remote event */

    flags = 0;
    desc->main_dma.dma_typeSize = E4_DMA_TYPE_SIZE (size_out, 
	    DMA_DataTypeByte, flags, ptl->putget->pg_retryCount);
    desc->main_dma.dma_cookie = elan4_local_cookie (
	    ptl->putget->pg_cpool, 
	    E4_COOKIE_TYPE_LOCAL_DMA, 
	    destvp);
    desc->main_dma.dma_vproc = destvp;

    LOG_PRINT (PTL_ELAN_DEBUG_PUT,
	    "chain_event %p param0 %lx param1 %lx \n",
	    desc->chain_event,
	    desc->chain_event->ev_Params[0],
	    desc->chain_event->ev_Params[1]);

    /* Make main memory coherent with IO domain (IA64) */
    MEMBAR_VISIBLE ();
}

#if OMPI_PTL_ELAN_ENABLE_GET
static void
mca_ptl_elan_init_get_desc (mca_ptl_elan_module_t *ptl, 
			    struct mca_ptl_elan_send_frag_t *frag,
			    mca_ptl_elan_recv_frag_t * recv_frag,
                            mca_pml_base_recv_request_t *pml_req,
			    mca_ptl_base_header_t *hdr,
			    size_t *size,
			    int flags)
{
    int         destvp;
    int         size_out;
    int         size_in;
    size_t      offset;
    ELAN4_CTX   *ctx;
    struct ompi_ptl_elan_putget_desc_t * desc;
    mca_ptl_base_header_t *recv_header;
    struct mca_ptl_elan_peer_t *ptl_peer;

    ctx  =  ptl->ptl_elan_ctx;
    recv_header= &recv_frag->frag_recv.frag_base.frag_header;
    ptl_peer = (struct mca_ptl_elan_peer_t *)
	recv_frag->frag_recv.frag_base.frag_peer,
    offset = pml_req->req_bytes_received, 
    desc = (ompi_ptl_elan_putget_desc_t *)frag->desc;
    destvp  = ptl_peer->peer_vp;
    size_in = *size;

    /* XXX: Match this from the sender addr after the first frag */
    desc->src_elan_addr = recv_header->hdr_frag.hdr_dst_ptr.lval;

    /* XXX: on recv side, from the addr after the first frag */
    desc->dst_elan_addr = hdr->hdr_ack.hdr_dst_addr.lval;
    desc->desc_buff = hdr;

    /* FIXME: initialize convertor and get the fragment 
     * copied with the convertor hanged over request */
    size_out = size_in;
    *size = size_out;

    /* XXX: hdr_frag and hdr_ack overlap partially, please be aware */
    desc->chain_dma.dma_typeSize = E4_DMA_TYPE_SIZE (
	    sizeof(mca_ptl_base_frag_header_t), 
	    DMA_DataTypeByte, DMA_QueueWrite, 8);
    desc->chain_dma.dma_cookie   = elan4_local_cookie(ptl->putget->pg_cpool,
	    E4_COOKIE_TYPE_LOCAL_DMA, destvp);
    desc->chain_dma.dma_vproc    = destvp;
    desc->chain_dma.dma_srcAddr  = elan4_main2elan (ctx, (void *) hdr);
    desc->chain_dma.dma_dstAddr  = 0x0ULL; 
    desc->chain_dma.dma_dstEvent = elan4_main2elan (ctx, 
	    (void *) ptl->queue->input);

    LOG_PRINT(PTL_ELAN_DEBUG_GET,
		"remote frag %p local req %p buffer %p size %d len %d\n",
	       	hdr->hdr_ack.hdr_src_ptr.pval,
	       	hdr->hdr_ack.hdr_dst_match.pval,
	       	hdr->hdr_ack.hdr_dst_addr.pval,
	       	hdr->hdr_ack.hdr_dst_size,
		*size);

#if OMPI_PTL_ELAN_COMP_QUEUE
    /* XXX: The chain dma will go directly into a command stream
     * so we need addend the command queue control bits.
     * Allocate space from command queues hanged off the CTX.
     */
    frag->frag_base.frag_header = *hdr; 
    ((mca_ptl_elan_ack_header_t *) &frag->frag_base.frag_header)->frag = frag; 
    PTL_ELAN4_GET_QBUFF (desc->comp_event->ev_Params[1], ctx, 8, CQ_Size8K);
    desc->comp_event->ev_CountAndType = E4_EVENT_INIT_VALUE(-32,
	    E4_EVENT_COPY, E4_EVENT_DTYPE_LONG, 8);
    desc->comp_dma.dma_cookie   = elan4_local_cookie(ptl->queue->tx_cpool,
	    E4_COOKIE_TYPE_LOCAL_DMA, ptl->elan_vp);

#if OMPI_PTL_ELAN_ONE_QUEUE
    *((mca_ptl_base_header_t *) desc->buff) = *hdr;
    ((mca_ptl_base_header_t *) desc->buff)->hdr_common.hdr_type += 8; 
    desc->comp_dma.dma_srcAddr  = elan4_main2elan (ctx, (void *)desc->buff);
#else
    desc->comp_dma.dma_srcAddr  = elan4_main2elan (ctx, 
	    (void *) &frag->frag_base.frag_header);
#endif
    memcpy ((void *)desc->comp_buff, (void *)&desc->comp_dma, 
	    sizeof (E4_DMA64));

    /* XXX: chain the comp event to the put_dma */
    desc->chain_dma.dma_srcEvent= elan4_main2elan(ctx, desc->comp_event);
#else
    desc->chain_dma.dma_srcEvent = elan4_main2elan (ctx, desc->elan_event);
    INITEVENT_WORD (ctx, (E4_Event *) desc->elan_event, &desc->main_doneWord);
    RESETEVENT_WORD (&desc->main_doneWord);
    PRIMEEVENT_WORD (ctx, (E4_Event *)desc->elan_event, 1);
#endif
    desc->chain_dma.dma_typeSize |= RUN_DMA_CMD;
    desc->chain_dma.dma_pad       = NOP_CMD;

    LOG_PRINT (PTL_ELAN_DEBUG_FLAG,
	    " desc %p chain_buff %p chain_event %p "
	    "src_addr %x dst_addr %x size %d\n",
	    (void*)desc, (void*)desc->chain_buff, 
	    (void*)desc->chain_event, (int)desc->src_elan_addr, 
	    (int)desc->dst_elan_addr, size_out);           

    /* Copy down the chain dma to the chain buffer in elan sdram  */
    memcpy ((void *)desc->chain_buff, (void *)&desc->chain_dma, 
	    sizeof (E4_DMA64));

    desc->chain_event->ev_CountAndType = E4_EVENT_INIT_VALUE(-32,
	    E4_EVENT_COPY, E4_EVENT_DTYPE_LONG, 8);
    desc->chain_event->ev_Params[0]    = elan4_main2elan (ctx, 
	    (void *)desc->chain_buff);

    /* XXX: The chain dma will go directly into a command stream
     * so we need addend the command queue control bits.
     * Allocate space from command queues hanged off the CTX.  */
    PTL_ELAN4_GET_QBUFF (desc->chain_event->ev_Params[1], ctx, 8, CQ_Size8K);

    /* Chain an event */
    desc->main_dma.dma_dstEvent= elan4_main2elan(ctx, 
	    (E4_Event *)desc->chain_event);

    desc->main_dma.dma_srcAddr = desc->src_elan_addr;
    desc->main_dma.dma_dstAddr = desc->dst_elan_addr;
    desc->main_dma.dma_srcEvent= 0x0ULL; /*disable remote event */

    flags = 0;
    desc->main_dma.dma_typeSize = E4_DMA_TYPE_SIZE (size_out, 
	    DMA_DataTypeByte, flags, ptl->putget->pg_retryCount);
    desc->main_dma.dma_cookie = elan4_remote_cookie(
	    ptl->putget->pg_cpool, 
	    E4_COOKIE_TYPE_REMOTE_DMA, 
	    destvp);
    desc->main_dma.dma_vproc = ptl->elan_vp; /* target is self */
    LOG_PRINT(PTL_ELAN_DEBUG_GET, 
	    "destvp %d type %d flag %d size %d\n", 
	    destvp, hdr->hdr_common.hdr_type,
	    hdr->hdr_common.hdr_flags,
	    hdr->hdr_common.hdr_size);
}
#endif /* End of OMPI_PTL_ELAN_ENABLE_GET */


/* XXX: a customized elan_waitWord function that do without a ELAN_STATE input,
 * Need to replace with elan4_waitevent_word () */
int         
mca_ptl_elan_wait_queue(mca_ptl_elan_module_t * ptl,
       	ompi_ptl_elan_recv_queue_t *rxq, long usecs)
{
    RAIL   *rail; 
    ELAN4_CTX  *ctx; 
    ADDR_SDRAM  ready;
    EVENT_WORD *readyWord; 

    rail  = (RAIL *)ptl->ptl_elan_rail;
    ctx   = ptl->ptl_elan_ctx;
    ready = rxq->qr_qEvent;
    readyWord = &rxq->qr_doneWord;

    LOG_PRINT(PTL_ELAN_DEBUG_NONE,
	    "rail %p ctx %p ready %p readyWord %p\n",
	    (void*)rail, (void*)ctx, (void*)ready, (void*)readyWord);

    /* Poll for usec (at least one), then go to sleep. */
    if (elan4_pollevent_word(ctx, readyWord, usecs)) {
	return 0xdeadbeef;
    }

    LOG_PRINT(PTL_ELAN_DEBUG_NONE,
	    "eventWord TIMED_OUT: ready %lx [%d.%x] \n",
	    ready, 
	    EVENT_COUNT(((EVENT32 *)(ready))),
	    EVENT_TYPE(((EVENT32 *)(ready))));

    /* XXXX Temporary Elan4 blocking wait code */
    {
	ELAN_SLEEP     *es;
	OPAL_LOCK(&mca_ptl_elan_component.elan_lock);
	if ((es = rail->r_sleepDescs) == NULL)
	    es = ompi_init_elan_sleepdesc (&mca_ptl_elan_global_state, rail);
	else
	    rail->r_sleepDescs = es->es_next;
	OPAL_UNLOCK(&mca_ptl_elan_component.elan_lock);

	LOG_PRINT(PTL_ELAN_DEBUG_NONE,
		"eventWord(%p): es %p cookie %x cmdq %p ecmdq %p\n",
		(void*)ready, (void*)es, es->es_cookie, (void*)es->es_cmdq, (void*)es->es_ecmdq);
	WAITEVENT_WORD(ctx, es->es_cmdq, es->es_ecmdq, es->es_cmdBlk, 
		es->es_cookie, ready, readyWord, usecs);

	OPAL_LOCK(&mca_ptl_elan_component.elan_lock);
	es->es_next = rail->r_sleepDescs;
	rail->r_sleepDescs = es;
	OPAL_UNLOCK(&mca_ptl_elan_component.elan_lock);
    }
    return 0xdeadbeef;
}

#if OMPI_PTL_ELAN_ENABLE_GET && defined (HAVE_GET_INTERFACE)
int
mca_ptl_elan_start_get (mca_ptl_elan_send_frag_t * frag,
			struct mca_ptl_elan_peer_t *ptl_peer,
			struct mca_pml_base_recv_request_t *request,
			size_t offset,
			size_t *size,
			int flags)
{
    mca_ptl_elan_module_t *ptl;
    struct ompi_ptl_elan_putget_desc_t *gdesc;

    ptl = ptl_peer->peer_ptl;
    gdesc = (ompi_ptl_elan_putget_desc_t *)frag->desc;
    mca_ptl_elan_init_get_desc (ptl, frag, ptl_peer, 
	    req, offset, size, flags);

    /* XXX: 
     * Trigger a STEN packet to the remote side and then from there
     * a elan_get is triggered
     * Not sure which remote queue is being used by GET_DMA here */
    elan4_remote_dma(elan_ptl->putget->get_cmdq, 
	    (E4_DMA *)&gdesc->main_dma, destvp, 
	    elan4_local_cookie(elan_ptl->putget->pg_cpool,
	       	E4_COOKIE_TYPE_STEN , destvp));
    elan4_flush_cmdq_reorder (elan_ptl->putget->get_cmdq);
    MEMBAR_DRAIN();
    opal_list_append (&elan_ptl->send_frags, (opal_list_item_t *) frag);

    /* XXX: fragment state, remember the recv_frag may be gone */
    frag->desc->req = (mca_pml_base_request_t *) request ; /*recv req*/
    frag->desc->desc_status   = MCA_PTL_ELAN_DESC_LOCAL;
    frag->frag_base.frag_owner= &ptl_peer->peer_ptl->super;
    frag->frag_base.frag_peer = recv_frag->frag_recv.frag_base.frag_peer; 
    frag->frag_base.frag_addr = req->req_base.req_addr;/*final buff*/
    frag->frag_base.frag_size = *size; 
    frag->frag_progressed     = 0;

    return OMPI_SUCCESS;
}
#endif /* End of OMPI_PTL_ELAN_ENABLE_GET && HAVE_GET_INTERFACE */

int
mca_ptl_elan_start_desc (mca_ptl_elan_send_frag_t * frag,
			 struct mca_ptl_elan_peer_t *ptl_peer,
			 struct mca_pml_base_send_request_t *sendreq,
			 size_t offset,
			 size_t *size,
			 int flags)
{
    mca_ptl_elan_module_t *ptl;

    ptl = ptl_peer->peer_ptl;

    if (frag->desc->desc_type == MCA_PTL_ELAN_DESC_QDMA) {
        struct ompi_ptl_elan_qdma_desc_t *qdma;

        qdma = (ompi_ptl_elan_qdma_desc_t *)frag->desc;
        mca_ptl_elan_init_qdma_desc (frag, ptl, ptl_peer, 
		sendreq, offset, size, flags);
        elan4_run_dma_cmd (ptl->queue->tx_cmdq, (DMA *) & qdma->main_dma);
	elan4_flush_cmdq_reorder (ptl->queue->tx_cmdq);
        opal_list_append (&ptl->send_frags, (opal_list_item_t *) frag);

    } else if (MCA_PTL_ELAN_DESC_PUT == frag->desc->desc_type) {

        struct ompi_ptl_elan_putget_desc_t *pdesc;

        pdesc = (ompi_ptl_elan_putget_desc_t *)frag->desc;
        mca_ptl_elan_init_put_desc (frag, ptl, ptl_peer, sendreq, 
		offset, size, flags);
        elan4_run_dma_cmd (ptl->putget->put_cmdq, (E4_DMA *) &pdesc->main_dma);
	elan4_flush_cmdq_reorder (ptl->putget->put_cmdq);

        /* Insert frag into the list of outstanding DMA's */
        opal_list_append (&ptl->send_frags, (opal_list_item_t *) frag);
    } else {
        ompi_output (0, "To support GET and Other types of DMA "
	       "are not supported right now \n");
        return OMPI_ERROR;
    }

    /* fragment state */
    frag->frag_base.frag_owner = (struct mca_ptl_base_module_t *) 
	&ptl_peer->peer_ptl->super;
    frag->frag_base.frag_peer = (struct mca_ptl_base_peer_t *) ptl_peer;
    frag->frag_base.frag_addr = NULL;
    frag->frag_base.frag_size = *size;
    frag->frag_progressed  = 0;
    frag->frag_ack_pending = 0; /* this is ack for internal elan */

    return OMPI_SUCCESS;
}

#if OMPI_PTL_ELAN_ENABLE_GET
/* Initialize an ack descriptor and queue it to the command queue */
int
mca_ptl_elan_get_with_ack ( mca_ptl_base_module_t * ptl, 
			    mca_ptl_elan_send_frag_t * frag,
			    mca_ptl_elan_recv_frag_t * recv_frag)
{
    struct ompi_ptl_elan_putget_desc_t *gdesc;
    mca_ptl_base_header_t *hdr;
    mca_pml_base_recv_request_t* request;
    mca_ptl_elan_module_t *elan_ptl;
    int    destvp;
    int    flags;
    size_t remain_len, recv_len;

    flags = 0; /* XXX: No special flags for get */
    elan_ptl = (mca_ptl_elan_module_t *) ptl;
    request  = recv_frag->frag_recv.frag_request;
    destvp   = ((mca_ptl_elan_peer_t *)
	    recv_frag->frag_recv.frag_base.frag_peer)->peer_vp;
    frag->desc->desc_type = MCA_PTL_ELAN_DESC_GET; 
    gdesc = (ompi_ptl_elan_putget_desc_t *)frag->desc;
    hdr   = (mca_ptl_base_header_t *) &frag->frag_base.frag_header;
    recv_len = 
	 recv_frag->frag_recv.frag_base.frag_header.hdr_frag.hdr_frag_length;

    /* XXX: since send_frag provides a header inside, 
     *      no need to allocate a QDMA descriptor w/ buff */
    remain_len = request->req_bytes_packed - recv_len;
    hdr->hdr_common.hdr_type = MCA_PTL_HDR_TYPE_FIN_ACK;
    hdr->hdr_common.hdr_flags= 0;
    hdr->hdr_common.hdr_size = sizeof(mca_ptl_base_ack_header_t);
    hdr->hdr_ack.hdr_src_ptr = 
	recv_frag->frag_recv.frag_base.frag_header.hdr_frag.hdr_src_ptr;
    hdr->hdr_ack.hdr_dst_match.lval = 0; 
    hdr->hdr_ack.hdr_dst_match.pval = request;
    hdr->hdr_ack.hdr_dst_addr.pval = 0; 
    hdr->hdr_ack.hdr_dst_addr.lval = elan4_main2elan(
	    elan_ptl->ptl_elan_ctx, 
	    ((char*)request->req_base.req_addr + recv_len));
    hdr->hdr_ack.hdr_dst_size = remain_len + recv_len;

    mca_ptl_elan_init_get_desc (elan_ptl, frag, recv_frag, 
	    request, hdr, &remain_len, flags);

    /* Trigger a STEN packet to the remote side and then from there
     * a elan_get is triggered */
    elan4_remote_dma(elan_ptl->putget->get_cmdq, 
	    (E4_DMA *)&gdesc->main_dma, destvp, 
	    elan4_local_cookie(elan_ptl->putget->pg_cpool,
	       	E4_COOKIE_TYPE_STEN , destvp));
    elan4_flush_cmdq_reorder (elan_ptl->putget->get_cmdq);
    MEMBAR_DRAIN();
    opal_list_append (&elan_ptl->send_frags, (opal_list_item_t *) frag);

    /* XXX: fragment state, remember recv_frag may be gone */
    frag->desc->req = (mca_pml_base_request_t *) request ; /*recv req*/
    frag->desc->desc_status   = MCA_PTL_ELAN_DESC_LOCAL;
    frag->frag_base.frag_owner= ptl;
    frag->frag_base.frag_peer = recv_frag->frag_recv.frag_base.frag_peer; 
    /* FIXME: final buff*/
    frag->frag_base.frag_addr = request->req_base.req_addr;
    frag->frag_base.frag_size = remain_len;
    frag->frag_progressed     = 0;

    LOG_PRINT(PTL_ELAN_DEBUG_GET, "remote frag %p"
	       " local req %p buffer %p size %d len %d\n",
	       	hdr->hdr_ack.hdr_src_ptr.pval,
	       	hdr->hdr_ack.hdr_dst_match.pval,
	       	hdr->hdr_ack.hdr_dst_addr.pval,
	       	hdr->hdr_ack.hdr_dst_size,
		remain_len);

    return OMPI_SUCCESS;
}
#endif  /* End of OMPI_PTL_ELAN_ENABLE_GET */

/* Initialize an ack descriptor and queue it to the command queue */
int
mca_ptl_elan_start_ack ( mca_ptl_base_module_t * ptl, 
			 mca_ptl_elan_send_frag_t * desc,
			 mca_ptl_elan_recv_frag_t * recv_frag)
{
    struct ompi_ptl_elan_qdma_desc_t *qdma;
    mca_ptl_base_header_t *hdr;
    mca_pml_base_recv_request_t* request;
    mca_ptl_elan_module_t *elan_ptl;
    ELAN4_CTX *ctx;

    int recv_len;
    int destvp;

    destvp = ((mca_ptl_elan_peer_t *)
	    recv_frag->frag_recv.frag_base.frag_peer)->peer_vp;
    recv_len = 
	 recv_frag->frag_recv.frag_base.frag_header.hdr_frag.hdr_frag_length;
	
    elan_ptl = (mca_ptl_elan_module_t *) ptl;
    desc->desc->desc_type = MCA_PTL_ELAN_DESC_QDMA; 
    qdma = (ompi_ptl_elan_qdma_desc_t *)desc->desc;
    ctx  = elan_ptl->ptl_elan_ctx;
    hdr = &desc->frag_base.frag_header;
    request = recv_frag->frag_recv.frag_request;

    hdr->hdr_common.hdr_type = MCA_PTL_HDR_TYPE_ACK;
    hdr->hdr_common.hdr_flags = 0;
    hdr->hdr_common.hdr_size = sizeof(mca_ptl_base_ack_header_t);

    /* Remote send fragment descriptor */
    hdr->hdr_ack.hdr_src_ptr = 
	recv_frag->frag_recv.frag_base.frag_header.hdr_frag.hdr_src_ptr;

    /* Matched request from recv side */
    hdr->hdr_ack.hdr_dst_match.lval = 0; 
    hdr->hdr_ack.hdr_dst_match.pval = request;

    hdr->hdr_ack.hdr_dst_addr.pval = 0; 
    hdr->hdr_ack.hdr_dst_addr.lval = elan4_main2elan(ctx, 
	    (char *)request->req_base.req_addr + recv_len);
    hdr->hdr_ack.hdr_dst_size = request->req_bytes_packed - recv_len;

    LOG_PRINT(PTL_ELAN_DEBUG_ACK, "desc %p hdr %p packed %d received %d\n", 
	    (void*)desc, (void*)hdr, request->req_bytes_packed, recv_len);

#if OMPI_PTL_ELAN_COMP_QUEUE

    /* XXX: Need to have a way to differentiate different frag */
    ((mca_ptl_elan_ack_header_t *) hdr)->frag = desc; 
    PTL_ELAN4_GET_QBUFF (qdma->comp_event->ev_Params[1], ctx, 8, CQ_Size8K);
    qdma->comp_event->ev_CountAndType = E4_EVENT_INIT_VALUE(-32,
	    E4_EVENT_COPY, E4_EVENT_DTYPE_LONG, 8);
    qdma->comp_dma.dma_cookie   = elan4_local_cookie(
	    elan_ptl->queue->tx_cpool,
	    E4_COOKIE_TYPE_LOCAL_DMA, 
	    elan_ptl->elan_vp);

#if OMPI_PTL_ELAN_ONE_QUEUE
    *((mca_ptl_base_header_t *) qdma->buff) = *hdr;
    ((mca_ptl_base_header_t *) qdma->buff)->hdr_common.hdr_type += 8; 
    qdma->comp_dma.dma_srcAddr  = elan4_main2elan (ctx, (void *)qdma->buff);
#else
    qdma->comp_dma.dma_srcAddr  = elan4_main2elan (ctx, (void *) hdr);
#endif
    memcpy ((void *)qdma->comp_buff, (void *)&qdma->comp_dma, 
	    sizeof (E4_DMA64));

    /* XXX: Hardcoded DMA retry count
     * Initialize some of the dma structures */
    qdma->main_dma.dma_typeSize = E4_DMA_TYPE_SIZE (
	    sizeof(mca_ptl_base_ack_header_t),
	    DMA_DataTypeByte, DMA_QueueWrite, 16);
    qdma->main_dma.dma_vproc = destvp;
    qdma->main_dma.dma_cookie = elan4_local_cookie (
	    elan_ptl->queue->tx_cpool, 
	    E4_COOKIE_TYPE_LOCAL_DMA, destvp);
    qdma->main_dma.dma_srcAddr = elan4_main2elan(ctx, (void *) hdr);
    qdma->main_dma.dma_srcEvent= elan4_main2elan(ctx, qdma->comp_event);
#else
    /* Filling up QDMA descriptor */
    qdma->main_dma.dma_typeSize = E4_DMA_TYPE_SIZE (
	    sizeof(mca_ptl_base_ack_header_t),
	    DMA_DataTypeByte, DMA_QueueWrite, 16);
    qdma->main_dma.dma_vproc = destvp;
    qdma->main_dma.dma_cookie = elan4_local_cookie (
	    elan_ptl->queue->tx_cpool, 
	    E4_COOKIE_TYPE_LOCAL_DMA, destvp);
    qdma->main_dma.dma_srcAddr = elan4_main2elan(ctx, (void*)hdr);
#endif

    /* Make main memory coherent with IO domain (IA64) */
    MEMBAR_VISIBLE ();
    elan4_run_dma_cmd (elan_ptl->queue->tx_cmdq, (DMA *) & qdma->main_dma);
    elan4_flush_cmdq_reorder (elan_ptl->queue->tx_cmdq);

    /* Insert desc into the list of outstanding DMA's */
    opal_list_append (&elan_ptl->send_frags, (opal_list_item_t *) desc);

    /* fragment state */
    desc->desc->req = NULL;
    desc->frag_base.frag_owner = ptl;
    desc->frag_base.frag_peer = recv_frag->frag_recv.frag_base.frag_peer; 
    desc->frag_base.frag_addr = NULL;
    desc->frag_base.frag_size = 0;
    desc->frag_progressed     = 0;
    desc->desc->desc_status   = MCA_PTL_ELAN_DESC_LOCAL;

    return OMPI_SUCCESS;
}

int
mca_ptl_elan_drain_recv (struct mca_ptl_elan_module_t *ptl)
{
    struct ompi_ptl_elan_queue_ctrl_t *queue;
    ompi_ptl_elan_recv_queue_t *rxq;
    ELAN_CTX   *ctx;
    int         rc;

    queue = ptl->queue;
    rxq   = queue->rxq;
    ctx   = ptl->ptl_elan_ctx;

ptl_elan_recv_comp:
    OPAL_LOCK (&queue->rx_lock);
#if OMPI_PTL_ELAN_THREADING
    rc = mca_ptl_elan_wait_queue(ptl, rxq, 1);
#else
    rc = (*(int *) (&rxq->qr_doneWord));
#endif
    if (rc) {
	mca_ptl_base_header_t *header;

	header = (mca_ptl_base_header_t *) rxq->qr_fptr;

#if OMPI_PTL_ELAN_THREADING
	if (header->hdr_common.hdr_type == MCA_PTL_HDR_TYPE_STOP) {
	    /* XXX: release the lock and quit the thread */
	    OPAL_UNLOCK (&queue->rx_lock);
	    return OMPI_SUCCESS;
	} 
#endif
	switch (header->hdr_common.hdr_type) {
	case MCA_PTL_HDR_TYPE_MATCH:
	case MCA_PTL_HDR_TYPE_FRAG:
	    /* a data fragment */
	    mca_ptl_elan_data_frag (ptl, header);
	    break;
	case MCA_PTL_HDR_TYPE_ACK:
	case MCA_PTL_HDR_TYPE_NACK:
	    mca_ptl_elan_ctrl_frag (ptl, header);
	    break;
	case MCA_PTL_HDR_TYPE_FIN:
	    mca_ptl_elan_last_frag (ptl, header);
	    break;
	case MCA_PTL_HDR_TYPE_FIN_ACK:
	    mca_ptl_elan_last_frag_ack (ptl, header);
	    break;
	default:
	    fprintf(stderr, "[%s:%d] unknow fragment type %d\n",
		    __FILE__, __LINE__,
		    header->hdr_common.hdr_type);
	    fflush(stderr);
	    break;
	}

	/* Work out the new front pointer */
	if (rxq->qr_fptr == rxq->qr_top) {
	    rxq->qr_fptr = rxq->qr_base;
	    rxq->qr_efptr = rxq->qr_efitem;
	} else {
	    rxq->qr_fptr = (void *) ((uintptr_t) rxq->qr_fptr
				     + queue->rx_slotsize);
	    rxq->qr_efptr += queue->rx_slotsize;
	}

	/* PCI Write, Reset the event 
	 * Order RESETEVENT wrt to wait_event_cmd */
	queue->input->q_fptr = rxq->qr_efptr;
	RESETEVENT_WORD (&rxq->qr_doneWord);
	MEMBAR_STORESTORE ();

	/* Re-prime queue event by issuing a waitevent(1) on it */
	elan4_wait_event_cmd (rxq->qr_cmdq,
		/* Is qr_elanDone really a main memory address? */
		MAIN2ELAN (ctx, rxq->qr_elanDone),
		E4_EVENT_INIT_VALUE (-32, E4_EVENT_WRITE,
		    E4_EVENT_DTYPE_LONG, 0), 
		MAIN2ELAN (ctx, (void *) &rxq->qr_doneWord),
		0xfeedfacedeadbeefLL);
	elan4_flush_cmdq_reorder (rxq->qr_cmdq);
    }
    OPAL_UNLOCK (&queue->rx_lock);

#if OMPI_PTL_ELAN_THREADING
    goto ptl_elan_recv_comp;
#endif

    return OMPI_SUCCESS;
}

int
mca_ptl_elan_update_desc (struct mca_ptl_elan_module_t *ptl)
{
    ELAN4_CTX  *ctx;
    int         rc = 0;

#if OMPI_PTL_ELAN_COMP_QUEUE 
    struct ompi_ptl_elan_comp_queue_t  *comp;

    ompi_ptl_elan_recv_queue_t *rxq;

    comp = ptl->comp;
    ctx  = ptl->ptl_elan_ctx;
    rxq  = comp->rxq;
ptl_elan_send_comp:
    OPAL_LOCK (&comp->rx_lock);
#if OMPI_PTL_ELAN_THREADING
    /* XXX: block on the recv queue without holding a lock */
    rc = mca_ptl_elan_wait_queue(ptl, rxq, 1);
#else
    /* XXX: Just test and go */
    rc = (*(int *) (&rxq->qr_doneWord));
#endif

    if (rc) {
	mca_ptl_elan_send_frag_t *frag;
	mca_ptl_base_header_t *header;
	ompi_ptl_elan_base_desc_t *basic;

	header = (mca_ptl_base_header_t *) rxq->qr_fptr;

#if OMPI_PTL_ELAN_THREADING
	if (header->hdr_common.hdr_type == MCA_PTL_HDR_TYPE_STOP) {
	    /* XXX: release the lock and quit the thread */
	    OPAL_UNLOCK (&comp->rx_lock);
	    return OMPI_SUCCESS;
	}
#endif
	if (header->hdr_common.hdr_type == MCA_PTL_HDR_TYPE_ACK 
		|| header->hdr_common.hdr_type == MCA_PTL_HDR_TYPE_FIN_ACK) {
	    frag = ((mca_ptl_elan_ack_header_t*)header)->frag;
	} else {
	    frag = (mca_ptl_elan_send_frag_t *)
		header->hdr_frag.hdr_src_ptr.pval; 
	}
	basic = (ompi_ptl_elan_base_desc_t*)frag->desc;

	LOG_PRINT(PTL_ELAN_DEBUG_SEND, "frag %p desc %p \n", (void*)frag, (void*)basic);

	/* XXX: please reset additional chained event for put/get desc */
	mca_ptl_elan_send_desc_done (frag, 
		(mca_pml_base_send_request_t *) basic->req);

#if OMPI_PTL_ELAN_COMP_QUEUE
	PTL_ELAN4_FREE_QBUFF (ctx, frag->desc->comp_event->ev_Params[1], 8);
#endif

	/* Work out the new front pointer */
	if (rxq->qr_fptr == rxq->qr_top) {
	    rxq->qr_fptr = rxq->qr_base;
	    rxq->qr_efptr = rxq->qr_efitem;
	} else {
	    rxq->qr_fptr = (void *) ((uintptr_t) rxq->qr_fptr
				     + comp->rx_slotsize);
	    rxq->qr_efptr += comp->rx_slotsize;
	}

	/* PCI Write, Reset the event 
	 * Order RESETEVENT wrt to wait_event_cmd */
	comp->input->q_fptr = rxq->qr_efptr;
	RESETEVENT_WORD (&rxq->qr_doneWord);
	MEMBAR_STORESTORE ();

	/* Re-prime comp event by issuing a waitevent(1) on it */
	elan4_wait_event_cmd (rxq->qr_cmdq,
		/* Is qr_elanDone really a main memory address? */
		MAIN2ELAN (ctx, rxq->qr_elanDone),
		E4_EVENT_INIT_VALUE (-32, E4_EVENT_WRITE,
		    E4_EVENT_DTYPE_LONG, 0), 
		MAIN2ELAN (ctx, (void *) &rxq->qr_doneWord),
		0xfeedfacedeadbeefLL);
	elan4_flush_cmdq_reorder (rxq->qr_cmdq);
    }
    OPAL_UNLOCK (&comp->rx_lock);

#if OMPI_PTL_ELAN_THREADING
    goto ptl_elan_send_comp;
#endif

#else
    ctx  = ptl->ptl_elan_ctx;
    while (opal_list_get_size (&ptl->send_frags) > 0) {
	mca_ptl_elan_send_frag_t   *frag;

	frag = (mca_ptl_elan_send_frag_t *)
	    opal_list_get_first (&ptl->send_frags);
	rc = * ((int *) (&frag->desc->main_doneWord));
	if (rc) {
	    ompi_ptl_elan_base_desc_t *basic;

	    /* Remove the desc, update the request, return to free list */
	    frag = (mca_ptl_elan_send_frag_t *)
		opal_list_remove_first (&ptl->send_frags);
	    basic = (ompi_ptl_elan_base_desc_t*)frag->desc;

	    LOG_PRINT(PTL_ELAN_DEBUG_SEND, "frag %p desc %p \n", frag, basic);

	    mca_ptl_elan_send_desc_done (frag, 
		    (mca_pml_base_send_request_t *) basic->req);
	    INITEVENT_WORD (ctx, basic->elan_event, &basic->main_doneWord);
	    RESETEVENT_WORD (&basic->main_doneWord);
	    PRIMEEVENT_WORD (ctx, basic->elan_event, 1);
	} else {
	    /* XXX: Stop at any incomplete send desc */
	    break;
	}
    } /* end of the while loop */
#endif 

    return OMPI_SUCCESS;
}

int
mca_ptl_elan_lookup(struct mca_ptl_elan_module_t *ptl)
{
    struct ompi_ptl_elan_queue_ctrl_t *queue;
    ompi_ptl_elan_recv_queue_t *rxq;
    ELAN_CTX   *ctx;
    int         rc;

    queue = ptl->queue;
    rxq   = queue->rxq;
    ctx   = ptl->ptl_elan_ctx;

ptl_elan_recv_comp:
    OPAL_LOCK (&queue->rx_lock);
#if OMPI_PTL_ELAN_THREADING
    rc = mca_ptl_elan_wait_queue(ptl, rxq, 1);
#else
    rc = (*(int *) (&rxq->qr_doneWord));
#endif
    if (rc) {
	mca_ptl_base_header_t *header;

	header = (mca_ptl_base_header_t *) rxq->qr_fptr;

    if (header->hdr_common.hdr_type >= 8) {
	mca_ptl_elan_send_frag_t *frag;
	ompi_ptl_elan_base_desc_t *basic;

	header->hdr_common.hdr_type = header->hdr_common.hdr_type - 8;

#if OMPI_PTL_ELAN_THREADING
	if (header->hdr_common.hdr_type == MCA_PTL_HDR_TYPE_STOP) {
	    /* XXX: release the lock and quit the thread */
	    OPAL_UNLOCK (&queue->rx_lock);
	    return OMPI_SUCCESS;
	}
#endif
	if (header->hdr_common.hdr_type == MCA_PTL_HDR_TYPE_ACK 
		|| header->hdr_common.hdr_type == MCA_PTL_HDR_TYPE_FIN_ACK) {
	    frag = ((mca_ptl_elan_ack_header_t*)header)->frag;
	} else {
	    frag = (mca_ptl_elan_send_frag_t *)
		header->hdr_frag.hdr_src_ptr.pval; 
	}
	basic = (ompi_ptl_elan_base_desc_t*)frag->desc;

	LOG_PRINT(PTL_ELAN_DEBUG_SEND, "frag %p desc %p \n", (void*)frag, (void*)basic);

	/* XXX: please reset additional chained event for put/get desc */
	mca_ptl_elan_send_desc_done (frag, 
		(mca_pml_base_send_request_t *) basic->req);

#if OMPI_PTL_ELAN_COMP_QUEUE
	PTL_ELAN4_FREE_QBUFF (ctx, frag->desc->comp_event->ev_Params[1], 8);
#endif

    } else {

#if OMPI_PTL_ELAN_THREADING
	if (header->hdr_common.hdr_type == MCA_PTL_HDR_TYPE_STOP) {
	    /* XXX: release the lock and quit the thread */
	    OPAL_UNLOCK (&queue->rx_lock);
	    return OMPI_SUCCESS;
	} 
#endif

	switch (header->hdr_common.hdr_type) {
	case MCA_PTL_HDR_TYPE_MATCH:
	case MCA_PTL_HDR_TYPE_FRAG:
	    /* a data fragment */
	    mca_ptl_elan_data_frag (ptl, header);
	    break;
	case MCA_PTL_HDR_TYPE_ACK:
	case MCA_PTL_HDR_TYPE_NACK:
	    mca_ptl_elan_ctrl_frag (ptl, header);
	    break;
	case MCA_PTL_HDR_TYPE_FIN:
	    mca_ptl_elan_last_frag (ptl, header);
	    break;
	case MCA_PTL_HDR_TYPE_FIN_ACK:
	    mca_ptl_elan_last_frag_ack (ptl, header);
	    break;
	default:
	    fprintf(stderr, "[%s:%d] unknow fragment type %d\n",
		    __FILE__, __LINE__,
		    header->hdr_common.hdr_type);
	    fflush(stderr);
	    break;
	}
    }

	/* Work out the new front pointer */
	if (rxq->qr_fptr == rxq->qr_top) {
	    rxq->qr_fptr = rxq->qr_base;
	    rxq->qr_efptr = rxq->qr_efitem;
	} else {
	    rxq->qr_fptr = (void *) ((uintptr_t) rxq->qr_fptr
				     + queue->rx_slotsize);
	    rxq->qr_efptr += queue->rx_slotsize;
	}

	/* PCI Write, Reset the event 
	 * Order RESETEVENT wrt to wait_event_cmd */
	queue->input->q_fptr = rxq->qr_efptr;
	RESETEVENT_WORD (&rxq->qr_doneWord);
	MEMBAR_STORESTORE ();

	/* Re-prime queue event by issuing a waitevent(1) on it */
	elan4_wait_event_cmd (rxq->qr_cmdq,
		/* Is qr_elanDone really a main memory address? */
		MAIN2ELAN (ctx, rxq->qr_elanDone),
		E4_EVENT_INIT_VALUE (-32, E4_EVENT_WRITE,
		    E4_EVENT_DTYPE_LONG, 0), 
		MAIN2ELAN (ctx, (void *) &rxq->qr_doneWord),
		0xfeedfacedeadbeefLL);
	elan4_flush_cmdq_reorder (rxq->qr_cmdq);
    }
    OPAL_UNLOCK (&queue->rx_lock);

#if OMPI_PTL_ELAN_THREADING
    goto ptl_elan_recv_comp;
#endif

    return OMPI_SUCCESS;
}

