/**
 *   ztex.c - cgminer worker for Ztex 1.15x fpga board
 *
 *   Copyright (c) 2012 nelisky.btc@gmail.com
 *
 *   This work is based upon the Java SDK provided by ztex which is
 *   Copyright (C) 2009-2011 ZTEX GmbH.
 *   http://www.ztex.de
 *
 *   This work is based upon the icarus.c worker which is
 *   Copyright 2012 Luke Dashjr
 *   Copyright 2012 Xiangfu <xiangfu@openmobilefree.com>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License version 2 as
 *   published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, see http://www.gnu.org/licenses/.
**/

#include "miner.h"
#include <unistd.h>
#include <sha2.h>
#include "libztex.h"
#include "util.h"

#define GOLDEN_BACKLOG 5

struct device_drv ztex_drv;

static int option_offset = -1;		// KRAMBLE used for ztex_clock option

// Forward declarations
static void ztex_disable(struct thr_info* thr);
static bool ztex_prepare(struct thr_info *thr);

static void ztex_selectFpga(struct libztex_device* ztex)
{
	if (ztex->root->numberOfFpgas > 1) {
		if (ztex->root->selectedFpga != ztex->fpgaNum)
			mutex_lock(&ztex->root->mutex);
		libztex_selectFpga(ztex);
	}
}

static void ztex_releaseFpga(struct libztex_device* ztex)
{
	if (ztex->root->numberOfFpgas > 1) {
		ztex->root->selectedFpga = -1;
		mutex_unlock(&ztex->root->mutex);
	}
}

static void ztex_detect(void)
{
	int cnt;
	int i,j;
	int fpgacount;
	struct libztex_dev_list **ztex_devices;
	struct libztex_device *ztex_slave;
	struct cgpu_info *ztex;

	cnt = libztex_scanDevices(&ztex_devices);
	if (cnt > 0)
		applog(LOG_WARNING, "Found %d ztex board%s", cnt, cnt > 1 ? "s" : "");

	for (i = 0; i < cnt; i++) {
		ztex = calloc(1, sizeof(struct cgpu_info));
		ztex->drv = &ztex_drv;
		ztex->device_ztex = ztex_devices[i]->dev;
		ztex->threads = 1;
		ztex->device_ztex->fpgaNum = 0;
		ztex->device_ztex->root = ztex->device_ztex;
		add_cgpu(ztex);

		fpgacount = libztex_numberOfFpgas(ztex->device_ztex);

		if (fpgacount > 1)
			pthread_mutex_init(&ztex->device_ztex->mutex, NULL);

		for (j = 1; j < fpgacount; j++) {
			ztex = calloc(1, sizeof(struct cgpu_info));
			ztex->drv = &ztex_drv;
			ztex_slave = calloc(1, sizeof(struct libztex_device));
			memcpy(ztex_slave, ztex_devices[i]->dev, sizeof(struct libztex_device));
			ztex->device_ztex = ztex_slave;
			ztex->threads = 1;
			ztex_slave->fpgaNum = j;
			ztex_slave->root = ztex_devices[i]->dev;
			ztex_slave->repr[strlen(ztex_slave->repr) - 1] = ('1' + j);
			add_cgpu(ztex);
		}

		applog(LOG_WARNING,"%s: Found Ztex (fpga count = %d) , mark as %d", ztex->device_ztex->repr, fpgacount, ztex->device_id);
	}

	if (cnt > 0)
		libztex_freeDevList(ztex_devices);
}

