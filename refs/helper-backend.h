#ifndef REFS_HELPER_BACKEND_H
#define REFS_HELPER_BACKEND_H

/*
 * Ref helper backend: delegate ref storage to an external process.
 *
 * Uses the shared helper process (helper.h) which handles both ref
 * and ODB operations in a single "git-local-<name>" binary,
 * matching how remote helpers handle both in one process.
 *
 * See helper.h for the full protocol documentation.
 */

extern struct ref_storage_be refs_be_helper;

#endif
