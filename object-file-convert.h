#ifndef OBJECT_CONVERT_H
#define OBJECT_CONVERT_H

struct repository;
struct object_id;
struct git_hash_algo;
struct strbuf;
#include "object.h"

int repo_oid_to_algop(struct repository *repo, const struct object_id *src,
		      const struct git_hash_algo *to, struct object_id *dest);

/*
 * Compute the compatibility-algorithm object id for an object's contents, used
 * when the repository tracks extensions.compatObjectFormat. This is a property
 * of the object's bytes, not of any storage backend, so the object database
 * computes it once and hands it to whichever source stores the object. Returns
 * 0 and fills `compat_oid` on success, -1 if the repository has no compat
 * algorithm configured.
 */
int repo_compute_compat_oid(struct repository *repo,
			    const void *buf, unsigned long len,
			    enum object_type type,
			    struct object_id *compat_oid);

/*
 * Convert an object file from one hash algorithm to another algorithm.
 * Return -1 on failure, 0 on success.
 */
int convert_object_file(struct repository *repo,
			struct strbuf *outbuf,
			const struct git_hash_algo *from,
			const struct git_hash_algo *to,
			const void *buf, size_t len,
			enum object_type type,
			int gentle);

#endif /* OBJECT_CONVERT_H */
