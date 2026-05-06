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

#include <ctype.h>
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
	gchar label[80];
	gchar power_path[PATH_MAX];
	gdouble power_uw;  /* power in microwatts */
	gdouble power_max_uw;  /* max/cap power in microwatts, 0 if not available */
} PowerSourceData;

static PowerSourceData *sources_list = NULL;

static gboolean
list_power_hwmon (PowerSourceData **list, gboolean init)
{
	static const char *hwmon_root = "/sys/class/hwmon";

	DIR *dir;
	struct dirent *dirent;
	char path[PATH_MAX];
	char name[64];
	guint count = 0;
	guint i;

	dir = opendir(hwmon_root);
	if (dir == NULL)
		return FALSE;

	if (init) {
		/* First pass: count valid hwmon devices with power*_average */
		while ((dirent = readdir(dir)) != NULL) {
			/* Only process hwmonN entries */
			if (strncmp(dirent->d_name, "hwmon", 5) != 0)
				continue;
			if (!isdigit(dirent->d_name[5]))
				continue;

			/* Check for power*_average file */
			g_snprintf(path, sizeof(path), "%s/%s/power1_average", hwmon_root, dirent->d_name);
			if (access(path, R_OK) != 0) {
				g_debug("[graph-power] %s: no power1_average", dirent->d_name);
				continue;
			}

			g_debug("[graph-power] found hwmon with power: %s", dirent->d_name);
			count++;
		}

		if (count == 0) {
			closedir(dir);
			return FALSE;
		}

		*list = g_new0(PowerSourceData, count + 1);

		/* Second pass: populate the list */
		rewinddir(dir);
		i = 0;
		while ((dirent = readdir(dir)) != NULL) {
			if (strncmp(dirent->d_name, "hwmon", 5) != 0)
				continue;
			if (!isdigit(dirent->d_name[5]))
				continue;

			g_snprintf(path, sizeof(path), "%s/%s/power1_average", hwmon_root, dirent->d_name);
			if (access(path, R_OK) != 0)
				continue;

			PowerSourceData *li = &(*list)[i];
			g_strlcpy(li->power_path, path, sizeof(li->power_path));

			/* Read device name */
			g_snprintf(path, sizeof(path), "%s/%s/name", hwmon_root, dirent->d_name);
			if (info_file_read_string_s(path, name, sizeof(name), NULL) && name[0] != '\0') {
				/* Read power label if available */
				g_snprintf(path, sizeof(path), "%s/%s/power1_label", hwmon_root, dirent->d_name);
				gchar label[64];
				if (info_file_read_string_s(path, label, sizeof(label), NULL) && label[0] != '\0') {
					g_snprintf(li->name, sizeof(li->name), "%s (%s)", label, name);
				} else {
					g_snprintf(li->name, sizeof(li->name), "%s", name);
				}
			} else {
				g_snprintf(li->name, sizeof(li->name), "%s", dirent->d_name);
			}

			g_strlcpy(li->label, li->name, sizeof(li->label));

			/* Try to read power limit/cap (power1_max, power1_cap, etc.) */
			li->power_max_uw = 0;  /* default: no limit found */
			const char *limit_files[] = { "power1_max", "power1_cap", NULL };
			for (int j = 0; limit_files[j] != NULL; j++) {
				g_snprintf(path, sizeof(path), "%s/%s/%s", hwmon_root, dirent->d_name, limit_files[j]);
				guint64 limit_val = 0;
				if (info_file_read_uint64(path, &limit_val) && limit_val > 0) {
					li->power_max_uw = (gdouble)limit_val;
					g_debug("[graph-power] %s: detected limit from %s: %.0f W", li->name, limit_files[j], limit_val / 1e6);
					break;
				}
			}
			i++;
		}

		closedir(dir);
		return TRUE;
	}

	/* Non-init: just refresh power values */
	if (*list == NULL) {
		g_debug("[graph-power] sources_list is NULL in non-init mode");
		return FALSE;
	}

	for (i = 0; (*list)[i].power_path[0] != '\0'; i++) {
		guint64 val = 0;
		if (info_file_read_uint64((*list)[i].power_path, &val)) {
			(*list)[i].power_uw = (gdouble)val;
			g_debug("[graph-power] %s: power=%.3f W", (*list)[i].name, val / 1e6);
		} else {
			g_debug("[graph-power] %s: failed to read power from %s", (*list)[i].name, (*list)[i].power_path);
		}
	}

	return TRUE;
}


