/*
 * Copyright (C) 2016 Mario Cianciolo <mr.udda@gmail.com>
 *
 * This file is part of multiload-ng.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include <config.h>

#include <inttypes.h>
#include <math.h>
#include <ctype.h>
#include <mntent.h>
#include <stdlib.h>

#include "graph-data.h"
#include "autoscaler.h"
#include "info-file.h"
#include "preferences.h"
#include "util.h"


static const char *fstype_ignore_list[] = { "rootfs", "smbfs", "nfs", "cifs", "fuse.", NULL };

/* Cached device list to avoid rescanning mounts on every update */
static GArray *cached_devices = NULL;  /* array of gchar* (device names) */
static gint rescan_counter = 0;  /* trigger rescan every 5 calls */


MultiloadFilter *
multiload_graph_disk_get_filter (LoadGraph *g, DiskData *xd)
{
	char *buf = NULL;
	size_t n = 0;

	guint64 blocks;
	char device[20], prefix[20], label[30];
	guint i;

	MultiloadFilter *filter = multiload_filter_new();

	FILE *f = info_file_required_fopen("/proc/partitions", "r");

	while(getline(&buf, &n, f) >= 0) {
		if (2 != fscanf(f, "%*u %*u %"G_GUINT64_FORMAT" %s", &blocks, device))
			continue;

		// extract block device and partition names
		gboolean is_partition = FALSE;
		g_strlcpy(prefix, device, sizeof(prefix));
		for (i=0; prefix[i] != '\0'; i++) {
			if (isdigit(prefix[i])) {
				prefix[i] = '\0';
				is_partition = TRUE;
				break;
			}
		}

		// generate sysfs path
		char sysfs_path[PATH_MAX];
		if (is_partition)
			g_snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/%s/stat", prefix, device);
		else
			g_snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/stat", device);

		if (access(sysfs_path, R_OK) != 0)
			continue;

		gchar *size = format_size_for_display(blocks*1024, g->multiload->size_format_iec);
		g_snprintf(label, sizeof(label), "%s (%s)", device, size);
		g_free(size);

		multiload_filter_append_with_label(filter, device, label);
	}

	g_free(buf);
	fclose(f);

	multiload_filter_import_existing(filter, g->config->filter);

	return filter;
}


