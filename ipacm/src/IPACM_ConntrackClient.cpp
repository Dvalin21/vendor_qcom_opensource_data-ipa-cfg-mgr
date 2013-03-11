/* 
Copyright (c) 2013, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
		* Redistributions of source code must retain the above copyright
				notice, this list of conditions and the following disclaimer.
		* Redistributions in binary form must reproduce the above
				copyright notice, this list of conditions and the following
				disclaimer in the documentation and/or other materials provided
				with the distribution.
		* Neither the name of The Linux Foundation nor the names of its
				contributors may be used to endorse or promote products derived
				from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <iostream>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include "IPACM_ConntrackListener.h"
#include "IPACM_ConntrackClient.h"
#include "IPACM_Log.h"

#define LO_NAME "lo"

extern IPACM_EvtDispatcher cm_dis;

IPACM_ConntrackClient *IPACM_ConntrackClient::pInstance = NULL;
IPACM_ConntrackListener *ct = new IPACM_ConntrackListener();

/* ================================
		 Local Function Definitions
		 =================================
*/
#ifdef IPACM_DEBUG
void IPACM_ConntrackClient::iptodot(const char *type, uint32_t ipAddr)
{
	int i;
	unsigned char octet[4] = { 0 };
	IPACMDBG("Received IPv4 addr: 0x%x\n", ipAddr);

	for(i = 0; i < 4; i++)
	{
		octet[i] = (ipAddr >> (i * 8)) & 0xFF;
	}

	IPACMDBG("%s:", type);
	IPACMDBG("%d.%d.%d.%d\n", octet[3], octet[2], octet[1], octet[0]);
}
#endif

IPACM_ConntrackClient::IPACM_ConntrackClient()
{
	IPACMDBG("%s %d", __FUNCTION__, __LINE__);

	tcp_hdl = NULL;
	udp_hdl = NULL;
	tcp_filter = NULL;
	udp_filter = NULL;
}

IPACM_ConntrackClient* IPACM_ConntrackClient::GetInstance()
{
	if(pInstance == NULL)
	{
		pInstance = new IPACM_ConntrackClient();
	}

	return pInstance;
}

int IPACM_ConntrackClient::IPAConntrackEventCB
(
	 enum nf_conntrack_msg_type type,
	 struct nf_conntrack *ct,
	 void *data
	 )
{
	ipacm_cmd_q_data evt_data;
	ipacm_ct_evt_data *ct_data;

	IPACMDBG("Event callback called with msgtype: %d\n",type);

	ct_data = (ipacm_ct_evt_data *)malloc(sizeof(ipacm_ct_evt_data));
	if(ct_data == NULL)
	{
		IPACMERR("unable to allocate memory \n");
		return -1;
	}

	ct_data->ct = ct;
	ct_data->type = type;

	evt_data.event = IPA_PROCESS_CT_MESSAGE;
	evt_data.evt_data = (void *)ct_data;

	if(0 != IPACM_EvtDispatcher::PostEvt(&evt_data))
	{
		IPACMERR("Error sending Conntrack message to processing thread!\n");
		free(ct_data);
	}

#if 0
	IPACMDBG("Posted message to Cmd Queue\n");
#endif

	/* NFCT_CB_STOLEN means that the conntrack object is not released after the
	 callback That must be manually done later when the object is no longer needed. */
	return NFCT_CB_STOLEN;
}


/* Function which sets up filters to ignore
		 connections to and from local interfaces */
