/*
*  C Implementation: ppp_fsm
*
* Description:
*
*
* Author:  <xeb@mail.ru>, (C) 2009
*
* Copyright: See COPYING file that comes with this distribution
*
*/

#include "triton/triton.h"
#include "ppp.h"
#include "ppp_fsm.h"

void send_term_req(struct ppp_layer_t *layer);
void send_term_ack(struct ppp_layer_t *layer);
void send_echo_reply(struct ppp_layer_t *layer);

static void init_req_counter(struct ppp_layer_t *layer,int timeout);
static void zero_req_counter(struct ppp_layer_t *layer);
static int restart_timer_func(struct triton_timer_t*t);

void ppp_fsm_init(struct ppp_layer_t *layer)
{
	layer->fsm_state=FSM_Initial;
	layer->restart_timer.active=0;
	layer->restart_timer.pd=layer;
	layer->restart_timer.expire=restart_timer_func;
	layer->restart_timer.period=3000;
	layer->restart_counter=0;
	layer->max_terminate=2;
	layer->max_configure=10;
	layer->max_failure=5;
	layer->seq=0;
}

void ppp_fsm_lower_up(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Initial:
			layer->fsm_state=FSM_Closed;
			break;
		case FSM_Starting:
			//if (layer->init_req_cnt) layer->init_req_cnt(layer);
			init_req_counter(layer,layer->max_configure);
			if (layer->send_conf_req) layer->send_conf_req(layer);
			layer->fsm_state=FSM_Req_Sent;
			break;
		default:
			break;
	}
}

void ppp_fsm_lower_down(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Closed:
		case FSM_Closing:
			layer->fsm_state=FSM_Initial;
			break;
		case FSM_Stopped:
			if (layer->layer_started) layer->layer_started(layer);
			layer->fsm_state=FSM_Starting;
			break;
		case FSM_Stopping:
		case FSM_Req_Sent:
		case FSM_Ack_Rcvd:
		case FSM_Ack_Sent:
			layer->fsm_state=FSM_Starting;
			break;
		case FSM_Opened:
			if (layer->layer_down) layer->layer_down(layer);
			layer->fsm_state=FSM_Starting;
			break;
		default:
			break;
	}
}

void ppp_fsm_open(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Initial:
			if (layer->layer_started) layer->layer_started(layer);
			layer->fsm_state=FSM_Starting;
			break;
		case FSM_Starting:
			break;
		case FSM_Closed:
			//if (layer->init_req_cnt) layer->init_req_cnt(layer);
			init_req_counter(layer,layer->max_configure);
			if (layer->send_conf_req) layer->send_conf_req(layer);
			layer->fsm_state=FSM_Req_Sent;
			break;
		case FSM_Closing:
		case FSM_Stopping:
			layer->fsm_state=FSM_Stopping;
		case FSM_Stopped:
		case FSM_Opened:
			if (layer->opt_restart)
			{
				ppp_fsm_lower_down(layer);
				ppp_fsm_lower_up(layer);
			}
			break;
		default:
			break;
	}
}

void ppp_fsm_close(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Starting:
			if (layer->layer_finished) layer->layer_finished(layer);
			layer->fsm_state=FSM_Initial;
			break;
		case FSM_Stopped:
			layer->fsm_state=FSM_Closed;
			break;
		case FSM_Stopping:
			layer->fsm_state=FSM_Closing;
			break;
		case FSM_Opened:
			if (layer->layer_down) layer->layer_down(layer);
		case FSM_Req_Sent:
		case FSM_Ack_Rcvd:
		case FSM_Ack_Sent:
			//if (layer->init_req_cnt) layer->init_req_cnt(layer);
			init_req_counter(layer,layer->max_terminate);
			send_term_req(layer);
			layer->fsm_state=FSM_Closing;
			break;
		default:
			break;
	}
}

void ppp_fsm_timeout0(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Closing:
		case FSM_Stopping:
			send_term_req(layer);
			break;
		case FSM_Ack_Rcvd:
			layer->fsm_state=FSM_Req_Sent;
		case FSM_Req_Sent:
		case FSM_Ack_Sent:
			if (layer->send_conf_req) layer->send_conf_req(layer);
			break;
		default:
			break;
	}
}

