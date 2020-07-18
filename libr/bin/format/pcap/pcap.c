#include <r_bin.h>

#include "pcap.h"

void pcap_obj_free (pcap_obj_t *obj) {
	if (obj) {
		free (obj->header);
		r_list_free (obj->recs);
		r_buf_fini (obj->b);
		free (obj);
	}
}

static bool pcap_obj_init_hdr(pcap_obj_t *obj) {
	pcap_hdr_t *hdr = R_NEW0 (pcap_hdr_t);
	if (!hdr) {
		return false;
	}
	hdr->magic = r_buf_read_ble32_at (obj->b, 0, obj->bigendian);
	hdr->version_major = r_buf_read_ble16_at (obj->b, 4, obj->bigendian);
	hdr->version_minor = r_buf_read_ble16_at (obj->b, 6, obj->bigendian);
	hdr->this_zone = r_buf_read_ble32_at (obj->b, 8, obj->bigendian);
	hdr->ts_accuracy = r_buf_read_ble32_at (obj->b, 12, obj->bigendian);
	hdr->max_pkt_len = r_buf_read_ble32_at (obj->b, 16, obj->bigendian);
	hdr->network = r_buf_read_ble32_at (obj->b, 20, obj->bigendian);
	obj->header = hdr;
	return true;
}

void pcaprec_free(pcaprec_t *rec) {
	if (rec) {
		free (rec->hdr);
		free (rec->link.ether_hdr);
		free (rec->net.ipv4_hdr);
		free (rec->transport.tcp_hdr);
		free (rec->data);
		free (rec);
	}
}

static void parse_tcp (pcaprec_t *rec, ut8 *buf, ut32 size, bool bigendian) {
	pcaprec_tcp_t *tcp = R_NEW0 (pcaprec_tcp_t);
	if (!tcp) {
		return;
	}
	tcp->src_port = r_read_at_be16 (buf, 0);
	tcp->dst_port = r_read_at_be16 (buf, 2);
	tcp->seq_num = r_read_at_be32 (buf, 4);
	tcp->ack_num = r_read_at_be32 (buf, 8);
	tcp->hdr_len = r_read_at_be8 (buf, 12);
	tcp->flags = r_read_at_be16 (buf, 13);
	tcp->win_sz = r_read_at_be16 (buf, 15);
	tcp->chksum = r_read_at_be16 (buf, 17);
	tcp->urgnt_ptr = r_read_at_be16 (buf, 19);
	rec->transport.tcp_hdr = tcp;

	// data offset at (((tcp->hdr_len >> 4) & 0x0F) * 4)
	ut32 dataoff = ((tcp->hdr_len & 0xF0) >> 2);
	rec->datasz = size - dataoff;
	rec->data = malloc (rec->datasz);
	if (!rec->data) {
		free (tcp);
		return;
	}
	memcpy (rec->data, buf + dataoff, rec->datasz);
}

static void parse_ipv4 (pcaprec_t *rec, ut8 *buf, bool bigendian) {
	pcaprec_ipv4_t *ipv4 = R_NEW0 (pcaprec_ipv4_t);
	if (!ipv4) {
		return;
	}
	ipv4->ver_len = r_read_at_be8 (buf, 0);
	ipv4->diff_serv = r_read_at_be8 (buf, 1);
	ipv4->tot_len = r_read_at_be16 (buf, 2);
	ipv4->id = r_read_at_be16 (buf, 4);
	ipv4->flag_frag = r_read_at_be16 (buf, 6);
	ipv4->ttl = r_read_at_be8 (buf, 8);
	ipv4->protocol = r_read_at_be8 (buf, 9);
	ipv4->chksum = r_read_at_be16 (buf, 10);
	ipv4->src = r_read_at_be32 (buf, 12);
	ipv4->dst = r_read_at_be32 (buf, 16);

	switch (ipv4->protocol) {
	case TRANSPORT_TCP:
	{
		ut32 tcpoff = ((ipv4->ver_len & 0x0F) * 4);
		parse_tcp (rec, buf + tcpoff, ipv4->tot_len - tcpoff, bigendian);
		break;
	}
	default:
		break;
	}
	rec->net.ipv4_hdr = ipv4;
}