void
multiload_graph_power_init (LoadGraph *g, PowerData *xd)
{
	xd->name[0] = '\0';
	xd->power_w = 0.0;

	if (list_power_hwmon(&sources_list, TRUE)) {
		g->multiload->graph_config[GRAPH_POWER].visible = TRUE;
		g_debug("[graph-power] hwmon power sources detected");
	} else {
		g->multiload->graph_config[GRAPH_POWER].visible = FALSE;
		g_debug("[graph-power] No hwmon power sources detected");
	}
}


MultiloadFilter *
multiload_graph_power_get_filter (LoadGraph *g, PowerData *xd)
{
	PowerSourceData *list = NULL;
	guint i;

	MultiloadFilter *filter = multiload_filter_new();

	if (list_power_hwmon(&list, TRUE)) {
		for (i = 0; list[i].power_path[0] != '\0'; i++)
			multiload_filter_append(filter, list[i].name);

		multiload_filter_import_existing(filter, g->config->filter);
	}

	g_free(list);
	return filter;
}


void
multiload_graph_power_get_data (int Maximum, int data[1], LoadGraph *g, PowerData *xd, gboolean first_call)
{
	PowerSourceData *use = NULL;
	guint i;
	gdouble max_w;

	/* Use configured scaler_max if set; otherwise use autoscaling */
	if (graph_types[g->id].scaler_max > 0) {
		max_w = (gdouble)graph_types[g->id].scaler_max;
	} else {
		max_w = 1000.0;  /* default max 1000W for auto-scaling */
	}

	if (!list_power_hwmon(&sources_list, FALSE)) {
		g_debug("[graph-power] list_power_hwmon returned FALSE in get_data");
		data[0] = 0;
		xd->name[0] = '\0';
		xd->power_w = 0.0;
		return;
	}

	if (g->config->filter_enable && g->config->filter[0] != '\0') {
		g_debug("[graph-power] filter enabled: '%s'", g->config->filter);
		for (i = 0; sources_list[i].power_path[0] != '\0'; i++) {
			if (strcmp(sources_list[i].name, g->config->filter) == 0) {
				use = &sources_list[i];
				g_debug("[graph-power] Using source '%s' (selected by filter)", sources_list[i].name);
				break;
			}
		}
	}

	if (use == NULL) {
		/* Pick the source with highest power draw */
		g_debug("[graph-power] no filter match, picking highest power source");
		for (i = 1; sources_list[i].power_path[0] != '\0'; i++) {
			if (use == NULL || sources_list[i].power_uw > use->power_uw)
				use = &sources_list[i];
		}
		if (!use)
			use = &sources_list[0];
	}

	/* Convert microwatts to watts */
	gdouble power_w = use->power_uw / 1e6;

	/* Try to use detected hardware power limit if available and no fixed max configured */
	if (graph_types[g->id].scaler_max <= 0) {
		if (use->power_max_uw > 0) {
			/* Use detected limit */
			max_w = use->power_max_uw / 1e6;
			g_debug("[graph-power] using detected limit: %.0f W", max_w);
		} else if (power_w > max_w * 0.5) {
			/* Fall back to auto-scaling based on observed values */
			max_w = ceil(power_w / 100.0) * 100.0;
			if (max_w < 500.0)
				max_w = 500.0;
		}
	}

	data[0] = rint (Maximum * (float)power_w / (float)max_w);
	g_strlcpy(xd->name, use->name, sizeof(xd->name));
	xd->power_w = power_w;
	g_debug("[graph-power] result: name='%s' power=%.3f W data[0]=%d", use->name, power_w, data[0]);
}


void
multiload_graph_power_cmdline_output (LoadGraph *g, PowerData *xd)
{
	g_snprintf(g->output_str[0], sizeof(g->output_str[0]), "%.3f", xd->power_w);
}


void
multiload_graph_power_tooltip_update (char *buf_title, size_t len_title, char *buf_text, size_t len_text, LoadGraph *g, PowerData *xd, gint style)
{
	if (style == MULTILOAD_TOOLTIP_STYLE_DETAILED) {
		g_snprintf(buf_title, len_title, "%s", xd->name[0] != '\0' ? xd->name : graph_types[g->id].label);
		g_snprintf(buf_text, len_text, "%.3f W", xd->power_w);
	} else {
		g_snprintf(buf_text, len_text, "%.3f W", xd->power_w);
	}
}
