/* download.h: the installer's downloads (download.c): the network started from the dashboard's settings, then a file
fetched over HTTPS (or HTTP), following redirects, as GitHub sends a release's file to its file host.

The server's certificate isn't checked: the installer has no certificate authorities to check it against, and no
clock it can trust. What's downloaded is trusted only for its signature instead (package.c: a package signed by a key
in keys.txt), and nothing secret is sent. */
#ifndef OXBOX_DOWNLOAD_H
#define OXBOX_DOWNLOAD_H

/* where updates come from: the project's latest release's package (installer_url.txt beside the installer, its first
line, names another: https:// or http://) */
#define DOWNLOAD_DEFAULT_URL "https://github.com/AnthonyChester3D/halo-1-unlimited/releases/latest/download/opence.oxpk"

/* what download.c tells as it goes: a step (text), or how much is in so far (total 0: not known) */
struct download_report
{
	void (*step)(void *context, char const *text);
	void (*progress)(void *context, unsigned long done, unsigned long total);
	void *context;
};

/* the network, started once (later calls return what the first did): 0 when the Xbox has an address (address: it,
as text), else why not in error (256 bytes) */
int download_network(struct download_report const *report, char *address, char *error);

/* fetches url, each part of its body to put() as it comes (nonzero: stop), at most `most` bytes; 0 when it's all
in (*size: how much), else why not in error (256 bytes) */
typedef int (*download_write_t)(void *context, unsigned char const *data, unsigned long size);
int download(char const *url, unsigned long most, download_write_t put, void *write_context,
	struct download_report const *report, unsigned long *size, char *error);

#endif
