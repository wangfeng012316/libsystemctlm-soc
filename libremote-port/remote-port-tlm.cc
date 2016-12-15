/*
 * System-C TLM-2.0 remoteport glue
 *
 * Copyright (c) 2013 Xilinx Inc
 * Written by Edgar E. Iglesias
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define SC_INCLUDE_DYNAMIC_PROCESSES

#include <inttypes.h>
#include <sys/utsname.h>

#include "systemc.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/tlm_quantumkeeper.h"
#include <iostream>

extern "C" {
#include "safeio.h"
#include "remote-port-proto.h"
#include "remote-port-sk.h"
};
#include "remote-port-tlm.h"

using namespace sc_core;
using namespace std;

remoteport_packet::remoteport_packet(void)
{
	u8 = NULL;
	size = 0;
	alloc(sizeof *pkt);
}

void remoteport_packet::alloc(size_t new_size)
{
	if (size < new_size) {
		u8 = (uint8_t *) realloc(u8, new_size);
		if (u8 == NULL) {
			cerr << "out of mem" << endl;
			exit(EXIT_FAILURE);
		}
		size = new_size;
	}
}

remoteport_tlm::remoteport_tlm(sc_module_name name,
			int fd,
			const char *sk_descr)
	: sc_module(name),
	  rst("rst")
{
	this->fd = fd;
	this->sk_descr = sk_descr;
	this->rp_pkt_id = 0;

	memset(devs, 0, sizeof devs);
	memset(&peer, 0, sizeof peer);

	SC_THREAD(process);
}

void remoteport_tlm::register_dev(unsigned int dev_id, remoteport_tlm_dev *dev)
{
	assert(dev_id < RP_MAX_DEVS);
	assert(this->devs[dev_id] == NULL);
	this->devs[dev_id] = dev;
	dev->adaptor = this;
	dev->dev_id = dev_id;
}

/* Convert an sc_time into int64 nanoseconds trying to avoid rounding errors.
   Why make this easy when you don't have to?  */
int64_t remoteport_tlm::rp_map_time(sc_time t)
{
	sc_time tr, tmp;
	double dtr;

	tr = sc_get_time_resolution();
	dtr = tr.to_seconds() * 1000 * 1000 * 1000;

	tmp = t * dtr;
	return tmp.value();
}

void remoteport_tlm::tie_off(void)
{
	unsigned int i;

	for (i = 0; i < RP_MAX_DEVS; i++) {
		if (devs[i]) {
			devs[i]->tie_off();
		}
	}
}

ssize_t remoteport_tlm::rp_read(void *rbuf, size_t count)
{
	ssize_t r;

	r = safe_read(fd, rbuf, count);
	if (r < (ssize_t)count) {
		if (r < 0)
			perror(__func__);
		exit(EXIT_FAILURE);
	}
	return r;
}

ssize_t remoteport_tlm::rp_write(const void *wbuf, size_t count)
{
	ssize_t r;

	r = safe_write(fd, wbuf, count);
	if (r < (ssize_t)count) {
		if (r < 0)
			perror(__func__);
		exit(EXIT_FAILURE);
	}
	return r;
}

void remoteport_tlm::rp_cmd_hello(struct rp_pkt &pkt)
{
	if (pkt.hello.version.major != RP_VERSION_MAJOR) {
		cerr << "RP Version missmatch"
			<< " remote=" << pkt.hello.version.major
			<< "." << pkt.hello.version.minor
			<< " local=" << RP_VERSION_MAJOR
			<< "." << RP_VERSION_MINOR
			<< endl;
		exit(EXIT_FAILURE);
	}

	if (pkt.hello.caps.len) {
		void *caps = (char *) &pkt + pkt.hello.caps.offset;

		rp_process_caps(&peer, caps, pkt.hello.caps.len);
	}
}

void remoteport_tlm::account_time(int64_t rclk)
{
	int64_t lclk;
	int64_t delta_ns;
	sc_time delta;

	lclk = rp_map_time(m_qk.get_current_time());
	if (lclk >= rclk)
		return;

	delta_ns = rclk - lclk;

	assert(delta_ns >= 0);
	delta = sc_time((double) delta_ns, SC_NS);
	assert(delta >= SC_ZERO_TIME);
	if (delta > m_qk.get_global_quantum()) {
		delta = m_qk.get_global_quantum();
	}

#if 0
	cout << "account rclk=" << rclk << " current=" << m_qk.get_current_time() << " delta=" << delta << endl;
#endif
	m_qk.inc(delta);
	return;
}

