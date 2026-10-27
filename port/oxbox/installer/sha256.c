/* sha256.c: SHA-256 (FIPS 180-4), for the installer's checks of the player's Halo XBE and of what it writes */
#include "sha256.h"

#include <string.h>

static unsigned long const k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) ((((x) >> (n)) | ((x) << (32 - (n)))) & 0xffffffffu)

static void block(struct sha256 *s, unsigned char const *p)
{
	unsigned long w[64], a, b, c, d, e, f, g, h, t1, t2;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = (unsigned long)p[i * 4] << 24 | (unsigned long)p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (i = 16; i < 64; i++)
	{
		unsigned long s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		unsigned long s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = (w[i - 16] + s0 + w[i - 7] + s1) & 0xffffffffu;
	}
	a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
	for (i = 0; i < 64; i++)
	{
		t1 = (h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i]) & 0xffffffffu;
		t2 = ((ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) & 0xffffffffu;
		h = g, g = f, f = e, e = (d + t1) & 0xffffffffu, d = c, c = b, b = a, a = (t1 + t2) & 0xffffffffu;
	}
	s->h[0] = (s->h[0] + a) & 0xffffffffu, s->h[1] = (s->h[1] + b) & 0xffffffffu;
	s->h[2] = (s->h[2] + c) & 0xffffffffu, s->h[3] = (s->h[3] + d) & 0xffffffffu;
	s->h[4] = (s->h[4] + e) & 0xffffffffu, s->h[5] = (s->h[5] + f) & 0xffffffffu;
	s->h[6] = (s->h[6] + g) & 0xffffffffu, s->h[7] = (s->h[7] + h) & 0xffffffffu;
}

void sha256_start(struct sha256 *s)
{
	static unsigned long const start[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(s->h, start, sizeof(start));
	s->length = 0;
	s->used = 0;
}

void sha256_add(struct sha256 *s, void const *data, unsigned long size)
{
	unsigned char const *p = data;
	s->length += size;
	if (s->used)
	{
		unsigned long n = 64 - s->used < size ? 64 - s->used : size;
		memcpy(s->buffer + s->used, p, n);
		s->used += n, p += n, size -= n;
		if (s->used < 64)
			return;
		block(s, s->buffer);
		s->used = 0;
	}
	for (; size >= 64; p += 64, size -= 64)
		block(s, p);
	memcpy(s->buffer, p, size);
	s->used = size;
}

/* the same number of zeros */
void sha256_add_zeros(struct sha256 *s, unsigned long size)
{
	static unsigned char const zeros[4096];
	while (size)
	{
		unsigned long n = size < sizeof(zeros) ? size : sizeof(zeros);
		sha256_add(s, zeros, n);
		size -= n;
	}
}

void sha256_end(struct sha256 *s, unsigned char *digest)
{
	unsigned long long bits = (unsigned long long)s->length * 8;
	unsigned char pad[72];
	unsigned long n = (s->used < 56 ? 56 : 120) - s->used;
	int i;
	memset(pad, 0, sizeof(pad));
	pad[0] = 0x80;
	for (i = 0; i < 8; i++)
		pad[n + i] = (unsigned char)(bits >> (56 - 8 * i));
	sha256_add(s, pad, n + 8);
	for (i = 0; i < 8; i++)
	{
		digest[i * 4] = (unsigned char)(s->h[i] >> 24), digest[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
		digest[i * 4 + 2] = (unsigned char)(s->h[i] >> 8), digest[i * 4 + 3] = (unsigned char)s->h[i];
	}
}

void sha256(void const *data, unsigned long size, unsigned char *digest)
{
	struct sha256 s;
	sha256_start(&s);
	sha256_add(&s, data, size);
	sha256_end(&s, digest);
}
