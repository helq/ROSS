/*
 * ROSS: Rensselaer's Optimistic Simulation System.
 * Copyright (c) 1999-2003 Rensselaer Polytechnic Instutitute.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *      This product includes software developed by David Bauer,
 *      Dr. Christopher D.  Carothers, and Shawn Pearce of the
 *      Department of Computer Science at Rensselaer Polytechnic
 *      Institute.
 *
 * 4. Neither the name of the University nor of the developers may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * 5. The use or inclusion of this software or its documentation in
 *    any commercial product or distribution of this software to any
 *    other party without specific, written prior permission is
 *    prohibited.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 */

#include <ross.h>
#include <mpi.h>

struct act_q
{
	const char	 *name;

	tw_event	**event_list;
	char		**buffers;
	MPI_Request	 *req_list;
	int		 *idx_list;
	MPI_Status	 *status_list;

	unsigned int	  cur;
};

#define BUFFER_SIZE 500
#define EVENT_TAG 1
//#define EVENT_SIZE(e) g_tw_event_msg_sz
#define EVENT_SIZE(e) BUFFER_SIZE
#define STATS_TAG 2

static struct act_q posted_sends;
static struct act_q posted_recvs;
static tw_eventq outq;

static unsigned int read_buffer = 50000;
static unsigned int send_buffer = 50000;
static int world_size = 1;

static const tw_optdef mpi_opts[] = {
	TWOPT_GROUP("ROSS MPI Kernel"),
	TWOPT_UINT(
		"read-buffer",
		read_buffer,
		"network read buffer size in # of events"),
	TWOPT_UINT(
		"send-buffer",
		send_buffer,
		"network send buffer size in # of events"),
	TWOPT_END()
};

const tw_optdef *
tw_net_init(int *argc, char ***argv)
{
	int my_rank;

	if (MPI_Init(argc, argv) != MPI_SUCCESS)
		tw_error(TW_LOC, "MPI_Init failed.");
	if (MPI_Comm_rank(MPI_COMM_WORLD, &my_rank) != MPI_SUCCESS)
		tw_error(TW_LOC, "Cannot get MPI_Comm_rank(MPI_COMM_WORLD)");

	g_tw_masternode = 0;
	g_tw_mynode = my_rank;

	if(tw_node_eq(&g_tw_mynode, &g_tw_masternode))
#if ROSS_NET_aggressive
		printf("ROSS Network Mode: aggressive\n");
#else
		printf("ROSS Network Mode: conservative\n");
#endif

	return mpi_opts;
}

static void
init_q(struct act_q *q, const char *name)
{
	unsigned int i;
	unsigned int n;

	if(q == &posted_sends)
		n = send_buffer;
	else
		n = read_buffer;

	q->name = name;
	q->event_list = tw_calloc(TW_LOC, name, sizeof(*q->event_list), n);
	q->req_list = tw_calloc(TW_LOC, name, sizeof(*q->event_list), n);
	q->idx_list = tw_calloc(TW_LOC, name, sizeof(*q->idx_list), n);
	q->status_list = tw_calloc(TW_LOC, name, sizeof(*q->status_list), n);
	q->buffers = tw_calloc(TW_LOC, name, sizeof(*q->buffers), n);

	for(i = 0; i < n; i++)
		q->buffers[i] = tw_calloc(TW_LOC, "", BUFFER_SIZE, 1);
}

unsigned int
tw_nnodes(void)
{
	return world_size;
}

void
tw_net_start(void)
{
	if (MPI_Comm_size(MPI_COMM_WORLD, &world_size) != MPI_SUCCESS)
		tw_error(TW_LOC, "Cannot get MPI_Comm_size(MPI_COMM_WORLD)");

	tw_pe_create(1);
	tw_pe_init(0, g_tw_mynode);
	g_tw_pe[0]->hash_t = tw_hash_create();

	if (send_buffer < 1)
		tw_error(TW_LOC, "network send buffer must be >= 1");
	if (read_buffer < 1)
		tw_error(TW_LOC, "network read buffer must be >= 1");

	init_q(&posted_sends, "MPI send queue");
	init_q(&posted_recvs, "MPI recv queue");

	g_tw_net_device_size = read_buffer;
}

