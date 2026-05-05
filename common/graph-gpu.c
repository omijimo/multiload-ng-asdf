/*
 * Copyright (C) 2026 GitHub Copilot
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

#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "graph-data.h"
#include "info-file.h"
#include "multiload-config.h"
#include "preferences.h"
#include "util.h"


typedef struct {
	gchar name[80];
	gchar busy_path[PATH_MAX];
	double busy;
} GpuSourceData;

static GpuSourceData *sources_list = NULL;

static gboolean
list_gpu_amdgpu (GpuSourceData **list, gboolean init)
{
	static const char *drm_root = "/sys/class/drm";

	DIR *dir;
	struct dirent *dirent;
	char path[PATH_MAX];
	char device_path[PATH_MAX];
	char resolved[PATH_MAX];
	char driver[64];
	char label[80];
	const char *base;
	guint count = 0;
	guint i;

	dir = opendir(drm_root);
	if (dir == NULL)
		return FALSE;

	if (init) {
		/* First pass: count valid amdgpu cards with gpu_busy_percent */
		while ((dirent = readdir(dir)) != NULL) {
			/* Only process cardN entries */
			if (strncmp(dirent->d_name, "card", 4) != 0)
				continue;
			if (!isdigit(dirent->d_name[4]))
				continue;

			/* Check for gpu_busy_percent file */
			g_snprintf(path, sizeof(path), "%s/%s/device/gpu_busy_percent", drm_root, dirent->d_name);
			if (access(path, R_OK) != 0) {
				g_debug("[graph-gpu] %s: no gpu_busy_percent", dirent->d_name);
				continue;
			}

			/* Verify the device driver is amdgpu */
			g_snprintf(device_path, sizeof(device_path), "%s/%s/device/driver", drm_root, dirent->d_name);
			ssize_t driver_len = readlink(device_path, resolved, sizeof(resolved) - 1);
			if (driver_len <= 0) {
				g_debug("[graph-gpu] %s: readlink(driver) failed", dirent->d_name);
				continue;
			}
			resolved[driver_len] = '\0';
			base = strrchr(resolved, '/');
			if (!base || strcmp(base + 1, "amdgpu") != 0) {
				g_debug("[graph-gpu] %s: driver='%s' (expected amdgpu)", dirent->d_name, base ? base+1 : "(null)");
				continue;
			}
			g_strlcpy(driver, base + 1, sizeof(driver));

			g_debug("[graph-gpu] found amdgpu: %s", dirent->d_name);
			count++;
		}

		if (count == 0) {
			closedir(dir);
			return FALSE;
		}

		*list = g_new0(GpuSourceData, count + 1);

		/* Second pass: populate the list */
		rewinddir(dir);
		i = 0;
		while ((dirent = readdir(dir)) != NULL) {
			if (strncmp(dirent->d_name, "card", 4) != 0)
				continue;
			if (!isdigit(dirent->d_name[4]))
				continue;

			g_snprintf(path, sizeof(path), "%s/%s/device/gpu_busy_percent", drm_root, dirent->d_name);
			if (access(path, R_OK) != 0)
				continue;

			g_snprintf(device_path, sizeof(device_path), "%s/%s/device/driver", drm_root, dirent->d_name);
			ssize_t driver_len = readlink(device_path, resolved, sizeof(resolved) - 1);
			if (driver_len <= 0)
				continue;
			resolved[driver_len] = '\0';
			base = strrchr(resolved, '/');
			if (!base || strcmp(base + 1, "amdgpu") != 0)
				continue;
			g_strlcpy(driver, base + 1, sizeof(driver));

			GpuSourceData *li = &(*list)[i];
			g_strlcpy(li->busy_path, path, sizeof(li->busy_path));

			/* Build a human-readable label from the resolved device path */
			g_snprintf(device_path, sizeof(device_path), "%s/%s/device", drm_root, dirent->d_name);
			if (realpath(device_path, resolved) != NULL) {
				base = strrchr(resolved, '/');
				if (base != NULL && base[1] != '\0')
					g_snprintf(label, sizeof(label), "%s (%s)", base + 1, driver);
				else
					g_snprintf(label, sizeof(label), "%s", driver);
			} else {
				g_snprintf(label, sizeof(label), "%s", driver);
			}

			g_strlcpy(li->name, label, sizeof(li->name));
			i++;
		}

		closedir(dir);
		return TRUE;
	}

	/* Non-init: just refresh busy values */
	if (*list == NULL) {
		g_debug("[graph-gpu] sources_list is NULL in non-init mode");
		return FALSE;
	}

	for (i = 0; (*list)[i].busy_path[0] != '\0'; i++) {
		gdouble val = 0;
		if (info_file_read_double((*list)[i].busy_path, &val, 1.0)) {
			(*list)[i].busy = val;
			g_debug("[graph-gpu] %s: busy=%.1f", (*list)[i].name, val);
		} else {
			g_debug("[graph-gpu] %s: failed to read busy from %s", (*list)[i].name, (*list)[i].busy_path);
		}
	}

	return TRUE;
}