int IPACM_ConntrackClient::IPA_Conntrack_Filters_Ignore_Local_Addrs
(
	 struct nfct_filter *filter
)
{
	char   buf[1024];
	struct ifconf ifc;
	struct ifreq *ifr;
	int    i, sck, nInterfaces;
	struct nfct_filter_ipv4 filter_ipv4;

	/* ignore whatever is destined to or originates from broadcast ip address */
	filter_ipv4.addr = 0xffffffff;
	filter_ipv4.mask = 0xffffffff;

	nfct_filter_set_logic(filter,
												NFCT_FILTER_DST_IPV4,
												NFCT_FILTER_LOGIC_NEGATIVE);

	nfct_filter_add_attr(filter, NFCT_FILTER_DST_IPV4, &filter_ipv4);

	nfct_filter_set_logic(filter,
												NFCT_FILTER_SRC_IPV4,
												NFCT_FILTER_LOGIC_NEGATIVE);

	nfct_filter_add_attr(filter, NFCT_FILTER_SRC_IPV4, &filter_ipv4);



	/* Get a socket handle. */
	sck = socket(AF_INET, SOCK_DGRAM, 0);
	if(sck < 0)
	{
		PERROR("socket");
		return -1;
	}

	/* Query available interfaces. */
	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;

	/* get iface list */
	if(ioctl(sck, SIOCGIFCONF, &ifc) < 0)
	{
		PERROR("ioctl(SIOCGIFCONF)");
		return -1;
	}

	/* Iterate through the list of interfaces. */
	ifr         = ifc.ifc_req;
	nInterfaces = ifc.ifc_len / sizeof(struct ifreq);

#ifdef IPACM_DEBUG
	IPACMDBG("====Printing Local Interfaces=====\n");
#endif

	for(i = 0; i < nInterfaces; i++)
	{
		/* Interface request structure */
		struct ifreq *item = &ifr[i];

#ifdef IPACM_DEBUG
		/* Show the device name and IP address */
		if(item->ifr_name != NULL)
		{
			IPACMDBG("%s: IP %s\n",
							 item->ifr_name,
							 inet_ntoa(((struct sockaddr_in *)&item->ifr_addr)->sin_addr));
		}
#endif

		/* Convert data to host-byte order */
		filter_ipv4.addr =
			 ntohl(inet_addr(inet_ntoa(((struct sockaddr_in *)&item->ifr_addr)->sin_addr)));
		filter_ipv4.mask = 0xffffffff;

		/* ignore whatever is destined to or originates from local interfaces */
		if(item->ifr_name != NULL)
		{
			if(strncmp(ct->wan_ifname, item->ifr_name, strlen(item->ifr_name)) != 0)
			{
				IPACMDBG("ignore connections destinated to interface %s\n", item->ifr_name);
				IPACM_ConntrackClient::iptodot("with ipv4 address:", filter_ipv4.addr);
				nfct_filter_set_logic(filter,
															NFCT_FILTER_DST_IPV4,
															NFCT_FILTER_LOGIC_NEGATIVE);

				nfct_filter_add_attr(filter, NFCT_FILTER_DST_IPV4, &filter_ipv4);
			}
		}

		IPACMDBG("ignore connections orignated to interface %s\n", item->ifr_name);
		IPACM_ConntrackClient::iptodot("with ipv4 address:", filter_ipv4.addr);
		nfct_filter_set_logic(filter,
													NFCT_FILTER_SRC_IPV4,
													NFCT_FILTER_LOGIC_NEGATIVE);

		nfct_filter_add_attr(filter, NFCT_FILTER_SRC_IPV4, &filter_ipv4);

		/* Find broadcast address for non lo interfaces */
		if(strncmp(LO_NAME, item->ifr_name, 2) != 0)
		{
			/* Get the broadcast address */
			if(ioctl(sck, SIOCGIFBRDADDR, item) < 0)
			{
				PERROR("broadcast address error: ioctl(SIOCGIFBRDADDR)");
				return -1;
			}

#ifdef IPACM_DEBUG
			/* Show the device name and IP address */
			if(item->ifr_name != NULL)
			{
				IPACMDBG("%s: BroadCast IP %s\n",
								 item->ifr_name,
								 inet_ntoa(((struct sockaddr_in *)&item->ifr_broadaddr)->sin_addr));
			}
#endif

			/* Convert data to host-byte order */
			filter_ipv4.addr =
				 ntohl(inet_addr(inet_ntoa(((struct sockaddr_in *)&item->ifr_broadaddr)->sin_addr)));
			filter_ipv4.mask = 0xffffffff;
			
			IPACMDBG("ignore connections destinated to interface %s broadcast\n", item->ifr_name);
			IPACM_ConntrackClient::iptodot("with ipv4 address:", filter_ipv4.addr);
			nfct_filter_set_logic(filter,
														NFCT_FILTER_DST_IPV4,
														NFCT_FILTER_LOGIC_NEGATIVE);

			nfct_filter_add_attr(filter, NFCT_FILTER_DST_IPV4, &filter_ipv4);
		}

	}

	close(sck);
	return 0;
} /* IPA_Conntrack_Filters_Ignore_Local_Addrs() */

