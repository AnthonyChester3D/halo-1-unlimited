/* oxbox_perf.h: the hooks of the developers' performance and memory log (oxbox: port/oxbox/oxbox_perf.c).

The log's code, data and buffers are in an XBE section of their own (OXPERF), which isn't loaded at boot. With
perf_log = 1 in opence.ini, start-up loads it (oxbox_sdk.c oxbox_perf_load) and points oxbox_perf at its hooks;
otherwise oxbox_perf stays NULL and each hook site is a test of it. */

#ifndef OXBOX_PERF_H
#define OXBOX_PERF_H

struct oxbox_perf_hooks
{
	/* once a frame, at its end (main_loop): the frame's probe marks (main.c's frame_probe_marks, LARGE_INTEGERs:
	start, input, network, menus, network end, ticks, camera, drawing, gpu wait, present) */
	void (*frame)(void const *marks, long mark_count);
};

extern struct oxbox_perf_hooks *oxbox_perf;

#endif
