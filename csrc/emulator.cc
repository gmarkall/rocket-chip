// See LICENSE for license details.

#include "htif_emulator.h"
#include "emulator.h"
#include "mm.h"
#include "mm_dramsim2.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <boost/circular_buffer.hpp>

#define MEM_SIZE_BITS 3
#define MEM_LEN_BITS 8
#define MEM_RESP_BITS 2

/* In order to signal when start_trigger and stop_trigger are called, the BEEBS
 * board support functions perform a specific sequence of instructions that the
 * emulator watches out for. For the start_trigger function, it is:
 *
 * addi a0, a0, 0x45 ; 'E'
 * addi a0, a0, 0x4D ; 'M'
 * addi a0, a0, 0x42 ; 'B'
 * addi a0, a0, 0x45 ; 'E'
 *
 * and for the stop_trigger function, it is:
 *
 * addi a0, a0, 0x43 ; 'C'
 * addi a0, a0, 0x4F ; 'O'
 * addi a0, a0, 0x53 ; 'S'
 * addi a0, a0, 0x4D ; 'M'
 *
 * The MAGIC_STARTx and MAGIC_STOPx values are the encodings of these
 * instructions.
 */

#define MAGIC_LEN 4

#define MAGIC_START0 0x04550513
#define MAGIC_START1 0x04d50513
#define MAGIC_START2 0x04250513
#define MAGIC_START3 0x04550513

#define MAGIC_STOP0 0x04350513
#define MAGIC_STOP1 0x04f50513
#define MAGIC_STOP2 0x05350513
#define MAGIC_STOP3 0x04d50513

/* The MagicTracker keeps track of recently executed instructions, and watches
 * for the magic sequences. The emulator can check with the MagicTracker to
 * see if a magic sequence has just been encountered.
 */

class MagicTracker {
private:
  boost::circular_buffer<unsigned long> insts;
  bool needs_reset;
  bool needs_emit_cycle_count;

public:
  MagicTracker(): insts(MAGIC_LEN), needs_reset(false), needs_emit_cycle_count(false) {}

  void nextInst(dat_t<32> inst) {
    /* Only push the instruction into the buffer if there's none there yet,
     * which happens at startup, or if this instruction differs from the one on
     * the previous cycle (because instructions can take more than one cycle to
     * retire (or commit, or something).
     */
    if (insts.empty() || inst.to_ulong() != insts.back()) {
      insts.push_back(inst.to_ulong());

      /* Check if we're at a magic point. We only do this when we push a new
       * instruction (and not every cycle) because if the last instruction in
       * the magic sequence stays current for several cycles, we will think
       * we need to reset the counter or emit the cycle count for each of those
       * cycles.
       */

      if ( insts[0] == MAGIC_START0
        && insts[1] == MAGIC_START1
        && insts[2] == MAGIC_START2
        && insts[3] == MAGIC_START3)
      {
        needs_reset = true;
      }

      if ( insts[0] == MAGIC_STOP0
        && insts[1] == MAGIC_STOP1
        && insts[2] == MAGIC_STOP2
        && insts[3] == MAGIC_STOP3)
      {
        needs_emit_cycle_count = true;
      }
    }
  }

  /* Functions for the emulator to check if we're at a magic point */

  bool hitStart() {
    if (needs_reset) {
      needs_reset = false;
      return true;
    }

    return false;
  }

  bool hitStop() {
    if (needs_emit_cycle_count) {
      needs_emit_cycle_count = false;
      return true;
    }

    return false;
  }

};

htif_emulator_t* htif;
void handle_sigterm(int sig)
{
  htif->stop();
}

