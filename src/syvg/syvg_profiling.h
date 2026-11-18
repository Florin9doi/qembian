/* EGL_NOK_resource_profiling reply layout (src/syvg/syvg_egl.cpp, EGL2_FN_QueryProfilingDataNOK).
 *
 * The phone's graphics-memory monitor GOOM parses this stream:
 *   CMemoryMonitor::GetFreeMemory()            goommonitor/src/goommemorymonitor.cpp
 *   CGOomWindowGroupList::RefreshL()           goommonitor/src/goomwindowgrouplist.cpp
 *
 * Two rules, both learned the hard way (docs/devices/nokia-808-rm807/frame-uploads.md):
 *  1. TOTAL and USED go out on EVERY query. GetFreeMemory() looks for them whatever query bits the
 *     caller set; a reply without them parses as totalMem == totalUsed == 0, so free memory is 0,
 *     GOOM declares low graphics memory and ALF is switched to its lowest-memory mode (software
 *     rendering: a full-screen bitmap upload per dirty rect). GOOM asks with
 *     EGL_PROF_QUERY_MEMORY_USAGE_BIT_NOK alone (0x2), so gating the pair on the global bit (0x1) is
 *     exactly the bug that cost the GPU path on the 808 and the 701.
 *  2. Each per-process entry is three token,value pairs - PROCESS_ID, PROCESS_USED_PRIVATE_MEMORY,
 *     PROCESS_USED_SHARED - and every value is ONE word on a 32-bit build. Padding words desynchronise
 *     the parse (GoomWindowGroupList advances 1 word for the id and 1 for each memory figure).
 */
#ifndef SYVG_PROFILING_H
#define SYVG_PROFILING_H

#include <stdint.h>

enum {
	SYVG_PROF_TOTAL_MEMORY   = 0x3070,	/* EGL_PROF_TOTAL_MEMORY_NOK */
	SYVG_PROF_USED_MEMORY    = 0x3071,	/* EGL_PROF_USED_MEMORY_NOK */
	SYVG_PROF_PROCESS_ID     = 0x3072,	/* EGL_PROF_PROCESS_ID_NOK */
	SYVG_PROF_PROCESS_PRIV   = 0x3073,	/* EGL_PROF_PROCESS_USED_PRIVATE_MEMORY_NOK */
	SYVG_PROF_PROCESS_SHARED = 0x3074	/* EGL_PROF_PROCESS_USED_SHARED_MEMORY_NOK */
};

#define SYVG_PROF_QUERY_MEMORY_USAGE 0x0002u	/* EGL_PROF_QUERY_MEMORY_USAGE_BIT_NOK */
#define SYVG_PROF_WORDS_PER_PROCESS 6

/* Writes the token,value stream for a query: the global pair always, one entry per process only when
 * the caller asked for memory usage. Returns the number of words the stream needs (may exceed aCap:
 * the caller then sends the first aCap words, as EGL specifies with data_size). */
static inline int syvg_profiling_fill(uint32_t aBits, uint32_t aTotalBytes, uint32_t aUsedBytes,
									  const uint32_t* aPids, const uint64_t* aBytes, int aCount,
									  int32_t* aOut, int aCap)
{
	int n = 0;
	#define SYVG_PUT(v) do { if (n < aCap) aOut[n] = (int32_t)(v); ++n; } while (0)
	SYVG_PUT(SYVG_PROF_TOTAL_MEMORY); SYVG_PUT(aTotalBytes);
	SYVG_PUT(SYVG_PROF_USED_MEMORY);  SYVG_PUT(aUsedBytes);
	if (aBits & SYVG_PROF_QUERY_MEMORY_USAGE)
		{
		for (int k = 0; k < aCount; ++k)
			{
			SYVG_PUT(SYVG_PROF_PROCESS_ID);     SYVG_PUT(aPids[k]);
			SYVG_PUT(SYVG_PROF_PROCESS_PRIV);   SYVG_PUT((uint32_t)aBytes[k]);
			SYVG_PUT(SYVG_PROF_PROCESS_SHARED); SYVG_PUT(0);
			}
		}
	#undef SYVG_PUT
	return n;
}

#endif /* SYVG_PROFILING_H */
