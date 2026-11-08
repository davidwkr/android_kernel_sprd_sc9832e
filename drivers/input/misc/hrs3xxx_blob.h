/*
 * Glue between the hrs3xxx driver and the stock TYHX HX3918 measurement code
 * (an external object carved from the device's own boot.img, see
 * devices/HRS3XXX_INTERFACE.md). Without CONFIG_HRS3XXX_TYHX_BLOB the hooks
 * are empty and the driver behaves as it did before.
 */
#ifndef _HRS3XXX_BLOB_H
#define _HRS3XXX_BLOB_H

#include <linux/err.h>
#include <linux/types.h>

struct i2c_client;
struct input_dev;
struct hrsb;

#ifdef CONFIG_HRS3XXX_TYHX_BLOB

/* Allocates the measurement context. The context starts "not live". */
struct hrsb *hrsb_create(struct i2c_client *client, struct input_dev *input);
void hrsb_destroy(struct hrsb *b);
/* Allow (true) or forbid (false) start; false also makes works no-ops. */
void hrsb_set_live(struct hrsb *b, bool live);
/* Starts (or restarts) measuring; mode is the "enable" value 1..3. */
int hrsb_start(struct hrsb *b, unsigned int mode);
/* Timers, then works, then the stock exit; sleeps, call without locks the
 * measurement works take (none in the core driver). */
void hrsb_stop(struct hrsb *b);
bool hrsb_running(struct hrsb *b);
/*
 * System suspend/resume as stock: the algorithm and the chip keep their
 * state. Suspend only stops the timers and works (remembering which timers
 * were on), resume re-arms those; no exit/init, no power change. Both are
 * idempotent.
 */
void hrsb_suspend(struct hrsb *b);
void hrsb_resume(struct hrsb *b);

#else

static inline struct hrsb *hrsb_create(struct i2c_client *client,
				       struct input_dev *input)
{
	return NULL;
}
static inline void hrsb_destroy(struct hrsb *b) { }
static inline void hrsb_set_live(struct hrsb *b, bool live) { }
static inline int hrsb_start(struct hrsb *b, unsigned int mode)
{
	return -ENODEV;
}
static inline void hrsb_stop(struct hrsb *b) { }
static inline bool hrsb_running(struct hrsb *b) { return false; }
static inline void hrsb_suspend(struct hrsb *b) { }
static inline void hrsb_resume(struct hrsb *b) { }

#endif /* CONFIG_HRS3XXX_TYHX_BLOB */

#endif /* _HRS3XXX_BLOB_H */