static void parse_ipv6 (pcaprec_t *rec, ut8 *buf, bool bigendian) {
	pcaprec_ipv6_t *ipv6 = R_NEW0 (pcaprec_ipv6_t);
	if (!ipv6) {
		return;
	}
	memcpy (ipv6, buf, sizeof (pcaprec_ipv6_t));
	ipv6->vc_flow = r_read_at_be32 (buf, 0);
	ipv6->plen = r_read_at_be16 (buf, 4);
	switch (ipv6->nxt) {
	case TRANSPORT_TCP:
		parse_tcp (rec, buf + sizeof (pcaprec_ipv6_t), ipv6->plen, bigendian);
		break;
	default:
		break;
	}
	rec->net.ipv6_hdr = ipv6;
}

static void parse_ether (pcaprec_t *rec, ut8 *buf, bool bigendian) {
	pcaprec_ether_t *ether = R_NEW0 (pcaprec_ether_t);
	if (!ether) {
		return;
	}
	// dst 6 bytes
	// src 6 bytes
	memcpy (ether, buf, 12);
	ether->type = r_read_at_be16 (buf, 12);
	switch (ether->type) {
	case NET_IPV4:
		parse_ipv4 (rec, buf + sizeof (pcaprec_ether_t), bigendian);
		break;
	case NET_IPV6:
		parse_ipv6 (rec, buf + sizeof (pcaprec_ether_t), bigendian);
		break;
	}
	rec->link.ether_hdr = ether;
}

static bool pcap_obj_init_recs (pcap_obj_t *obj) {
	ut64 off = sizeof (pcap_hdr_t);
	ut64 size = r_buf_size (obj->b);
	if (size == 0 || size == UT64_MAX) {
		return false;
	}
	RList *recs = r_list_newf ((RListFree)pcaprec_free);
	if (!recs) {
		return false;
	}

	while (off < size) {
		pcaprec_hdr_t *rec_hdr = R_NEW0 (pcaprec_hdr_t);
		if (!rec_hdr) {
			goto error;
		}
		rec_hdr->ts_sec = r_buf_read_ble32_at (obj->b, off, obj->bigendian);
		rec_hdr->ts_usec = r_buf_read_ble32_at (obj->b, off + 4, obj->bigendian);
		rec_hdr->incl_len = r_buf_read_ble32_at (obj->b, off + 8, obj->bigendian);
		rec_hdr->orig_len = r_buf_read_ble32_at (obj->b, off + 12, obj->bigendian);
		if (off + sizeof (rec_hdr) + rec_hdr->incl_len > size) {
			free (rec_hdr);
			goto error;
		}
		off += 16;

		pcaprec_t *rec = R_NEW0 (pcaprec_t);
		if (!rec) {
			free (rec_hdr);
			goto error;
		}
		rec->paddr = off;
		rec->hdr = rec_hdr;
		ut8 *pktbuf = malloc (rec_hdr->incl_len);
		if (!pktbuf) {
			free (rec_hdr);
			goto error;
		}
		if (r_buf_read_at (obj->b, off, pktbuf, rec_hdr->incl_len) < 0) {
			free (rec->data);
			free (rec_hdr);
			goto error;
		}

		switch (obj->header->network) {
		case LINK_ETHERNET:
			parse_ether (rec, pktbuf, obj->bigendian);
		default:
			break;
		}
		free (pktbuf);
		r_list_append (recs, rec);
		off += rec_hdr->incl_len;
	}
	obj->recs = recs;
	return true;
error:
	r_list_free (recs);
	return false;
}

static bool pcap_obj_init (pcap_obj_t *obj) {
	switch (r_buf_read_be32_at (obj->b, 0)) {
	case PCAP_MAGIC_LE:
		obj->bigendian = false;
		obj->is_nsec = false;
		break;
	case PCAP_MAGIC_BE:
		obj->bigendian = true;
		obj->is_nsec = false;
		break;
	case PCAP_NSEC_MAGIC_LE:
		obj->bigendian = false;
		obj->is_nsec = true;
		break;
	case PCAP_NSEC_MAGIC_BE:
		obj->bigendian = true;
		obj->is_nsec = true;
		break;
	default:
		return false;
	}

	if (!pcap_obj_init_hdr (obj)) {
		return false;
	}
	if (!pcap_obj_init_recs (obj)) {
		return false;
	}
	return true;
}

pcap_obj_t *pcap_obj_new_buf(RBuffer *buf) {
	r_return_val_if_fail (buf, NULL);

	pcap_obj_t *obj = R_NEW0 (pcap_obj_t);
	if (!obj) {
		return NULL;
	}
	obj->b = r_buf_ref (buf);

	if (!pcap_obj_init (obj)) {
		pcap_obj_free (obj);
		return NULL;
	}
	return obj;
}

