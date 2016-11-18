/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2013-2014  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <arpa/inet.h>
#include <linux/filter.h>
#include <ell/ell.h>

#include "crypto.h"
#include "eapol.h"
#include "ie.h"
#include "util.h"
#include "mpdu.h"
#include "eap.h"
#include "handshake.h"

struct l_queue *state_machines;

eapol_deauthenticate_func_t deauthenticate = NULL;
eapol_rekey_offload_func_t rekey_offload = NULL;

static struct l_io *pae_io;
eapol_tx_packet_func_t tx_packet = NULL;
void *tx_user_data;

/*
 * BPF filter to match skb->dev->type == 1 (ARPHRD_ETHER) and
 * match skb->protocol == 0x888e (PAE).
 */
static struct sock_filter pae_filter[] = {
	{ 0x28,  0,  0, 0xfffff01c },	/* ldh #hatype		*/
	{ 0x15,  0,  3, 0x00000001 },	/* jne #1, drop		*/
	{ 0x28,  0,  0, 0xfffff000 },	/* ldh #proto		*/
	{ 0x15,  0,  1, 0x0000888e },	/* jne #0x888e, drop	*/
	{ 0x06,  0,  0, 0xffffffff },	/* ret #-1		*/
	{ 0x06,  0,  0, 0000000000 },	/* drop: ret #0		*/
};

static const struct sock_fprog pae_fprog = { .len = 6, .filter = pae_filter };

static struct l_io *pae_open(void)
{
	struct l_io *io;
	int fd;

	fd = socket(PF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
							htons(ETH_P_ALL));
	if (fd < 0)
		return NULL;

	if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER,
					&pae_fprog, sizeof(pae_fprog)) < 0) {
		close(fd);
		return NULL;
	}

	io = l_io_new(fd);
	l_io_set_close_on_destroy(io, true);

	return io;
}

static bool pae_read(struct l_io *io, void *user_data)
{
	int fd = l_io_get_fd(io);
	struct sockaddr_ll sll;
	socklen_t sll_len;
	ssize_t bytes;
	uint8_t frame[2304]; /* IEEE Std 802.11 ch. 8.2.3 */

	memset(&sll, 0, sizeof(sll));
	sll_len = sizeof(sll);

	bytes = recvfrom(fd, frame, sizeof(frame), 0,
				(struct sockaddr *) &sll, &sll_len);
	if (bytes <= 0) {
		l_error("EAPoL read socket: %s", strerror(errno));
		return false;
	}

	if (sll.sll_halen != ETH_ALEN)
		return true;

	__eapol_rx_packet(sll.sll_ifindex, sll.sll_addr, frame, bytes);

	return true;
}

static void pae_destroy()
{
	pae_io = NULL;
}

static void pae_write(uint32_t ifindex, const uint8_t *aa, const uint8_t *spa,
			const struct eapol_frame *ef)
{
	size_t frame_size;
	struct sockaddr_ll sll;
	ssize_t r;
	int fd;

	if (!pae_io) {
		if (tx_packet) /* Used for unit tests */
			tx_packet(ifindex, aa, spa, ef, tx_user_data);

		return;
	}

	fd = l_io_get_fd(pae_io);

	memset(&sll, 0, sizeof(sll));
	sll.sll_family = AF_PACKET;
	sll.sll_ifindex = ifindex;
	sll.sll_protocol = htons(ETH_P_PAE);
	sll.sll_halen = ETH_ALEN;
	memcpy(sll.sll_addr, aa, ETH_ALEN);

	frame_size = sizeof(struct eapol_header) +
			L_BE16_TO_CPU(ef->header.packet_len);

	r = sendto(fd, ef, frame_size, 0,
			(struct sockaddr *) &sll, sizeof(sll));
	if (r < 0)
		l_error("EAPoL write socket: %s", strerror(errno));
}

void eapol_pae_open()
{
	pae_io = pae_open();
	if (!pae_io)
		return;

	l_io_set_read_handler(pae_io, pae_read, NULL, pae_destroy);
}

void eapol_pae_close()
{
	l_io_destroy(pae_io);
}

static inline bool mem_is_zero(const uint8_t *field, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (field[i] != 0)
			return false;

	return true;
}

#define VERIFY_IS_ZERO(field)					\
	do {							\
		if (!mem_is_zero((field), sizeof((field))))	\
			return false;				\
	} while (false)						\

/*
 * MIC calculation depends on the selected hash function.  The has function
 * is given in the EAPoL Key Descriptor Version field.
 *
 * The MIC length is always 16 bytes for currently known Key Descriptor
 * Versions.
 *
 * The input struct eapol_key *frame should have a zero-d MIC field
 */
bool eapol_calculate_mic(const uint8_t *kck, const struct eapol_key *frame,
				uint8_t *mic)
{
	size_t frame_len = sizeof(struct eapol_key);

	frame_len += L_BE16_TO_CPU(frame->key_data_len);

	switch (frame->key_descriptor_version) {
	case EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_MD5_ARC4:
		return hmac_md5(kck, 16, frame, frame_len, mic, 16);
	case EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_SHA1_AES:
		return hmac_sha1(kck, 16, frame, frame_len, mic, 16);
	case EAPOL_KEY_DESCRIPTOR_VERSION_AES_128_CMAC_AES:
		return cmac_aes(kck, 16, frame, frame_len, mic, 16);
	default:
		return false;
	}
}

bool eapol_verify_mic(const uint8_t *kck, const struct eapol_key *frame)
{
	size_t frame_len = sizeof(struct eapol_key);
	uint8_t mic[16];
	struct iovec iov[3];
	struct l_checksum *checksum = NULL;

	iov[0].iov_base = (void *) frame;
	iov[0].iov_len = offsetof(struct eapol_key, key_mic_data);

	memset(mic, 0, sizeof(mic));
	iov[1].iov_base = mic;
	iov[1].iov_len = sizeof(mic);

	iov[2].iov_base = ((void *) frame) +
				offsetof(struct eapol_key, key_data_len);
	iov[2].iov_len = frame_len - offsetof(struct eapol_key, key_data_len) +
				L_BE16_TO_CPU(frame->key_data_len);

	switch (frame->key_descriptor_version) {
	case EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_MD5_ARC4:
		checksum = l_checksum_new_hmac(L_CHECKSUM_MD5, kck, 16);
		break;
	case EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_SHA1_AES:
		checksum = l_checksum_new_hmac(L_CHECKSUM_SHA1, kck, 16);
		break;
	case EAPOL_KEY_DESCRIPTOR_VERSION_AES_128_CMAC_AES:
		checksum = l_checksum_new_cmac_aes(kck, 16);
		break;
	default:
		return false;
	}

	if (checksum == NULL)
		return false;

	l_checksum_updatev(checksum, iov, 3);
	l_checksum_get_digest(checksum, mic, 16);
	l_checksum_free(checksum);

	if (!memcmp(frame->key_mic_data, mic, 16))
		return true;

	return false;
}

