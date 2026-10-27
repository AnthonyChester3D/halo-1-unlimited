/* sha256.h: SHA-256 */
#ifndef OXBOX_SHA256_H
#define OXBOX_SHA256_H

struct sha256
{
	unsigned long h[8];
	unsigned long long length;
	unsigned char buffer[64];
	unsigned long used;
};

void sha256_start(struct sha256 *s);
void sha256_add(struct sha256 *s, void const *data, unsigned long size);
void sha256_add_zeros(struct sha256 *s, unsigned long size);
void sha256_end(struct sha256 *s, unsigned char *digest);
void sha256(void const *data, unsigned long size, unsigned char *digest);

#endif
