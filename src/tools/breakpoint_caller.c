#include "global.h"
#include "breakpoint_caller.h"
#include "util.h"
#include "file_util.h"
#include "db_graph.h"
#include "db_node.h"
#include "seq_reader.h"
#include "kmer_occur.h"
#include "graph_crawler.h"

typedef struct {
  size_t first_runid, num_runs;
} PathRefRun;

typedef struct
{
  // Specific to this instance
  const size_t threadid, nthreads;

  // Temporary memory used by this instance
  GraphCrawler crawlers[2]; // [0] => FORWARD, [1] => REVERSE
  dBNodeBuffer nbuf;

  // 5p flank
  KOccurRunBuffer koruns_5p, koruns_5p_ended;
  KOccurRunBuffer koruns_3p, koruns_3p_ended;
  dBNodeBuffer flank5pbuf;

  // Where the paths meet the ref
  PathRefRun *path_refs;
  KOccurRunBuffer path_run_buf;

  // Passed to all instances
  const KOGraph kograph;
  const dBGraph *db_graph;
  gzFile gzout;
  pthread_mutex_t *const out_lock;
  size_t *callid;
  const size_t min_ref_nkmers, max_ref_nkmers; // Set these from API
} BreakpointCaller;

static BreakpointCaller* brkpt_callers_new(size_t num_callers, gzFile gzout,
                                           size_t min_ref_flank,
                                           size_t max_ref_flank,
                                           const KOGraph kograph,
                                           const dBGraph *db_graph)
{
  const size_t ncols = db_graph->num_of_cols;
  BreakpointCaller *callers = ctx_malloc(num_callers * sizeof(BreakpointCaller));

  pthread_mutex_t *out_lock = ctx_malloc(sizeof(pthread_mutex_t));
  if(pthread_mutex_init(out_lock, NULL) != 0) die("mutex init failed");

  size_t *callid = ctx_calloc(1, sizeof(size_t));

  // Each colour in each caller can have a path at once
  PathRefRun *path_refs = ctx_calloc(num_callers*ncols, sizeof(PathRefRun));

  size_t i;
  for(i = 0; i < num_callers; i++)
  {
    BreakpointCaller tmp = {.threadid = i,
                            .nthreads = num_callers,
                            .kograph = kograph,
                            .db_graph = db_graph,
                            .gzout = gzout,
                            .out_lock = out_lock,
                            .callid = callid,
                            .path_refs = path_refs+i*ncols,
                            .min_ref_nkmers = min_ref_flank,
                            .max_ref_nkmers = max_ref_flank};

    memcpy(&callers[i], &tmp, sizeof(BreakpointCaller));

    db_node_buf_alloc(&callers[i].nbuf, 1024);
    db_node_buf_alloc(&callers[i].flank5pbuf, 1024);
    kmer_run_buf_alloc(&callers[i].koruns_5p, 512);
    kmer_run_buf_alloc(&callers[i].koruns_5p_ended, 512);
    kmer_run_buf_alloc(&callers[i].koruns_3p, 512);
    kmer_run_buf_alloc(&callers[i].koruns_3p_ended, 512);
    kmer_run_buf_alloc(&callers[i].path_run_buf, 512);
    graph_crawler_alloc(&callers[i].crawlers[0], db_graph);
    graph_crawler_alloc(&callers[i].crawlers[1], db_graph);
  }

  return callers;
}

static void brkpt_callers_destroy(BreakpointCaller *callers, size_t num_callers)
{
  size_t i;
  for(i = 0; i < num_callers; i++) {
    db_node_buf_dealloc(&callers[i].nbuf);
    db_node_buf_dealloc(&callers[i].flank5pbuf);
    kmer_run_buf_dealloc(&callers[i].koruns_5p);
    kmer_run_buf_dealloc(&callers[i].koruns_5p_ended);
    kmer_run_buf_dealloc(&callers[i].koruns_3p);
    kmer_run_buf_dealloc(&callers[i].koruns_3p_ended);
    kmer_run_buf_dealloc(&callers[i].path_run_buf);
    graph_crawler_dealloc(&callers[i].crawlers[0]);
    graph_crawler_dealloc(&callers[i].crawlers[1]);
  }
  pthread_mutex_destroy(callers[0].out_lock);
  ctx_free(callers[0].out_lock);
  ctx_free(callers[0].callid);
  ctx_free(callers[0].path_refs);
  ctx_free(callers);
}