uint8_t *eapol_decrypt_key_data(const uint8_t *kek,
				const struct eapol_key *frame,
				size_t *decrypted_size)
{
	size_t key_data_len = L_BE16_TO_CPU(frame->key_data_len);
	const uint8_t *key_data = frame->key_data;
	size_t expected_len;
	uint8_t *buf;

	switch (frame->key_descriptor_version) {
	case EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_MD5_ARC4:
		expected_len = key_data_len;
		break;
	case EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_SHA1_AES:
	case EAPOL_KEY_DESCRIPTOR_VERSION_AES_128_CMAC_AES:
		expected_len = key_data_len - 8;
		break;
	default:
		return NULL;
	};

	buf = l_new(uint8_t, expected_len);

	switch (frame->key_descriptor_version) {
	case EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_MD5_ARC4:
	{
		uint8_t key[32];
		bool ret;

		memcpy(key, frame->eapol_key_iv, 16);
		memcpy(key + 16, kek, 16);

		ret = arc4_skip(key, 32, 256, key_data, key_data_len, buf);
		memset(key, 0, sizeof(key));

		if (!ret)
			goto error;

		break;
	}
	case EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_SHA1_AES:
	case EAPOL_KEY_DESCRIPTOR_VERSION_AES_128_CMAC_AES:
		if (key_data_len < 8 || key_data_len % 8)
			goto error;

		if (!aes_unwrap(kek, key_data, key_data_len, buf))
			goto error;

		break;
	}

	if (decrypted_size)
		*decrypted_size = expected_len;

	return buf;

error:
	l_free(buf);
	return NULL;
}

const struct eapol_key *eapol_key_validate(const uint8_t *frame, size_t len)
{
	const struct eapol_key *ek;
	uint16_t key_data_len;

	if (len < sizeof(struct eapol_key))
		return NULL;

	ek = (const struct eapol_key *) frame;

	switch (ek->header.protocol_version) {
	case EAPOL_PROTOCOL_VERSION_2001:
	case EAPOL_PROTOCOL_VERSION_2004:
		break;
	default:
		return NULL;
	}

	if (ek->header.packet_type != 3)
		return NULL;

	switch (ek->descriptor_type) {
	case EAPOL_DESCRIPTOR_TYPE_RC4:
	case EAPOL_DESCRIPTOR_TYPE_80211:
	case EAPOL_DESCRIPTOR_TYPE_WPA:
		break;
	default:
		return NULL;
	}

	switch (ek->key_descriptor_version) {
	case EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_MD5_ARC4:
	case EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_SHA1_AES:
	case EAPOL_KEY_DESCRIPTOR_VERSION_AES_128_CMAC_AES:
		break;
	default:
		return NULL;
	}

	key_data_len = L_BE16_TO_CPU(ek->key_data_len);
	if (len < sizeof(struct eapol_key) + key_data_len)
		return NULL;

	return ek;
}

#define VERIFY_PTK_COMMON(ek)	\
	if (!ek->key_type)	\
		return false;	\
	if (ek->smk_message)	\
		return false;	\
	if (ek->request)	\
		return false;	\
	if (ek->error)		\
		return false	\

bool eapol_verify_ptk_1_of_4(const struct eapol_key *ek)
{
	/* Verify according to 802.11, Section 11.6.6.2 */
	VERIFY_PTK_COMMON(ek);

	if (ek->install)
		return false;

	if (!ek->key_ack)
		return false;

	if (ek->key_mic)
		return false;

	if (ek->secure)
		return false;

	if (ek->encrypted_key_data)
		return false;

	if (ek->wpa_key_id)
		return false;

	VERIFY_IS_ZERO(ek->eapol_key_iv);
	VERIFY_IS_ZERO(ek->key_rsc);
	VERIFY_IS_ZERO(ek->reserved);
	VERIFY_IS_ZERO(ek->key_mic_data);

	return true;
}

bool eapol_verify_ptk_2_of_4(const struct eapol_key *ek)
{
	uint16_t key_len;

	/* Verify according to 802.11, Section 11.6.6.3 */
	VERIFY_PTK_COMMON(ek);

	if (ek->install)
		return false;

	if (ek->key_ack)
		return false;

	if (!ek->key_mic)
		return false;

	if (ek->secure)
		return false;

	if (ek->encrypted_key_data)
		return false;

	if (ek->wpa_key_id)
		return false;

	key_len = L_BE16_TO_CPU(ek->key_length);
	if (key_len != 0)
		return false;

	VERIFY_IS_ZERO(ek->eapol_key_iv);
	VERIFY_IS_ZERO(ek->key_rsc);
	VERIFY_IS_ZERO(ek->reserved);

	return true;
}

bool eapol_verify_ptk_3_of_4(const struct eapol_key *ek, bool is_wpa)
{
	uint16_t key_len;

	/* Verify according to 802.11, Section 11.6.6.4 */
	VERIFY_PTK_COMMON(ek);

	/*
	 * TODO: Handle cases where install might be 0:
	 * For PTK generation, 0 only if the AP does not support key mapping
	 * keys, or if the STA has the No Pairwise bit (in the RSN Capabilities
	 * field) equal to 1 and only the group key is used.
	 */
	if (!ek->install)
		return false;

	if (!ek->key_ack)
		return false;

	if (!ek->key_mic)
		return false;

	if (ek->secure != !is_wpa)
		return false;

	/* Must be encrypted when GTK is present but reserved in WPA */
	if (!ek->encrypted_key_data && !is_wpa)
		return false;

	if (ek->wpa_key_id)
		return false;

	key_len = L_BE16_TO_CPU(ek->key_length);
	if (key_len != 16 && key_len != 32)
		return false;

	VERIFY_IS_ZERO(ek->reserved);

	/* 0 (Version 2) or random (Version 1) */
	if (ek->key_descriptor_version ==
			EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_SHA1_AES)
		L_WARN_ON(!mem_is_zero(ek->eapol_key_iv,
						sizeof(ek->eapol_key_iv)));

	return true;
}

