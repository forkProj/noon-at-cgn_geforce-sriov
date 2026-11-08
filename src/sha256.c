// SPDX-License-Identifier: MIT
/* SHA-256 (FIPS 180-4) for the install manifest: uninstall removes only files whose hash still matches. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gs_lifecycle.h"

struct sha256 {
	uint32_t h[8];
	uint64_t len;
	unsigned char buf[64];
	size_t fill;
};

static const uint32_t k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, unsigned int n)
{
	return (x >> n) | (x << (32 - n));
}

static void block(struct sha256 *s, const unsigned char *p)
{
	uint32_t w[64], a, b, c, d, e, f, g, h;
	unsigned int i;

	for (i = 0; i < 16; i++)
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
		       (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
	for (i = 16; i < 64; i++) {
		uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
	e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (i = 0; i < 64; i++) {
		uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
		uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));

		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
	s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void init(struct sha256 *s)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(s->h, iv, sizeof(iv));
	s->len = 0;
	s->fill = 0;
}

static void update(struct sha256 *s, const void *data, size_t len)
{
	const unsigned char *p = data;

	s->len += len;
	while (len) {
		size_t n = 64 - s->fill;

		if (n > len)
			n = len;
		memcpy(s->buf + s->fill, p, n);
		s->fill += n;
		p += n;
		len -= n;
		if (s->fill == 64) {
			block(s, s->buf);
			s->fill = 0;
		}
	}
}

static void final(struct sha256 *s, char hex[65])
{
	static const char digits[] = "0123456789abcdef";
	uint64_t bits = s->len * 8;
	unsigned char pad = 0x80, len8[8];
	unsigned int i;

	update(s, &pad, 1);
	pad = 0;
	while (s->fill != 56)
		update(s, &pad, 1);
	for (i = 0; i < 8; i++)
		len8[i] = (unsigned char)(bits >> (56 - 8 * i));
	update(s, len8, 8);
	for (i = 0; i < 32; i++) {
		unsigned char byte = (unsigned char)(s->h[i / 4] >> (24 - 8 * (i % 4)));

		hex[2 * i] = digits[byte >> 4];
		hex[2 * i + 1] = digits[byte & 15];
	}
	hex[64] = '\0';
}

void gs_sha256_buf(const void *data, size_t len, char hex[65])
{
	struct sha256 s;

	init(&s);
	update(&s, data, len);
	final(&s, hex);
}

/*
 * Strict, for the install manifest: path itself must be a regular file. A symbolic link is never
 * followed (O_NOFOLLOW): it fails with -ELOOP, so install does not take over and uninstall does not
 * remove a link put where an installed file was, whatever it points at. A missing path gives
 * -ENOENT, a directory -EISDIR and any other file type (FIFO, device) -EINVAL. O_NONBLOCK keeps
 * the open of a FIFO from waiting for a writer.
 */
int gs_sha256_file(const char *path, char hex[65])
{
	unsigned char buf[65536];
	struct sha256 s;
	struct stat st;
	int fd, e;

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0)
		e = errno;
	else if (S_ISDIR(st.st_mode))
		e = EISDIR;
	else if (!S_ISREG(st.st_mode))
		e = EINVAL;
	else
		e = 0;
	if (e) {
		close(fd);
		return -e;
	}
	init(&s);
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			e = -errno;
			close(fd);
			return e;
		}
		if (n == 0)
			break;
		update(&s, buf, (size_t)n);
	}
	close(fd);
	final(&s, hex);
	return 0;
}

_Static_assert(GS_PATH_MAX >= PATH_MAX, "realpath() writes up to PATH_MAX bytes");

/*
 * For the vGPU plugin, which is documented as a link (libnvidia-vgpu.so -> the versioned copy):
 * path is resolved once with realpath() into real, then real is hashed by gs_sha256_file(), the
 * same strict open and checks. A link swapped in at real after the resolution is refused, not
 * followed. A dangling link gives -ENOENT. Read real only when 0 is returned.
 */
int gs_sha256_file_resolved(const char *path, char hex[65], char real[GS_PATH_MAX])
{
	if (!realpath(path, real))
		return -errno;
	return gs_sha256_file(real, hex);
}