void ppp_fsm_timeout1(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Closing:
			if (layer->layer_finished) layer->layer_finished(layer);
			layer->fsm_state=FSM_Closed;
			break;
		case FSM_Stopping:
			if (layer->layer_finished) layer->layer_finished(layer);
			layer->fsm_state=FSM_Stopped;
			break;
		case FSM_Ack_Rcvd:
		case FSM_Req_Sent:
		case FSM_Ack_Sent:
			if (layer->layer_finished) layer->layer_finished(layer);
			layer->fsm_state=FSM_Stopped;
			layer->opt_passive=1;
			break;
		default:
			break;
	}
}

void ppp_fsm_recv_conf_req_good(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Closed:
			send_term_ack(layer);
			break;
		case FSM_Stopped:
			//if (layer->init_req_cnt) layer->init_req_cnt(layer);
			init_req_counter(layer,layer->max_configure);
			if (layer->send_conf_req) layer->send_conf_req(layer);
		case FSM_Req_Sent:
		case FSM_Ack_Sent:
			if (layer->send_conf_ack) layer->send_conf_ack(layer);
			layer->fsm_state=FSM_Ack_Sent;
			break;
		case FSM_Ack_Rcvd:
			if (layer->send_conf_ack) layer->send_conf_ack(layer);
			//tlu
			if (layer->layer_up) layer->layer_up(layer);
			layer->fsm_state=FSM_Opened;
			break;
		case FSM_Opened:
			if (layer->layer_down) layer->layer_down(layer);
			if (layer->send_conf_req) layer->send_conf_req(layer);
			if (layer->send_conf_ack) layer->send_conf_ack(layer);
			layer->fsm_state=FSM_Ack_Sent;
			break;
		default:
			break;
	}
}

void ppp_fsm_recv_conf_req_bad(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Closed:
			send_term_ack(layer);
			break;
		case FSM_Stopped:
			//if (layer->init_req_cnt) layer->init_req_cnt(layer);
			init_req_counter(layer,layer->max_configure);
			if (layer->send_conf_req) layer->send_conf_req(layer);
		case FSM_Ack_Sent:
			if (layer->send_conf_rej) layer->send_conf_rej(layer);
			layer->fsm_state=FSM_Req_Sent;
			break;
		case FSM_Req_Sent:
		case FSM_Ack_Rcvd:
			if (layer->send_conf_rej) layer->send_conf_rej(layer);
			break;
		case FSM_Opened:
			if (layer->layer_down) layer->layer_down(layer);
			if (layer->send_conf_req) layer->send_conf_req(layer);
			if (layer->send_conf_rej) layer->send_conf_rej(layer);
			layer->fsm_state=FSM_Req_Sent;
			break;
		default:
			break;
	}
}

void ppp_fsm_recv_conf_ack(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Closed:
		case FSM_Stopped:
			send_term_ack(layer);
			break;
		case FSM_Req_Sent:
			//if (layer->init_req_cnt) layer->init_req_cnt(layer);
			init_req_counter(layer,layer->max_configure);
			layer->fsm_state=FSM_Ack_Rcvd;
			break;
		case FSM_Ack_Rcvd:
			if (layer->send_conf_req) layer->send_conf_req(layer);
			layer->fsm_state=FSM_Req_Sent;
			break;
		case FSM_Ack_Sent:
			//if (layer->init_req_cnt) layer->init_req_cnt(layer);
			init_req_counter(layer,layer->max_configure);
			//tlu
			if (layer->layer_up) layer->layer_up(layer);
			layer->fsm_state=FSM_Opened;
			break;
		case FSM_Opened:
			if (layer->layer_down) layer->layer_down(layer);
			if (layer->send_conf_req) layer->send_conf_req(layer);
			layer->fsm_state=FSM_Req_Sent;
		default:
			break;
	}
}

void ppp_fsm_recv_conf_rej(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Closed:
		case FSM_Stopped:
			send_term_ack(layer);
			break;
		case FSM_Req_Sent:
			//if (layer->init_req_cnt) layer->init_req_cnt(layer);
			init_req_counter(layer,layer->max_failure);
			if (layer->send_conf_req) layer->send_conf_req(layer);
			break;
		case FSM_Ack_Rcvd:
			if (layer->send_conf_req) layer->send_conf_req(layer);
			layer->fsm_state=FSM_Req_Sent;
			break;
		case FSM_Ack_Sent:
			//if (layer->init_req_cnt) layer->init_req_cnt(layer);
			init_req_counter(layer,layer->max_configure);
			if (layer->send_conf_req) layer->send_conf_req(layer);
			break;
		case FSM_Opened:
			if (layer->layer_down) layer->layer_down(layer);
			if (layer->send_conf_req) layer->send_conf_req(layer);
			layer->fsm_state=FSM_Req_Sent;
			break;
		default:
			break;
	}
}