static inline void ko_gzprint(gzFile gzout, size_t kmer_size,
                              KOGraph kograph, KOccurRun korun,
                              size_t contig_start)
{
  const char strand[] = {'+','-'};
  const char *chrom = kograph_chrom(kograph,korun).name;
  ctx_assert(korun.first <= korun.last || korun.strand == STRAND_MINUS);
  // get end coord as inclusive coord as start-end
  // (Note: start may be greater than end if strand is minus)
  size_t start, end, qoffset;
  if(korun.strand == STRAND_PLUS) {
    start = korun.first;
    end = korun.last+kmer_size-1;
  } else {
    start = korun.first+kmer_size-1;
    end = korun.last;
  }
  qoffset = korun.qoffset - contig_start;
  // +1 to coords to convert to 1-based
  gzprintf(gzout, "%s:%zu-%zu:%c:%u",
           chrom, start+1, end+1, strand[korun.strand], qoffset+1);
}

static inline void koruns_gzprint(gzFile gzout, size_t kmer_size,
                                  KOGraph kograph,
                                  const KOccurRun *koruns, size_t n,
                                  size_t contig_start)
{
  size_t i;
  if(n == 0) return;
  ko_gzprint(gzout, kmer_size, kograph, koruns[0], contig_start);
  for(i = 1; i < n; i++) {
    gzputc(gzout, ',');
    ko_gzprint(gzout, kmer_size, kograph, koruns[i], contig_start);
  }
}

static void process_contig(BreakpointCaller *caller,
                           const uint32_t *cols, size_t ncols,
                           dBNodeBuffer *flank5p, dBNodeBuffer *nbuf,
                           const KOccurRun *flank5p_runs, size_t num_flank5p_runs,
                           const KOccurRun *flank3p_runs, size_t num_flank3p_runs)
{
  gzFile gzout = caller->gzout;
  KOGraph kograph = caller->kograph;
  const size_t kmer_size = caller->db_graph->kmer_size;

  ctx_assert(ncols > 0);

  // we never re-met the ref
  if(num_flank3p_runs == 0) return;

  // Find first place we meet the ref
  size_t i, end = flank3p_runs[0].qoffset;
  size_t callid = __sync_fetch_and_add((volatile size_t*)caller->callid, 1);

  // Swallow up some of the path into the 3p flank
  size_t shift3p = MIN2(kmer_size-1, end);
  end -= shift3p;

  pthread_mutex_lock(caller->out_lock);

  // 5p flank with list of ref intersections
  gzprintf(gzout, ">call.%zu.5pflank chr=", callid);
  koruns_gzprint(gzout, kmer_size, kograph, flank5p_runs, num_flank5p_runs, 0);
  gzputc(gzout, '\n');
  db_nodes_gzprint(flank5p->data, flank5p->len, caller->db_graph, gzout);
  gzputc(gzout, '\n');

  // 3p flank with list of ref intersections
  gzprintf(gzout, ">call.%zu.3pflank chr=", callid);
  koruns_gzprint(gzout, kmer_size, kograph, flank3p_runs, num_flank3p_runs, end+shift3p);
  gzputc(gzout, '\n');
  db_nodes_gzprint_cont(nbuf->data+end, nbuf->len-end, caller->db_graph, gzout);
  gzputc(gzout, '\n');

  // Print path with list of colours
  gzprintf(gzout, ">call.%zu.path cols=%zu", callid, cols[0]);
  for(i = 1; i < ncols; i++) gzprintf(gzout, ",%zu", cols[i]);
  gzputc(gzout, '\n');
  db_nodes_gzprint_cont(nbuf->data, end, caller->db_graph, gzout);
  gzprintf(gzout, "\n\n");

  pthread_mutex_unlock(caller->out_lock);
}