static bool ztex_updateFreq(struct libztex_device* ztex)
{
	int i, maxM, bestM;
	double bestR, r;

	for (i = 0; i < ztex->freqMaxM; i++)
		if (ztex->maxErrorRate[i + 1] * i < ztex->maxErrorRate[i] * (i + 20))
			ztex->maxErrorRate[i + 1] = ztex->maxErrorRate[i] * (1.0 + 20.0 / i);

	maxM = 0;
	while (maxM < ztex->freqMDefault && ztex->maxErrorRate[maxM + 1] < LIBZTEX_MAXMAXERRORRATE)
		maxM++;
	while (maxM < ztex->freqMaxM && ztex->errorWeight[maxM] > 150 && ztex->maxErrorRate[maxM + 1] < LIBZTEX_MAXMAXERRORRATE)
		maxM++;

	bestM = 0;
	bestR = 0;
	for (i = 0; i <= maxM; i++) {
		r = (i + 1 + (i == ztex->freqM? LIBZTEX_ERRORHYSTERESIS: 0)) * (1 - ztex->maxErrorRate[i]);
		if (r > bestR) {
			bestM = i;
			bestR = r;
		}
	}

	if (bestM != ztex->freqM) {
		ztex_selectFpga(ztex);
		libztex_setFreq(ztex, bestM);
		ztex_releaseFpga(ztex);
	}

	maxM = ztex->freqMDefault;
	while (maxM < ztex->freqMaxM && ztex->errorWeight[maxM + 1] > 100)
		maxM++;
	if ((bestM < (1.0 - LIBZTEX_OVERHEATTHRESHOLD) * maxM) && bestM < maxM - 1) {
		ztex_selectFpga(ztex);
		libztex_resetFpga(ztex);
		ztex_releaseFpga(ztex);
		applog(LOG_ERR, "%s: frequency drop of %.1f%% detect. This may be caused by overheating. FPGA is shut down to prevent damage.",
		       ztex->repr, (1.0 - 1.0 * bestM / maxM) * 100);
		return false;
	}

	return true;
}


/* ztex_checkNonce moved to cgminer.c for blake

static uint32_t ztex_checkNonce(struct work *work, uint32_t nonce)
{
	uint32_t *data32 = (uint32_t *)(work->data);
	unsigned char swap[80];
	uint32_t *swap32 = (uint32_t *)swap;
	unsigned char hash1[32];
	unsigned char hash2[32];
	uint32_t *hash2_32 = (uint32_t *)hash2;
	int i;

	swap32[76/4] = htonl(nonce);

	for (i = 0; i < 76 / 4; i++)
		swap32[i] = swab32(data32[i]);

	sha2(swap, 80, hash1);
	sha2(hash1, 32, hash2);

	return htonl(hash2_32[7]);
}
*/

