/*************************************************************************/
/*  packet_peer_udp_posix.cpp                                            */
/*************************************************************************/
/*                       This file is part of:                           */
/*                           GODOT ENGINE                                */
/*                    http://www.godotengine.org                         */
/*************************************************************************/
/* Copyright (c) 2007-2016 Juan Linietsky, Ariel Manzur.                 */
/*                                                                       */
/* Permission is hereby granted, free of charge, to any person obtaining */
/* a copy of this software and associated documentation files (the       */
/* "Software"), to deal in the Software without restriction, including   */
/* without limitation the rights to use, copy, modify, merge, publish,   */
/* distribute, sublicense, and/or sell copies of the Software, and to    */
/* permit persons to whom the Software is furnished to do so, subject to */
/* the following conditions:                                             */
/*                                                                       */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.       */
/*                                                                       */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,       */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF    */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.*/
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY  */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,  */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE     */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                */
/*************************************************************************/
#include "packet_peer_udp_posix.h"

#ifdef UNIX_ENABLED


#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <stdio.h>

#ifndef NO_FCNTL
	#ifdef __HAIKU__
		#include <fcntl.h>
	#else
		#include <sys/fcntl.h>
	#endif
#else
#include <sys/ioctl.h>
#endif

#ifdef JAVASCRIPT_ENABLED
#include <arpa/inet.h>
#endif

#include "drivers/unix/socket_helpers.h"

int PacketPeerUDPPosix::get_available_packet_count() const {

	Error err = const_cast<PacketPeerUDPPosix*>(this)->_poll(false);
	if (err!=OK)
		return 0;

	return queue_count;
}

Error PacketPeerUDPPosix::get_packet(const uint8_t **r_buffer,int &r_buffer_size) const{

	Error err = const_cast<PacketPeerUDPPosix*>(this)->_poll(false);
	if (err!=OK)
		return err;
	if (queue_count==0)
		return ERR_UNAVAILABLE;

	uint32_t size;
	uint8_t type;
	rb.read(&type, 1, true);
	if (type == IP_Address::TYPE_IPV4) {
		rb.read((uint8_t*)&packet_ip.field8,4,true);
		packet_ip.type = IP_Address::TYPE_IPV4;
	} else {
		rb.read((uint8_t*)&packet_ip.field8,16,true);
		packet_ip.type = IP_Address::TYPE_IPV6;
	};
	rb.read((uint8_t*)&packet_port,4,true);
	rb.read((uint8_t*)&size,4,true);
	rb.read(packet_buffer,size,true);
	--queue_count;
	*r_buffer=packet_buffer;
	r_buffer_size=size;
	return OK;

}
Error PacketPeerUDPPosix::put_packet(const uint8_t *p_buffer,int p_buffer_size){

	ERR_FAIL_COND_V(peer_addr.type == IP_Address::TYPE_NONE, ERR_UNCONFIGURED);

	int sock = _get_socket(peer_addr.type);
	ERR_FAIL_COND_V( sock == -1, FAILED );
	struct sockaddr_storage addr;
	size_t addr_size = _set_sockaddr(&addr, peer_addr, peer_port);

	errno = 0;
	int err;

	while ( (err = sendto(sock, p_buffer, p_buffer_size, 0, (struct sockaddr*)&addr, addr_size)) != p_buffer_size) {

		if (errno != EAGAIN) {
			return FAILED;
		}
	}

	return OK;
}

int PacketPeerUDPPosix::get_max_packet_size() const{

	return 512; // uhm maybe not
}

