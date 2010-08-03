/*
*  C Implementation: ctrl
*
* Description:
*
*
* Author:  <xeb@mail.ru>, (C) 2009
*
* Copyright: See COPYING file that comes with this distribution
*
*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "if_pppox.h"

#include "list.h"
#include "pptp_prot.h"
#include "triton/triton.h"
#include "pptpd.h"
#include "log.h"
#include "ppp.h"


#define TIMEOUT 10000

#define STATE_IDLE 0
#define STATE_ESTB 1
#define STATE_FIN  10

struct pptp_conn_t
{
	struct triton_md_handler_t *h;
	int state;

	u_int8_t *in_buf;
	int in_size;
	u_int8_t *out_buf;
	int out_size;
	int out_pos;

	struct ppp_t *ppp;
};

static void pptp_read(struct triton_md_handler_t *h);
static void pptp_write(struct triton_md_handler_t *h);
static void pptp_timeout(struct triton_md_handler_t *h);

static void ctrl_read(struct triton_md_handler_t *h)
{
	struct triton_md_handler_t *hc;
	struct pptp_conn_t *conn;

	int fd;
	int n=read(h->fd,&fd,sizeof(fd));
	if (n!=4)
	{
		log_error("too short message from controlling thread\n");
		return;
	}

	conn=malloc(sizeof(*conn));
	memset(conn,0,sizeof(*conn));
	conn->in_buf=malloc(PPTP_CTRL_SIZE_MAX);
	conn->out_buf=malloc(PPTP_CTRL_SIZE_MAX);

	hc=malloc(sizeof(*hc));
	memset(hc,0,sizeof(*hc));
	hc->fd=fd;
	hc->twait=TIMEOUT;
	hc->read=pptp_read;
	hc->write=pptp_write;
	hc->timeout=pptp_timeout;

	hc->pd=conn;
	conn->h=hc;

	conn->ppp=alloc_ppp();

	triton_md_register_handler(hc);
	triton_md_enable_handler(hc,MD_MODE_READ);
}

int ctrl_init(struct ctrl_thread_t*ctrl)
{
	struct triton_md_handler_t *h=malloc(sizeof(*h));
	memset(h,0,sizeof(*h));
	h->fd=ctrl->pipe_fd[0];
	h->twait=-1;
	h->read=ctrl_read;
	triton_md_register_handler(h);
	triton_md_enable_handler(h,MD_MODE_READ);

	return 0;
}

static void disconnect(struct pptp_conn_t *conn)
{
	close(conn->h->fd);
	triton_md_unregister_handler(conn->h);
	free(conn->h);
	free(conn);
}

static int post_msg(struct pptp_conn_t *conn,void *buf,int size)
{
	int n;
	if (conn->out_size)
	{
		log_debug("post_msg: buffer is not empty\n");
		return -1;
	}

	n=write(conn->h->fd,buf,size);
	if (n<0)
	{
		if (errno==EINTR) n=0;
		else
		{
			log_debug("post_msg: failed to write socket %i\n",errno);
			return -1;
		}
	}

	if (n<size)
	{
		memcpy(conn->out_buf,buf+n,size-n);
		triton_md_enable_handler(conn->h,MD_MODE_WRITE);
	}

	return 0;
}

static int send_pptp_stop_ctrl_conn_rqst(struct pptp_conn_t *conn,int reason,int err_code)
{
	struct pptp_stop_ctrl_conn msg={
		.header=PPTP_HEADER_CTRL(PPTP_STOP_CTRL_CONN_RQST),
		.reason_result=hton8(reason),
		.error_code=hton8(err_code),
	};

	return post_msg(conn,&msg,sizeof(msg));
}

static int send_pptp_stop_ctrl_conn_rply(struct pptp_conn_t *conn,int reason,int err_code)
{
	struct pptp_stop_ctrl_conn msg={
		.header=PPTP_HEADER_CTRL(PPTP_STOP_CTRL_CONN_RPLY),
		.reason_result=hton8(reason),
		.error_code=hton8(err_code),
	};

	return post_msg(conn,&msg,sizeof(msg));
}
static int pptp_stop_ctrl_conn_rqst(struct pptp_conn_t *conn)
{
	struct pptp_stop_ctrl_conn *msg=(struct pptp_stop_ctrl_conn *)conn->in_buf;
	log_info("PPTP_STOP_CTRL_CONN_RQST reason=%i error_code=%i\n",msg->reason_result,msg->error_code);

	conn->state=STATE_FIN;
	conn->h->twait=1000;

	return send_pptp_stop_ctrl_conn_rply(conn,PPTP_CONN_STOP_OK,0);
}

static int send_pptp_start_ctrl_conn_rply(struct pptp_conn_t *conn,int res_code,int err_code)
{
	struct pptp_start_ctrl_conn msg={
		.header=PPTP_HEADER_CTRL(PPTP_START_CTRL_CONN_RPLY),
		.version=htons(PPTP_VERSION),
		.result_code=res_code,
		.error_code=err_code,
		.framing_cap=htonl(PPTP_FRAME_SYNC),
		.bearer_cap=htonl(0),
		.max_channels=htons(1),
		.firmware_rev=htons(PPTP_FIRMWARE_VERSION),
	};

	memset(msg.hostname,0,sizeof(msg.hostname));
	strcpy((char*)msg.hostname,PPTP_HOSTNAME);

	memset(msg.vendor,0,sizeof(msg.vendor));
	strcpy((char*)msg.vendor,PPTP_VENDOR);

	return post_msg(conn,&msg,sizeof(msg));
}
static int pptp_start_ctrl_conn_rqst(struct pptp_conn_t *conn)
{
	struct pptp_start_ctrl_conn *msg=(struct pptp_start_ctrl_conn *)conn->in_buf;

	if (conn->state!=STATE_IDLE)
	{
		log_info("unexpected PPTP_START_CTRL_CONN_RQST\n");
		if (send_pptp_start_ctrl_conn_rply(conn,PPTP_CONN_RES_EXISTS,0))
			return -1;
		return 0;
	}

	if (msg->version!=htons(PPTP_VERSION))
	{
		log_info("PPTP version mismatch: expecting %x, received %s\n",PPTP_VERSION,msg->version);
		if (send_pptp_start_ctrl_conn_rply(conn,PPTP_CONN_RES_PROTOCOL,0))
			return -1;
		return 0;
	}
	if (!(ntohl(msg->framing_cap)&PPTP_FRAME_SYNC))
	{
		log_info("connection does not supports sync mode\n");
		if (send_pptp_start_ctrl_conn_rply(conn,PPTP_CONN_RES_GE,0))
			return -1;
		return 0;
	}
	if (send_pptp_start_ctrl_conn_rply(conn,PPTP_CONN_RES_SUCCESS,0))
		return -1;

	conn->state=STATE_ESTB;

	return 0;
}

static int send_pptp_out_call_rply(struct pptp_conn_t *conn,struct pptp_out_call_rqst *rqst,int call_id,int res_code,int err_code)
{
	struct pptp_out_call_rply msg={
		.header=PPTP_HEADER_CTRL(PPTP_OUT_CALL_RPLY),
		.call_id=htons(call_id),
		.call_id_peer=rqst->call_id,
		.result_code=res_code,
		.error_code=err_code,
		.cause_code=0,
		.speed=rqst->bps_max,
		.recv_size=rqst->recv_size,
		.delay=0,
		.channel=0,
	};

	return post_msg(conn,&msg,sizeof(msg));
}

static int pptp_out_call_rqst(struct pptp_conn_t *conn)
{
	struct pptp_out_call_rqst *msg=(struct pptp_out_call_rqst *)conn->in_buf;
	struct sockaddr_pppox src_addr,dst_addr;
  struct sockaddr_in addr;
	socklen_t addrlen;
	int pptp_sock;

	if (conn->state!=STATE_ESTB)
	{
		log_info("unexpected PPTP_OUT_CALL_RQST\n");
		if (send_pptp_out_call_rply(conn,msg,0,PPTP_CALL_RES_GE,PPTP_GE_NOCONN))
			return -1;
		return 0;
	}

	src_addr.sa_family=AF_PPPOX;
	src_addr.sa_protocol=PX_PROTO_PPTP;
	src_addr.sa_addr.pptp.call_id=0;
	addrlen=sizeof(addr); getsockname(conn->h->fd,(struct sockaddr*)&addr,&addrlen);
	src_addr.sa_addr.pptp.sin_addr=addr.sin_addr;

	dst_addr.sa_family=AF_PPPOX;
	dst_addr.sa_protocol=PX_PROTO_PPTP;
	dst_addr.sa_addr.pptp.call_id=htons(msg->call_id);
	addrlen=sizeof(addr); getpeername(conn->h->fd,(struct sockaddr*)&addr,&addrlen);
	dst_addr.sa_addr.pptp.sin_addr=addr.sin_addr;

	pptp_sock=socket(AF_PPPOX,SOCK_STREAM,PX_PROTO_PPTP);
	if (pptp_sock<0)
	{
		log_error("failed to create PPTP socket (%s)\n",strerror(errno));
		return -1;
	}
	if (bind(pptp_sock,(struct sockaddr*)&src_addr,sizeof(src_addr)))
	{
		log_error("failed to bind PPTP socket (%s)\n",strerror(errno));
		close(pptp_sock);
		return -1;
	}
	addrlen=sizeof(src_addr);
	getsockname(pptp_sock,(struct sockaddr*)&src_addr,&addrlen);

	if (connect(pptp_sock,(struct sockaddr*)&dst_addr,sizeof(dst_addr)))
	{
		log_error("failed to connect PPTP socket (%s)\n",strerror(errno));
		close(pptp_sock);
		return -1;
	}

	if (send_pptp_out_call_rply(conn,msg,src_addr.sa_addr.pptp.call_id,PPTP_CALL_RES_OK,0))
		return -1;

	conn->ppp->fd=pptp_sock;
	conn->ppp->chan_name=strdup(inet_ntoa(dst_addr.sa_addr.pptp.sin_addr));
	establish_ppp(conn->ppp);

	return 0;
}

static int process_packet(struct pptp_conn_t *conn)
{
	struct pptp_header *hdr=(struct pptp_header *)conn->in_buf;
	switch(ntohs(hdr->ctrl_type))
	{
		case PPTP_START_CTRL_CONN_RQST:
			return pptp_start_ctrl_conn_rqst(conn);
		case PPTP_STOP_CTRL_CONN_RQST:
			return pptp_stop_ctrl_conn_rqst(conn);
		case PPTP_OUT_CALL_RQST:
			return pptp_out_call_rqst(conn);
	}
	return 0;
}

static void pptp_read(struct triton_md_handler_t *h)
{
	struct pptp_conn_t *conn=(struct pptp_conn_t *)h->pd;
	struct pptp_header *hdr=(struct pptp_header *)conn->in_buf;
	int n;

	n=read(h->fd,conn->in_buf,PPTP_CTRL_SIZE_MAX-conn->in_size);
	if (n<=0)
	{
		if (errno==EINTR) return;
		disconnect(conn);
		return;
	}
	conn->in_size+=n;
	if (conn->in_size>=sizeof(*hdr))
	{
		if (hdr->magic!=htonl(PPTP_MAGIC)) goto drop;
		if (ntohs(hdr->length)>=PPTP_CTRL_SIZE_MAX) goto drop;
		if (ntohs(hdr->length)>conn->in_size) goto drop;
		if (ntohs(hdr->length)==conn->in_size)
		{
			if (ntohs(hdr->length)!=PPTP_CTRL_SIZE(ntohs(hdr->ctrl_type))) goto drop;
			if (process_packet(conn)) goto drop;
			conn->in_size=0;
		}
	}
	h->twait=TIMEOUT;
	return;
drop:
	disconnect(conn);
	return;
}
static void pptp_write(struct triton_md_handler_t *h)
{
	struct pptp_conn_t *conn=(struct pptp_conn_t *)h->pd;
	int n=write(h->fd,conn->out_buf+conn->out_pos,conn->out_size-conn->out_pos);

	if (n<0)
	{
		if (errno==EINTR) n=0;
		else
		{
			log_debug("post_msg: failed to write socket %i\n",errno);
			disconnect(conn);
			return;
		}
	}

	conn->out_pos+=n;
	if (conn->out_pos==conn->out_size)
	{
		conn->out_pos=0;
		conn->out_size=0;
		triton_md_disable_handler(h,MD_MODE_WRITE);
	}
	h->twait=TIMEOUT;
}
static void pptp_timeout(struct triton_md_handler_t *h)
{
}