bool eapol_verify_ptk_4_of_4(const struct eapol_key *ek, bool is_wpa)
{
	uint16_t key_len;

	/* Verify according to 802.11, Section 11.6.6.5 */
	VERIFY_PTK_COMMON(ek);

	if (ek->install)
		return false;

	if (ek->key_ack)
		return false;

	if (!ek->key_mic)
		return false;

	if (ek->secure != !is_wpa)
		return false;

	if (ek->encrypted_key_data)
		return false;

	if (ek->wpa_key_id)
		return false;

	key_len = L_BE16_TO_CPU(ek->key_length);
	if (key_len != 0)
		return false;

	VERIFY_IS_ZERO(ek->key_nonce);
	VERIFY_IS_ZERO(ek->eapol_key_iv);
	VERIFY_IS_ZERO(ek->key_rsc);
	VERIFY_IS_ZERO(ek->reserved);

	return true;
}

#define VERIFY_GTK_COMMON(ek)	\
	if (ek->key_type)	\
		return false;	\
	if (ek->smk_message)	\
		return false;	\
	if (ek->request)	\
		return false;	\
	if (ek->error)		\
		return false;	\
	if (ek->install)	\
		return false	\

bool eapol_verify_gtk_1_of_2(const struct eapol_key *ek, bool is_wpa)
{
	uint16_t key_len;

	VERIFY_GTK_COMMON(ek);

	if (!ek->key_ack)
		return false;

	if (!ek->key_mic)
		return false;

	if (!ek->secure)
		return false;

	/* Must be encrypted when GTK is present but reserved in WPA */
	if (!ek->encrypted_key_data && !is_wpa)
		return false;

	key_len = L_BE16_TO_CPU(ek->key_length);
	if (key_len == 0)
		return false;

	VERIFY_IS_ZERO(ek->reserved);

	/* 0 (Version 2) or random (Version 1) */
	if (ek->key_descriptor_version ==
			EAPOL_KEY_DESCRIPTOR_VERSION_HMAC_SHA1_AES)
		VERIFY_IS_ZERO(ek->eapol_key_iv);

	/*
	 * WPA_80211_v3_1, Section 2.2.4:
	 * "Key Index (bits 4 and 5): specifies the key id of the temporal
	 * key of the key derived from the message. The value of this shall be
	 * zero (0) if the value of Key Type (bit 4) is Pairwise (1). The Key
	 * Type and Key Index shall not both be 0 in the same message.
	 *
	 * Group keys shall not use key id 0. This means that key ids 1 to 3
	 * are available to be used to identify Group keys. This document
	 * recommends that implementations reserve key ids 1 and 2 for Group
	 * Keys, and that key id 3 is not used.
	 */
	if (is_wpa && !ek->wpa_key_id)
		return false;

	return true;
}

bool eapol_verify_gtk_2_of_2(const struct eapol_key *ek, bool is_wpa)
{
	uint16_t key_len;

	/* Verify according to 802.11, Section 11.6.7.3 */
	VERIFY_GTK_COMMON(ek);

	if (ek->key_ack)
		return false;

	if (!ek->key_mic)
		return false;

	if (!ek->secure)
		return false;

	if (ek->encrypted_key_data)
		return false;

	key_len = L_BE16_TO_CPU(ek->key_length);
	if (key_len != 0)
		return false;

	VERIFY_IS_ZERO(ek->key_nonce);
	VERIFY_IS_ZERO(ek->eapol_key_iv);
	VERIFY_IS_ZERO(ek->key_rsc);
	VERIFY_IS_ZERO(ek->reserved);

	return true;
}

static struct eapol_key *eapol_create_common(
				enum eapol_protocol_version protocol,
				enum eapol_key_descriptor_version version,
				bool secure,
				uint64_t key_replay_counter,
				const uint8_t snonce[],
				size_t extra_len,
				const uint8_t *extra_data,
				int key_type,
				bool is_wpa)
{
	size_t to_alloc = sizeof(struct eapol_key);
	struct eapol_key *out_frame = l_malloc(to_alloc + extra_len);

	memset(out_frame, 0, to_alloc + extra_len);

	out_frame->header.protocol_version = protocol;
	out_frame->header.packet_type = 0x3;
	out_frame->header.packet_len = L_CPU_TO_BE16(to_alloc + extra_len - 4);
	out_frame->descriptor_type = is_wpa ? EAPOL_DESCRIPTOR_TYPE_WPA :
		EAPOL_DESCRIPTOR_TYPE_80211;
	out_frame->key_descriptor_version = version;
	out_frame->key_type = key_type;
	out_frame->install = false;
	out_frame->key_ack = false;
	out_frame->key_mic = true;
	out_frame->secure = secure;
	out_frame->error = false;
	out_frame->request = false;
	out_frame->encrypted_key_data = false;
	out_frame->smk_message = false;
	out_frame->key_length = 0;
	out_frame->key_replay_counter = L_CPU_TO_BE64(key_replay_counter);
	memcpy(out_frame->key_nonce, snonce, sizeof(out_frame->key_nonce));
	out_frame->key_data_len = L_CPU_TO_BE16(extra_len);
	memcpy(out_frame->key_data, extra_data, extra_len);

	return out_frame;
}

struct eapol_key *eapol_create_ptk_2_of_4(
				enum eapol_protocol_version protocol,
				enum eapol_key_descriptor_version version,
				uint64_t key_replay_counter,
				const uint8_t snonce[],
				size_t extra_len,
				const uint8_t *extra_data,
				bool is_wpa)
{
	return eapol_create_common(protocol, version, false, key_replay_counter,
					snonce, extra_len, extra_data, 1,
					is_wpa);
}