void remoteport_tlm::rp_say_hello(void)
{
	uint32_t caps[] = {
		CAP_BUSACCESS_EXT_BASE,
	};
	struct rp_pkt_hello pkt;
	size_t len;

	len = rp_encode_hello_caps(rp_pkt_id++, 0,
				   &pkt, RP_VERSION_MAJOR, RP_VERSION_MINOR,
				   caps, caps, sizeof caps / sizeof caps[0]);
	rp_write(&pkt, len);
	rp_write(caps, sizeof caps);
}

void remoteport_tlm::rp_cmd_sync(struct rp_pkt &pkt, bool can_sync)
{
	size_t plen;
        int64_t clk;

	account_time(pkt.sync.timestamp);

	clk = rp_map_time(m_qk.get_current_time());
	plen = rp_encode_sync_resp(pkt.hdr.id,
				   pkt.hdr.dev, &pkt_tx.pkt->sync,
				   clk);
	rp_write(pkt_tx.pkt, plen);

	/* Relaxing this sync to run in parallel with the remote
	   speeds up simulation significantly but allows us to skew off
	   time (in theory). The added inaccuracy is not really observable
	   to any side of the simulation though.  */
	if (can_sync && m_qk.need_sync()) {
		m_qk.sync();
	}
//	cout << "sync done " << m_qk.get_current_time() << endl;

}

bool remoteport_tlm::rp_process(bool can_sync)
{
	ssize_t r;

	while (1) {
		remoteport_tlm_dev *dev;
		unsigned char *data;
		uint32_t dlen;
		size_t datalen;

		r = rp_read(&pkt_rx.pkt->hdr, sizeof pkt_rx.pkt->hdr);
		if (r < 0)
			perror(__func__);

		rp_decode_hdr(pkt_rx.pkt);

		pkt_rx.alloc(sizeof pkt_rx.pkt->hdr + pkt_rx.pkt->hdr.len);
		r = rp_read(&pkt_rx.pkt->hdr + 1, pkt_rx.pkt->hdr.len);

		dlen = rp_decode_payload(pkt_rx.pkt);
		data = pkt_rx.u8 + sizeof pkt_rx.pkt->hdr + dlen;
		datalen = pkt_rx.pkt->hdr.len - dlen;
		if (pkt_rx.pkt->hdr.flags & RP_PKT_FLAGS_response) {
			pkt_rx.data_offset = sizeof pkt_rx.pkt->hdr + dlen;
			return true;
		}

		dev = devs[pkt_rx.pkt->hdr.dev];
//		printf("%s: cmd=%d dev=%d\n", __func__, pkt_rx.pkt->hdr.cmd, pkt_rx.pkt->hdr.dev);
		switch (pkt_rx.pkt->hdr.cmd) {
		case RP_CMD_hello:
			rp_cmd_hello(*pkt_rx.pkt);
			break;
		case RP_CMD_write:
			assert(dev);
			dev->cmd_write(*pkt_rx.pkt, can_sync, data, datalen);
			break;
		case RP_CMD_read:
			assert(dev);
			dev->cmd_read(*pkt_rx.pkt, can_sync);
			break;
		case RP_CMD_interrupt:
			assert(dev);
			dev->cmd_interrupt(*pkt_rx.pkt, can_sync);
			break;
		case RP_CMD_sync:
                        rp_cmd_sync(*pkt_rx.pkt, can_sync);
			break;
		default:
			assert(0);
			break;
		}
		/* We've just processed peer packets and it is
		   likely running freely. Good spot for a local sync.  */
		if (can_sync) {
			m_qk.sync();
		}
	}
	return false;
}

void remoteport_tlm::process(void)
{
	if (fd == -1) {
		fd = sk_open(sk_descr);
		if (fd == -1) {
			printf("Failed to create remote-port socket connection!\n");
			if (sk_descr) {
				perror(sk_descr);
			}
			exit(EXIT_FAILURE);
			return;
		}
	}

	m_qk.reset();
	wait(rst.negedge_event());

	rp_say_hello();

	while (1) {
		rp_process(true);
	}
	m_qk.sync();
	return;
}
