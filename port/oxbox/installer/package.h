/* package.h: an OpenCE package (port/oxbox/tools/mkpackage.py has its format): how to make a build of OpenCE from
the player's own clean Halo XBE */
#ifndef OXBOX_PACKAGE_H
#define OXBOX_PACKAGE_H

#define PACKAGE_MAX_XBES 8

/* one build it makes, from one retail XBE */
struct package_xbe
{
	unsigned char retail_sha256[32];
	unsigned long retail_size;
	unsigned long build_size;
	unsigned char build_sha256[32]; /* (its pictures as zeros) */
	unsigned long steps, steps_size;
	char name[33];
};

struct package
{
	char name[65];
	unsigned char *body;
	unsigned long body_size;
	int count;
	struct package_xbe xbes[PACKAGE_MAX_XBES];
};

/* reads a package and checks it whole (its hash, and every step's sizes); 0 when it's good, else an error's text */
int package_load(char const *path, struct package *package, char *error);
void package_free(struct package *package);

/* the build made from the retail XBE with this SHA-256, or NULL */
struct package_xbe const *package_find(struct package const *package, unsigned char const *retail_sha256);

/* the certificate's allowed media and regions as the retail discs' XBEs have them. A copy patched to run from the
hard disk or in any region (a common change, which many tools make) is matched as the disc's: the build is made from
the disc's bytes, and the copy itself is what's kept and put back */
#define PACKAGE_DISC_MEDIA 0x00000002ul
#define PACKAGE_DISC_REGIONS 0x00000001ul
struct package_patch
{
	int patched;
	unsigned long cert, media, regions;
};

/* the build for this retail XBE (whole, in memory), as it is or patched (patch->patched: then data is left as the
disc's, and package_unpatch puts the copy's own media and regions back), or NULL */
struct package_xbe const *package_match(struct package const *package, unsigned char *data, unsigned long size,
	struct package_patch *patch);
void package_unpatch(unsigned char *data, struct package_patch const *patch);
void package_repatch(unsigned char *data, struct package_patch const *patch);

/* makes the build from the retail XBE (whole, in memory), its pictures from pictures.h's (NULL: blank), each part to
write() as it's made (nonzero: stop); 0 when it's made and its SHA-256 (pictures as zeros) is the package's.
Every step was checked by package_load */
typedef int (*package_write_t)(void *context, unsigned char const *data, unsigned long size);
int package_make(struct package const *package, struct package_xbe const *xbe, unsigned char const *retail,
	unsigned char const *pictures, unsigned char const *header, package_write_t write, void *context, char *error);

/* the parts of a build that are pictures (made on the Xbox, so not in its hash): count of them, each's offset, size */
int package_pictures(struct package const *package, struct package_xbe const *xbe, unsigned long *offsets,
	unsigned long *sizes, int most);

#endif