void
tw_net_abort(void)
{
	MPI_Abort(MPI_COMM_WORLD, 1);
	exit(1);
}

void
tw_net_stop(void)
{
	if (MPI_Finalize() != MPI_SUCCESS)
		tw_error(TW_LOC, "Failed to finalize MPI");
}

void
tw_net_barrier(tw_pe * pe)
{
	if (MPI_Barrier(MPI_COMM_WORLD) != MPI_SUCCESS)
		tw_error(TW_LOC, "Failed to wait for MPI_Barrier");
}

tw_stime
tw_net_minimum(tw_pe *me)
{
	tw_stime m = DBL_MAX;
	tw_event *e;
	int i;

	e = outq.head;
	while (e) {
		if (m > e->recv_ts)
			m = e->recv_ts;
		e = e->next;
	}

	for (i = 0; i < posted_sends.cur; i++) {
		e = posted_sends.event_list[i];
		if (m > e->recv_ts)
			m = e->recv_ts;
	}

	return m;
}

static int
test_q(
	struct act_q *q,
	tw_pe *me,
	void (*finish)(tw_pe *, tw_event *, char *))
{
	int ready, i, n;
	char *tmp;

	if (!q->cur)
		return 0;

	if (MPI_Testsome(
		q->cur,
		q->req_list,
		&ready,
		q->idx_list,
		q->status_list) != MPI_SUCCESS) {
		tw_error(
			TW_LOC,
			"MPI_testsome failed with %u items in %s",
			q->cur,
			q->name);
	}

	if (1 > ready)
		return 0;

	for (i = 0; i < ready; i++)
	{
		tw_event *e;

		n = q->idx_list[i];
		e = q->event_list[n];
		q->event_list[n] = NULL;

		finish(me, e, q->buffers[n]);
	}

	/* Collapse the lists to remove any holes we left. */
	for (i = 0, n = 0; i < q->cur; i++) {
		if (q->event_list[i]) {
			if (i != n) {
				// swap the event pointers
				q->event_list[n] = q->event_list[i];

				// swap the buffers
				tmp = q->buffers[n];
				q->buffers[n] = q->buffers[i];
				q->buffers[i] = tmp;

				// copy the request handles
				memcpy(
					&q->req_list[n],
					&q->req_list[i],
					sizeof(q->req_list[0]));
			}
			n++;
		}
	}
	q->cur -= ready;

	return 1;
}

static int
recv_begin(tw_pe *me)
{
	MPI_Status status;

	tw_event	*e = NULL;

	int flag = 0;
	int changed = 0;

	while (posted_recvs.cur < read_buffer)
	{
		unsigned id = posted_recvs.cur;

		MPI_Iprobe(MPI_ANY_SOURCE,
				MPI_ANY_TAG,
				MPI_COMM_WORLD,
				&flag,
				&status);

		if(flag)
		{
			if(me->abort_event == (e = tw_event_grab(me)))
			{
				if(tw_gvt_inprogress(me))
					tw_error(TW_LOC, "out of events in GVT!");

				break;
			}
		} else
		{
			return changed;
		}

#if 1
		if(!flag || 
			//MPI_Irecv(e,
			MPI_Irecv(posted_recvs.buffers[id],
				EVENT_SIZE(e),
				MPI_BYTE,
				MPI_ANY_SOURCE,
				EVENT_TAG,
				MPI_COMM_WORLD,
				&posted_recvs.req_list[id]) != MPI_SUCCESS)
#else
		if(MPI_Recv(e,
				EVENT_SIZE(e),
				MPI_BYTE,
				MPI_ANY_SOURCE,
				EVENT_TAG,
				MPI_COMM_WORLD, &status) != MPI_SUCCESS)
#endif
		{
			tw_event_free(me, e);
			return changed;
		}

#if 1
		posted_recvs.event_list[id] = e;
		posted_recvs.cur++;
#else
	recv_finish(me, e);
#endif
		changed = 1;
	}

	return changed;
}

