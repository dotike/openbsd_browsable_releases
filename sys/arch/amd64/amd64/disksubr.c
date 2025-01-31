/*	$OpenBSD: disksubr.c,v 1.68 2014/08/30 10:41:10 miod Exp $	*/
/*	$NetBSD: disksubr.c,v 1.21 1996/05/03 19:42:03 christos Exp $	*/

/*
 * Copyright (c) 1996 Theo de Raadt
 * Copyright (c) 1982, 1986, 1988 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/disklabel.h>
#include <sys/disk.h>
#include <sys/reboot.h>
#include <sys/conf.h>

#include <machine/biosvar.h>

/*
 * Attempt to read a disk label from a device
 * using the indicated strategy routine.
 * The label must be partly set up before this:
 * secpercyl, secsize and anything required for a block i/o read
 * operation in the driver's strategy/start routines
 * must be filled in before calling us.
 *
 * If dos partition table requested, attempt to load it and
 * find disklabel inside a DOS partition.
 *
 * We would like to check if each MBR has a valid DOSMBR_SIGNATURE, but
 * we cannot because it doesn't always exist. So.. we assume the
 * MBR is valid.
 */
int
readdisklabel(dev_t dev, void (*strat)(struct buf *),
    struct disklabel *lp, int spoofonly)
{
	bios_diskinfo_t *pdi;
	struct buf *bp = NULL;
	dev_t devno;
	int error;

	if ((error = initdisklabel(lp)))
		goto done;

	/* Look for any BIOS geometry information we should honour. */
	devno = chrtoblk(dev);
	if (devno == NODEV)
		devno = dev;
	pdi = bios_getdiskinfo(MAKEBOOTDEV(major(devno), 0, 0, DISKUNIT(devno),
	    RAW_PART));
	if (pdi != NULL && pdi->bios_heads > 0 && pdi->bios_sectors > 0) {
#ifdef DEBUG
		printf("Disk GEOM %u/%u/%u -> BIOS GEOM %u/%u/%llu\n",
		    lp->d_ntracks, lp->d_nsectors, lp->d_ncylinders,
		    pdi->bios_heads, pdi->bios_sectors,
		    DL_GETDSIZE(lp) / (pdi->bios_heads * pdi->bios_sectors));
#endif
		lp->d_ntracks = pdi->bios_heads;
		lp->d_nsectors = pdi->bios_sectors;
		lp->d_secpercyl = pdi->bios_sectors * pdi->bios_heads;
		lp->d_ncylinders = DL_GETDSIZE(lp) / lp->d_secpercyl;
	}

	/* get a buffer and initialize it */
	bp = geteblk((int)lp->d_secsize);
	bp->b_dev = dev;

#if defined(GPT)
	error = readgptlabel(bp, strat, lp, NULL, spoofonly);
	if (error == 0)
		goto done;
#endif

	error = readdoslabel(bp, strat, lp, NULL, spoofonly);
	if (error == 0)
		goto done;

#if defined(CD9660)
	error = iso_disklabelspoof(dev, strat, lp);
	if (error == 0)
		goto done;
#endif
#if defined(UDF)
	error = udf_disklabelspoof(dev, strat, lp);
	if (error == 0)
		goto done;
#endif

done:
	if (bp) {
		bp->b_flags |= B_INVAL;
		brelse(bp);
	}
	disk_change = 1;
	return (error);
}

/*
 * Write disk label back to device after modification.
 */
int
writedisklabel(dev_t dev, void (*strat)(struct buf *), struct disklabel *lp)
{
	daddr_t partoff = -1;
	int error = EIO;
	int offset;
	struct disklabel *dlp;
	struct buf *bp = NULL;

	/* get a buffer and initialize it */
	bp = geteblk((int)lp->d_secsize);
	bp->b_dev = dev;

#if defined(GPT)
        if (readgptlabel(bp, strat, lp, &partoff, 1) != 0)
#endif
		if (readdoslabel(bp, strat, lp, &partoff, 1) != 0)
                	goto done;

	/* Read it in, slap the new label in, and write it back out */
	bp->b_blkno = DL_BLKTOSEC(lp, partoff + DOS_LABELSECTOR) *
	    DL_BLKSPERSEC(lp);
	offset = DL_BLKOFFSET(lp, partoff + DOS_LABELSECTOR);
	bp->b_bcount = lp->d_secsize;
	CLR(bp->b_flags, B_READ | B_WRITE | B_DONE);
	SET(bp->b_flags, B_BUSY | B_READ | B_RAW);
	(*strat)(bp);
	if ((error = biowait(bp)) != 0)
		goto done;

	dlp = (struct disklabel *)(bp->b_data + offset);
	*dlp = *lp;
	CLR(bp->b_flags, B_READ | B_WRITE | B_DONE);
	SET(bp->b_flags, B_BUSY | B_WRITE | B_RAW);
	(*strat)(bp);
	error = biowait(bp);

done:
	if (bp) {
		bp->b_flags |= B_INVAL;
		brelse(bp);
	}
	disk_change = 1;
	return (error);
}