struct eapol_key *eapol_create_ptk_4_of_4(
				enum eapol_protocol_version protocol,
				enum eapol_key_descriptor_version version,
				uint64_t key_replay_counter,
				bool is_wpa)
{
	uint8_t snonce[32];

	memset(snonce, 0, sizeof(snonce));
	return eapol_create_common(protocol, version,
					is_wpa ? false : true,
					key_replay_counter, snonce, 0, NULL,
					1, is_wpa);
}

struct eapol_key *eapol_create_gtk_2_of_2(
				enum eapol_protocol_version protocol,
				enum eapol_key_descriptor_version version,
				uint64_t key_replay_counter,
				bool is_wpa, uint8_t wpa_key_id)
{
	uint8_t snonce[32];
	struct eapol_key *step2;

	memset(snonce, 0, sizeof(snonce));
	step2 = eapol_create_common(protocol, version, true,
					key_replay_counter, snonce, 0, NULL,
					0, is_wpa);

	if (!step2)
		return step2;

	/*
	 * WPA_80211_v3_1, Section 2.2.4:
	 * "The Key Type and Key Index shall not both be 0 in the same message"
	 *
	 * The above means that even though sending the key index back to the
	 * AP has no practical value, we must still do so.
	 */
	if (is_wpa)
		step2->wpa_key_id = wpa_key_id;

	return step2;
}

struct eapol_buffer {
	size_t len;
	uint8_t data[0];
};

struct eapol_sm {
	struct handshake_state *handshake;
	enum eapol_protocol_version protocol_version;
	uint64_t replay_counter;
	eapol_sm_event_func_t event_func;
	void *user_data;
	struct l_timeout *timeout;
	struct l_timeout *eapol_start_timeout;
	bool have_replay:1;
	bool started:1;
	bool use_eapol_start:1;
	struct eap_state *eap;
	struct eapol_buffer *early_frame;
};

static void eapol_sm_destroy(void *value)
{
	struct eapol_sm *sm = value;

	l_timeout_remove(sm->timeout);
	l_timeout_remove(sm->eapol_start_timeout);

	if (sm->eap)
		eap_free(sm->eap);

	l_free(sm->early_frame);

	l_free(sm);

	l_queue_remove(state_machines, sm);
}

struct eapol_sm *eapol_sm_new(struct handshake_state *hs)
{
	struct eapol_sm *sm;

	sm = l_new(struct eapol_sm, 1);

	sm->handshake = hs;

	return sm;
}

void eapol_sm_free(struct eapol_sm *sm)
{
	eapol_sm_destroy(sm);
}

void eapol_sm_set_protocol_version(struct eapol_sm *sm,
				enum eapol_protocol_version protocol_version)
{
	sm->protocol_version = protocol_version;
}

void eapol_sm_set_user_data(struct eapol_sm *sm, void *user_data)
{
	sm->user_data = user_data;
}

void eapol_sm_set_event_func(struct eapol_sm *sm, eapol_sm_event_func_t func)
{
	sm->event_func = func;
}

static inline void handshake_failed(struct eapol_sm *sm, uint16_t reason_code)
{
	if (deauthenticate)
		deauthenticate(sm->handshake->ifindex,
				sm->handshake->aa, sm->handshake->spa,
				reason_code, sm->user_data);

	eapol_sm_free(sm);
}

static void eapol_timeout(struct l_timeout *timeout, void *user_data)
{
	struct eapol_sm *sm = user_data;

	sm->timeout = NULL;
	handshake_failed(sm, MPDU_REASON_CODE_4WAY_HANDSHAKE_TIMEOUT);
}

static void eapol_write(struct eapol_sm *sm, const struct eapol_frame *ef)
{
	pae_write(sm->handshake->ifindex,
			sm->handshake->aa, sm->handshake->spa, ef);
}

static void send_eapol_start(struct l_timeout *timeout, void *user_data)
{
	struct eapol_sm *sm = user_data;
	uint8_t buf[sizeof(struct eapol_frame)];
	struct eapol_frame *frame = (struct eapol_frame *) buf;

	l_timeout_remove(sm->eapol_start_timeout);
	sm->eapol_start_timeout = NULL;

	if (!sm->protocol_version)
		sm->protocol_version = EAPOL_PROTOCOL_VERSION_2001;

	frame->header.protocol_version = sm->protocol_version;
	frame->header.packet_type = 1;
	l_put_be16(0, &frame->header.packet_len);

	eapol_write(sm, frame);
}

static void eapol_handle_ptk_1_of_4(struct eapol_sm *sm,
					const struct eapol_key *ek)
{
	const struct crypto_ptk *ptk;
	struct eapol_key *step2;
	uint8_t mic[16];
	uint8_t *ies;
	size_t ies_len;
	const uint8_t *own_ie = sm->handshake->own_ie;

	if (!eapol_verify_ptk_1_of_4(ek))
		goto error_unspecified;

	handshake_state_new_snonce(sm->handshake);

	handshake_state_set_anonce(sm->handshake, ek->key_nonce);

	if (!handshake_state_derive_ptk(sm->handshake))
		goto error_unspecified;

	if (sm->handshake->akm_suite &
			(IE_RSN_AKM_SUITE_FT_OVER_8021X |
			 IE_RSN_AKM_SUITE_FT_USING_PSK |
			 IE_RSN_AKM_SUITE_FT_OVER_SAE_SHA256)) {
		struct ie_rsn_info rsn_info;
		const uint8_t *mde = sm->handshake->mde;
		const uint8_t *fte = sm->handshake->fte;

		/*
		 * Rebuild the RSNE to include the PMKR1Name and append
		 * MDE + FTE.
		 */
		ies = alloca(512);

		if (ie_parse_rsne_from_data(own_ie, own_ie[1] + 2,
						&rsn_info) < 0)
			goto error_unspecified;

		rsn_info.num_pmkids = 1;
		rsn_info.pmkids = sm->handshake->pmk_r1_name;

		ie_build_rsne(&rsn_info, ies);
		ies_len = ies[1] + 2;

		memcpy(ies + ies_len, mde, mde[1] + 2);
		ies_len += mde[1] + 2;

		memcpy(ies + ies_len, fte, fte[1] + 2);
		ies_len += fte[1] + 2;
	} else {
		ies_len = own_ie[1] + 2;
		ies = (uint8_t *) own_ie;
	}

	step2 = eapol_create_ptk_2_of_4(sm->protocol_version,
					ek->key_descriptor_version,
					sm->replay_counter,
					sm->handshake->snonce, ies_len, ies,
					sm->handshake->wpa_ie);

	ptk = handshake_state_get_ptk(sm->handshake);

	if (!eapol_calculate_mic(ptk->kck, step2, mic)) {
		l_info("MIC calculation failed. "
			"Ensure Kernel Crypto is available.");
		l_free(step2);
		handshake_failed(sm, MPDU_REASON_CODE_UNSPECIFIED);

		return;
	}

	memcpy(step2->key_mic_data, mic, sizeof(mic));
	eapol_write(sm, (struct eapol_frame *) step2);
	l_free(step2);

	l_timeout_remove(sm->timeout);
	sm->timeout = NULL;

	return;

error_unspecified:
	handshake_failed(sm, MPDU_REASON_CODE_UNSPECIFIED);
}