int main(int argc, char** argv)
{
  unsigned random_seed = (unsigned)time(NULL) ^ (unsigned)getpid();
  uint64_t max_cycles = -1;
  uint64_t trace_count = 0;
  uint64_t start = 0;
  int ret = 0;
  const char* vcd = NULL;
  const char* loadmem = NULL;
  FILE *vcdfile = NULL;
  bool dramsim2 = false;
  bool log = false;
  bool print_cycles = false;
  uint64_t memsz_mb = MEM_SIZE / (1024*1024);
  mm_t *mm[N_MEM_CHANNELS];
  MagicTracker tracker;

  for (int i = 1; i < argc; i++)
  {
    std::string arg = argv[i];
    if (arg.substr(0, 2) == "-v")
      vcd = argv[i]+2;
    else if (arg.substr(0, 9) == "+memsize=")
      memsz_mb = atoll(argv[i]+9);
    else if (arg.substr(0, 2) == "-s")
      random_seed = atoi(argv[i]+2);
    else if (arg == "+dramsim")
      dramsim2 = true;
    else if (arg == "+verbose")
      log = true;
    else if (arg.substr(0, 12) == "+max-cycles=")
      max_cycles = atoll(argv[i]+12);
    else if (arg.substr(0, 9) == "+loadmem=")
      loadmem = argv[i]+9;
    else if (arg.substr(0, 7) == "+start=")
      start = atoll(argv[i]+7);
    else if (arg.substr(0, 12) == "+cycle-count")
      print_cycles = true;
  }

  const int disasm_len = 24;
  if (vcd)
  {
    // Create a VCD file
    vcdfile = strcmp(vcd, "-") == 0 ? stdout : fopen(vcd, "w");
    assert(vcdfile);
    fprintf(vcdfile, "$scope module Testbench $end\n");
    fprintf(vcdfile, "$var reg %d NDISASM_WB wb_instruction $end\n", disasm_len*8);
    fprintf(vcdfile, "$var reg 64 NCYCLE cycle $end\n");
    fprintf(vcdfile, "$upscope $end\n");
  }


  // The chisel generated code
  Top_t tile;
  srand(random_seed);
  tile.init(random_seed);

  uint64_t mem_width = MEM_DATA_BITS / 8;

  // Instantiate and initialize main memory
  for (int i = 0; i < N_MEM_CHANNELS; i++) {
    mm[i] = dramsim2 ? (mm_t*)(new mm_dramsim2_t) : (mm_t*)(new mm_magic_t);
    try {
      mm[i]->init(memsz_mb*1024*1024 / N_MEM_CHANNELS, mem_width, CACHE_BLOCK_BYTES);
    } catch (const std::bad_alloc& e) {
      fprintf(stderr,
          "Failed to allocate %ld bytes (%ld MiB) of memory\n"
          "Set smaller amount of memory using +memsize=<N> (in MiB)\n",
              memsz_mb*1024*1024, memsz_mb);
      exit(-1);
    }
  }

  if (loadmem) {
    void *mems[N_MEM_CHANNELS];
    for (int i = 0; i < N_MEM_CHANNELS; i++)
      mems[i] = mm[i]->get_data();
    load_mem(mems, loadmem, CACHE_BLOCK_BYTES, N_MEM_CHANNELS);
  }

  // Instantiate HTIF
  htif = new htif_emulator_t(std::vector<std::string>(argv + 1, argv + argc));
  int htif_bits = tile.Top__io_host_in_bits.width();
  assert(htif_bits % 8 == 0 && htif_bits <= val_n_bits());

  signal(SIGTERM, handle_sigterm);

  // reset for one host_clk cycle to handle pipelined reset
  tile.Top__io_host_in_valid = LIT<1>(0);
  tile.Top__io_host_out_ready = LIT<1>(0);
  for (int i = 0; i < 3; i += tile.Top__io_host_clk_edge.to_bool())
  {
    tile.clock_lo(LIT<1>(1));
    tile.clock_hi(LIT<1>(1));
  }

  dat_t<1> *mem_ar_valid[N_MEM_CHANNELS];
  dat_t<1> *mem_ar_ready[N_MEM_CHANNELS];
  dat_t<MEM_ADDR_BITS> *mem_ar_bits_addr[N_MEM_CHANNELS];
  dat_t<MEM_ID_BITS> *mem_ar_bits_id[N_MEM_CHANNELS];
  dat_t<MEM_SIZE_BITS> *mem_ar_bits_size[N_MEM_CHANNELS];
  dat_t<MEM_LEN_BITS> *mem_ar_bits_len[N_MEM_CHANNELS];

  dat_t<1> *mem_aw_valid[N_MEM_CHANNELS];
  dat_t<1> *mem_aw_ready[N_MEM_CHANNELS];
  dat_t<MEM_ADDR_BITS> *mem_aw_bits_addr[N_MEM_CHANNELS];
  dat_t<MEM_ID_BITS> *mem_aw_bits_id[N_MEM_CHANNELS];
  dat_t<MEM_SIZE_BITS> *mem_aw_bits_size[N_MEM_CHANNELS];
  dat_t<MEM_LEN_BITS> *mem_aw_bits_len[N_MEM_CHANNELS];

  dat_t<1> *mem_w_valid[N_MEM_CHANNELS];
  dat_t<1> *mem_w_ready[N_MEM_CHANNELS];
  dat_t<MEM_DATA_BITS> *mem_w_bits_data[N_MEM_CHANNELS];
  dat_t<MEM_STRB_BITS> *mem_w_bits_strb[N_MEM_CHANNELS];
  dat_t<1> *mem_w_bits_last[N_MEM_CHANNELS];

  dat_t<1> *mem_b_valid[N_MEM_CHANNELS];
  dat_t<1> *mem_b_ready[N_MEM_CHANNELS];
  dat_t<MEM_RESP_BITS> *mem_b_bits_resp[N_MEM_CHANNELS];
  dat_t<MEM_ID_BITS> *mem_b_bits_id[N_MEM_CHANNELS];

  dat_t<1> *mem_r_valid[N_MEM_CHANNELS];
  dat_t<1> *mem_r_ready[N_MEM_CHANNELS];
  dat_t<MEM_RESP_BITS> *mem_r_bits_resp[N_MEM_CHANNELS];
  dat_t<MEM_ID_BITS> *mem_r_bits_id[N_MEM_CHANNELS];
  dat_t<MEM_DATA_BITS> *mem_r_bits_data[N_MEM_CHANNELS];
  dat_t<1> *mem_r_bits_last[N_MEM_CHANNELS];

#include TBFRAG

  while (!htif->done() && trace_count < max_cycles && ret == 0)
  {
    for (int i = 0; i < N_MEM_CHANNELS; i++) {
      *mem_ar_ready[i] = LIT<1>(mm[i]->ar_ready());
      *mem_aw_ready[i] = LIT<1>(mm[i]->aw_ready());
      *mem_w_ready[i] = LIT<1>(mm[i]->w_ready());

      *mem_b_valid[i] = LIT<1>(mm[i]->b_valid());
      *mem_b_bits_resp[i] = LIT<64>(mm[i]->b_resp());
      *mem_b_bits_id[i] = LIT<64>(mm[i]->b_id());

      *mem_r_valid[i] = LIT<1>(mm[i]->r_valid());
      *mem_r_bits_resp[i] = LIT<64>(mm[i]->r_resp());
      *mem_r_bits_id[i] = LIT<64>(mm[i]->r_id());
      *mem_r_bits_last[i] = LIT<1>(mm[i]->r_last());

      memcpy(mem_r_bits_data[i]->values, mm[i]->r_data(), mem_width);
    }

    try {
      tile.clock_lo(LIT<1>(0));
    } catch (std::runtime_error& e) {
      max_cycles = trace_count; // terminate cleanly after this cycle
      ret = 1;
      std::cerr << e.what() << std::endl;
    }

    for (int i = 0; i < N_MEM_CHANNELS; i++) {
      mm[i]->tick(
        mem_ar_valid[i]->to_bool(),
        mem_ar_bits_addr[i]->lo_word() - MEM_BASE,
        mem_ar_bits_id[i]->lo_word(),
        mem_ar_bits_size[i]->lo_word(),
        mem_ar_bits_len[i]->lo_word(),

        mem_aw_valid[i]->to_bool(),
        mem_aw_bits_addr[i]->lo_word() - MEM_BASE,
        mem_aw_bits_id[i]->lo_word(),
        mem_aw_bits_size[i]->lo_word(),
        mem_aw_bits_len[i]->lo_word(),

        mem_w_valid[i]->to_bool(),
        mem_w_bits_strb[i]->lo_word(),
        mem_w_bits_data[i]->values,
        mem_w_bits_last[i]->to_bool(),

        mem_r_ready[i]->to_bool(),
        mem_b_ready[i]->to_bool()
      );
    }

    if (tile.Top__io_host_clk_edge.to_bool())
    {
      static bool htif_in_valid = false;
      static val_t htif_in_bits;
      if (tile.Top__io_host_in_ready.to_bool() || !htif_in_valid)
        htif_in_valid = htif->recv_nonblocking(&htif_in_bits, htif_bits/8);
      tile.Top__io_host_in_valid = LIT<1>(htif_in_valid);
      tile.Top__io_host_in_bits = LIT<64>(htif_in_bits);

      if (tile.Top__io_host_out_valid.to_bool())
        htif->send(tile.Top__io_host_out_bits.values, htif_bits/8);
      tile.Top__io_host_out_ready = LIT<1>(1);
    }

    if (log && trace_count >= start)
      tile.print(stderr);

    /* Added functionality to reset / emit cycle count if necessary */

    tracker.nextInst(tile.getInst());
    if (tracker.hitStart()) {
      printf("Emulator: resetting cycle count\n");
      trace_count = 0;
    }

    if (tracker.hitStop()) {
      printf("Emulator: Cycle count is %d\n", trace_count);
    }

    /* End added functionality */

    // make sure we dump on cycle 0 to get dump_init
    if (vcd && (trace_count == 0 || trace_count >= start))
      tile.dump(vcdfile, trace_count);

    tile.clock_hi(LIT<1>(0));
    trace_count++;
  }

  if (vcd)
    fclose(vcdfile);

  if (htif->exit_code())
  {
    fprintf(stderr, "*** FAILED *** (code = %d, seed %d) after %ld cycles\n", htif->exit_code(), random_seed, trace_count);
    ret = htif->exit_code();
  }
  else if (trace_count == max_cycles)
  {
    fprintf(stderr, "*** FAILED *** (timeout, seed %d) after %ld cycles\n", random_seed, trace_count);
    ret = 2;
  }
  else if (log || print_cycles)
  {
    fprintf(stderr, "Completed after %ld cycles\n", trace_count);
  }

  delete htif;

  return ret;
}
