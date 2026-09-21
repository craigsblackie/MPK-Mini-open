#include "sevenbit.h"

size_t sevenbit_encoded_size(size_t len)
{
	size_t groups = len / 7u;
	size_t remainder = len % 7u;
	return groups * 8u + (remainder != 0u ? remainder + 1u : 0u);
}

size_t sevenbit_decoded_size(size_t len)
{
	size_t groups = len / 8u;
	size_t remainder = len % 8u;
	if (remainder == 1u) return 0u; /* a lone MSB byte carries nothing */
	return groups * 7u + (remainder != 0u ? remainder - 1u : 0u);
}

size_t sevenbit_encode(const uint8_t *in, size_t len, uint8_t *out)
{
	size_t written = 0;
	size_t pos = 0;
	while (pos < len) {
		size_t group = len - pos;
		if (group > 7u) group = 7u;

		size_t msb_index = written++;
		uint8_t msbs = 0;
		for (size_t i = 0; i < group; i++) {
			uint8_t byte = in[pos + i];
			if (byte & 0x80u) msbs |= (uint8_t)(1u << i);
			out[written++] = (uint8_t)(byte & 0x7fu);
		}
		out[msb_index] = msbs;
		pos += group;
	}
	return written;
}

size_t sevenbit_decode(const uint8_t *in, size_t len, uint8_t *out)
{
	if (sevenbit_decoded_size(len) == 0u && len != 0u) return 0u;

	size_t written = 0;
	size_t pos = 0;
	while (pos < len) {
		size_t group = len - pos - 1u; /* bytes following the MSB byte */
		if (group > 7u) group = 7u;

		uint8_t msbs = in[pos];
		if (msbs & 0x80u) return 0u;
		for (size_t i = 0; i < group; i++) {
			uint8_t byte = in[pos + 1u + i];
			if (byte & 0x80u) return 0u;
			out[written++] = (uint8_t)(byte | ((msbs & (1u << i)) ? 0x80u : 0u));
		}
		pos += group + 1u;
	}
	return written;
}