static void pcaprec_tcp_sym_add(RList *list, pcaprec_t* rec, ut64 paddr, ut32 size) {
	RBinSymbol *ptr = R_NEW0 (RBinSymbol);
	if (!ptr) {
		return;
	}
	pcaprec_tcp_t *tcp = rec->transport.tcp_hdr;
	ut32 datasz = size - ((tcp->hdr_len & 0xF0) >> 2);
	ptr->name = r_str_newf ("0x%"PFMT64x": Transmission Control Protocol, Src Port: %d, Dst"
		" port: %d, Len: %d", paddr, tcp->src_port, tcp->dst_port, datasz);
	ptr->paddr = ptr->vaddr = paddr;
	r_list_append (list, ptr);
}

static void pcaprec_ipv4_sym_add(RList *list, pcaprec_t* rec, ut64 paddr) {
	RBinSymbol *ptr = R_NEW0 (RBinSymbol);
	if (!ptr) {
		return;
	}
	pcaprec_ipv4_t *ipv4 = rec->net.ipv4_hdr;
	ptr->name = r_str_newf ("0x%"PFMT64x": IPV%d, Src: %d.%d.%d.%d, Dst: %d.%d.%d.%d",
		paddr, (ipv4->ver_len >> 4) & 0x0F,
	(ipv4->src >> 24) & 0xFF, (ipv4->src >> 16) & 0xFF,
	(ipv4->src >> 8) & 0xFF, ipv4->src & 0xFF,
	(ipv4->dst >> 24) & 0xFF, (ipv4->dst >> 16) & 0xFF,
	(ipv4->dst >> 8) & 0xFF, ipv4->dst & 0xFF);
	ptr->paddr = ptr->vaddr = paddr;
	r_list_append (list, ptr);

	switch (ipv4->protocol) {
	case TRANSPORT_TCP:
	{
		ut32 tcpoff = ((ipv4->ver_len & 0x0F) * 4);
		pcaprec_tcp_sym_add (list, rec, paddr + tcpoff, ipv4->tot_len - tcpoff);
		break;
	}
	default:
		break;
	}
}

static void pcaprec_ipv6_sym_add(RList *list, pcaprec_t* rec, ut64 paddr) {
	RBinSymbol *ptr = R_NEW0 (RBinSymbol);
	if (!ptr) {
		return;
	}
	pcaprec_ipv6_t *ipv6 = rec->net.ipv6_hdr;
	const char *src = ipv6_addr_string (ipv6->src);
	const char *dst = ipv6_addr_string (ipv6->dst);
	ptr->name = r_str_newf ("0x%"PFMT64x": IPV6, Src: %s, Dst: %s", paddr, src, dst);
	ptr->paddr = ptr->vaddr = paddr;
	r_list_append (list, ptr);
	free ((char *)src);
	free ((char *)dst);

	switch (ipv6->nxt) {
	case TRANSPORT_TCP:
		pcaprec_tcp_sym_add (list, rec, paddr + sizeof (pcaprec_ipv6_t), ipv6->plen);
		break;
	default:
		break;
	}
}

void pcaprec_ether_sym_add(RList *list, pcaprec_t *rec, ut64 paddr) {
	RBinSymbol *ptr = R_NEW0 (RBinSymbol);
	if (!ptr) {
		return;
	}
	pcaprec_ether_t *ether = rec->link.ether_hdr;
	ptr->name = r_str_newf ("0x%"PFMT64x": Ethernet, Src: %02"PFMT32x ":%02"PFMT32x ":%02"PFMT32x
		":%02"PFMT32x ":%02"PFMT32x ":%02"PFMT32x ", Dst: %02"PFMT32x
		":%02"PFMT32x ":%02"PFMT32x ":%02"PFMT32x ":%02"PFMT32x ":%02"PFMT32x,
		paddr, ether->src[0], ether->src[1], ether->src[2], ether->src[3], ether->src[4], ether->src[5],
		ether->dst[0], ether->dst[1], ether->dst[2], ether->dst[3], ether->dst[4], ether->dst[5]);
	ptr->paddr = ptr->vaddr = paddr;
	r_list_append (list, ptr);

	switch (ether->type) {
	case NET_IPV4:
		pcaprec_ipv4_sym_add (list, rec, paddr + sizeof (pcaprec_ether_t));
		break;
	case NET_IPV6:
		pcaprec_ipv6_sym_add (list, rec, paddr + sizeof (pcaprec_ether_t));
		break;
	default:
		break;
	}
}