static void
recv_finish(tw_pe *me, tw_event *e, char * buffer)
{
	tw_pe		*dest_pe;

	tw_memory	*memory;
	tw_memory	*last;

	tw_fd		 mem_fd;

	size_t		 mem_size;

	unsigned	 position = 0;

	me->s_nrecv_network++;
	me->s_nwhite_recv++;

	memcpy(e, buffer, g_tw_event_msg_sz);
	position += g_tw_event_msg_sz;

	e->dest_lp = tw_getlocal_lp((tw_lpid) e->dest_lp);
	dest_pe = e->dest_lp->pe;

	if(e->send_pe > tw_nnodes()-1)
		tw_error(TW_LOC, "bad sendpe_id: %d", e->send_pe);

	e->cancel_next = NULL;
	e->caused_by_me = NULL;
	e->cause_next = NULL;

	if(e->recv_ts < me->GVT)
		tw_error(TW_LOC, "%d: Received straggler from %d: %lf (%d)", 
			 me->id,  e->send_pe, e->recv_ts, e->state.cancel_q);

	if(tw_gvt_inprogress(me))
		me->trans_msg_ts = min(me->trans_msg_ts, e->recv_ts);

	// if cancel event, retrieve and flush
	// else, store in hash table
	if(e->state.cancel_q)
	{
		tw_event *cancel = tw_hash_remove(me->hash_t, e, e->send_pe);

		// NOTE: it is possible to cancel the event we
		// are currently processing at this PE since this
		// MPI module lets me read cancel events during 
		// event sends over the network.

		cancel->state.cancel_q = 1;
		cancel->state.remote = 0;

		tw_mutex_lock(&dest_pe->cancel_q_lck);
		cancel->cancel_next = dest_pe->cancel_q;
		dest_pe->cancel_q = cancel;
		tw_mutex_unlock(&dest_pe->cancel_q_lck);

		//e->event_id = e->state.cancel_q = 0;
		//e->state.remote = 0;

		tw_event_free(me, e);

		return;
	}

	tw_hash_insert(me->hash_t, e, e->send_pe);
	e->state.remote = 1;

	mem_size = (size_t) e->memory;
	mem_fd = (tw_fd) e->prev;

	last = NULL;
	while(mem_size)
	{
		memory = tw_memory_alloc(e->dest_lp, mem_fd);

		if(last)
			last->next = memory;
		else
			e->memory = memory;

		memcpy(memory, &buffer[position], mem_size);
		position += mem_size;

		memory->prev = (tw_memory *) mem_fd;

		mem_size = (size_t) memory->next;
		mem_fd = (tw_fd) memory->prev;

		last = memory;
	}

	if(me == dest_pe && e->dest_lp->kp->last_time <= e->recv_ts) {
		/* Fast case, we are sending to our own PE and
		 * there is no rollback caused by this send.
		 */
		tw_pq_enqueue(dest_pe->pq, e);
		return;
	}

	if(tw_node_eq(&me->node, &dest_pe->node)) {
		/* Slower, but still local send, so put into top
		 * of dest_pe->event_q. 
		 */
		e->state.owner = TW_pe_event_q;

		tw_mutex_lock(&dest_pe->event_q_lck);
		tw_eventq_push(&dest_pe->event_q, e);
		tw_mutex_unlock(&dest_pe->event_q_lck);
		return;
	}

	/* Never should happen; MPI should have gotten the
	 * message to the correct node without needing us
	 * to redirect the message there for it.  This is
	 * probably a serious bug with the event headers
	 * not being formatted right.
	 */
	tw_error(
		TW_LOC,
		"Event recived by PE %u but meant for PE %u",
		me->id,
		dest_pe->id);
}

