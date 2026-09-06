/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / LC3 decoder filter, based on Google's liblc3.
 *
 *  LC3 is the codec of Bluetooth LE Audio. It has no container of its own; the
 *  .lc3 file this filter reads is the bitstream format of the reference tools
 *  (elc3/dlc3): an 18-byte header carrying the frame duration, sampling rate,
 *  channel count and total sample count, then one length-prefixed block per
 *  frame holding every channel's data back to back.
 *
 *  The whole file is decoded in one pass, which is what the format invites -
 *  frames are fixed-duration and the header already says how many samples the
 *  result must hold.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <lc3.h>

#define LC3_MAX_CHANNELS 2

typedef struct
{
	GF_FilterPid *ipid, *opid;
	u32 sample_rate, nb_chan;
} GF_LC3DecCtx;

static u32 lc3_rd16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }

static GF_Err lc3dec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_LC3DecCtx *ctx = (GF_LC3DecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	/* Corrected in process() from the file header; GPAC resolves the graph
	 * from these before any data flows. 48 kHz stereo is LE Audio's usual. */
	ctx->sample_rate = 48000;
	ctx->nb_chan = 2;

	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(ctx->sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ctx->sample_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(ctx->nb_chan));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT,
	                           &PROP_LONGUINT(GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT));

	return GF_OK;
}

static GF_Err lc3dec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, hdr_size, pos, ich;
	u32 frame_us, srate, nchan, nsamples;
	int frame_samples, delay, encode_samples, produced = 0;
	lc3_decoder_t dec[LC3_MAX_CHANNELS] = {NULL, NULL};
	void *mem[LC3_MAX_CHANNELS] = {NULL, NULL};
	s16 *frame_pcm = NULL;
	GF_Err e = GF_OK;
	GF_LC3DecCtx *ctx = (GF_LC3DecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data || (size < 18))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[LC3Dec] File too short to hold an LC3 header\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	/* header: file_id, header_size, srate/100, bitrate/100, channels,
	 * frame duration in 10 us units, error-protection mode, sample count */
	if (lc3_rd16(data) != (0x1C | (0xCC << 8)))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[LC3Dec] Not an LC3 bitstream file\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}
	hdr_size = lc3_rd16(data + 2);
	srate = lc3_rd16(data + 4) * 100;
	nchan = lc3_rd16(data + 8);
	frame_us = lc3_rd16(data + 10) * 10;
	nsamples = lc3_rd16(data + 14) | (lc3_rd16(data + 16) << 16);

	if (lc3_rd16(data + 12) != 0)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[LC3Dec] Error-protected LC3 streams are not handled\n"));
		return GF_NOT_SUPPORTED;
	}
	if (!nchan || (nchan > LC3_MAX_CHANNELS) || !srate || !nsamples || (hdr_size < 18) || (hdr_size > size))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[LC3Dec] Unsupported LC3 stream: %u channels, %u Hz\n", nchan, srate));
		return GF_NOT_SUPPORTED;
	}

	frame_samples = lc3_frame_samples((int)frame_us, (int)srate);
	delay = lc3_delay_samples((int)frame_us, (int)srate);
	if ((frame_samples <= 0) || (delay < 0))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[LC3Dec] Unsupported frame duration %u us at %u Hz\n", frame_us, srate));
		return GF_NOT_SUPPORTED;
	}
	/* The encoder's algorithmic delay sits in front of the signal; dlc3 drops
	 * exactly that many samples, and so does this filter. */
	encode_samples = (int)nsamples + delay;

	if ((srate != ctx->sample_rate) || (nchan != ctx->nb_chan))
	{
		ctx->sample_rate = srate;
		ctx->nb_chan = nchan;
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(srate));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(srate));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(nchan));
		gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT,
		                           &PROP_LONGUINT((nchan == 2)
		                                              ? (GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT)
		                                              : GF_AUDIO_CH_FRONT_CENTER));
	}

	for (ich = 0; ich < nchan; ich++)
	{
		mem[ich] = gf_malloc(lc3_decoder_size((int)frame_us, (int)srate));
		if (!mem[ich])
		{
			e = GF_OUT_OF_MEM;
			goto cleanup;
		}
		dec[ich] = lc3_setup_decoder((int)frame_us, (int)srate, (int)srate, mem[ich]);
		if (!dec[ich])
		{
			e = GF_NOT_SUPPORTED;
			goto cleanup;
		}
	}

	frame_pcm = (s16 *)gf_malloc((size_t)frame_samples * nchan * 2);
	dst_pck = gf_filter_pck_new_alloc(ctx->opid, nsamples * nchan * 2, &output);
	if (!frame_pcm || !dst_pck)
	{
		e = GF_OUT_OF_MEM;
		goto cleanup;
	}

	pos = hdr_size;
	while ((produced < (int)nsamples) && (pos + 2 <= size))
	{
		u32 block_bytes = lc3_rd16(data + pos);
		const u8 *in_ptr = data + pos + 2;
		int offset, nwrite;

		pos += 2;
		if (pos + block_bytes > size)
			break;
		pos += block_bytes;

		for (ich = 0; ich < nchan; ich++)
		{
			u32 frame_bytes = block_bytes / nchan + ((ich < block_bytes % nchan) ? 1 : 0);
			/* a zero-length block means a lost frame: liblc3 conceals it */
			lc3_decode(dec[ich], block_bytes ? in_ptr : NULL, (int)frame_bytes,
			           LC3_PCM_FORMAT_S16, frame_pcm + ich, (int)nchan);
			in_ptr += frame_bytes;
		}

		/* the delay is trimmed off the very first frame only */
		offset = produced ? 0 : delay;
		nwrite = frame_samples - offset;
		if (nwrite > (int)nsamples - produced)
			nwrite = (int)nsamples - produced;
		if (nwrite <= 0)
			continue;

		memcpy(output + (size_t)produced * nchan * 2,
		       frame_pcm + (size_t)offset * nchan,
		       (size_t)nwrite * nchan * 2);
		produced += nwrite;
	}

	if (!produced)
	{
		gf_filter_pck_discard(dst_pck);
		e = GF_NON_COMPLIANT_BITSTREAM;
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[LC3Dec] No decodable frame found\n"));
		goto cleanup;
	}
	if (produced < (int)nsamples)
		gf_filter_pck_truncate(dst_pck, (u32)produced * nchan * 2);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_duration(dst_pck, (u32)produced);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);

cleanup:
	if (frame_pcm)
		gf_free(frame_pcm);
	for (ich = 0; ich < LC3_MAX_CHANNELS; ich++)
		if (mem[ich])
			gf_free(mem[ich]);
	gf_filter_pid_drop_packet(ctx->ipid);
	if (e != GF_OK)
		return e;
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void lc3dec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability LC3DecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "lc3"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/lc3|audio/x-lc3"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister LC3DecoderRegister = {
	.name = "lc3dec",
	GF_FS_SET_DESCRIPTION("LC3 (Bluetooth LE Audio) decoder")
		GF_FS_SET_HELP("This filter decodes LC3 bitstream files, the codec of Bluetooth LE Audio, using Google's liblc3.")
			.private_size = sizeof(GF_LC3DecCtx),
	SETCAPS(LC3DecCaps),
	.configure_pid = lc3dec_configure_pid,
	.process = lc3dec_process,
	.finalize = lc3dec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE lc3dec_register(GF_FilterSession *session)
{
	return &LC3DecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_lc3dec(void) {
    gf_filter_auto_register("lc3dec", lc3dec_register);
}