static int64_t ztex_scanhash(struct thr_info *thr, struct work *work,
                              __maybe_unused int64_t max_nonce)
{
	struct libztex_device *ztex;
	unsigned char sendbuf[76];
	int i, j, k;
	uint32_t *backlog;
	int backlog_p = 0, backlog_max;
	uint32_t *lastnonce;
	uint32_t nonce, noncecnt = 0;
	bool overflow, found;
	struct libztex_hash_data hdata[GOLDEN_BACKLOG];

	if (thr->cgpu->deven == DEV_DISABLED)
		return -1;

	ztex = thr->cgpu->device_ztex;

	// Copy / Reverse The Remaining Block Header Data
	memcpy(sendbuf,     work->data + 72, 4);
	memcpy(sendbuf + 4, work->data + 68, 4);
	memcpy(sendbuf + 8, work->data + 64, 4);
	
	// Copy / Reverse The Midstate
	swab512(sendbuf + 12, work->midstate);

	
//unsigned char* b = (unsigned char*)(sendbuf);
//applog(LOG_WARNING, "Buf: %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15],b[16],b[17],b[18],b[19],b[20],b[21],b[22],b[23],b[24],b[25],b[26],b[27],b[28],b[29],b[30],b[31],b[32],b[33],b[34],b[35],b[36],b[37],b[38],b[39],b[40],b[41],b[42],b[43],b[44],b[45],b[46],b[47],b[48],b[49],b[50],b[51],b[52],b[53],b[54],b[55],b[56],b[57],b[58],b[59],b[60],b[61],b[62],b[63],b[64],b[65],b[66],b[67],b[68],b[69],b[70],b[71],b[72],b[73],b[74],b[75]);


	
	ztex_selectFpga(ztex);
	i = libztex_sendHashData(ztex, sendbuf);
	if (i < 0) {
		// Something wrong happened in send
		applog(LOG_ERR, "%s: Failed to send hash data with err %d, retrying", ztex->repr, i);
		nmsleep(500);
		i = libztex_sendHashData(ztex, sendbuf);
		if (i < 0) {
			// And there's nothing we can do about it
			ztex_disable(thr);
			applog(LOG_ERR, "%s: Failed to send hash data with err %d, giving up", ztex->repr, i);
			ztex_releaseFpga(ztex);
			return -1;
		}
	}
	ztex_releaseFpga(ztex);

#if 0
	char *data = bin2hex(work->data, sizeof(work->data));
	applog(LOG_WARNING, "%s: sent data %s", ztex->repr, data);
#endif

	lastnonce = calloc(1, sizeof(uint32_t)*ztex->numNonces);
	if (lastnonce == NULL) {
		applog(LOG_ERR, "%s: failed to allocate lastnonce[%d]", ztex->repr, ztex->numNonces);
		return -1;
	}

	/* Add an extra slot for detecting dupes that lie around */
	backlog_max = ztex->numNonces * (2 + ztex->extraSolutions);
	backlog = calloc(1, sizeof(uint32_t) * backlog_max);
	if (backlog == NULL) {
		applog(LOG_ERR, "%s: failed to allocate backlog[%d]", ztex->repr, backlog_max);
		return -1;
	}

	overflow = false;
	int count = 0;
	int validNonces = 0;
	double errorCount = 0;

	applog(LOG_DEBUG, "%s: entering poll loop", ztex->repr);
	while (!(overflow || thr->work_restart)) {
		count++;

		int sleepcount = 0;
		while (thr->work_restart == 0 && sleepcount < 25) {
			nmsleep(10);
			sleepcount += 1;
		}

		if (thr->work_restart) {
			applog(LOG_DEBUG, "%s: New work detected", ztex->repr);
			break;
		}

		ztex_selectFpga(ztex);
		i = libztex_readHashData(ztex, &hdata[0]);
		
		if (i < 0) {
			// Something wrong happened in read
			applog(LOG_ERR, "%s: Failed to read hash data with err %d, retrying", ztex->repr, i);
			nmsleep(500);
			i = libztex_readHashData(ztex, &hdata[0]);
			if (i < 0) {
				// And there's nothing we can do about it
				ztex_disable(thr);
				applog(LOG_ERR, "%s: Failed to read hash data with err %d, giving up", ztex->repr, i);
				free(lastnonce);
				free(backlog);
				ztex_releaseFpga(ztex);
				return -1;
			}
		}
		ztex_releaseFpga(ztex);

		if (thr->work_restart) {
			applog(LOG_DEBUG, "%s: New work detected", ztex->repr);
			break;
		}

		ztex->errorCount[ztex->freqM] *= 0.995;
		ztex->errorWeight[ztex->freqM] = ztex->errorWeight[ztex->freqM] * 0.995 + 1.0;

		for (i = 0; i < ztex->numNonces; i++) {
			nonce = hdata[i].nonce;
			if (nonce > noncecnt)
				noncecnt = nonce;
			// KRAMBLE don't overflow if nonce == 0 (eg on lockup)
			if ( (((0xffffffff - nonce) < (nonce - lastnonce[i])) || nonce < lastnonce[i]) && nonce ) {
//				applog(LOG_WARNING, "%s: overflow nonce=%08x lastnonce=%08x", ztex->repr, nonce, lastnonce[i]);
				applog(LOG_DEBUG, "%s: overflow nonce=%08x lastnonce=%08x", ztex->repr, nonce, lastnonce[i]);
				overflow = true;
			} else
				lastnonce[i] = nonce;

			if (ztex_checkNonce(work, nonce) != (hdata->hash7)) {
//				applog(LOG_WARNING, "%s: checkNonce failed for %08X", ztex->repr, nonce);		// KRAMBLE not in production ??
				applog(LOG_DEBUG, "%s: checkNonce failed for %08X", ztex->repr, nonce);

				// do not count errors in the first 500ms after sendHashData (2x250 wait time)
				if (count > 2) {
				
					thr->cgpu->hw_errors++;
					errorCount += (1.0 / ztex->numNonces);
				}
			}
			else
				validNonces++;

			for (j=0; j<=ztex->extraSolutions; j++) {
				nonce = hdata[i].goldenNonce[j];

				if (nonce == ztex->offsNonces) {
					continue;
				}

				// precheck the extraSolutions since they often fail
				if (j > 0 && ztex_checkNonce(work, nonce) != 0) {
					continue;
				}

				found = false;
				for (k = 0; k < backlog_max; k++) {
					if (backlog[k] == nonce) {
						found = true;
						break;
					}
				}
				if (!found) {
					applog(LOG_WARNING, "%s: Test Share found %08x", ztex->repr, nonce);		// KRAMBLE useful to show its working
					applog(LOG_DEBUG, "%s: Test Share found N%dE%d", ztex->repr, i, j);
					backlog[backlog_p++] = nonce;

					if (backlog_p >= backlog_max)
						backlog_p = 0;

					work->blk.nonce = 0xffffffff;
//
//
//
//
//
					submit_nonce(thr, work, nonce);
					// applog(LOG_WARNING, "%s: submitted %08x", ztex->repr, nonce);
					applog(LOG_DEBUG, "%s: submitted %08x", ztex->repr, nonce);					
				}
			}
		}
	}

	// only add the errorCount if we had at least some valid nonces or
	// had no valid nonces in the last round
	if (errorCount > 0.0) {
		if (ztex->nonceCheckValid > 0 && validNonces == 0) {
			applog(LOG_ERR, "%s: resetting %.1f errors", ztex->repr, errorCount);
		}
		else {
			ztex->errorCount[ztex->freqM] += errorCount;
		}
	}

	// remember the number of valid nonces for the check in the next round
	ztex->nonceCheckValid = validNonces;

	ztex->errorRate[ztex->freqM] = ztex->errorCount[ztex->freqM] /	ztex->errorWeight[ztex->freqM] * (ztex->errorWeight[ztex->freqM] < 100? ztex->errorWeight[ztex->freqM] * 0.01: 1.0);
	if (ztex->errorRate[ztex->freqM] > ztex->maxErrorRate[ztex->freqM])
		ztex->maxErrorRate[ztex->freqM] = ztex->errorRate[ztex->freqM];

#if 1	// KRAMBLE OPTIONALLY DISABLE
	if (!ztex_updateFreq(ztex)) {
		// Something really serious happened, so mark this thread as dead!
		free(lastnonce);
		free(backlog);
		
		return -1;
	}
#endif

	applog(LOG_DEBUG, "%s: exit %1.8X", ztex->repr, noncecnt);

	work->blk.nonce = 0xffffffff;

	free(lastnonce);
	free(backlog);

	return noncecnt;
}