/* Initialize TCP Filter */
int IPACM_ConntrackClient::IPA_Conntrack_TCP_Filter_Init(void)
{
	int ret = 0;
	IPACM_ConntrackClient *pClient;

	IPACMDBG("\n");

	pClient = IPACM_ConntrackClient::GetInstance();
	if(pClient == NULL)
	{
		IPACMERR("unable to get conntrack client instance\n");
		return -1;
	}

	ret = IPA_Conntrack_Filters_Ignore_Local_Addrs(pClient->tcp_filter);
	if(ret == -1)
	{
		IPACMERR("Unable to set local addr filters\n");
		return -1;
	}

	ret = nfct_filter_set_logic(pClient->tcp_filter,
															NFCT_FILTER_L4PROTO,
															NFCT_FILTER_LOGIC_POSITIVE);
	if(ret == -1)
	{
		IPACMERR("Unable to set filter logic\n");
		return -1;
	}

	/* set protocol filters as tcp and udp */
	nfct_filter_add_attr_u32(pClient->tcp_filter, NFCT_FILTER_L4PROTO, IPPROTO_TCP);


	struct nfct_filter_proto tcp_proto_state;
	tcp_proto_state.proto = IPPROTO_TCP;
	tcp_proto_state.state = TCP_CONNTRACK_ESTABLISHED;

	ret = nfct_filter_set_logic(pClient->tcp_filter,
															NFCT_FILTER_L4PROTO_STATE,
															NFCT_FILTER_LOGIC_POSITIVE);
	if(ret == -1)
	{
		IPACMERR("unable to set filter logic\n");
		return -1;
	}
	nfct_filter_add_attr(pClient->tcp_filter,
											 NFCT_FILTER_L4PROTO_STATE,
											 &tcp_proto_state);


	tcp_proto_state.proto = IPPROTO_TCP;
	tcp_proto_state.state = TCP_CONNTRACK_FIN_WAIT;
	ret = nfct_filter_set_logic(pClient->tcp_filter,
															NFCT_FILTER_L4PROTO_STATE,
															NFCT_FILTER_LOGIC_POSITIVE);
	if(ret == -1)
	{
		IPACMERR("unable to set filter logic\n");
		return -1;
	}

	nfct_filter_add_attr(pClient->tcp_filter,
											 NFCT_FILTER_L4PROTO_STATE,
											 &tcp_proto_state);
	return 0;
}


/* Initialize UDP Filter */
int IPACM_ConntrackClient::IPA_Conntrack_UDP_Filter_Init(void)
{
	int ret = 0;
	IPACM_ConntrackClient *pClient = IPACM_ConntrackClient::GetInstance();

	ret = IPA_Conntrack_Filters_Ignore_Local_Addrs(pClient->udp_filter);
	if(ret == -1)
	{
		IPACMERR("Unable to set local addr filters\n");
		return -1;
	}

	ret = nfct_filter_set_logic(pClient->udp_filter,
															NFCT_FILTER_L4PROTO,
															NFCT_FILTER_LOGIC_POSITIVE);
	if(ret == -1)
	{
		IPACMERR("unable to set filter logic\n");
	}
	/* set protocol filters as tcp and udp */
	nfct_filter_add_attr_u32(pClient->udp_filter, NFCT_FILTER_L4PROTO, IPPROTO_UDP);

	return 0;
}

