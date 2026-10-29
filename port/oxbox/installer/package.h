/* package.h: an OpenCE package (port/oxbox/tools/mkpackage.py has its format): how to make a build of OpenCE from
the player's own retail Halo XBE, and who signed it (port/oxbox/tools/oxsign.py) */
#ifndef OXBOX_PACKAGE_H
#define OXBOX_PACKAGE_H

#define PACKAGE_MAX_XBES 8
#define PACKAGE_MAX_SIGNATURES 4
#define PACKAGE_MAX_MANIFEST 1024

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
	int format;
	unsigned long release, network; /* (format 2) */
	char manifest[PACKAGE_MAX_MANIFEST + 1];
	unsigned char *body;
	unsigned long body_size;
	int count;
	struct package_xbe xbes[PACKAGE_MAX_XBES];
	/* what's signed (the header and the manifest), and the signatures: each a key's id and an Ed25519 signature */
	unsigned char *signed_part;
	unsigned long signed_size;
	int signatures;
	unsigned char signature_ids[PACKAGE_MAX_SIGNATURES][8], signature[PACKAGE_MAX_SIGNATURES][64];
	/* the installer it carries (tools/mkpackage.py --installer): its version (0: none), and where it is in the body
	(checked against the manifest's SHA-256 by package_load) */
	unsigned long installer_version, installer_offset, installer_size;
	/* the stock maps' showcase files it carries (mkpackage.py --showcase): how many, and their section in the body
	(checked by package_load; read with package_showcase_file) */
	unsigned long showcase_count, showcase_offset, showcase_size;
};

/* a public key the installer trusts (keys.txt, made into keys.h by tools/mkkeys.py) */
struct package_key
{
	char const *name;
	unsigned char id[8], key[32];
};

enum
{
	PACKAGE_UNSIGNED,    /* no signatures (or format 1) */
	PACKAGE_SIGNED,      /* signed by a key it trusts */
	PACKAGE_UNKNOWN_KEY, /* signed, but by no key it trusts */
	PACKAGE_BAD          /* a signature by a key it trusts doesn't match: changed after it was signed */
};

/* reads a package and checks it whole (its hash, and every step's sizes); 0 when it's good, else an error's text */
int package_load(char const *path, struct package *package, char *error);
void package_free(struct package *package);

/* the same package (its header and manifest: the body's SHA-256 among them) */
int package_same(struct package const *a, struct package const *b);

/* the n-th showcase file (n < showcase_count): its name ("bloodgulch.txt": checked safe), its bytes and size */
void package_showcase_file(struct package const *package, unsigned long n, char *name, unsigned char const **data,
	unsigned long *size);

/* a manifest line's value ("date", "notes"...), up to size bytes; empty when it has none */
void package_manifest_value(struct package const *package, char const *key, char *value, int size);

/* who signed it: PACKAGE_SIGNED (and *signer, the key's name), or why not */
int package_verify(struct package const *package, struct package_key const *keys, int count, char const **signer);

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