static const uint8_t *eapol_find_rsne(const uint8_t *data, size_t data_len,
					const uint8_t **optional)
{
	struct ie_tlv_iter iter;
	const uint8_t *first = NULL;

	ie_tlv_iter_init(&iter, data, data_len);

	while (ie_tlv_iter_next(&iter)) {
		if (ie_tlv_iter_get_tag(&iter) != IE_TYPE_RSN)
			continue;

		if (!first) {
			first = ie_tlv_iter_get_data(&iter) - 2;
			continue;
		}

		if (optional)
			*optional = ie_tlv_iter_get_data(&iter) - 2;

		return first;
	}

	return first;
}

static const uint8_t *eapol_find_wpa_ie(const uint8_t *data, size_t data_len)
{
	struct ie_tlv_iter iter;

	ie_tlv_iter_init(&iter, data, data_len);

	while (ie_tlv_iter_next(&iter)) {
		if (ie_tlv_iter_get_tag(&iter) != IE_TYPE_VENDOR_SPECIFIC)
			continue;

		if (is_ie_wpa_ie(ie_tlv_iter_get_data(&iter),
				ie_tlv_iter_get_length(&iter)))
			return ie_tlv_iter_get_data(&iter) - 2;
	}

	return NULL;
}

static void eapol_handle_ptk_3_of_4(struct eapol_sm *sm,
					const struct eapol_key *ek,
					const uint8_t *decrypted_key_data,
					size_t decrypted_key_data_size)
{
	const struct crypto_ptk *ptk;
	struct eapol_key *step4;
	uint8_t mic[16];
	const uint8_t *gtk;
	size_t gtk_len;
	const uint8_t *igtk;
	size_t igtk_len;
	const uint8_t *rsne;
	const uint8_t *optional_rsne = NULL;
	uint8_t gtk_key_index;
	uint8_t igtk_key_index;

	if (!eapol_verify_ptk_3_of_4(ek, sm->handshake->wpa_ie)) {
		handshake_failed(sm, MPDU_REASON_CODE_UNSPECIFIED);
		return;
	}

	/*
	 * 11.6.6.4: "On reception of Message 3, the Supplicant silently
	 * discards the message if ... or if the ANonce value in Message 3
	 * differs from the ANonce value in Message 1"
	 */
	if (memcmp(sm->handshake->anonce, ek->key_nonce, sizeof(ek->key_nonce)))
		return;

	/*
	 * 11.6.6.4: "Verifies the RSNE. If it is part of a Fast BSS Transition
	 * Initial Mobility Domain Association, see 12.4.2. Otherwise, if it is
	 * not identical to that the STA received in the Beacon or Probe
	 * Response frame, the STA shall disassociate.
	 */
	if (!sm->handshake->wpa_ie)
		rsne = eapol_find_rsne(decrypted_key_data,
					decrypted_key_data_size,
					&optional_rsne);
	else
		rsne = eapol_find_wpa_ie(decrypted_key_data,
					decrypted_key_data_size);

	if (!rsne)
		goto error_ie_different;

	if (!handshake_util_ap_ie_matches(rsne, sm->handshake->ap_ie,
						sm->handshake->wpa_ie))
		goto error_ie_different;

	if (sm->handshake->akm_suite &
			(IE_RSN_AKM_SUITE_FT_OVER_8021X |
			 IE_RSN_AKM_SUITE_FT_USING_PSK |
			 IE_RSN_AKM_SUITE_FT_OVER_SAE_SHA256)) {
		struct ie_tlv_iter iter;
		struct ie_rsn_info ie_info;
		const uint8_t *mde = sm->handshake->mde;
		const uint8_t *fte = sm->handshake->fte;

		ie_parse_rsne_from_data(rsne, rsne[1] + 2, &ie_info);

		if (ie_info.num_pmkids != 1 || memcmp(ie_info.pmkids,
						sm->handshake->pmk_r1_name, 16))
			goto error_ie_different;

		ie_tlv_iter_init(&iter, decrypted_key_data,
					decrypted_key_data_size);

		while (ie_tlv_iter_next(&iter))
			switch (ie_tlv_iter_get_tag(&iter)) {
			case IE_TYPE_MOBILITY_DOMAIN:
				if (memcmp(ie_tlv_iter_get_data(&iter) - 2,
						mde, mde[1] + 2))
					goto error_ie_different;

				break;

			case IE_TYPE_FAST_BSS_TRANSITION:
				if (memcmp(ie_tlv_iter_get_data(&iter) - 2,
						fte, fte[1] + 2))
					goto error_ie_different;

				break;
			}
	}

	/*
	 * 11.6.6.4: "If a second RSNE is provided in the message, the
	 * Supplicant uses the pairwise cipher suite specified in the second
	 * RSNE or deauthenticates."
	 */
	if (optional_rsne) {
		struct ie_rsn_info info1;
		struct ie_rsn_info info2;
		uint16_t override;

		if (ie_parse_rsne_from_data(rsne, rsne[1] + 2, &info1) < 0)
			goto error_ie_different;

		if (ie_parse_rsne_from_data(optional_rsne, optional_rsne[1] + 2,
						&info2) < 0)
			goto error_ie_different;

		/*
		 * 11.6.2:
		 * It may happen, for example, that a Supplicant selects a
		 * pairwise cipher suite which is advertised by an AP, but
		 * which policy disallows for this particular STA. An
		 * Authenticator may, therefore, insert a second RSNE to
		 * overrule the STA’s selection. An Authenticator’s SME shall
		 * insert the second RSNE, after the first RSNE, only for this
		 * purpose. The pairwise cipher suite in the second RSNE
		 * included shall be one of the ciphers advertised by the
		 * Authenticator. All other fields in the second RSNE shall be
		 * identical to the first RSNE.
		 *
		 * - Check that akm_suites and group_cipher are the same
		 *   between rsne1 and rsne2
		 * - Check that pairwise_ciphers is not the same between rsne1
		 *   and rsne2
		 * - Check that rsne2 pairwise_ciphers is a subset of rsne
		 */
		if (info1.akm_suites != info2.akm_suites ||
				info1.group_cipher != info2.group_cipher)
			goto error_ie_different;

		override = info2.pairwise_ciphers;

		if (override == info1.pairwise_ciphers ||
				!(info1.pairwise_ciphers & override) ||
				__builtin_popcount(override) != 1) {
			handshake_failed(sm,
				MPDU_REASON_CODE_INVALID_PAIRWISE_CIPHER);
			return;
		}

		handshake_state_override_pairwise_cipher(sm->handshake,
								override);
	}

	/*
	 * TODO: Handle IE_RSN_CIPHER_SUITE_NO_GROUP_TRAFFIC case
	 */
	if (!sm->handshake->wpa_ie) {
		gtk = handshake_util_find_gtk_kde(decrypted_key_data,
							decrypted_key_data_size,
							&gtk_len);
		if (!gtk || gtk_len < 8) {
			handshake_failed(sm, MPDU_REASON_CODE_UNSPECIFIED);
			return;
		}

		/* TODO: Handle tx bit */

		gtk_key_index = util_bit_field(gtk[0], 0, 2);
		gtk += 2;
		gtk_len -= 2;
	} else
		gtk = NULL;

	if (sm->handshake->mfp) {
		igtk = handshake_util_find_igtk_kde(decrypted_key_data,
							decrypted_key_data_size,
							&igtk_len);
		if (!igtk || igtk_len < 8) {
			handshake_failed(sm, MPDU_REASON_CODE_UNSPECIFIED);
			return;
		}

		igtk_key_index = util_bit_field(igtk[0], 0, 2);
		igtk += 2;
		igtk_len -= 2;
	} else
		igtk = NULL;

	step4 = eapol_create_ptk_4_of_4(sm->protocol_version,
					ek->key_descriptor_version,
					sm->replay_counter,
					sm->handshake->wpa_ie);

	ptk = handshake_state_get_ptk(sm->handshake);

	/*
	 * 802.11-2012, Section 11.6.6.4, step b):
	 * Verifies the Message 3 MIC. If the calculated MIC does not match
	 * the MIC that the Authenticator included in the EAPOL-Key frame,
	 * the Supplicant silently discards Message 3.
	 */
	if (!eapol_calculate_mic(ptk->kck, step4, mic))
		goto fail;

	memcpy(step4->key_mic_data, mic, sizeof(mic));
	eapol_write(sm, (struct eapol_frame *) step4);

	handshake_state_install_ptk(sm->handshake);

	if (gtk)
		handshake_state_install_gtk(sm->handshake, gtk_key_index,
						gtk, gtk_len, ek->key_rsc, 6);

	if (igtk)
		handshake_state_install_igtk(sm->handshake, igtk_key_index,
						igtk, igtk_len);

	if (rekey_offload)
		rekey_offload(sm->handshake->ifindex, ptk->kek, ptk->kck,
				sm->replay_counter, sm->user_data);

fail:
	l_free(step4);

	return;

error_ie_different:
	handshake_failed(sm, MPDU_REASON_CODE_IE_DIFFERENT);
}

