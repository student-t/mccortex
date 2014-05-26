#ifndef PATH_SET_H_
#define PATH_SET_H_

#include "packed_path.h"
#include "path_store.h"
#include "hash_table.h" // defines dBNode

typedef struct
{
  size_t seq; // colset is stored at (seq-cbytes)
  PathIndex pindex;
  PathLen orient:1, plen:PATH_LEN_BITS;
} PathEntry;

#include "objbuf_macro.h"
create_objbuf(pentrybuf, PathEntryBuffer, PathEntry);

#ifndef BYTE_BUFFER_DEFINED
  create_objbuf(bytebuf, ByteBuffer, uint8_t);
  #define BYTE_BUFFER_DEFINED
#endif

typedef struct
{
  size_t cbytes; // number of bytes in colourset
  ByteBuffer seqs;
  PathEntryBuffer members;
  bool sorted;
} PathSet;

#define path_set_seq(entry,set) ((set)->seqs.data + (entry)->seq)
#define path_set_colset(entry,set) (path_set_seq(entry,set) - (set)->cbytes)

void path_set_alloc(PathSet *set);
void path_set_dealloc(PathSet *set);

// Empty the path set
void path_set_reset(PathSet *set);

void path_set_init(PathSet *set, const PathStore *ps, hkey_t hkey);

void path_set_init2(PathSet *set, size_t cbytes,
                    const uint8_t *store, PathIndex pindex,
                    const FileFilter *fltr);

// Load paths into the set. If not reset, appends.
void path_set_load(PathSet *set, size_t cbytes,
                   const uint8_t *store, PathIndex pindex,
                   const FileFilter *fltr);

// Sort path set
void path_set_sort(PathSet *set);

// Remove redundant entries
void path_set_slim(PathSet *set);

// Remove entries from set that are in the filter set
// if `rmsubstr` then also remove substring matches
// Note: does not remove duplicates from `set`,
//       call path_set_slim() to do that
// if passed store, we use it to update colourset of paths in out_set
void path_set_merge(PathSet *out_set, PathSet *in_set,
                           bool rmsubstr, uint8_t *store);

// Sum of bytes required to store set
size_t path_set_get_bytes_sum(const PathSet *set);

#endif /* PATH_SET_H_ */
