/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_ZONES_H
#define LABWC_ZONES_H

/*
 * Implementation of the xx-zones-v1 protocol which allows clients to
 * position their toplevels within a compositor provided "zone".
 *
 * labwc provides one zone per output which covers the usable area of
 * that output. The zone handle is the output name.
 */

void zones_init(void);
void zones_finish(void);

/**
 * zones_update() - Re-evaluate the area of all zones.
 *
 * Should be called whenever the usable area of an output or the
 * output layout changed. Notifies clients about changed zone sizes
 * and item positions.
 */
void zones_update(void);

#endif /* LABWC_ZONES_H */
