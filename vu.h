#ifndef   VU_H
#define   VU_H

/**
 * Initialize VU measurements
 *
 * @param server    PulseAudio server; NULL for default
 * @param appname   Application name
 * @param devname   Source name; NULL for default
 * @param stream    Descriptive stream name
 * @param channels  Number of channels
 * @param rate      Samples per second per channel
 * @param samples   Samples per update
 * @return          Zero if success, nonzero if error.
*/
int  vu_start(const char *server,
              const char *appname,
              const char *devname,
              const char *stream,
              int         channels,
              int         rate,
              int         samples);

/**
 * Convert vu_start() return value to a string
*/
const char *vu_error(int);

/**
 * Stop VU measurements
*/
void  vu_stop(void);

/**
 * Wait for the next VU update
*/
void  vu_wait(void);

/**
 * Get latest VU peaks per channel; thread-safe
 *
 * @param peak      Array of floats to be populated
 * @param channels  Number of channels in peak array
 * @return          Zero if no new data available,
 *	            number of channels available if updated,
 *                  negative if an error occurred.
*/
int  vu_peak(float *to, int channels);

/**
 * Check if new VU peaks are available; thread-safe
*/
int  vu_peak_available(void);

#endif /* VU_H */