// Traverse backwards from node1 -> node0
static void traverse_5pflank(const BreakpointCaller *caller,
                             GraphCrawler *crawler, dBNode node0, dBNode node1)
{
  const dBGraph *db_graph = crawler->cache.db_graph;
  dBNode nodes[4];
  Nucleotide bases[4];
  size_t i, num_next;
  BinaryKmer bkmer0 = db_node_get_bkmer(db_graph, node0.key);

  num_next = db_graph_next_nodes(db_graph, bkmer0, node0.orient,
                                 db_node_edges(db_graph, node0.key, 0),
                                 nodes, bases);

  for(i = 0; i < num_next && !db_nodes_are_equal(nodes[i],node1); i++) {}

  // Go backwards to get 5p flank
  // NULL means loop from 0..(ncols-1)
  size_t kmerlimit[2] = {0, caller->min_ref_nkmers};
  graph_crawler_fetch(crawler, node0, nodes, bases, i, num_next,
                      NULL, db_graph->num_of_cols,
                      gcrawler_load_path_limit_kmer_len, NULL, kmerlimit);
}

static bool gcrawler_stop_at_ref_covg(GraphCache *cache, GCacheStep *step,
                                      void *arg)
{
  BreakpointCaller *caller = (BreakpointCaller*)arg;
  KOccurRunBuffer *koruns_3p = &caller->koruns_3p;
  KOccurRunBuffer *koruns_3p_ended = &caller->koruns_3p_ended;
  GCacheSnode *snode = graph_cache_snode(cache, step->supernode);
  GCachePath *path = graph_cache_path(cache, step->pathid);
  const dBNode *nodes = graph_cache_first_node(cache, snode);
  bool forward = (step->orient == FORWARD);

  // Use index of last step as qoffset
  size_t qoffset = path->num_steps-1;

  // Kmer occurance runs are added to koruns_3p_ended only if they end and are
  // longer than the mininum length in kmers (caller->min_ref_nkmers)
  kograph_filter_extend(caller->kograph,
                        nodes, snode->num_nodes, forward,
                        caller->min_ref_nkmers, qoffset,
                        koruns_3p, koruns_3p_ended);

  size_t i, max_ref_run = 0;
  for(i = 0; i < koruns_3p->len; i++)
    max_ref_run = MAX2(max_ref_run, korun_len(koruns_3p->data[i]));

  // Continue if...
  return (koruns_3p_ended->len == 0 || max_ref_run < caller->min_ref_nkmers);
}

// src, dst can point to the same place
// returns number of elements added
static inline size_t filter_koruns(KOccurRun *src, size_t n,
                                   KOccurRun *dst, size_t min_kmers)
{
  size_t i, j;
  for(i = j = 0; i < n; i++)
    if(korun_len(src[i]) >= min_kmers)
      dst[j++] = src[i];

  return j;
}

static void gcrawler_finish_ref_covg(GraphCache *cache, uint32_t pathid,
                                     void *arg)
{
  (void)cache; // this function passed for callback, don't need cache here
  BreakpointCaller *caller = (BreakpointCaller*)arg;
  const KOccurRunBuffer *koruns_3p = &caller->koruns_3p;
  const KOccurRunBuffer *koruns_3p_ended = &caller->koruns_3p_ended;
  KOccurRunBuffer *dst = &caller->path_run_buf;
  size_t init_len = dst->len;

  // Copy finished runs into array
  kmer_run_buf_ensure_capacity(dst, dst->len+koruns_3p->len+koruns_3p_ended->len);
  kmer_run_buf_append(dst, koruns_3p_ended->data, koruns_3p_ended->len);

  dst->len += filter_koruns(koruns_3p->data, koruns_3p->len,
                            dst->data+dst->len,
                            caller->min_ref_nkmers);

  caller->path_refs[pathid].first_runid = init_len;
  caller->path_refs[pathid].num_runs = dst->len - init_len;
}

static inline
KOccurRun* fetch_3pflank_ref_contact(BreakpointCaller *caller,
                                     const GraphCache *cache,
                                     uint32_t pathid)
{
  // Get path
  const GCachePath *path = graph_cache_path(cache, pathid);
  const GCacheStep *steps = graph_cache_step(cache, path->first_step);
  size_t num_steps = path->num_steps;

  // Get runs along the ref
  PathRefRun ref_run = caller->path_refs[pathid];
  size_t num_runs = ref_run.num_runs;
  KOccurRun *flank3p_runs = caller->path_run_buf.data + ref_run.first_runid;
  koruns_sort_by_qoffset(flank3p_runs, num_runs);

  // Set qoffset to be the kmer offset in the path
  size_t r, s, offset = 0;

  for(s = r = 0; s < num_steps; s++) {
    for(; r < num_runs && flank3p_runs[r].qoffset == s; r++) {
      flank3p_runs[r].qoffset = offset;
    }

    if(r == num_runs) break;

    const GCacheSnode *snode = graph_cache_snode(cache, steps[s].supernode);
    offset += snode->num_nodes;
  }

  return flank3p_runs;
}

