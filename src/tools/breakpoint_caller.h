#ifndef BREAKPOINT_CALLER_H_
#define BREAKPOINT_CALLER_H_

#include "seq_file.h"
#include "db_graph.h"
#include "cmd.h"

// Require 5 kmers on the reference before and after breakpoint
#define DEFAULT_MIN_REF_NKMERS 5
#define DEFAULT_MAX_REF_NKMERS 1000

// Adds input bkmers to the graph
void breakpoints_call(size_t num_of_threads,
                      const read_t *reads, size_t num_reads,
                      gzFile gzout, const char *out_path,
                      const char **seq_paths, size_t num_seq_paths,
                      size_t min_ref_flank, size_t max_ref_flank,
                      const CmdArgs *args, dBGraph *db_graph);

#endif /* BREAKPOINT_CALLER_H_ */