static void eapol_handle_gtk_1_of_2(struct eapol_sm *sm,
					const struct eapol_key *ek,
					const uint8_t *decrypted_key_data,
					size_t decrypted_key_data_size)
{
	const struct crypto_ptk *ptk;
	struct eapol_key *step2;
	uint8_t mic[16];
	const uint8_t *gtk;
	size_t gtk_len;
	uint8_t gtk_key_index;
	const uint8_t *igtk;
	size_t igtk_len;
	uint8_t igtk_key_index;

	if (!eapol_verify_gtk_1_of_2(ek, sm->handshake->wpa_ie)) {
		handshake_failed(sm, MPDU_REASON_CODE_UNSPECIFIED);
		return;
	}

	if (!sm->handshake->wpa_ie) {
		gtk = handshake_util_find_gtk_kde(decrypted_key_data,
							decrypted_key_data_size,
							&gtk_len);

		if (!gtk || gtk_len < 8)
			return;
	} else {
		gtk = decrypted_key_data;
		gtk_len = decrypted_key_data_size;

		if (!gtk || gtk_len < 6)
			return;
	}

	if (!sm->handshake->wpa_ie) {
		gtk_key_index = util_bit_field(gtk[0], 0, 2);
		gtk += 2;
		gtk_len -= 2;
	} else
		gtk_key_index = ek->wpa_key_id;

	if (sm->handshake->mfp) {
		igtk = handshake_util_find_igtk_kde(decrypted_key_data,
							decrypted_key_data_size,
							&igtk_len);
		if (!igtk || igtk_len < 8)
			return;

		igtk_key_index = util_bit_field(igtk[0], 0, 2);
		igtk += 2;
		igtk_len -= 2;
	} else
		igtk = NULL;

	step2 = eapol_create_gtk_2_of_2(sm->protocol_version,
					ek->key_descriptor_version,
					sm->replay_counter,
					sm->handshake->wpa_ie, ek->wpa_key_id);

	/*
	 * 802.11-2012, Section 11.6.7.3, step b):
	 * Verifies that the MIC is valid, i.e., it uses the KCK that is
	 * part of the PTK to verify that there is no data integrity error.
	 */
	ptk = handshake_state_get_ptk(sm->handshake);

	if (!eapol_calculate_mic(ptk->kck, step2, mic))
		goto done;

	memcpy(step2->key_mic_data, mic, sizeof(mic));
	eapol_write(sm, (struct eapol_frame *) step2);

	handshake_state_install_gtk(sm->handshake, gtk_key_index,
					gtk, gtk_len, ek->key_rsc, 6);

	if (igtk) {
		handshake_state_install_igtk(sm->handshake, igtk_key_index,
						igtk, igtk_len);
	}

done:
	l_free(step2);
}

