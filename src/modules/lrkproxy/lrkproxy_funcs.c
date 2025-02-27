/*
 * Copyright (C) 2001-2003 FhG Fokus
 * Copyright (C) 2014-2015 Sipwise GmbH, http://www.sipwise.com
 * Copyright (C) 2020 Mojtaba Esfandiari.S, Nasim-Telecom
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include "lrkproxy_funcs.h"
#include "../../core/dprint.h"
#include "../../core/config.h"
#include "../../core/ut.h"
#include "../../core/forward.h"
#include "../../core/resolve.h"
#include "../../core/globals.h"
#include "../../core/udp_server.h"
#include "../../core/pt.h"
#include "../../core/parser/msg_parser.h"
#include "../../core/trim.h"
#include "../../core/parser/parse_from.h"
#include "../../core/parser/contact/parse_contact.h"
#include "../../core/parser/parse_uri.h"
#include "../../core/parser/parse_content.h"
#include "../../core/parser/parser_f.h"
#include "../../core/parser/sdp/sdp_helpr_funcs.h"

static pv_spec_t *custom_sdp_ip_avp; /*!< AVP for custom_sdp_ip setting */


#define READ(val) \
	(*(val + 0) + (*(val + 1) << 8) + (*(val + 2) << 16) + (*(val + 3) << 24))
#define advance(_ptr, _n, _str, _error)           \
	do {                                          \
		if((_ptr) + (_n) > (_str).s + (_str).len) \
			goto _error;                          \
		(_ptr) = (_ptr) + (_n);                   \
	} while(0);
#define one_of_16(_x, _t)                                                     \
	(_x == _t[0] || _x == _t[15] || _x == _t[8] || _x == _t[2] || _x == _t[3] \
			|| _x == _t[4] || _x == _t[5] || _x == _t[6] || _x == _t[7]       \
			|| _x == _t[1] || _x == _t[9] || _x == _t[10] || _x == _t[11]     \
			|| _x == _t[12] || _x == _t[13] || _x == _t[14])
#define one_of_8(_x, _t)                                                     \
	(_x == _t[0] || _x == _t[7] || _x == _t[1] || _x == _t[2] || _x == _t[3] \
			|| _x == _t[4] || _x == _t[5] || _x == _t[6])


/**
 * return:
 * -1: error
 *  1: text or sdp
 *  2: multipart
 *  3: trickle ice sdp fragment
 */