// Walk the graph remembering the last time we met the ref
// When traversal fails, dump sequence up to last meeting with the ref
static void follow_break(BreakpointCaller *caller, dBNode node)
{
  size_t i, j, k, num_next;
  dBNode next_nodes[4];
  Nucleotide next_nucs[4];
  const dBGraph *db_graph = caller->db_graph;

  BinaryKmer bkey = db_node_get_bkmer(db_graph, node.key);
  Edges edges = db_node_get_edges(db_graph, node.key, 0);

  num_next = db_graph_next_nodes(db_graph, bkey, node.orient, edges,
                                 next_nodes, next_nucs);

  // Filter out next nodes in the reference
  for(i = 0, j = 0; i < num_next; i++) {
    if(kograph_num(caller->kograph, next_nodes[i].key) == 0) {
      next_nodes[j] = next_nodes[i];
      next_nucs[j] = next_nucs[i];
      j++;
    }
  }

  // Abandon if all options are in ref or none are
  if(j == num_next || j == 0) return;
  num_next = j;

  // Follow all paths not in ref, in all colours
  GraphCrawler *fw_crawler = &caller->crawlers[node.orient];
  GraphCrawler *rv_crawler = &caller->crawlers[!node.orient];
  dBNodeBuffer *nbuf = &caller->nbuf, *flank5pbuf = &caller->flank5pbuf;
  KOccurRunBuffer *koruns_5p = &caller->koruns_5p;
  KOccurRunBuffer *koruns_5p_ended = &caller->koruns_5p_ended;
  GCMultiColPath *flank5p_path, *multicol_path;
  KOccurRun *flank3p_runs;

  // Loop over possible next nodes at this junction
  for(i = 0; i < num_next; i++)
  {
    // Go backwards to get 5p flank
    traverse_5pflank(caller, rv_crawler, db_node_reverse(next_nodes[i]),
                     db_node_reverse(node));

    // Loop over the flanks we got
    for(j = 0; j < rv_crawler->num_paths; j++)
    {
      // Get 5p flank
      db_node_buf_reset(flank5pbuf);
      graph_crawler_get_path_nodes(rv_crawler, j, flank5pbuf);
      db_nodes_reverse_complement(flank5pbuf->data, flank5pbuf->len);

      // Check this 5p flank is in the ref
      kmer_run_buf_reset(koruns_5p);
      kmer_run_buf_reset(koruns_5p_ended);
      kograph_filter_extend(caller->kograph,
                            flank5pbuf->data, flank5pbuf->len, true,
                            caller->min_ref_nkmers, 0,
                            koruns_5p, koruns_5p_ended);

      koruns_5p->len = filter_koruns(koruns_5p->data,
                                     koruns_5p->len,
                                     koruns_5p->data,
                                     caller->min_ref_nkmers);

      koruns_5p->len += filter_koruns(koruns_5p_ended->data,
                                      koruns_5p_ended->len,
                                      koruns_5p->data+koruns_5p->len,
                                      caller->min_ref_nkmers);

      kmer_run_buf_append(koruns_5p, koruns_5p_ended->data, koruns_5p_ended->len);

      if(koruns_5p->len > 0)
      {
        // Only traverse in the colours we have a flank for
        flank5p_path = &rv_crawler->multicol_paths[j];

        // Reset caller
        kmer_run_buf_reset(&caller->koruns_3p);
        kmer_run_buf_reset(&caller->koruns_3p_ended);
        kmer_run_buf_reset(&caller->path_run_buf);

        // functions gcrawler_stop_at_ref_covg(), gcrawler_finish_ref_covg()
        // both fill koruns_3p, koruns_3p_ended and path_run_buf

        graph_crawler_fetch(fw_crawler, node,
                            next_nodes, next_nucs, j, num_next,
                            flank5p_path->cols, flank5p_path->num_cols,
                            gcrawler_stop_at_ref_covg,
                            gcrawler_finish_ref_covg,
                            caller);

        // Assemble contigs - fetch forwards for each path for given 5p flank
        for(k = 0; k < fw_crawler->num_paths; k++)
        {
          // Fetch nodes
          db_node_buf_reset(nbuf);
          graph_crawler_get_path_nodes(fw_crawler, k, nbuf);
          multicol_path = &fw_crawler->multicol_paths[k];
          size_t pathid = multicol_path->pathid;

          // Fetch 3pflank ref position
          size_t num_flank3p_runs = caller->path_refs[pathid].num_runs;
          flank3p_runs = fetch_3pflank_ref_contact(caller, &fw_crawler->cache,
                                                   pathid);

          process_contig(caller, multicol_path->cols, multicol_path->num_cols,
                         flank5pbuf, nbuf,
                         koruns_5p->data, koruns_5p->len,
                         flank3p_runs, num_flank3p_runs);
        }
      }
    }
  }
}