static struct eapol_sm *eapol_find_sm(uint32_t ifindex, const uint8_t *aa)
{
	const struct l_queue_entry *entry;
	struct eapol_sm *sm;

	for (entry = l_queue_get_entries(state_machines); entry;
					entry = entry->next) {
		sm = entry->data;

		if (sm->handshake->ifindex != ifindex)
			continue;

		if (memcmp(sm->handshake->aa, aa, ETH_ALEN))
			continue;

		return sm;
	}

	return NULL;
}

static void eapol_key_handle(struct eapol_sm *sm,
				const uint8_t *frame, size_t len)
{
	const struct eapol_key *ek;
	const struct crypto_ptk *ptk;
	uint8_t *decrypted_key_data = NULL;
	size_t key_data_len = 0;
	uint64_t replay_counter;

	ek = eapol_key_validate(frame, len);
	if (!ek)
		return;

	/* Wrong direction */
	if (!ek->key_ack)
		return;

	/* Further Descriptor Type check */
	if (!sm->handshake->wpa_ie &&
			ek->descriptor_type != EAPOL_DESCRIPTOR_TYPE_80211)
		return;
	else if (sm->handshake->wpa_ie &&
			ek->descriptor_type != EAPOL_DESCRIPTOR_TYPE_WPA)
		return;

	replay_counter = L_BE64_TO_CPU(ek->key_replay_counter);

	/*
	 * 11.6.6.2: "If the Key Replay Counter field value is less than or
	 * equal to the current local value, the Supplicant discards the
	 * message.
	 *
	 * 11.6.6.4: "On reception of Message 3, the Supplicant silently
	 * discards the message if the Key Replay Counter field value has
	 * already been used...
	 */
	if (sm->have_replay && sm->replay_counter >= replay_counter)
		return;

	sm->replay_counter = replay_counter;
	sm->have_replay = true;

	ptk = handshake_state_get_ptk(sm->handshake);

	if (ek->key_mic) {
		/* Haven't received step 1 yet, so no ptk */
		if (!sm->handshake->have_snonce)
			return;

		if (!eapol_verify_mic(ptk->kck, ek))
			return;
	}

	if ((ek->encrypted_key_data && !sm->handshake->wpa_ie) ||
			(ek->key_type == 0 && sm->handshake->wpa_ie)) {
		/* Haven't received step 1 yet, so no ptk */
		if (!sm->handshake->have_snonce)
			return;

		decrypted_key_data = eapol_decrypt_key_data(ptk->kek, ek,
						&key_data_len);
		if (!decrypted_key_data)
			return;
	} else
		key_data_len = L_BE16_TO_CPU(ek->key_data_len);

	if (ek->key_type == 0) {
		/* Only GTK handshake allowed after PTK handshake complete */
		if (!sm->handshake->ptk_complete)
			goto done;

		if (!decrypted_key_data)
			goto done;

		eapol_handle_gtk_1_of_2(sm, ek, decrypted_key_data,
					key_data_len);
		goto done;
	}

	/* If no MIC, then assume packet 1, otherwise packet 3 */
	if (!ek->key_mic)
		eapol_handle_ptk_1_of_4(sm, ek);
	else {
		if (sm->handshake->ptk_complete)
			goto done;

		if (!key_data_len)
			goto done;

		eapol_handle_ptk_3_of_4(sm, ek,
					decrypted_key_data ?: ek->key_data,
					key_data_len);
	}

done:
	l_free(decrypted_key_data);
}

/* This respresentes the eapMsg message in 802.1X Figure 8-1 */
static void eapol_eap_msg_cb(const uint8_t *eap_data, size_t len,
					void *user_data)
{
	struct eapol_sm *sm = user_data;
	uint8_t buf[sizeof(struct eapol_frame) + len];
	struct eapol_frame *frame = (struct eapol_frame *) buf;

	frame->header.protocol_version = sm->protocol_version;
	frame->header.packet_type = 0;
	l_put_be16(len, &frame->header.packet_len);

	memcpy(frame->data, eap_data, len);

	eapol_write(sm, frame);
}

/* This respresentes the eapTimout, eapFail and eapSuccess messages */
static void eapol_eap_complete_cb(enum eap_result result, void *user_data)
{
	struct eapol_sm *sm = user_data;

	l_info("EAP completed with %s", result == EAP_RESULT_SUCCESS ?
			"eapSuccess" : (result == EAP_RESULT_FAIL ?
				"eapFail" : "eapTimeout"));

	eap_free(sm->eap);
	sm->eap = NULL;

	if (result != EAP_RESULT_SUCCESS)
		handshake_failed(sm, MPDU_REASON_CODE_IEEE8021X_FAILED);
}