void* IPACM_ConntrackClient::UDPConnTimeoutUpdate(void *ptr)
{

#ifdef IPACM_DEBUG
	IPACMDBG("\n");
#endif

	while(1)
	{
		NatApp::GetInstance()->UpdateUDPTimeStamp();
		sleep(UDP_TIMEOUT_UPDATE);
	} /* end of while(1) loop */

#ifdef IPACM_DEBUG
	IPACMDBG("Returning from %s() %d\n", __FUNCTION__, __LINE__);
#endif

	return NULL;
}

/* Thread to initialize TCP Conntrack Filters*/
void* IPACM_ConntrackClient::TCPRegisterWithConnTrack(void *)
{
	int ret;
	IPACM_ConntrackClient *pClient;

	IPACMDBG("\n");

	pClient = IPACM_ConntrackClient::GetInstance();
	if(pClient == NULL)
	{
		IPACMERR("unable to get conntrack client instance\n");
		return NULL;
	}

	pClient->tcp_hdl = nfct_open(CONNTRACK, NF_NETLINK_CONNTRACK_UPDATE);
	if(pClient->tcp_hdl == NULL)
	{
		PERROR("nfct_open\n");
		return NULL;
	}

	/* Allocate new filter */
	pClient->tcp_filter = nfct_filter_create();
	if(pClient->tcp_filter == NULL)
	{
		IPACMERR("unable to create TCP filter\n");
		return NULL;
	}

	/* Initialize the filter */
	ret = IPA_Conntrack_TCP_Filter_Init();
	if(ret == -1)
	{
		IPACMERR("Unable to initliaze TCP Filter\n");
		return NULL;
	}

	/* Attach the filter to net filter handler */
	ret = nfct_filter_attach(nfct_fd(pClient->tcp_hdl),
													 pClient->tcp_filter);
	if(ret == -1)
	{
		IPACMDBG("unable to attach TCP filter\n");
		return NULL;
	}

	/* Register callback with netfilter handler */
	IPACMDBG("tcp handle:%p, fd:%d\n", pClient->tcp_hdl, nfct_fd(pClient->tcp_hdl));
	nfct_callback_register(pClient->tcp_hdl, NFCT_T_UPDATE, IPAConntrackEventCB, NULL);

	/* Block to catch events from net filter connection track */
	/* nfct_catch() receives conntrack events from kernel-space, by default it 
			 blocks waiting for events. */
	IPACMDBG("Waiting for events\n");

	ret = nfct_catch(pClient->tcp_hdl);
	if(ret == -1)
	{
		IPACMERR("(%d)(%s)\n", ret, strerror(errno));
		return NULL;
	}

	IPACMDBG("Exit from tcp thread\n");

	/* destroy the filter.. this will not detach the filter */
	nfct_filter_destroy(pClient->tcp_filter);
	pClient->tcp_filter = NULL;

	/* de-register the callback */
	nfct_callback_unregister(pClient->tcp_hdl);
	/* close the handle */
	nfct_close(pClient->tcp_hdl);
  pClient->tcp_hdl = NULL;

	pthread_exit(NULL);
	return NULL;
}