void breakpoint_caller_node(hkey_t hkey, BreakpointCaller *caller)
{
  Edges edges;

  graph_crawler_reset(&caller->crawlers[0]);
  graph_crawler_reset(&caller->crawlers[1]);

  // check node is in the ref
  if(kograph_num(caller->kograph, hkey) > 0) {
    edges = db_node_get_edges(caller->db_graph, hkey, 0);
    if(edges_get_outdegree(edges, FORWARD) > 1) {
      follow_break(caller, (dBNode){.key = hkey, .orient = FORWARD});
    }
    if(edges_get_outdegree(edges, REVERSE) > 1) {
      follow_break(caller, (dBNode){.key = hkey, .orient = REVERSE});
    }
  }
}

static void breakpoint_caller(void *ptr)
{
  BreakpointCaller *caller = (BreakpointCaller*)ptr;
  ctx_assert(caller->db_graph->num_edge_cols == 1);

  HASH_ITERATE_PART(&caller->db_graph->ht, caller->threadid, caller->nthreads,
                    breakpoint_caller_node, caller);
}

static void breakpoints_print_header(gzFile gzout, const CmdArgs *args,
                                     const char **seq_paths, size_t nseq_paths)
{
  char datestr[9], cwd[PATH_MAX + 1];
  size_t i;

  ctx_assert(nseq_paths > 0);

  time_t date = time(NULL);
  strftime(datestr, 9, "%Y%m%d", localtime(&date));

  gzprintf(gzout, "##fileFormat=CtxBreakpointsv0.1\n##fileDate=%s\n", datestr);
  gzprintf(gzout, "##cmd=\"%s\"\n", args->cmdline);
  if(futil_get_current_dir(cwd) != NULL) gzprintf(gzout, "##wkdir=%s\n", cwd);

  gzprintf(gzout, "##reference=%s", seq_paths[0]);
  for(i = 1; i < nseq_paths; i++) gzprintf(gzout, ":%s", seq_paths[i]);
  gzputc(gzout, '\n');

  gzprintf(gzout, "##ctxVersion=\""VERSION_STATUS_STR"\"\n");
  gzprintf(gzout, "##ctxKmerSize=%i\n", MAX_KMER_SIZE);
}

void breakpoints_call(size_t num_of_threads,
                      const read_t *reads, size_t num_reads,
                      gzFile gzout, const char *out_path,
                      const char **seq_paths, size_t num_seq_paths,
                      size_t min_ref_flank, size_t max_ref_flank,
                      const CmdArgs *args, dBGraph *db_graph)
{
  KOGraph kograph = kograph_create(reads, num_reads, true,
                                   num_of_threads, db_graph);

  BreakpointCaller *callers = brkpt_callers_new(num_of_threads, gzout,
                                                min_ref_flank, max_ref_flank,
                                                kograph, db_graph);

  status("Running BreakpointCaller with %zu thread%s, output to: %s",
         num_of_threads, util_plural_str(num_of_threads),
         futil_outpath_str(out_path));

  status("  Finding breakpoints after at least %zu kmers (%zubp) of homology",
         min_ref_flank, min_ref_flank+db_graph->kmer_size-1);

  breakpoints_print_header(gzout, args, seq_paths, num_seq_paths);

  util_run_threads(callers, num_of_threads, sizeof(callers[0]),
                   num_of_threads, breakpoint_caller);

  char call_num_str[100];
  ulong_to_str(callers[0].callid[0], call_num_str);
  status("  %s calls printed to %s", call_num_str, futil_outpath_str(out_path));

  brkpt_callers_destroy(callers, num_of_threads);
  kograph_free(kograph);
}
