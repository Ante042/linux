// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Offline trace-prefix decoder. No device access and no peer pointer following. */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "afkv2-codec.h"
#include "epic/dpavserv-edid.h"

#define PREFIX_LIMIT 256U
#define SERVICE_HEADER_SIZE 64U
#define STANDARD_SERVICE_TYPE 0xc0U

static int parse_number(const char *text, u64 *result)
{
	char *end;
	unsigned long long value;

	if (!text[0] || text[0] == '-')
		return -1;
	errno = 0;
	value = strtoull(text, &end, 0);
	if (errno || *end)
		return -1;
	*result = value;
	return 0;
}

static void print_result(const char *name, bool checked, int result)
{
	if (checked)
		printf(",\"%s\":%d", name, result);
	else
		printf(",\"%s\":null", name);
}

static void decode(const u8 *bytes, size_t captured, size_t declared,
		   bool ranges_supplied, u64 rx_base, u32 rx_capacity,
		   u64 tx_base, u32 tx_capacity)
{
	struct afkv2_frame frame;
	struct afkv2_command command;
	size_t frame_size, payload_size, payload_captured, command_header;
	size_t inline_declared, service_captured, result_size = 0, edid_size = 0;
	const u8 *service, *edid;
	int frame_rc, command_rc, service_rc, edid_rc, shape_rc = -EPROTO;
	bool shape_checked;
	u32 length, service_length;

	printf("{\"captured\":%zu,\"record_size\":%zu,\"missing_bytes\":%zu",
	       captured, declared, declared - captured);
	frame_rc = afkv2_decode_frame(bytes, captured, false, &frame);
	printf(",\"production_frame_rc\":%d,\"record_complete\":%s",
	       frame_rc, captured == declared ? "true" : "false");
	if (captured < AFKV2_HEADER_SIZE) {
		printf(",\"classification\":\"truncated_frame_header\"}\n");
		return;
	}
	length = afkv2_get32(bytes + 4);
	frame_size = (size_t)length + 8;
	printf(",\"frame\":{\"sequence\":%u,\"transport_flags\":%u,"
	       "\"interface\":%u,\"length\":%u,\"token\":\"0x%016" PRIx64 "\","
	       "\"type\":%u,\"category\":%u,\"interface_flags\":%u,\"reserved\":%u}",
	       bytes[0], bytes[1], afkv2_get16(bytes + 2), length,
	       afkv2_get64(bytes + 8), bytes[16], bytes[17],
	       afkv2_get16(bytes + 18), afkv2_get32(bytes + 20));
	if (length < 16 || length - 16 > AFKV2_MAX_PAYLOAD ||
	    frame_size != declared || bytes[17] > AFKV2_RESPONSE) {
		printf(",\"classification\":\"malformed_frame\"}\n");
		return;
	}
	if (!afkv2_get16(bytes + 2)) {
		printf(",\"classification\":\"driver_rejected_zero_interface\"}\n");
		return;
	}
	if (bytes[17] == AFKV2_REPORT) {
		printf(",\"classification\":\"report_not_command_response\"}\n");
		return;
	}
	payload_size = length - 16;
	payload_captured = captured - AFKV2_HEADER_SIZE;
	command_rc = afkv2_decode_command(bytes + AFKV2_HEADER_SIZE,
					payload_captured, &command);
	printf(",\"production_command_metadata_rc\":%d", command_rc);
	if (command_rc) {
		printf(",\"classification\":\"truncated_or_malformed_command\"}\n");
		return;
	}
	command_header = AFKV2_COMMAND_SIZE + (command.oob ? AFKV2_OOB_SIZE : 0);
	if (payload_size < command_header) {
		printf(",\"classification\":\"malformed_command_length\"}\n");
		return;
	}
	inline_declared = payload_size - command_header;
	printf(",\"command\":{\"oob\":%s,\"id\":%u,\"value\":%u,"
	       "\"value_role\":\"%s\",\"inline_declared\":%zu,\"inline_captured\":%zu}",
	       command.oob ? "true" : "false", command.id, command.value,
	       bytes[17] == AFKV2_RESPONSE ? "status" : "response_capacity",
	       inline_declared, command.inline_size);
	shape_checked = !frame_rc && captured == declared && bytes[17] == AFKV2_RESPONSE;
	printf(",\"request_correlation\":\"not_available_without_original_request\"");
	if (command.oob) {
		if (!frame_rc && captured == declared && bytes[17] == AFKV2_RESPONSE)
			shape_rc = afkv2_check_response(&frame, &command, frame.interface,
						       frame.type, command.id, command.rx_address,
						       command.rx_size, command.tx_address,
						       command.tx_size);
		print_result("production_oob_shape_rc", shape_checked, shape_rc);
		if (shape_checked)
			print_result("production_inline_shape_rc", true,
				     afkv2_check_inline_response(&frame, &command, frame.interface,
							 frame.type, command.id, AFKV2_MAX_PAYLOAD));
		else
			print_result("production_inline_shape_rc", false, 0);
		printf(",\"oob\":{\"rx_address\":\"0x%016" PRIx64 "\","
		       "\"tx_address\":\"0x%016" PRIx64 "\",\"rx_size\":%u,\"tx_size\":%u,"
		       "\"memory_followed\":false,\"live_slot_ownership\":\"not_available_offline\"",
		       command.rx_address, command.tx_address, command.rx_size, command.tx_size);
		if (ranges_supplied) {
			/* Address equality/bounds are evidence only, not a live owner pin. */
			printf(",\"supplied_request_descriptors_match\":%s",
			       command.rx_address == rx_base && command.tx_address == tx_base &&
			       command.rx_size <= rx_capacity && command.tx_size <= tx_capacity ?
			       "true" : "false");
		}
		printf("},\"classification\":\"oob_descriptor_payload_not_captured\"}\n");
		return;
	}
	if (!frame_rc && captured == declared && bytes[17] == AFKV2_RESPONSE)
		shape_rc = afkv2_check_inline_response(&frame, &command, frame.interface,
						      frame.type, command.id, AFKV2_MAX_PAYLOAD);
	print_result("production_inline_shape_rc", shape_checked, shape_rc);
	if (shape_checked)
		print_result("production_oob_shape_rc", true,
			     afkv2_check_response(&frame, &command, frame.interface, frame.type,
					   command.id, 0, 0, 0, 0));
	else
		print_result("production_oob_shape_rc", false, 0);
	if (bytes[17] == AFKV2_RESPONSE && command.value) {
		printf(",\"classification\":\"nonzero_transport_status\"}\n");
		return;
	}
	if (bytes[16] != STANDARD_SERVICE_TYPE) {
		printf(",\"classification\":\"not_standard_service\"}\n");
		return;
	}
	service = command.inline_data;
	service_captured = command.inline_size;
	if (service_captured < SERVICE_HEADER_SIZE) {
		printf(",\"classification\":\"%s\"}\n",
		       inline_declared ? "service_header_not_captured" : "no_inline_service_payload");
		return;
	}
	service_length = afkv2_get32(service + 8);
	printf(",\"service\":{\"group\":%u,\"command\":%u,\"data_len\":%u,"
	       "\"magic\":\"0x%08x\",\"body_captured\":%zu,\"body_declared_in_frame\":%zu}",
	       afkv2_get16(service + 2), afkv2_get32(service + 4), service_length,
	       afkv2_get32(service + 12), service_captured - SERVICE_HEADER_SIZE,
	       inline_declared - SERVICE_HEADER_SIZE);
	service_rc = afkv2_service_reply_size(service, service_captured, 1, 7,
					     DPAV_EDID_HEADER_SIZE + EDID_BUF_SIZE, &result_size);
	printf(",\"production_edid_service_rc\":%d", service_rc);
	if (afkv2_get32(service + 12) != 0x69706378 ||
	    afkv2_get16(service + 2) != 1 || afkv2_get32(service + 4) != 7) {
		printf(",\"classification\":\"not_matched_edid_service_reply\"}\n");
		return;
	}
	if (service_length < DPAV_EDID_HEADER_SIZE ||
	    service_captured - SERVICE_HEADER_SIZE < DPAV_EDID_HEADER_SIZE) {
		printf(",\"classification\":\"edid_header_absent_or_truncated\"}\n");
		return;
	}
	edid = service + SERVICE_HEADER_SIZE;
	printf(",\"edid\":{\"max_size\":%" PRIu64 ",\"used_size\":%" PRIu64 ","
	       "\"reply_bytes_declared\":%u,\"reply_bytes_captured\":%zu,"
	       "\"data_bytes_captured_after_header\":%zu}",
	       afkv2_get64(edid), afkv2_get64(edid + 32), service_length,
	       service_captured - SERVICE_HEADER_SIZE,
	       service_captured - SERVICE_HEADER_SIZE - DPAV_EDID_HEADER_SIZE);
	if (bytes[17] != AFKV2_RESPONSE) {
		printf(",\"classification\":\"edid_request_not_response\"}\n");
		return;
	}
	if (service_rc || captured != declared || frame_rc || shape_rc) {
		printf(",\"classification\":\"incomplete_or_malformed_service_reply\"}\n");
		return;
	}
	edid_rc = dpavserv_edid_size(edid, result_size, &edid_size);
	printf(",\"production_edid_rc\":%d,\"validated_edid_bytes\":%zu", edid_rc, edid_size);
	printf(",\"classification\":\"%s\"}\n",
	       !edid_rc ? "valid_edid_payload" :
	       !afkv2_get64(edid + 32) ? "empty_edid_reply_rejected" :
	       result_size == DPAV_EDID_HEADER_SIZE ? "edid_data_absent_rejected" :
	       "invalid_edid_payload_rejected");
}