/* This respresentes the eapResults message */
static void eapol_eap_results_cb(const uint8_t *msk_data, size_t msk_len,
				const uint8_t *emsk_data, size_t emsk_len,
				const uint8_t *iv, size_t iv_len,
				void *user_data)
{
	struct eapol_sm *sm = user_data;

	l_debug("EAP key material received");

	/*
	 * 802.11i 8.5.1.2:
	 *    "When not using a PSK, the PMK is derived from the AAA key.
	 *    The PMK shall be computed as the first 256 bits (bits 0–255)
	 *    of the AAA key: PMK ← L(PTK, 0, 256)."
	 * 802.11 11.6.1.3:
	 *    "When not using a PSK, the PMK is derived from the MSK.
	 *    The PMK shall be computed as the first 256 bits (bits 0–255)
	 *    of the MSK: PMK ← L(MSK, 0, 256)."
	 * RFC5247 explains AAA-Key refers to the MSK and confirms the
	 * first 32 bytes of the MSK are used.  MSK is at least 64 octets
	 * long per RFC3748.  Note WEP derives the PTK from MSK differently.
	 *
	 * In a Fast Transition initial mobility domain association the PMK
	 * maps to the XXKey except with EAP:
	 * 802.11 11.6.1.7.3:
	 *    "If the AKM negotiated is 00-0F-AC:3, then XXKey shall be the
	 *    second 256 bits of the MSK (which is derived from the IEEE
	 *    802.1X authentication), i.e., XXKey = L(MSK, 256, 256)."
	 */

	if (sm->handshake->akm_suite == IE_RSN_AKM_SUITE_FT_OVER_8021X)
		handshake_state_set_pmk(sm->handshake, msk_data + 32);
	else
		handshake_state_set_pmk(sm->handshake, msk_data);
}

static void eapol_eap_event_cb(unsigned int event,
				const void *event_data, void *user_data)
{
	struct eapol_sm *sm = user_data;

	if (!sm->event_func)
		return;

	sm->event_func(event, event_data, sm->user_data);
}

void eapol_sm_set_use_eapol_start(struct eapol_sm *sm, bool enabled)
{
	sm->use_eapol_start = enabled;
}

static void eapol_rx_packet(struct eapol_sm *sm,
					const uint8_t *frame, size_t len)
{
	const struct eapol_header *eh;

	if (!sm->started) {
		struct eapol_buffer *buf;

		/*
		 * If the state machine hasn't started yet save the frame
		 * for processing later.
		 */
		if (sm->early_frame) /* Is the 1-element queue full */
			return;

		buf = l_malloc(sizeof(struct eapol_buffer) + len);

		buf->len = len;
		memcpy(buf->data, frame, len);

		sm->early_frame = buf;

		return;
	}

	/* Validate Header */
	if (len < sizeof(struct eapol_header))
		return;

	eh = (const struct eapol_header *) frame;

	switch (eh->protocol_version) {
	case EAPOL_PROTOCOL_VERSION_2001:
	case EAPOL_PROTOCOL_VERSION_2004:
		break;
	default:
		return;
	}

	if (len < (size_t) 4 + L_BE16_TO_CPU(eh->packet_len))
		return;

	if (!sm->protocol_version)
		sm->protocol_version = eh->protocol_version;

	switch (eh->packet_type) {
	case 0: /* EAPOL-EAP */
		l_timeout_remove(sm->eapol_start_timeout);
		sm->eapol_start_timeout = 0;

		if (!sm->eap) {
			/* If we're not configured for EAP, send a NAK */
			sm->eap = eap_new(eapol_eap_msg_cb,
						eapol_eap_complete_cb, sm);

			if (!sm->eap)
				return;

			eap_set_key_material_func(sm->eap,
							eapol_eap_results_cb);
		}

		eap_rx_packet(sm->eap, frame + 4,
				L_BE16_TO_CPU(eh->packet_len));

		break;

	case 3: /* EAPOL-Key */
		if (sm->eap) /* An EAP negotiation in progress? */
			return;

		if (!sm->handshake->have_pmk)
			return;

		eapol_key_handle(sm, frame, len);
		break;

	default:
		return;
	}
}

void __eapol_rx_packet(uint32_t ifindex, const uint8_t *aa,
					const uint8_t *frame, size_t len)
{
	struct eapol_sm *sm = eapol_find_sm(ifindex, aa);

	if (!sm)
		return;

	eapol_rx_packet(sm, frame, len);
}

void __eapol_update_replay_counter(uint32_t ifindex, const uint8_t *spa,
				const uint8_t *aa, uint64_t replay_counter)
{
	struct eapol_sm *sm;

	sm = eapol_find_sm(ifindex, aa);

	if (!sm)
		return;

	if (sm->replay_counter >= replay_counter)
		return;

	sm->replay_counter = replay_counter;
}

void __eapol_set_tx_packet_func(eapol_tx_packet_func_t func)
{
	tx_packet = func;
}

void __eapol_set_tx_user_data(void *user_data)
{
	tx_user_data = user_data;
}

void __eapol_set_deauthenticate_func(eapol_deauthenticate_func_t func)
{
	deauthenticate = func;
}

void __eapol_set_rekey_offload_func(eapol_rekey_offload_func_t func)
{
	rekey_offload = func;
}

void eapol_register(struct eapol_sm *sm)
{
	l_queue_push_head(state_machines, sm);
}

void eapol_start(struct eapol_sm *sm)
{

	sm->timeout = l_timeout_create(2, eapol_timeout, sm, NULL);

	sm->started = true;

	if (sm->use_eapol_start) {
		/*
		 * We start a short timeout, if EAP packets are not received
		 * from AP, then we send the EAPoL-Start
		 */
		sm->eapol_start_timeout =
				l_timeout_create(1, send_eapol_start, sm, NULL);
	}

	if (sm->handshake->settings_8021x) {
		sm->eap = eap_new(eapol_eap_msg_cb, eapol_eap_complete_cb, sm);

		if (!sm->eap)
			goto eap_error;

		if (!eap_load_settings(sm->eap, sm->handshake->settings_8021x,
					"EAP-")) {
			eap_free(sm->eap);
			sm->eap = NULL;

			goto eap_error;
		}

		eap_set_key_material_func(sm->eap, eapol_eap_results_cb);
		eap_set_event_func(sm->eap, eapol_eap_event_cb);
	}

	/* Process any frames received early due to scheduling */
	if (sm->early_frame) {
		struct eapol_buffer *tmp = sm->early_frame;

		sm->early_frame = NULL;
		eapol_rx_packet(sm, tmp->data, tmp->len);
		l_free(tmp);
	}

	return;

eap_error:
	l_error("Error initializing EAP for ifindex %i",
			(int) sm->handshake->ifindex);
}

bool eapol_init()
{
	state_machines = l_queue_new();

	return true;
}

bool eapol_exit()
{
	if (!l_queue_isempty(state_machines))
		l_warn("stale eapol state machines found");

	l_queue_destroy(state_machines, eapol_sm_destroy);

	return true;
}