void ppp_fsm_recv_term_req(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Opened:
			if (layer->layer_down) layer->layer_down(layer);
			send_term_req(layer);
			//if (layer->zero_req_cnt) layer->zero_req_cnt(layer);
			zero_req_counter(layer);
			layer->fsm_state=FSM_Stopping;
			break;
		case FSM_Req_Sent:
		case FSM_Ack_Rcvd:
		case FSM_Ack_Sent:
			send_term_req(layer);
			layer->fsm_state=FSM_Req_Sent;
			break;
		default:
			send_term_req(layer);
			break;
	}
}

void ppp_fsm_recv_term_ack(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Closing:
			if (layer->layer_finished) layer->layer_finished(layer);
			layer->fsm_state=FSM_Closed;
			break;
		case FSM_Stopping:
			if (layer->layer_finished) layer->layer_finished(layer);
			layer->fsm_state=FSM_Stopped;
			break;
		case FSM_Ack_Rcvd:
			layer->fsm_state=FSM_Req_Sent;
			break;
		case FSM_Opened:
			if (layer->layer_down) layer->layer_down(layer);
			if (layer->send_conf_req) layer->send_conf_req(layer);
			layer->fsm_state=FSM_Req_Sent;
			break;
		default:
			break;
	}
}

void ppp_fsm_recv_unk(struct ppp_layer_t *layer)
{
	if (layer->send_conf_rej) layer->send_conf_rej(layer);
}

void ppp_fsm_recv_code_rej_perm(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Ack_Rcvd:
			layer->fsm_state=FSM_Req_Sent;
			break;
		default:
			break;
	}
}

void ppp_fsm_recv_code_rej_bad(struct ppp_layer_t *layer)
{
	switch(layer->fsm_state)
	{
		case FSM_Opened:
			if (layer->layer_down) layer->layer_down(layer);
			//if (layer->init_req_cnt) layer->init_req_cnt(layer);
			init_req_counter(layer,layer->max_configure);
			if (layer->send_conf_req) layer->send_conf_req(layer);
			layer->fsm_state=FSM_Stopping;
			break;
		case FSM_Closing:
			if (layer->layer_finished) layer->layer_finished(layer);
			layer->fsm_state=FSM_Closed;
			break;
		case FSM_Stopping:
		case FSM_Req_Sent:
		case FSM_Ack_Rcvd:
		case FSM_Ack_Sent:
			if (layer->layer_finished) layer->layer_finished(layer);
			layer->fsm_state=FSM_Stopped;
			break;
		default:
			break;
	}
}

void ppp_fsm_recv_echo(struct ppp_layer_t *layer)
{
	if (layer->fsm_state==FSM_Opened)
		send_echo_reply(layer);
}

void send_term_req(struct ppp_layer_t *layer)
{
	struct ppp_hdr_t hdr={
		.code=TERMREQ,
		.id=++layer->seq,
		.len=4,
	};

	ppp_send(layer->ppp,&hdr,hdr.len);
}
void send_term_ack(struct ppp_layer_t *layer)
{
	struct ppp_hdr_t hdr={
		.code=TERMACK,
		.id=layer->recv_id,
		.len=4,
	};

	ppp_send(layer->ppp,&hdr,hdr.len);
}
void send_echo_reply(struct ppp_layer_t *layer)
{
	struct ppp_hdr_t hdr={
		.code=ECHOREP,
		.id=layer->recv_id,
		.len=8,
	};

	*(int*)hdr.data=layer->magic_num;

	ppp_send(layer->ppp,&hdr,hdr.len);
}

void ppp_fsm_recv(struct ppp_layer_t *layer)
{
}

static void init_req_counter(struct ppp_layer_t *layer,int timeout)
{
	triton_timer_del(&layer->restart_timer);
	triton_timer_add(&layer->restart_timer);
	layer->restart_counter=timeout;
}
static void zero_req_counter(struct ppp_layer_t *layer)
{
	triton_timer_del(&layer->restart_timer);
	triton_timer_add(&layer->restart_timer);
	layer->restart_counter=0;
}

static int restart_timer_func(struct triton_timer_t*t)
{
	struct ppp_layer_t *layer=(struct ppp_layer_t *)t->pd;

	if (layer->restart_counter)
	{
		ppp_fsm_timeout0(layer);
		layer->restart_counter--;
		return 1;
	}

	ppp_fsm_timeout1(layer);
	return 0;
}