int main(int argc, char **argv)
{
	u8 bytes[PREFIX_LIMIT + 1];
	u64 declared, rx_base = 0, rx_capacity = 0, tx_base = 0, tx_capacity = 0;
	size_t captured;

	if ((argc != 2 && argc != 6) || parse_number(argv[1], &declared) ||
	    declared > AFKV2_HEADER_SIZE + AFKV2_MAX_PAYLOAD) {
		fputs("usage: decoder record_size [rx_base rx_capacity tx_base tx_capacity] < prefix.bin\n", stderr);
		return 2;
	}
	if (argc == 6 && (parse_number(argv[2], &rx_base) || parse_number(argv[3], &rx_capacity) ||
			 parse_number(argv[4], &tx_base) || parse_number(argv[5], &tx_capacity) ||
			 rx_capacity > UINT32_MAX || tx_capacity > UINT32_MAX))
		return 2;
	captured = fread(bytes, 1, sizeof(bytes), stdin);
	if (ferror(stdin) || captured > PREFIX_LIMIT || captured > declared) {
		fputs("invalid prefix length (maximum 256, no longer than record)\n", stderr);
		return 2;
	}
	decode(bytes, captured, declared, argc == 6, rx_base, rx_capacity, tx_base, tx_capacity);
	return 0;
}
