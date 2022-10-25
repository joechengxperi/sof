// SPDX-License-Identifier: BSD-3-Clause
//
// Author: 
//
// Validation codec implementation to demonstrate Codec Adapter API

#include <sof/audio/module_adapter/module/generic.h>
#include <sof/lib/wait.h>

/* bb7559e3-28ac-4a75-8f4f-480493ce39c7*/
DECLARE_SOF_RT_UUID("validation_codec", validation_uuid, 0xbb7559e3, 0x28ac, 0x4a75,
		    0x8f, 0x4f, 0x48, 0x04, 0x93, 0xce, 0x39, 0xc7);
DECLARE_TR_CTX(validation_tr, SOF_UUID(validation_uuid), LOG_LEVEL_INFO);

#define MAX_EXPECTED_VALIDATION_CONFIG_DATA_SIZE 8192

/*
 * Delay that doesn't cause issue - used for sanity checking ticks and time are consistent
 * - Delay is 160000 ticks, which at 600Mhz is 266us
 * - With log messages either side uncommented, log reports 270us
 * - Both are consistent with each other
 */
//#define DELAY_TIME_IN_TICKS 160000

/*	
 * Delay that does cause issue
 * - Delay is 180000 ticks, which at 600Mhz is 300us
 * - In the sof log, the LL schduler reports the pipeline task as taking approx. 190000 ticks
 */
#define DELAY_TIME_IN_TICKS 180000

static int validation_codec_init(struct processing_module *mod)
{
	comp_info(mod->dev, "validation_codec_init() start");
	return 0;
}

static int validation_codec_prepare(struct processing_module *mod)
{
	struct comp_dev *dev = mod->dev;
	struct module_data *codec = &mod->priv;

	comp_info(dev, "validation_codec_prepare()");

	codec->mpd.in_buff = rballoc(0, SOF_MEM_CAPS_RAM, mod->period_bytes);
	if (!codec->mpd.in_buff) {
		comp_err(dev, "validation_codec_prepare(): Failed to alloc in_buff");
		return -ENOMEM;
	}
	codec->mpd.in_buff_size = mod->period_bytes;

	codec->mpd.out_buff = rballoc(0, SOF_MEM_CAPS_RAM, mod->period_bytes);
	if (!codec->mpd.out_buff) {
		comp_err(dev, "validation_codec_prepare(): Failed to alloc out_buff");
		rfree(codec->mpd.in_buff);
		return -ENOMEM;
	}
	codec->mpd.out_buff_size = mod->period_bytes;

	return 0;
}

static int validation_codec_init_process(struct processing_module *mod)
{
	struct module_data *codec = &mod->priv;
	struct comp_dev *dev = mod->dev;

	comp_dbg(dev, "validation_codec_init_process()");

	codec->mpd.produced = 0;
	codec->mpd.consumed = 0;
	codec->mpd.init_done = 1;

	return 0;
}

static int
validation_codec_process(struct processing_module *mod,
			  struct input_stream_buffer *input_buffers, int num_input_buffers,
			  struct output_stream_buffer *output_buffers, int num_output_buffers)
{
	struct comp_dev *dev = mod->dev;
	struct module_data *codec = &mod->priv;

	/* Proceed only if we have enough data to fill the module buffer completely */
	if (input_buffers[0].size < codec->mpd.in_buff_size) {
		comp_dbg(dev, "validation_codec_process(): not enough data to process");
		return -ENODATA;
	}

	if (!codec->mpd.init_done)
		validation_codec_init_process(mod);

	memcpy_s(codec->mpd.in_buff, codec->mpd.in_buff_size,
		 input_buffers[0].data, codec->mpd.in_buff_size);

	comp_dbg(dev, "validation_codec_process()");

	memcpy_s(codec->mpd.out_buff, codec->mpd.out_buff_size,
		 codec->mpd.in_buff, codec->mpd.in_buff_size);
	codec->mpd.produced = mod->period_bytes;
	codec->mpd.consumed = mod->period_bytes;
	input_buffers[0].consumed = codec->mpd.consumed;

	/* copy the produced samples into the output buffer */
	memcpy_s(output_buffers[0].data, codec->mpd.produced, codec->mpd.out_buff,
		 codec->mpd.produced);
	output_buffers[0].size = codec->mpd.produced;

	//comp_info(dev, "validation_codec_process() wait_delay start");
	
	/* Simulate processing load */
	wait_delay(DELAY_TIME_IN_TICKS);
		
	//comp_info(dev, "validation_codec_process() wait_delay end");

	return 0;
}

static int validation_codec_reset(struct processing_module *mod)
{
	comp_info(mod->dev, "validation_codec_reset()");

	/* nothing to do */
	return 0;
}

static int validation_codec_free(struct processing_module *mod)
{
	struct comp_dev *dev = mod->dev;
	struct module_data *codec = &mod->priv;

	comp_info(dev, "validation_codec_free()");

	rfree(codec->mpd.in_buff);
	rfree(codec->mpd.out_buff);

	return 0;
}

static struct module_interface validation_interface = {
	.init  = validation_codec_init,
	.prepare = validation_codec_prepare,
	.process = validation_codec_process,
	.reset = validation_codec_reset,
	.free = validation_codec_free
};

DECLARE_MODULE_ADAPTER(validation_interface, validation_uuid, validation_tr);