const char* pcap_network_string(ut32 network) {
	switch (network) {
	case LINK_NOLINK:
		return "No link-layer encapsulation";
	case LINK_ETHERNET:
		return "Ethernet";
	case LINK_ETHERNET_3MB:
		return "3Mb Ethernet";
	case LINK_AX_25:
		return "AX.25";
	case LINK_PRONET:
		return "ProNET";
	case LINK_CHAOS:
		return "CHAOS";
	case LINK_TOKEN_RING:
		return "Token Ring";
	case LINK_ARCNET:
		return "ARCNET";
	case LINK_SLIP:
		return "SLIP";
	case LINK_PPP:
		return "PPP";
	case LINK_FDDI:
		return "FDDI";
	case LINK_RFC_1483_ATM_1:
	case LINK_RFC_1483_ATM_2:
		return "RFC 1483 ATM";
	case LINK_RAW_IP_1:
	case LINK_RAW_IP_2:
		return "raw IP";
	case LINK_BSDOS_SLIP_1:
	case LINK_BSDOS_SLIP_2:
		return "BSD/OS SLIP";
	case LINK_BSDOS_PPP_1:
	case LINK_BSDOS_PPP_2:
		return "BSD/OS PPP";
	case LINK_LINUX_ATM_CLASSICAL_IP:
		return "Linux ATM Classical IP";
	case LINK_PPP_CISCO_HDLC:
		return "PPP or Cisco HDLC";
	case LINK_PPP_OVER_ETHERNET:
		return "PPP-over-Ethernet";
	case LINK_SYMANTEC_FIREWALL:
		return "Symantec Enterprise Firewall";
	case LINK_BSDOS_CISCO_HDLC:
		return "BSD/OS Cisco HDLC";
	case LINK_802_11:
		return "802.11";
	case LINK_LINUX_CLASSICAL_IP_ATM:
		return "Linux Classical IP over ATM";
	case LINK_FRAME_RELAY:
		return "Frame Relay";
	case LINK_OPENBSD_LOOPBACK:
		return "OpenBSD loopback";
	case LINK_OPENBSD_IPSEC_ENC:
		return "OpenBSD IPsec encrypted";
	case LINK_CISCO_HDLC:
		return "Cisco HDLC";
	case LINK_LINUX_COOKED:
		return "Linux \"cooked\"";
	case LINK_LOCALTALK:
		return "LocalTalk";
	case LINK_OPENBSD_PFLOG:
		return "OpenBSD PFLOG";
	case LINK_802_11_PRISM:
		return "802.11 with Prism header";
	case LINK_RFC_2625_IP_FIBRE_CHANNEL:
		return "RFC 2625 over Fibre Channel";
	case LINK_SUNATM:
		return "SunATM";
	case LINK_802_11_RADIOTAP:
		return "802.11 with radiotap header";
	case LINK_LINUX_ARCNET:
		return "Linux ARCNET";
	case LINK_APPLE_IP_IEEE_1394:
		return "Apple IP over IEEE 1394";
	case LINK_MTP2:
		return "MTP2";
	case LINK_MTP3:
		return "MTP3";
	case LINK_DOCSIS:
		return "DOCSIS";
	case LINK_IRDA:
		return "IrDA";
	case LINK_802_11_AVS_HDR:
		return "802.11 with AVS header";
	default:
		return "Unkown";
	}
}

const char *ipv6_addr_string(ut8 *addr) {
	size_t i;
	size_t start = -1;
	size_t tmp = -1;
	size_t len = 0, maxlen = 0;

	ut16 words[8] = { 0 };
	for (i = 0; i < 8; i++) {
		words[i] = r_read_at_be16 (addr, i * 2);
	}

	// find the longest sequence of zero field
	for (i = 0; i < 8; i++) {
		if (words[i] == 0) {
			if (tmp == -1) {
				tmp = i;
				len = 1;
			} else {
				len++;
			}
			continue;
		}

		if (len > maxlen) {
			maxlen = len;
			start = tmp;
		}
		tmp = -1;
	}

	if (maxlen > 1) {
		RStrBuf *addr = r_strbuf_new (NULL);
		for (i = 0; i < 8; i++) {
			if (i == start) {
				r_strbuf_append (addr, "::");
				i += maxlen - 1;
			} else {
				r_strbuf_appendf (addr, "%x", words[i]); 
			}
		}
		return r_strbuf_drain (addr);
	}
	return r_str_newf ("%x:%x:%x:%x:%x:%x:%x:%x",
		words[0], words[1], words[2], words[3],
		words[4], words[5], words[6], words[7]);
}