int check_content_type(struct sip_msg *msg)
{
	static unsigned int appl[16] = {0x6c707061 /*appl*/, 0x6c707041 /*Appl*/,
			0x6c705061 /*aPpl*/, 0x6c705041 /*APpl*/, 0x6c507061 /*apPl*/,
			0x6c507041 /*ApPl*/, 0x6c505061 /*aPPl*/, 0x6c505041 /*APPl*/,
			0x4c707061 /*appL*/, 0x4c707041 /*AppL*/, 0x4c705061 /*aPpL*/,
			0x4c705041 /*APpL*/, 0x4c507061 /*apPL*/, 0x4c507041 /*ApPL*/,
			0x4c505061 /*aPPL*/, 0x4c505041 /*APPL*/};
	static unsigned int icat[16] = {0x74616369 /*icat*/, 0x74616349 /*Icat*/,
			0x74614369 /*iCat*/, 0x74614349 /*ICat*/, 0x74416369 /*icAt*/,
			0x74416349 /*IcAt*/, 0x74414369 /*iCAt*/, 0x74414349 /*ICAt*/,
			0x54616369 /*icaT*/, 0x54616349 /*IcaT*/, 0x54614369 /*iCaT*/,
			0x54614349 /*ICaT*/, 0x54416369 /*icAT*/, 0x54416349 /*IcAT*/,
			0x54414369 /*iCAT*/, 0x54414349 /*ICAT*/};
	static unsigned int ion_[8] = {0x006e6f69 /*ion_*/, 0x006e6f49 /*Ion_*/,
			0x006e4f69 /*iOn_*/, 0x006e4f49 /*IOn_*/, 0x004e6f69 /*ioN_*/,
			0x004e6f49 /*IoN_*/, 0x004e4f69 /*iON_*/, 0x004e4f49 /*ION_*/};
	static unsigned int sdp_[8] = {0x00706473 /*sdp_*/, 0x00706453 /*Sdp_*/,
			0x00704473 /*sDp_*/, 0x00704453 /*SDp_*/, 0x00506473 /*sdP_*/,
			0x00506453 /*SdP_*/, 0x00504473 /*sDP_*/, 0x00504453 /*SDP_*/};
	str str_type;
	unsigned int x;
	char *p;

	if(!msg->content_type) {
		LM_WARN("the header Content-TYPE is absent!"
				"let's assume the content is text/plain ;-)\n");
		return 1;
	}

	trim_len(str_type.len, str_type.s, msg->content_type->body);
	if(str_type.len >= 15 && (*str_type.s == 'm' || *str_type.s == 'M')
			&& strncasecmp(str_type.s, "multipart/mixed", 15) == 0) {
		return 2;
	}
	p = str_type.s;
	advance(p, 4, str_type, error_1);
	x = READ(p - 4);
	if(!one_of_16(x, appl))
		goto other;
	advance(p, 4, str_type, error_1);
	x = READ(p - 4);
	if(!one_of_16(x, icat))
		goto other;
	advance(p, 3, str_type, error_1);
	x = READ(p - 3) & 0x00ffffff;
	if(!one_of_8(x, ion_))
		goto other;

	/* skip spaces and tabs if any */
	while(*p == ' ' || *p == '\t')
		advance(p, 1, str_type, error_1);
	if(*p != '/') {
		LM_ERR("no / found after primary type\n");
		goto error;
	}
	advance(p, 1, str_type, error_1);
	while((*p == ' ' || *p == '\t') && p + 1 < str_type.s + str_type.len)
		advance(p, 1, str_type, error_1);

	advance(p, 3, str_type, error_1);
	x = READ(p - 3) & 0x00ffffff;
	if(!one_of_8(x, sdp_)) {
		if(strncasecmp(p - 3, "trickle-ice-sdpfrag", 19) == 0)
			return 3;
		goto other;
	}

	if(*p == ';' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'
			|| *p == 0) {
		LM_DBG("type <%.*s> found valid\n", (int)(p - str_type.s), str_type.s);
		return 1;
	} else {
		LM_ERR("bad end for type!\n");
		return -1;
	}

error_1:
	LM_ERR("body ended :-(!\n");
error:
	return -1;
other:
	LM_ERR("invalid type for a message\n");
	return -1;
}


/*
 * Get message body and check Content-Type header field
 */
int extract_body(struct sip_msg *msg, str *body)
{
	char c;
	int ret;
	str mpdel;
	char *rest, *p1, *p2;
	struct hdr_field hf;
	unsigned int mime;

	body->s = get_body(msg);
	if(body->s == 0) {
		LM_ERR("failed to get the message body\n");
		goto error;
	}

	/*
     * Better use the content-len value - no need of any explicit
     * parsing as get_body() parsed all headers and Content-Length
     * body header is automatically parsed when found.
     */
	if(msg->content_length == 0) {
		LM_ERR("failed to get the content length in message\n");
		goto error;
	}

	body->len = get_content_length(msg);
	if(body->len == 0) {
		LM_ERR("message body has length zero\n");
		goto error;
	}

	if(body->len + body->s > msg->buf + msg->len) {
		LM_ERR("content-length exceeds packet-length by %d\n",
				(int)((body->len + body->s) - (msg->buf + msg->len)));
		goto error;
	}

	/* no need for parse_headers(msg, EOH), get_body will
     * parse everything */
	/*is the content type correct?*/
	if((ret = check_content_type(msg)) == -1) {
		LM_ERR("content type mismatching\n");
		goto error;
	}

	if(ret != 2)
		goto done;

	/* multipart body */
	if(get_mixed_part_delimiter(&msg->content_type->body, &mpdel) < 0) {
		goto error;
	}
	p1 = find_sdp_line_delimiter(body->s, body->s + body->len, mpdel);
	if(p1 == NULL) {
		LM_ERR("empty multipart content\n");
		return -1;
	}
	p2 = p1;
	c = 0;
	for(;;) {
		p1 = p2;
		if(p1 == NULL || p1 >= body->s + body->len)
			break; /* No parts left */
		p2 = find_next_sdp_line_delimiter(
				p1, body->s + body->len, mpdel, body->s + body->len);
		/* p2 is text limit for application parsing */
		rest = eat_line(p1 + mpdel.len + 2, p2 - p1 - mpdel.len - 2);
		if(rest > p2) {
			LM_ERR("Unparsable <%.*s>\n", (int)(p1 - p1), p1);
			return -1;
		}
		while(rest < p2) {
			memset(&hf, 0, sizeof(struct hdr_field));
			rest = get_sdp_hdr_field(rest, p2, &hf);
			if(hf.type == HDR_EOH_T)
				break;
			if(hf.type == HDR_ERROR_T)
				return -1;
			if(hf.type == HDR_CONTENTTYPE_T) {
				if(decode_mime_type(hf.body.s, hf.body.s + hf.body.len, &mime)
						== NULL)
					return -1;
				if(((((unsigned int)mime) >> 16) == TYPE_APPLICATION)
						&& ((mime & 0x00ff) == SUBTYPE_SDP)) {
					c = 1;
				}
			}
		} /* end of while */
		if(c == 1) {
			if(rest < p2 && *rest == '\r')
				rest++;
			if(rest < p2 && *rest == '\n')
				rest++;
			if(rest < p2 && p2[-1] == '\n')
				p2--;
			if(rest < p2 && p2[-1] == '\r')
				p2--;
			body->s = rest;
			body->len = p2 - rest;
			goto done;
		}
	}

error:
	return -1;

done:
	/*LM_DBG("DEBUG:extract_body:=|%.*s|\n",body->len,body->s);*/
	return ret; /* mirrors return type of check_content_type */
}

