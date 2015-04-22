/*= -*- c-basic-offset: 4; indent-tabs-mode: nil; -*-
 *
 * librsync -- the library for network deltas
 * $Id: whole.c,v 1.23 2003/06/12 05:47:23 wayned Exp $
 *
 * Copyright (C) 2000, 2001 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

                              /*
                               | Is it possible that software is not
                               | like anything else, that it is meant
                               | to be discarded: that the whole point
                               | is to always see it as a soap bubble?
                               |        -- Alan Perlis
                               */



#include <config.h>

#include <assert.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "librsync.h"

#include "trace.h"
#include "fileutil.h"
#include "sumset.h"
#include "job.h"
#include "buf.h"
#include "whole.h"
#include "util.h"
#include "search.h"

/**
 * Run a job continuously, with input to/from the two specified files.
 * The job should already be set up, and must be free by the caller
 * after return.
 *
 * Buffers of ::rs_inbuflen and ::rs_outbuflen are allocated for
 * temporary storage.
 *
 * \param in_file Source of input bytes, or NULL if the input buffer
 * should not be filled.
 *
 * \return RS_DONE if the job completed, or otherwise an error result.
 */
rs_result
rs_whole_run(rs_job_t *job, MFILE *in_file, MFILE *out_file)
{
    rs_buffers_t    buf;
    rs_result       result;
    rs_filebuf_t    *in_fb = NULL, *out_fb = NULL;

    if (in_file)
        in_fb = rs_filebuf_new(in_file, rs_inbuflen);

    if (out_file)
        out_fb = rs_filebuf_new(out_file, rs_outbuflen);

    result = rs_job_drive(job, &buf,
                          in_fb ? rs_infilebuf_fill : NULL, in_fb,
                          out_fb ? rs_outfilebuf_drain : NULL, out_fb);

    if (in_fb)
        rs_filebuf_free(in_fb);

    if (out_fb)
        rs_filebuf_free(out_fb);

    return result;
}



/**
 * Generate the signature of a basis file, and write it out to
 * another.
 *
 * \param new_block_len block size for signature generation, in bytes
 *
 * \param strong_len truncated length of strong checksums, in bytes
 *
 * \sa rs_sig_begin()
 */
rs_result
rs_sig_file(MFILE *old_file, MFILE *sig_file, size_t new_block_len,
            size_t strong_len, rs_stats_t *stats)
{
    rs_job_t        *job;
    rs_result       r;

    job = rs_sig_begin(new_block_len, strong_len);
    r = rs_whole_run(job, old_file, sig_file);
    if (stats)
        memcpy(stats, &job->stats, sizeof *stats);
    rs_job_free(job);

    return r;
}


/**
 * Load signatures from a signature file into memory.  Return a
 * pointer to the newly allocated structure in SUMSET.
 *
 * \sa rs_readsig_begin()
 */
rs_result
rs_loadsig_file(MFILE *sig_file, rs_signature_t **sumset, rs_stats_t *stats)
{
    rs_job_t            *job;
    rs_result           r;

    job = rs_loadsig_begin(sumset);
    r = rs_whole_run(job, sig_file, NULL);
    if (stats)
        memcpy(stats, &job->stats, sizeof *stats);
    rs_job_free(job);

    return r;
}



rs_result
rs_delta_file(rs_signature_t *sig, MFILE *new_file, MFILE *delta_file,
              rs_stats_t *stats)
{
    rs_job_t            *job;
    rs_result           r;

    job = rs_delta_begin(sig);

    r = rs_whole_run(job, new_file, delta_file);

    if (stats)
        memcpy(stats, &job->stats, sizeof *stats);

    rs_job_free(job);

    return r;
}



rs_result rs_patch_file(MFILE *basis_file, MFILE *delta_file, MFILE *new_file,
                        rs_stats_t *stats)
{
    rs_job_t            *job;
    rs_result           r;

    job = rs_patch_begin(rs_file_copy_cb, basis_file);

    r = rs_whole_run(job, delta_file, new_file);
    
    if (stats)
        memcpy(stats, &job->stats, sizeof *stats);

    rs_job_free(job);

    return r;
}

rs_result rs_reverse_delta(rs_signature_t *new_sig, MFILE *basic_file, size_t *match_list, rs_stats_t *state)
{
	rs_result      result = RS_DONE;
	rs_long_t      match_pos;
	Rollsum        weak_sum;

	RollsumInit(&weak_sum);
	if ((result = rs_build_hash_table(new_sig)) != RS_DONE)
		return RS_CORRUPT;

	/* while output is not blocked and there is a block of data */
	while (((basic_file->fptr + new_sig->block_len) < basic_file->len)) {
		/* check if this block matches */
		/* calculate the weak_sum if we don't have one */
		if (weak_sum.count == 0) {
			/* Update the weak_sum */
			RollsumUpdate(&weak_sum, (basic_file->src + basic_file->fptr), new_sig->block_len);
		}
		if (rs_search_for_block(RollsumDigest(&weak_sum),
			basic_file->src + basic_file->fptr,
			new_sig->block_len,
			new_sig,
			state,
			&match_pos))
		{
			/* set the match block index and reset the weak_sum */
			match_list[match_pos/new_sig->block_len] = basic_file->fptr;
			/* increment scoop_pos to point at next unscanned data */
			basic_file->fptr += new_sig->block_len;
			/* process the match data off the scoop*/
			RollsumInit(&weak_sum);
		} else {
			/* rotate the weak_sum and append the miss byte */
			RollsumRotate(&weak_sum,basic_file->src[basic_file->fptr],
				basic_file->src[basic_file->fptr + new_sig->block_len]);
			basic_file->fptr += 1;
		}
	}

	return result;
}