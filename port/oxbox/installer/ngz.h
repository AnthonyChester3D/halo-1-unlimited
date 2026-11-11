/* ngz.h: zlib-ng's deflate and inflate (port/third_party/zlib-ng, its native zng_ API) behind a small wall: its header and
zlib's can't both be in one file. The flush and status numbers are zlib's (Z_NO_FLUSH, Z_FINISH; Z_OK, Z_STREAM_END...) */
#ifndef OXBOX_NGZ_H
#define OXBOX_NGZ_H

#include <stdint.h>

struct ngz;
struct ngz *ngz_deflate(int level);             /* windowBits 15, memLevel 8, a zlib stream; NULL if it can't */
struct ngz *ngz_inflate(void);
/* one deflate (or inflate) call: *in and *in_size, *out and *out_size go on past what it took and made; zlib's status */
int ngz_run(struct ngz *z, int flush, unsigned char const **in, uint32_t *in_size, unsigned char **out, uint32_t *out_size);
void ngz_end(struct ngz *z);

#endif