/*
 * Some helper functions taken verbatim from tm module.
 */

/*
 * Extract Call-ID value
 * assumes the callid header is already parsed
 * (so make sure it is, before calling this function or
 *  it might fail even if the message _has_ a callid)
 */
int get_callid(struct sip_msg *_m, str *_cid)
{

	if((parse_headers(_m, HDR_CALLID_F, 0) == -1)) {
		LM_ERR("failed to parse call-id header\n");
		return -1;
	}

	if(_m->callid == NULL) {
		LM_ERR("call-id not found\n");
		return -1;
	}

	_cid->s = _m->callid->body.s;
	_cid->len = _m->callid->body.len;
	trim(_cid);
	return 0;
}

/*
 * Extract tag from To header field of a response
 */
int get_to_tag(struct sip_msg *_m, str *_tag)
{

	if(parse_to_header(_m) < 0) {
		LM_ERR("To header field missing\n");
		return -1;
	}

	if(get_to(_m)->tag_value.len) {
		_tag->s = get_to(_m)->tag_value.s;
		_tag->len = get_to(_m)->tag_value.len;
	} else {
		_tag->s = NULL; /* fixes gcc 4.0 warnings */
		_tag->len = 0;
	}

	return 0;
}

/*
 * Extract tag from From header field of a request
 */
int get_from_tag(struct sip_msg *_m, str *_tag)
{

	if(parse_from_header(_m) < 0) {
		LM_ERR("failed to parse From header\n");
		return -1;
	}

	if(get_from(_m)->tag_value.len) {
		_tag->s = get_from(_m)->tag_value.s;
		_tag->len = get_from(_m)->tag_value.len;
	} else {
		_tag->s = NULL; /* fixes gcc 4.0 warnings */
		_tag->len = 0;
	}

	return 0;
}

/*
 * Extract URI from the Contact header field
 */
int get_contact_uri(struct sip_msg *_m, struct sip_uri *uri, contact_t **_c)
{

	if((parse_headers(_m, HDR_CONTACT_F, 0) == -1) || !_m->contact)
		return -1;
	if(!_m->contact->parsed && parse_contact(_m->contact) < 0) {
		LM_ERR("failed to parse Contact body\n");
		return -1;
	}
	*_c = ((contact_body_t *)_m->contact->parsed)->contacts;
	if(*_c == NULL)
		/* no contacts found */
		return -1;

	if(parse_uri((*_c)->uri.s, (*_c)->uri.len, uri) < 0 || uri->host.len <= 0) {
		LM_ERR("failed to parse Contact URI [%.*s]\n", (*_c)->uri.len,
				((*_c)->uri.s) ? (*_c)->uri.s : "");
		return -1;
	}
	return 0;
}

/*
 * Extract branch from Via header
 */
int get_via_branch(struct sip_msg *msg, int vianum, str *_branch)
{
	struct via_body *via;
	struct via_param *p;

	if(parse_via_header(msg, vianum, &via) < 0)
		return -1;

	for(p = via->param_lst; p; p = p->next) {
		if(p->name.len == strlen("branch")
				&& strncasecmp(p->name.s, "branch", strlen("branch")) == 0) {
			_branch->s = p->value.s;
			_branch->len = p->value.len;
			return 0;
		}
	}
	return -1;
}