static int
send_begin(tw_pe *me)
{
	int changed = 0;

	while (posted_sends.cur < send_buffer)
	{
		tw_event *e = tw_eventq_peek(&outq);
		tw_event *tmp_prev = NULL;

		tw_kp *kp;
		tw_lp *tmp_lp = NULL;
		tw_node	*dest_node = NULL;

		tw_memory *memory = NULL;
		tw_memory *m = NULL;

		char *buffer = NULL;

		size_t mem_size = 0;

		unsigned position = 0;
		unsigned id = posted_sends.cur;

		if (!e)
			break;

		if(e == me->abort_event)
			tw_error(TW_LOC, "Sending abort event!");

		dest_node = tw_net_onnode((*e->src_lp->type.map)
					((tw_lpid) e->dest_lp));

		if(!e->state.cancel_q)
			e->event_id = (tw_eventid) ++me->seq_num;

		e->send_pe = (tw_peid) g_tw_mynode;
		kp = e->src_lp->kp;

		// pack pointers
		tmp_prev = e->prev;
		tmp_lp = e->src_lp;

		// delete when working
		e->src_lp = NULL;

		memory = NULL;
		if(e->memory)
		{
			memory = e->memory;
			e->memory = (tw_memory *) tw_memory_getsize(kp, (tw_fd) memory->prev);
			e->prev = (tw_event *) memory->prev;
			mem_size = (size_t) e->memory;
		}

		buffer = posted_sends.buffers[id];
		memcpy(&buffer[position], e, g_tw_event_msg_sz);
		position += g_tw_event_msg_sz;

		// restore pointers
		e->prev = tmp_prev;
		e->src_lp = tmp_lp;

		m = NULL;
		while(memory)
		{
			m = memory->next;

			if(m)
			{
				memory->next = (tw_memory *)
						tw_memory_getsize(kp, (tw_fd) m->prev);
				memory->prev = (tw_memory *) m->prev;
			}

			if(position + mem_size > BUFFER_SIZE)
				tw_error(TW_LOC, "Out of buffer space!");

			memcpy(&buffer[position], memory, mem_size);
			position += mem_size;

			tw_memory_free_single(kp, memory, (tw_fd) memory->prev);

			if(NULL != (memory = m))
				mem_size = tw_memory_getsize(kp, (tw_fd) memory->prev);
		}

		e->memory = NULL;

#if 1
		//if (MPI_Isend(e,
		if (MPI_Isend(buffer,
			EVENT_SIZE(e),
			MPI_BYTE,
			*dest_node,
			EVENT_TAG,
			MPI_COMM_WORLD,
			&posted_sends.req_list[id]) != MPI_SUCCESS) {
			return changed;
		}
#else
		if (MPI_Send(e,
			EVENT_SIZE(e),
			MPI_BYTE,
			*dest_node,
			EVENT_TAG,
			MPI_COMM_WORLD) != MPI_SUCCESS) {
			return changed;
		}
#endif

		tw_eventq_pop(&outq);
		e->state.owner = e->state.cancel_q
			? TW_net_acancel
			: TW_net_asend;

		posted_sends.event_list[id] = e;
		posted_sends.cur++;
		me->s_nwhite_sent++;

		changed = 1;
	}
	return changed;
}