/* Thread to initialize UDP Conntrack Filters*/
void* IPACM_ConntrackClient::UDPRegisterWithConnTrack(void *)
{
	int ret;
	IPACM_ConntrackClient *pClient = NULL;
	
	IPACMDBG("\n");

	pClient = IPACM_ConntrackClient::GetInstance();
	if(pClient == NULL)
	{
		IPACMERR("unable to retrieve instance of conntrack client\n");
		return NULL;
	}

	pClient->udp_hdl = nfct_open(CONNTRACK,
															 NF_NETLINK_CONNTRACK_NEW | NF_NETLINK_CONNTRACK_DESTROY);
	if(pClient->udp_hdl == NULL)
	{
		PERROR("nfct_open\n");
		return NULL;
	}

	/* Add filter */
	//struct nfct_filter *udp_filter = NULL;

	/* Allocate new filter */
	pClient->udp_filter = nfct_filter_create();
	if(pClient->udp_filter == NULL)
	{
		IPACMERR("unable to create UDP filter\n");
		return NULL;
	}

	/* Initialize Filter */
	ret = IPA_Conntrack_UDP_Filter_Init();
	if(-1 == ret)
	{
		IPACMDBG("Unable to initalize udp filters\n");
		return NULL;
	}

	/* Attach the filter to net filter handler */
	ret = nfct_filter_attach(nfct_fd(pClient->udp_hdl), pClient->udp_filter);
	if(ret == -1)
	{
		IPACMDBG("unable to attach the filter\n");
		return NULL;
	}

	/* Register callback with netfilter handler */
	IPACMDBG("udp handle:%p, fd:%d\n", pClient->udp_hdl, nfct_fd(pClient->udp_hdl));
	nfct_callback_register(pClient->udp_hdl,
												 (nf_conntrack_msg_type)(NFCT_T_NEW | NFCT_T_DESTROY),
												 IPAConntrackEventCB,
												 NULL);

	/* Block to catch events from net filter connection track */
	ret = nfct_catch(pClient->udp_hdl);
	if(ret == -1)
	{
		IPACMDBG("(%d)(%s)\n", ret, strerror(errno));
		return NULL;
	}


	IPACMDBG("Exit from udp thread\n");

	/* destroy the filter.. this will not detach the filter */
	nfct_filter_destroy(pClient->udp_filter);
	pClient->udp_filter = NULL;

	/* de-register the callback */
	nfct_callback_unregister(pClient->udp_hdl);
	/* close the handle */
	nfct_close(pClient->udp_hdl);
  pClient->udp_hdl = NULL;
	
	pthread_exit(NULL);
	return NULL;
}

void IPACM_ConntrackClient::UpdateUDPFilters(void *param)
{
	int ret = 0;
	struct nfct_filter_ipv4 filter_ipv4;
	IPACM_ConntrackClient *pClient = NULL;

	/* Intialize with 255.255.255.255 */
	uint32_t bc_ip_addr = 0xFFFFFFFF;

	uint32_t ipv4_addr = ((ipacm_event_iface_up *)param)->ipv4_addr;
  uint32_t ipv4_addr_mask = ((ipacm_event_iface_up *)param)->addr_mask;

	IPACM_ConntrackClient::iptodot("Received ipv4 address and", ipv4_addr);
	IPACM_ConntrackClient::iptodot("ipv4 address mask", ipv4_addr_mask);

	pClient = IPACM_ConntrackClient::GetInstance();
	if(pClient == NULL)
	{
		IPACMERR("unable to retrieve conntrack client instance\n");
		return;
	}

	/* calculate broadcast address from addr and addr_mask */
	bc_ip_addr = (bc_ip_addr & (~ipv4_addr_mask));
	bc_ip_addr = (bc_ip_addr | (ipv4_addr & ipv4_addr_mask));

	/* netfitler expecting in host-byte order */
	filter_ipv4.addr = ipv4_addr;
  filter_ipv4.mask = 0xffffffff;

	IPACMDBG("ignoring interface:%s", ((ipacm_event_iface_up *)param)->ifname);
	IPACM_ConntrackClient::iptodot("with ipv4 address", filter_ipv4.addr);
	if(pClient->udp_filter != NULL)
	{
		nfct_filter_set_logic(pClient->udp_filter,
													NFCT_FILTER_DST_IPV4,
													NFCT_FILTER_LOGIC_NEGATIVE);

		nfct_filter_add_attr(pClient->udp_filter, NFCT_FILTER_DST_IPV4, &filter_ipv4);

		nfct_filter_set_logic(pClient->udp_filter,
													NFCT_FILTER_SRC_IPV4,
													NFCT_FILTER_LOGIC_NEGATIVE);

		nfct_filter_add_attr(pClient->udp_filter, NFCT_FILTER_SRC_IPV4, &filter_ipv4);
	}

  /* netfitler expecting in host-byte order */
	filter_ipv4.addr = bc_ip_addr;
	filter_ipv4.mask = 0xffffffff; 

	IPACM_ConntrackClient::iptodot("with broadcast address", filter_ipv4.addr);
  if(pClient->udp_filter != NULL)
	{
		nfct_filter_set_logic(pClient->udp_filter,
													NFCT_FILTER_DST_IPV4,
													NFCT_FILTER_LOGIC_NEGATIVE);

		nfct_filter_add_attr(pClient->udp_filter, NFCT_FILTER_DST_IPV4, &filter_ipv4);
	}

	/* Attach the filter to udp handle */
	if(pClient->udp_hdl != NULL)
	{
		IPACMDBG("attaching the filter to udp handle\n");
		ret = nfct_filter_attach(nfct_fd(pClient->udp_hdl), pClient->udp_filter);
		if(ret == -1)
		{
			PERROR("unable to attach the filter to udp handle\n");
			IPACMERR("udp handle:%p, fd:%d Error: %d\n",pClient->udp_hdl, nfct_fd(pClient->udp_hdl), ret);
			return;
		}
	}

	return;
}