int get_sdp_ipaddr_media(struct sip_msg *msg, str *ip_addr)
{
	sdp_session_cell_t *sdp_session;
	sdp_stream_cell_t *sdp_stream;
	sdp_info_t *sdp = (sdp_info_t *)msg->body;
	if(!sdp) {
		LM_INFO("sdp null\n");
		return -1;
	}

	char *s = ip_addr2a(&msg->rcv.src_ip);
	LM_INFO("=========>msg->rcv.src_ip:%s\n", s);
	//            LM_INFO("=========>msg->contact-body:%.*s\n", msg->contact->body.len, msg->contact->body.s);
	//            LM_INFO("=========>msg->contact-name:%.*s\n", msg->contact->name.len, msg->contact->name.s);

	pv_value_t pv_val;
	if(custom_sdp_ip_avp) {
		if((pv_get_spec_value(msg, custom_sdp_ip_avp, &pv_val) == 0)
				&& (pv_val.flags & PV_VAL_STR) && (pv_val.rs.len > 0)) {
			ip_addr->s = pv_val.rs.s;
			ip_addr->len = pv_val.rs.len;
			LM_INFO("=========>custom_sdp_ip_avp:%.*s\n", ip_addr->len,
					ip_addr->s);

			return 0;
		} else
			LM_DBG("invalid AVP value, using default user from RURI\n");
	}


	int sdp_session_num = 0;
	sdp_session = get_sdp_session(msg, sdp_session_num);

	if(!sdp_session) {
		LM_INFO("can not get the sdp session\n");
		return 0;
	}

	if(sdp_session->ip_addr.s && sdp_session->ip_addr.len > 0) {
		LM_INFO("sdp_session->ip_addr:%.*s\n", sdp_session->ip_addr.len,
				sdp_session->ip_addr.s);

		ip_addr->s = sdp_session->ip_addr.s;
		ip_addr->len = sdp_session->ip_addr.len;
		trim(ip_addr);

	} else {
		int sdp_stream_num = 0;
		sdp_stream = get_sdp_stream(msg, sdp_session_num, sdp_stream_num);
		if(!sdp_stream) {
			LM_INFO("can not get the sdp stream\n");
			return 0;
		}
		if(sdp_stream->ip_addr.s && sdp_stream->ip_addr.len > 0) {
			LM_INFO("sdp_stream->ip_addr:%.*s\n", sdp_stream->ip_addr.len,
					sdp_stream->ip_addr.s);
			ip_addr->s = sdp_stream->ip_addr.s;
			ip_addr->len = sdp_stream->ip_addr.len;
			trim(ip_addr);
		}
	}


	return 0;
}


int get_sdp_port_media(struct sip_msg *msg, str *port)
{
	//    sdp_session_cell_t *sdp_session;
	sdp_stream_cell_t *sdp_stream;
	int sdp_session_num = 0;

	sdp_info_t *sdp = (sdp_info_t *)msg->body;
	if(!sdp) {
		LM_INFO("sdp null\n");
		return -1;
	}

	//    sdp_session = get_sdp_session(msg, sdp_session_num);
	//    if(!sdp_session) {
	//                LM_INFO("can not get the sdp session\n");
	//        return 0;
	//    } else {
	//                LM_INFO("NEW_IP_ADDRESS:>%.*s>\n", sdp_session->ip_addr.len, sdp_session->ip_addr.s);
	//        lrk_sdp_info->ip_addr.s = sdp_session->ip_addr;
	int sdp_stream_num = 0;
	sdp_stream = get_sdp_stream(msg, sdp_session_num, sdp_stream_num);
	if(!sdp_stream) {
		LM_INFO("can not get the sdp stream\n");
		return -1;
	} else {
		//                    LM_INFO ("PORT:<%.*s>\n", sdp_stream->port.len, sdp_stream->port.s);
		//            str2int(&sdp_stream->port, lrk_sdp_info->port)
		port->s = sdp_stream->port.s;
		port->len = sdp_stream->port.len;
		trim(port);
	}
	//    }
	return 0;
}

void init_custom_sdp_ip(pv_spec_t *custom_sdp_ip_avp_p)
{
	custom_sdp_ip_avp = custom_sdp_ip_avp_p;
}