static void
send_finish(tw_pe *me, tw_event *e, char * buffer)
{
	me->s_nsend_network++;

	if (e->state.owner == TW_net_asend) {
		if (e->state.cancel_asend) {
			/* Event was cancelled during transmission.  We must
			 * send another message to pass the cancel flag to
			 * the other node.
			 */
			e->state.cancel_asend = 0;
			e->state.cancel_q = 1;
			tw_eventq_unshift(&outq, e);
		} else {
			/* Event finished transmission and was not cancelled.
			 * Add to our sent event queue so we can retain the
			 * event in case we need to cancel it later.  Note it
			 * is currently in remote format and must be converted
			 * back to local format for fossil collection.
			 */
			e->state.owner = TW_pe_sevent_q;
		}

		return;
	}

	if (e->state.owner == TW_net_acancel) {
		/* We just finished sending the cancellation message
		 * for this event.  We need to free the buffer and
		 * make it available for reuse.
		 */
		tw_event_free(me, e);
		return;
	}

	/* Never should happen, not unless we somehow broke this
	 * module's other functions related to sending an event.
	 */
	tw_error(
		TW_LOC,
		"Don't know how to finish send of owner=%u, cancel_q=%d",
		e->state.owner,
		e->state.cancel_q);
}

static void
service_queues(tw_pe *me)
{
	int changed;
	do {
		changed  = test_q(&posted_recvs, me, recv_finish);
		changed |= test_q(&posted_sends, me, send_finish);
		changed |= recv_begin(me);
		changed |= send_begin(me);
	} while (changed);
}

/*
 * NOTE: Chris believes that this network layer is too aggressive at
 * reading events out of the network.. so we are modifying the algorithm
 * to only send events when tw_net_send it called, and only read events
 * when tw_net_read is called.
 */
void
tw_net_read(tw_pe *me)
{
	service_queues(me);
}

void
tw_net_send(tw_event *e)
{
	tw_pe * me = e->src_lp->pe;

	e->state.remote = 0;
	e->state.owner = TW_net_outq;
	tw_eventq_unshift(&outq, e);

#if ROSS_NET_aggressive
	service_queues(me);
#else
	{
		int changed = 0;

		do
		{
			changed = test_q(&posted_sends, me, send_finish);
			changed |= send_begin(me);
		} while (changed);
	}
#endif
}

void
tw_net_cancel(tw_event *e)
{
	tw_pe *src_pe = e->src_lp->pe;

	switch (e->state.owner) {
	case TW_net_outq:
		/* Cancelled before we could transmit it.  Do not
		 * transmit the event and instead just release the
		 * buffer back into our own free list.
		 */
		tw_eventq_delete_any(&outq, e);
		tw_event_free(src_pe, e);

		return;

		break;

	case TW_net_asend:
		/* Too late.  We've already let MPI start to send
		 * this event over the network.  We can't pull it
		 * back now without sending another message to do
		 * the cancel.
		 *
		 * Setting the cancel_q flag will signal us to do
		 * another message send once the current send of
		 * this message is completed.
		 */
		e->state.cancel_asend = 1;
		break;

	case TW_pe_sevent_q:
		/* Way late; the event was already sent and is in
		 * our sent event queue.  Mark it as a cancel and
		 * place it at the front of the outq.
		 */
		e->state.cancel_q = 1;
		tw_eventq_unshift(&outq, e);
		break;

	default:
		/* Huh?  Where did you come from?  Why are we being
		 * told about you?  We did not send you so we cannot
		 * cancel you!
		 */
		tw_error(
			TW_LOC,
			"Don't know how to cancel event owned by %u",
			e->state.owner);
	}

	service_queues(src_pe);
}

tw_statistics	*
tw_net_statistics(tw_pe * me, tw_statistics * s)
{
	if(MPI_Reduce(&(s->max_run_time), 
			&me->statistics.max_run_time,
			1,
			MPI_DOUBLE,
			MPI_MAX,
			g_tw_masternode,
			MPI_COMM_WORLD) != MPI_SUCCESS)
		tw_error(TW_LOC, "Unable to reduce statistics!");

	if(MPI_Reduce(&s->tw_s_net_events, 
			&me->statistics.tw_s_net_events,
			14,
			MPI_LONG_LONG,
			MPI_SUM,
			g_tw_masternode,
			MPI_COMM_WORLD) != MPI_SUCCESS)
		tw_error(TW_LOC, "Unable to reduce statistics!");

	return &me->statistics;
}