void IPACM_ConntrackClient::UpdateTCPFilters(void *param)
{
	int ret = 0;
	uint32_t ipv4_addr = ((ipacm_event_iface_up *)param)->ipv4_addr;
  uint32_t ipv4_addr_mask = ((ipacm_event_iface_up *)param)->addr_mask;
	struct nfct_filter_ipv4 filter_ipv4;
	IPACM_ConntrackClient *pClient = NULL;
	/* Intialize with 255.255.255.255 */
	uint32_t bc_ip_addr = 0xFFFFFFFF;

	pClient = IPACM_ConntrackClient::GetInstance();
	if(pClient == NULL)
	{
		IPACMERR("unable to retrieve conntrack client instance\n");
		return;
	}

	/* calculate broadcast address from addr and addr_mask */
	bc_ip_addr = (bc_ip_addr & (~ipv4_addr_mask));
	bc_ip_addr = (bc_ip_addr | (ipv4_addr & ipv4_addr_mask));

	/* netfitler expecting in host-byte order */
	filter_ipv4.addr = ipv4_addr;
  filter_ipv4.mask = 0xffffffff;

	IPACMDBG("ignoring interface:%s", ((ipacm_event_iface_up *)param)->ifname);
	IPACM_ConntrackClient::iptodot("with ipv4 address", filter_ipv4.addr);

  if(pClient->tcp_filter != NULL)
	{
		nfct_filter_set_logic(pClient->tcp_filter,
													NFCT_FILTER_DST_IPV4,
													NFCT_FILTER_LOGIC_NEGATIVE);

		nfct_filter_add_attr(pClient->tcp_filter, NFCT_FILTER_DST_IPV4, &filter_ipv4);

		nfct_filter_set_logic(pClient->tcp_filter,
													NFCT_FILTER_SRC_IPV4,
													NFCT_FILTER_LOGIC_NEGATIVE);

		nfct_filter_add_attr(pClient->tcp_filter, NFCT_FILTER_SRC_IPV4, &filter_ipv4);
	}

  /* netfitler expecting in host-byte order */
	filter_ipv4.addr = bc_ip_addr;
	filter_ipv4.mask = 0xffffffff; 

	IPACM_ConntrackClient::iptodot("with broadcast address", filter_ipv4.addr);
  if(pClient->tcp_filter != NULL)
	{
		nfct_filter_set_logic(pClient->tcp_filter,
													NFCT_FILTER_DST_IPV4,
													NFCT_FILTER_LOGIC_NEGATIVE);

		nfct_filter_add_attr(pClient->tcp_filter, NFCT_FILTER_DST_IPV4, &filter_ipv4);
	}

	/* Attach the filter to tcp handle */
	if(pClient->tcp_hdl != NULL)
	{
		IPACMDBG("attaching the filter to tcp handle\n");
		ret = nfct_filter_attach(nfct_fd(pClient->tcp_hdl), pClient->tcp_filter);
		if(ret == -1)
		{
			PERROR("unable to attach the filter to tcp handle\n");
			IPACMERR("tcp handle:%p, fd:%d Error: %d\n",pClient->tcp_hdl, nfct_fd(pClient->tcp_hdl), ret);
			return;
		}
	}

  return;
}