void
multiload_graph_gpu_init (LoadGraph *g, GpuData *xd)
{
	xd->name[0] = '\0';
	xd->busy = 0.0;

	if (list_gpu_amdgpu(&sources_list, TRUE)) {
		g->multiload->graph_config[GRAPH_AMDGPU].visible = TRUE;
		g_debug("[graph-gpu] amdgpu busy-percent sources detected");
	} else {
		g->multiload->graph_config[GRAPH_AMDGPU].visible = FALSE;
		g_debug("[graph-gpu] No amdgpu busy-percent sources detected");
	}
}


MultiloadFilter *
multiload_graph_gpu_get_filter (LoadGraph *g, GpuData *xd)
{
	GpuSourceData *list = NULL;
	guint i;

	MultiloadFilter *filter = multiload_filter_new();

	if (list_gpu_amdgpu(&list, TRUE)) {
		for (i = 0; list[i].busy_path[0] != '\0'; i++)
			multiload_filter_append(filter, list[i].name);

		multiload_filter_import_existing(filter, g->config->filter);
	}

	g_free(list);
	return filter;
}


void
multiload_graph_gpu_get_data (int Maximum, int data[1], LoadGraph *g, GpuData *xd, gboolean first_call)
{
	GpuSourceData *use = NULL;
	guint i, m;
	int max = 100;

	if (!list_gpu_amdgpu(&sources_list, FALSE)) {
		g_debug("[graph-gpu] list_gpu_amdgpu returned FALSE in get_data");
		data[0] = 0;
		xd->name[0] = '\0';
		xd->busy = 0.0;
		return;
	}

	if (g->config->filter_enable && g->config->filter[0] != '\0') {
		g_debug("[graph-gpu] filter enabled: '%s'", g->config->filter);
		for (i = 0; sources_list[i].busy_path[0] != '\0'; i++) {
			if (strcmp(sources_list[i].name, g->config->filter) == 0) {
				use = &sources_list[i];
				g_debug("[graph-gpu] Using source '%s' (selected by filter)", sources_list[i].name);
				break;
			}
		}
	}

	if (use == NULL) {
		g_debug("[graph-gpu] no filter match, picking busiest GPU");
		for (i = 1, m = 0; sources_list[i].busy_path[0] != '\0'; i++) {
			if (sources_list[i].busy > sources_list[m].busy)
				m = i;
		}
		use = &sources_list[m];
	}

	data[0] = rint (Maximum * (float)use->busy / (float)max);
	g_strlcpy(xd->name, use->name, sizeof(xd->name));
	xd->busy = use->busy;
	g_debug("[graph-gpu] result: name='%s' busy=%.1f data[0]=%d", use->name, use->busy, data[0]);
}


void
multiload_graph_gpu_cmdline_output (LoadGraph *g, GpuData *xd)
{
	g_snprintf(g->output_str[0], sizeof(g->output_str[0]), "%.03f", xd->busy);
}


void
multiload_graph_gpu_tooltip_update (char *buf_title, size_t len_title, char *buf_text, size_t len_text, LoadGraph *g, GpuData *xd, gint style)
{
	if (style == MULTILOAD_TOOLTIP_STYLE_DETAILED) {
		g_snprintf(buf_title, len_title, "%s", xd->name[0] != '\0' ? xd->name : graph_types[g->id].label);
		g_snprintf(buf_text, len_text, "%.1f%%", xd->busy);
	} else {
		g_snprintf(buf_text, len_text, "%.1f%%", xd->busy);
	}
}
