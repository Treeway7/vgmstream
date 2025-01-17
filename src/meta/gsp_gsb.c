#include "meta.h"
#include "../layout/layout.h"
#include "../coding/coding.h"
#include "../util/chunks.h"

/* GSP+GSB - from Tecmo's Super Swing Golf 1 & 2 (Wii), Quantum Theory (PS3/X360) */
VGMSTREAM* init_vgmstream_gsp_gsb(STREAMFILE* sf) {
    VGMSTREAM* vgmstream = NULL;
    STREAMFILE* sf_head = NULL;
    int loop_flag, channel_count, sample_rate, num_samples, loop_start, loop_end;
    off_t start_offset, chunk_offset, first_offset;
    size_t data_size;
    int codec;

    
    /* checks */
    if (!check_extensions(sf,"gsb"))
        goto fail;

    sf_head = open_streamfile_by_ext(sf, "gsp");
    if (!sf_head) goto fail;

    if (!is_id32be(0x00,sf_head, "GSND"))
        goto fail;
    /* 0x04: version? */
    /* 0x08: 1? */
    /* 0x0c: 0? */
    first_offset = read_32bitBE(0x10,sf_head); /* usually 0x14 */

    if (!find_chunk_be(sf_head, 0x48454144,first_offset,1, &chunk_offset,NULL)) /* "HEAD" */
        goto fail;
    /* 0x00: header size */
    /* 0x04: num_chunks */

    if (!find_chunk_be(sf_head, 0x44415441,first_offset,1, &chunk_offset,NULL)) /* "DATA" */
        goto fail;
    data_size       = read_32bitBE(chunk_offset + 0x00,sf_head);
    codec           = read_32bitBE(chunk_offset + 0x04,sf_head);
    sample_rate     = read_32bitBE(chunk_offset + 0x08,sf_head);
    /* 0x0c: always 16? */
    channel_count   = read_16bitBE(chunk_offset + 0x0e,sf_head);
    /* 0x10: always 0? */
    num_samples     = read_32bitBE(chunk_offset + 0x14,sf_head);
    /* 0x18: always 0? */
    /* 0x1c: unk (varies with codec_id) */

    if (!find_chunk_be(sf_head, 0x42534943,first_offset,1, &chunk_offset,NULL)) /* "BSIC" */
        goto fail;
    /* 0x00/0x04: probably volume/pan/etc floats (1.0) */
    /* 0x08: null? */
    loop_flag   = read_8bit(chunk_offset+0x0c,sf_head);
    loop_start  = read_32bitBE(chunk_offset+0x10,sf_head);
    loop_end    = read_32bitBE(chunk_offset+0x14,sf_head);

    //if (!find_chunk_be(streamHeader, 0x4E414D45,first_offset,1, &chunk_offset,NULL)) /* "NAME" */
    //    goto fail;
    /* 0x00: name_size */
    /* 0x04+: name (same as filename) */


    start_offset = 0x00;


    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(channel_count,loop_flag);
    if (!vgmstream) goto fail;

    vgmstream->meta_type = meta_GSP_GSB;

    vgmstream->sample_rate = sample_rate;
    vgmstream->num_samples = num_samples;
    vgmstream->loop_start_sample = loop_start;
    vgmstream->loop_end_sample = loop_end;

    switch (codec) {
        case 0x04: { /* DSP [Super Swing Golf (Wii)] */
            size_t block_header_size;
            size_t num_blocks;

            vgmstream->coding_type = coding_NGC_DSP;
            vgmstream->layout_type = layout_blocked_gsb;

            if (!find_chunk_be(sf_head, 0x47434558,first_offset,1, &chunk_offset,NULL)) /* "GCEX" */
                goto fail;

            //vgmstream->current_block_size = read_32bitBE(chunk_offset+0x00,streamHeader);
            block_header_size = read_32bitBE(chunk_offset+0x04,sf_head);
            num_blocks = read_32bitBE(chunk_offset+0x08,sf_head);
            vgmstream->num_samples = (data_size - block_header_size * num_blocks) / 8 / vgmstream->channels * 14;
            /* 0x0c+: unk */

            dsp_read_coefs_be(vgmstream, sf_head, chunk_offset+0x18, 0x30);
            break;
        }
#ifdef VGM_USE_FFMPEG
        case 0x08: { /* ATRAC3 [Quantum Theory (PS3)] */
            int block_align, encoder_delay;

            block_align   = 0x98 * vgmstream->channels;
            encoder_delay = 1024 + 69*2; /* observed default, matches XMA (needed as many files start with garbage) */
            vgmstream->num_samples = atrac3_bytes_to_samples(data_size, block_align) - encoder_delay;
            /* fix num_samples as header samples seem to be modified to match altered (49999/48001) sample rates somehow */

            vgmstream->codec_data = init_ffmpeg_atrac3_raw(sf, start_offset,data_size, vgmstream->num_samples,vgmstream->channels,vgmstream->sample_rate, block_align, encoder_delay);
            if (!vgmstream->codec_data) goto fail;
            vgmstream->coding_type = coding_FFmpeg;
            vgmstream->layout_type = layout_none;

            /* set offset samples (offset 0 jumps to sample 0 > pre-applied delay, and offset end loops after sample end > adjusted delay) */
            vgmstream->loop_start_sample = atrac3_bytes_to_samples(loop_start, block_align); //- encoder_delay
            vgmstream->loop_end_sample = atrac3_bytes_to_samples(loop_end, block_align) - encoder_delay;
            break;
        }

        case 0x09: { /* XMA2 [Quantum Theory (PS3)] */
            if (!find_chunk_be(sf_head, 0x584D4558,first_offset,1, &chunk_offset,NULL)) /* "XMEX" */
                goto fail;
            /* 0x00: fmt0x166 header (BE) */
            /* 0x34: seek table */

            vgmstream->codec_data = init_ffmpeg_xma_chunk_split(sf_head, sf, start_offset, data_size, chunk_offset, 0x34);
            if (!vgmstream->codec_data) goto fail;
            vgmstream->coding_type = coding_FFmpeg;
            vgmstream->layout_type = layout_none;

            xma_fix_raw_samples(vgmstream, sf, start_offset,data_size, 0, 0,0); /* samples are ok */
            break;
        }
#endif
        default:
            goto fail;
    }


    if (!vgmstream_open_stream(vgmstream, sf, start_offset))
        goto fail;
    close_streamfile(sf_head);
    return vgmstream;

fail:
    close_streamfile(sf_head);
    close_vgmstream(vgmstream);
    return NULL;
}