static void ztex_statline_before(char *buf, struct cgpu_info *cgpu)
{
	if (cgpu->deven == DEV_ENABLED) {
		tailsprintf(buf, "%s-%d | ", cgpu->device_ztex->snString, cgpu->device_ztex->fpgaNum+1);
		tailsprintf(buf, "%0.1fMHz | ", cgpu->device_ztex->freqM1 * (cgpu->device_ztex->freqM + 1));
	}
}

static bool ztex_prepare(struct thr_info *thr)
{
	struct timeval now;
	struct cgpu_info *cgpu = thr->cgpu;
	struct libztex_device *ztex = cgpu->device_ztex;

	cgtime(&now);
	get_datestamp(cgpu->init, &now);

	ztex_selectFpga(ztex);
	if (libztex_configureFpga(ztex) != 0) {
		libztex_resetFpga(ztex);
		ztex_releaseFpga(ztex);
		applog(LOG_ERR, "%s: Disabling!", thr->cgpu->device_ztex->repr);
		thr->cgpu->deven = DEV_DISABLED;
		return true;
	}
	
	// KRAMBLE Handle options, based on get_options in driver-icarus.c
	// Use as --ztex-clock freqM:freqMaxM
	// Multiple comma separated vaues are allowed eg 160:180,180:184

	{	// Bare block to isolate variables

		char err_buf[BUFSIZ+1];
		char buf[BUFSIZ+1];
		char *ptr, *comma, *colon, *colon2;
		size_t max;
		int i, tmp;

		int this_option_offset = ++option_offset;

		if (opt_ztex_clock == NULL)
				buf[0] = '\0';
		else {
			ptr = opt_ztex_clock;
			for (i = 0; i < this_option_offset; i++) {
				comma = strchr(ptr, ',');
				if (comma == NULL)
					break;
				ptr = comma + 1;
			}

			comma = strchr(ptr, ',');
			if (comma == NULL)
				max = strlen(ptr);
			else
				max = comma - ptr;

			if (max > BUFSIZ)
				max = BUFSIZ;
			strncpy(buf, ptr, max);
			buf[max] = '\0';
		}


		if (*buf) {
			colon = strchr(buf, ':');
			if (colon)
				*(colon++) = '\0';

			if (*buf) {
				tmp = atoi(buf);
				if (tmp >= 100 && tmp <= 250)
					ztex->freqM = ztex->freqMDefault = tmp/4 - 1;	// NB 4Mhz units
				else {
					sprintf(err_buf, "Invalid ztex-clock must be between 100 and 250", buf);
					quit(1, err_buf);
				}
			}

			if (colon && *colon) {
				tmp = atoi(colon);
				if (tmp >= 100 && tmp <= 250) {
					if (tmp/4 - 1 >= ztex->freqM)
						ztex->freqMaxM = tmp/4 - 1;	// NB 4Mhz units
					else
					{
						sprintf(err_buf, "Invalid ztex-clock max must be less than min", buf);
						quit(1, err_buf);
					}
				}
				else {
					sprintf(err_buf, "Invalid ztex-clock must be between 100 and 250", buf);
					quit(1, err_buf);
				}
			}
		}
	
	}	// End bare block
	
	
	ztex->freqM = ztex->freqMaxM+1;		// KRAMBLE is in original
	// ztex_updateFreq(ztex);			// KRAMBLE Was already commented out in original

#if 1
	libztex_setFreq(ztex, ztex->freqMDefault);			// KRAMBLE PRODUCTION CODE
#else
	// KRAMBLE build customised settings for a specific board
	if (ztex->repr[strlen(ztex->repr)-1] == '4')
		libztex_setFreq(ztex, ztex->freqMDefault-1);	// Run it 4MHz slower
	else
		libztex_setFreq(ztex, ztex->freqMDefault);
#endif

	ztex_releaseFpga(ztex);
	applog(LOG_DEBUG, "%s: prepare", ztex->repr);
	return true;
}

static void ztex_shutdown(struct thr_info *thr)
{
	if (thr->cgpu->device_ztex != NULL) {
		if (thr->cgpu->device_ztex->fpgaNum == 0)
			pthread_mutex_destroy(&thr->cgpu->device_ztex->mutex);  
		applog(LOG_DEBUG, "%s: shutdown", thr->cgpu->device_ztex->repr);
		libztex_destroy_device(thr->cgpu->device_ztex);
		thr->cgpu->device_ztex = NULL;
	}
}

static void ztex_disable(struct thr_info *thr)
{
	struct cgpu_info *cgpu;

	applog(LOG_ERR, "%s: Disabling!", thr->cgpu->device_ztex->repr);
	cgpu = get_devices(thr->cgpu->device_id);
	cgpu->deven = DEV_DISABLED;
	ztex_shutdown(thr);
}

struct device_drv ztex_drv = {
	.drv_id = DRIVER_ZTEX,
	.dname = "ztex",
	.name = "ZTX",
	.drv_detect = ztex_detect,
	.get_statline_before = ztex_statline_before,
	.thread_prepare = ztex_prepare,
	.scanhash = ztex_scanhash,
	.thread_shutdown = ztex_shutdown,
};