void
multiload_graph_disk_get_data (int Maximum, int data [3], LoadGraph *g, DiskData *xd, gboolean first_call)
{
	FILE *f_mntent;
	FILE *f_stat;
	struct mntent *mnt;

	guint i;
	int max;

	char sysfs_path[PATH_MAX];
	char *device;
	char prefix[20];
guint64 read, write;
	guint64 read_total = 0, write_total = 0;
	guint64 readdiff, writediff;


	xd->partitions[0] = '\0';

	/* Rescan mounts every 5 calls to detect new/removed devices, but still read stats every call */
	if (rescan_counter++ % 5 == 0) {
		/* Free the old cache */
		if (cached_devices != NULL) {
			for (i = 0; i < cached_devices->len; i++) {
				gchar *dev = g_array_index(cached_devices, gchar*, i);
				g_free(dev);
			}
			g_array_free(cached_devices, TRUE);
		}

		/* Build a new device cache from current mounts */
		cached_devices = g_array_new(TRUE, FALSE, sizeof(gchar*));

		if ((f_mntent = setmntent(MOUNTED, "r")) != NULL) {
			while ((mnt = getmntent(f_mntent)) != NULL) {
				/* skip filesystems that do not have a block device */
				if (strncmp (mnt->mnt_fsname, "/dev/", 5) != 0)
					continue;

				/* skip filesystems of certain types, defined in fstype_ignore_list[] */
				gboolean ignore = FALSE;
				for (i=0; fstype_ignore_list[i] != NULL; i++) {
					if (strncmp (mnt->mnt_type, fstype_ignore_list[i], strlen(fstype_ignore_list[i])) == 0) {
						ignore = TRUE;
						break;
					}
				}
				if (ignore)
					continue;

				/* extract block device and partition names */
			gboolean is_partition = FALSE;
			device = &mnt->mnt_fsname[5];
			g_strlcpy(prefix, device, sizeof(prefix));

			/* NVMe partitions: nvme0n1p2 — 'p' before trailing digits */
			if (strncmp(device, "nvme", 4) == 0) {
				char *p = strstr(device, "p");
				if (p && isdigit(p[1])) {
					g_strlcpy(prefix, device, p - device + 1);
					is_partition = TRUE;
				}
			}

			/* Traditional partitions (sdX, hdX, vdX, etc.): first digit marks partition */
			if (!is_partition) {
				for (i=0; prefix[i] != '\0'; i++) {
					if (isdigit(prefix[i])) {
						prefix[i] = '\0';
						is_partition = TRUE;
						break;
					}
				}
			}

			/* apply user filter */
			if (g->config->filter_enable) {
					MultiloadFilter *filter = multiload_filter_new_from_existing(g->config->filter);
					for (i=0, ignore=TRUE; i<multiload_filter_get_length(filter); i++) {
						if (strcmp(multiload_filter_get_element_data(filter,i), device) == 0) {
							ignore = FALSE;
							break;
						}
					}
					if (ignore)
						g_debug("[graph-disk] Ignored device '%s' due to user filter", device);
					multiload_filter_free(filter);
				}
				if (ignore)
					continue;

				/* generate sysfs path and verify it exists */
				if (is_partition)
					g_snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/%s/stat", prefix, device);
				else
					g_snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/stat", device);

				if (access(sysfs_path, R_OK) != 0)
					continue;

				/* Add this device to the cache */
				gchar *dup = g_strdup(device);
				g_array_append_val(cached_devices, dup);
			}
			endmntent(f_mntent);
		}
	}

	/* Now read stat files for all cached devices (this happens every update) */
	if (cached_devices != NULL) {
		for (i = 0; i < cached_devices->len; i++) {
			device = g_array_index(cached_devices, gchar*, i);

			/* reconstruct the sysfs path */
			g_strlcpy(prefix, device, sizeof(prefix));
			gboolean is_partition = FALSE;

			/* NVMe partitions: nvme0n1p2 — 'p' before trailing digits */
			if (strncmp(device, "nvme", 4) == 0) {
				char *p = strstr(device, "p");
				if (p && isdigit(p[1])) {
					g_strlcpy(prefix, device, p - device + 1);
					is_partition = TRUE;
				}
			}

			/* Traditional partitions (sdX, hdX, vdX, etc.): first digit marks partition */
			if (!is_partition) {
				for (guint k = 0; prefix[k] != '\0'; k++) {
					if (isdigit(prefix[k])) {
						prefix[k] = '\0';
						is_partition = TRUE;
						break;
					}
				}
			}

			if (is_partition)
				g_snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/%s/stat", prefix, device);
			else
				g_snprintf(sysfs_path, PATH_MAX, "/sys/block/%s/stat", device);

			/* read data from sysfs */
			f_stat = fopen(sysfs_path, "r");
			if (f_stat == NULL)
				continue;
			int result = fscanf(f_stat, "%*u %*u %"SCNu64" %*u %*u %*u %"SCNu64" %*u", &read, &write);
			fclose(f_stat);
			if (result != 2)
				continue;

			/* data gathered - add to totals */
			read_total += read;
			write_total += write;

			g_strlcat (xd->partitions, device, sizeof(xd->partitions));
			g_strlcat (xd->partitions, ", ", sizeof(xd->partitions));
		}
	}

	if (strlen(xd->partitions) >= 2)
		xd->partitions[strlen(xd->partitions)-2] = 0;

	readdiff  = read_total  - xd->last_read;
	writediff = write_total - xd->last_write;

	xd->last_read  = read_total;
	xd->last_write = write_total;

	if (G_LIKELY(!first_call)) { // cannot calculate diff on first call
		max = autoscaler_get_max(&xd->scaler, g, readdiff + writediff);

		if (max == 0) {
			data[0] = readdiff  > 0 ? Maximum : 0;
			data[1] = writediff > 0 ? Maximum : 0;
		} else {
			data[0] = (float)Maximum *  readdiff / (float)max;
			data[1] = (float)Maximum * writediff / (float)max;
		}

		// read/write are relative to standard linux sectors (512 bytes, fixed)
		xd->read_speed  = calculate_speed(readdiff  * 512, g->config->interval);
		xd->write_speed = calculate_speed(writediff * 512, g->config->interval);
	}
}


void
multiload_graph_disk_cmdline_output (LoadGraph *g, DiskData *xd)
{
	g_snprintf(g->output_str[0], sizeof(g->output_str[0]), "%"G_GUINT64_FORMAT, xd->read_speed);
	g_snprintf(g->output_str[1], sizeof(g->output_str[1]), "%"G_GUINT64_FORMAT, xd->write_speed);
}


void
multiload_graph_disk_tooltip_update (char *buf_title, size_t len_title, char *buf_text, size_t len_text, LoadGraph *g, DiskData *xd, gint style)
{
	gchar *disk_read = format_rate_for_display(xd->read_speed, g->multiload->size_format_iec);
	gchar *disk_write = format_rate_for_display(xd->write_speed, g->multiload->size_format_iec);

	if (style == MULTILOAD_TOOLTIP_STYLE_DETAILED) {
		g_snprintf(buf_text, len_text, _(	"Monitored partitions: %s\n"
											"\n"
											"Read: %s\n"
											"Write: %s"),
											xd->partitions, disk_read, disk_write);
	} else {
		g_snprintf(buf_text, len_text, "\xe2\xac\x86%s \xe2\xac\x87%s", disk_read, disk_write);
	}
	g_free(disk_read);
	g_free(disk_write);
}
