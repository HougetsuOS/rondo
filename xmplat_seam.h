/*
 * rondo — XmPlat seam include (MIGRATION_GUIDE §4.1)
 *
 * Wraps the fork's internal XmPlatP.h.  The seam header carries static
 * inline one-shot helpers that are inevitably partly unused at any given
 * call site; silence that specific noise rather than weakening the
 * project's -Wall.
 */
#ifndef RONDO_XMPLAT_SEAM_H
#define RONDO_XMPLAT_SEAM_H

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <Xm/XmPlat/XmPlatP.h>
#pragma GCC diagnostic pop

#endif /* RONDO_XMPLAT_SEAM_H */