Error PacketPeerUDPPosix::listen(int p_port, IP_Address::AddrType p_address_type, int p_recv_buffer_size) {

	ERR_FAIL_COND_V(p_address_type != IP_Address::TYPE_IPV4 && p_address_type != IP_Address::TYPE_IPV6, ERR_INVALID_PARAMETER);

	close();
	int sock = _get_socket(p_address_type);
	if (sock == -1 )
		return ERR_CANT_CREATE;

	sockaddr_storage addr = {0};
	size_t addr_size = _set_listen_sockaddr(&addr, p_port, p_address_type, NULL);

	if (bind(sock, (struct sockaddr*)&addr, addr_size) == -1 ) {
		close();
		return ERR_UNAVAILABLE;
	}
	rb.resize(nearest_shift(p_recv_buffer_size));
	return OK;
}

void PacketPeerUDPPosix::close(){

	if (sockfd != -1)
		::close(sockfd);
	sockfd=-1;
	rb.resize(8);
	queue_count=0;
}


Error PacketPeerUDPPosix::wait() {

	return _poll(true);
}

Error PacketPeerUDPPosix::_poll(bool p_wait) {

	struct sockaddr_storage from = {0};
	socklen_t len = sizeof(struct sockaddr_storage);
	int ret;
	while ( (ret = recvfrom(sockfd, recv_buffer, MIN((int)sizeof(recv_buffer),MAX(rb.space_left()-12, 0)), p_wait?0:MSG_DONTWAIT, (struct sockaddr*)&from, &len)) > 0) {

		uint32_t port = 0;

		if (from.ss_family == AF_INET) {
			uint8_t type = (uint8_t)IP_Address::TYPE_IPV4;
			rb.write(&type, 1);
			struct sockaddr_in* sin_from = (struct sockaddr_in*)&from;
			rb.write((uint8_t*)&sin_from->sin_addr, 4);
			port = sin_from->sin_port;

		} else if (from.ss_family == AF_INET6) {

			uint8_t type = (uint8_t)IP_Address::TYPE_IPV6;
			rb.write(&type, 1);

			struct sockaddr_in6* s6_from = (struct sockaddr_in6*)&from;
			rb.write((uint8_t*)&s6_from->sin6_addr, 16);

			port = s6_from->sin6_port;

		} else {
			// WARN_PRINT("Ignoring packet with unknown address family");
			uint8_t type = (uint8_t)IP_Address::TYPE_NONE;
			rb.write(&type, 1);
		};

		rb.write((uint8_t*)&port, 4);
		rb.write((uint8_t*)&ret, 4);
		rb.write(recv_buffer, ret);

		len = sizeof(struct sockaddr_storage);
		++queue_count;
	};


	// TODO: Should ECONNRESET be handled here?
	if (ret == 0 || (ret == -1 && errno != EAGAIN) ) {
		close();
		return FAILED;
	};

	return OK;
}
bool PacketPeerUDPPosix::is_listening() const{

	return sockfd!=-1;
}

IP_Address PacketPeerUDPPosix::get_packet_address() const {

	return packet_ip;
}

int PacketPeerUDPPosix::get_packet_port() const{

	return packet_port;
}

int PacketPeerUDPPosix::_get_socket(IP_Address::AddrType p_type) {

	if (sockfd != -1)
		return sockfd;

	int family = p_type == IP_Address::TYPE_IPV6 ? AF_INET6 : AF_INET;

	sockfd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
	ERR_FAIL_COND_V( sockfd == -1, -1 );
	//fcntl(sockfd, F_SETFL, O_NONBLOCK);

	return sockfd;
}


void PacketPeerUDPPosix::set_send_address(const IP_Address& p_address,int p_port) {

	peer_addr=p_address;
	peer_port=p_port;
}

PacketPeerUDP* PacketPeerUDPPosix::_create() {

	return memnew(PacketPeerUDPPosix);
};

void PacketPeerUDPPosix::make_default() {

	PacketPeerUDP::_create = PacketPeerUDPPosix::_create;
};


PacketPeerUDPPosix::PacketPeerUDPPosix() {

	sockfd=-1;
	packet_port=0;
	queue_count=0;
	peer_port=0;
}

PacketPeerUDPPosix::~PacketPeerUDPPosix() {

	close();
}
#endif
