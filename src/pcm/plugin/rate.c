/*
 *  Rate conversion Plug-In
 *  Copyright (c) 1999 by Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Library General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
  
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <endian.h>
#include <byteswap.h>
#include "../pcm_local.h"

#define SHIFT	11
#define BITS	(1<<SHIFT)
#define MASK	(BITS-1)

/*
 *  Basic rate conversion plugin
 */
 
struct rate_private_data {
	int src_voices;
	int dst_voices;
	int src_rate;
	int dst_rate;
	unsigned int pitch;
	unsigned int pos;
	signed short last_L_S1, last_R_S1;
	signed short last_L_S2, last_R_S2;
	ssize_t old_src_size, old_dst_size;
};

static void mix_stereo(struct rate_private_data *data,
		       signed short *src_ptr, int src_size,
		       signed short *dst_ptr, int dst_size)
{
	unsigned int pos;
	signed int val;
	signed int L_S1, R_S1, L_S2, R_S2;
	
	pos = data->pos;
	L_S1 = data->last_L_S1;
	R_S1 = data->last_R_S1;
	L_S2 = data->last_L_S2;
	R_S2 = data->last_R_S2;
	if (pos >> SHIFT) {
		src_ptr += ((pos >> SHIFT) - 1) * 2; pos &= MASK;
		L_S1 = L_S2;
		R_S1 = R_S2;
		L_S2 = *src_ptr;
		R_S2 = *(src_ptr + 1);
	}
	while (dst_size-- > 0) {
		if (pos >> SHIFT) {
			src_ptr += (pos >> SHIFT) * 2; pos &= MASK;
			L_S1 = L_S2;
			R_S1 = R_S2;
			L_S2 = *src_ptr;
			R_S2 = *(src_ptr + 1);
		}
		
		val = L_S1 + ((L_S2 - L_S1) * (signed int)pos) / BITS;
		if (val < -32768)
			val = -32768;
		else if (val > 32767)
			val = 32767;
		*dst_ptr++ = val;
		// printf("L_S1 = %i, L_S2 = %i, pos = %i, val = %i\n", L_S1, L_S2, pos, val);
		
		val = R_S1 + ((R_S2 - R_S1) * (signed int)pos) / BITS;
		if (val < -32768)
			val = -32768;
		else if (val > 32767)
			val = 32767;
		*dst_ptr++ = val;
		
		pos += data->pitch;
	}
	data->last_L_S1 = L_S1;
	data->last_R_S1 = R_S1;
	data->last_L_S2 = L_S2;
	data->last_R_S2 = R_S2;
	data->pos = pos;
}

static ssize_t rate_transfer(snd_pcm_plugin_t *plugin,
			     char *src_ptr, size_t src_size,
			     char *dst_ptr, size_t dst_size)
{
	struct rate_private_data *data;

	if (plugin == NULL || src_ptr == NULL || src_size < 0 ||
	                      dst_ptr == NULL || dst_size < 0)
		return -EINVAL;
	if (src_size == 0)
		return 0;
	data = (struct rate_private_data *)snd_pcm_plugin_extra_data(plugin);
	if (data == NULL)
		return -EINVAL;
	if (data->src_voices == 2) {
		mix_stereo(data, (signed short *)src_ptr, src_size / 4,
				 (signed short *)dst_ptr, dst_size / 4);
		return (dst_size / 4) * 4;
	} else {
		return -EINVAL;
	}
}

static int rate_action(snd_pcm_plugin_t *plugin, snd_pcm_plugin_action_t action)
{
	struct rate_private_data *data;

	if (plugin == NULL)
		return -EINVAL;
	data = (struct rate_private_data *)snd_pcm_plugin_extra_data(plugin);
	switch (action) {
	case INIT:
	case PREPARE:
	case DRAIN:
	case FLUSH:
		data->pos = 0;
		data->last_L_S1 = data->last_R_S1 = 0;
		data->last_L_S2 = data->last_R_S2 = 0;
		break;
	}
	return 0;	/* silenty ignore other actions */
}

static ssize_t rate_src_size(snd_pcm_plugin_t *plugin, size_t size)
{
	struct rate_private_data *data;
	ssize_t res;

	if (!plugin || size <= 0)
		return -EINVAL;
	data = (struct rate_private_data *)snd_pcm_plugin_extra_data(plugin);
	res = (((size * data->pitch) + (BITS/2)) >> SHIFT) & ~3;
	if (size < 128*1024) {
		if (data->old_src_size == size)
			return data->old_dst_size;
		data->old_src_size = size;
		data->old_dst_size = res;
	}
	return res;
}

static ssize_t rate_dst_size(snd_pcm_plugin_t *plugin, size_t size)
{
	struct rate_private_data *data;
	ssize_t res;

	if (!plugin || size <= 0)
		return -EINVAL;
	data = (struct rate_private_data *)snd_pcm_plugin_extra_data(plugin);
	res = (((size << SHIFT) + (data->pitch / 2)) / data->pitch) & ~3;
	if (size < 128*1024) {
		if (data->old_dst_size == size)
			return data->old_src_size;
		data->old_dst_size = size;
		data->old_src_size = res;
	}
	return res;
}

int snd_pcm_plugin_build_rate(int src_format, int src_rate, int src_voices,
			      int dst_format, int dst_rate, int dst_voices,
			      snd_pcm_plugin_t **r_plugin)
{
	struct rate_private_data *data;
	snd_pcm_plugin_t *plugin;

	if (!r_plugin)
		return -EINVAL;
	*r_plugin = NULL;
	if (src_voices != 2 || dst_voices != 2)
		return -EINVAL;
	if (src_format != SND_PCM_SFMT_S16_LE ||
	    dst_format != SND_PCM_SFMT_S16_LE)
		return -EINVAL;
	if (src_rate == dst_rate)
		return -EINVAL;
	plugin = snd_pcm_plugin_build("rate format conversion",
				      sizeof(struct rate_private_data));
	if (plugin == NULL)
		return -ENOMEM;
	data = (struct rate_private_data *)snd_pcm_plugin_extra_data(plugin);
	data->src_voices = src_voices;
	data->dst_voices = dst_voices;
	data->src_rate = src_rate;
	data->dst_rate = dst_rate;
	data->pitch = ((src_rate << SHIFT) + (dst_rate >> 1)) / dst_rate;
	data->pos = 0;
	data->last_L_S1 = data->last_R_S1 = 0;
	data->last_L_S2 = data->last_R_S2 = 0;
	data->old_src_size = data->old_dst_size = 0;
	plugin->transfer = rate_transfer;
	plugin->src_size = rate_src_size;
	plugin->dst_size = rate_dst_size;
	plugin->action = rate_action;
	*r_plugin = plugin;
	return 0;
}
