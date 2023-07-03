//
//  Copyright (c) 2023, Rice University
//  All rights reserved.
//  
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are met:
//  
//  1. Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//  
//  2. Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//  
//  3. Neither the name of the copyright holder nor the names of its
//     contributors may be used to endorse or promote products derived from
//     this software without specific prior written permission.
//  
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
//  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
//  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// ----------------------------------------------------------------------
//
//  This program uses the Dyninst unknown instruction callback to
//  compare dyninst's handling of x86_64 instructions with XED.
//  Test for three things.
//
//    1. Unknown instructions that dyninst doesn't recognize but XED
//    says are valid.
//
//    2. Instructions that dyninst accepts but have the wrong length
//    according to XED.  This can also appear as a trolled region from
//    an unknown instn where dyninst mis-parses the previous instn.
//
//    3. Unclaimed regions (gaps) between basic blocks.
//
//  Note: this test uses XED and only runs on x86_64.
//
//  Mark W. Krentel
//  Rice University
//  June 2023
//
// ----------------------------------------------------------------------
//
//  Usage:
//    ./unknown-x86  [options]...  filename
//
//  Options:
//    -j num        use num openmp threads for parse phase (default 1)
//    -q            turn off all output except for summary
//    --fix         attempt to fix unknown instructions (default no)
//    --fix-all     attempt to fix all unknown and trolled instructions
//    --no-fix      do not fix any instructions
//    -h, --help    display usage message and exit
//
// ----------------------------------------------------------------------

#include <sys/types.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <omp.h>

#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <mutex>

#include <CFG.h>
#include <CodeObject.h>
#include <CodeSource.h>
#include <Function.h>
#include <Symtab.h>
#include <Instruction.h>
#include <InstructionDecoder.h>

extern "C" {
#include <xed-interface.h>
}

using namespace Dyninst;
using namespace ParseAPI;
using namespace SymtabAPI;
using namespace InstructionAPI;
using namespace std;

Symtab * the_symtab = NULL;

static mutex print_mutex;
static int initial_parse = 1;

//----------------------------------------------------------------------

// Summary stats

static long num_blocks = 0;
static long num_instns = 0;
static long num_bytes = 0;

static long num_unknown = 0;
static long num_unknown_valid = 0;
static long num_unknown_troll = 0;
static long num_unknown_error = 0;

static long num_bad_length = 0;
static long num_block_align_errors = 0;
static long num_block_length_errors = 0;

static long num_gaps = 0;
static long num_gaps_16 = 0;
static long num_gaps_64 = 0;
static long num_gaps_256 = 0;
static long num_gaps_other = 0;
static long num_overlap = 0;

static long size_gaps = 0;
static long size_gaps_16 = 0;
static long size_gaps_64 = 0;
static long size_gaps_256 = 0;
static long size_gaps_other = 0;

//----------------------------------------------------------------------

// Sort Functions by entry address, low to high.
static bool
FuncLessThan(ParseAPI::Function * f1, ParseAPI::Function * f2)
{
    return f1->addr() < f2->addr();
}

// Sort Blocks by start address, low to high.
static bool
BlockLessThan(Block * b1, Block * b2)
{
    return b1->start() < b2->start();
}

//----------------------------------------------------------------------

// Command-line options
class Options {
public:
    const char *filename;
    int   jobs;
    bool  quiet;
    bool  verbose;
    bool  fix_valid;
    bool  fix_troll;

    Options() {
	filename = NULL;
	jobs = 1;
	quiet = false;
	verbose = false;
	fix_valid = false;
	fix_troll = false;
    }
};

Options opts;

//----------------------------------------------------------------------

void
usage(string mesg)
{
    if (! mesg.empty()) {
	cout << "error: " << mesg << "\n\n";
    }

    cout << "usage:  unknown-x86  [options]...  filename\n\n"
	 << "options:\n"
	 << "  -j num        use num openmp threads for parse phase (default 1)\n"
	 << "  -q            turn off all output except for summary\n"
	 << "  --fix         attempt to fix unknown instructions (default no)\n"
	 << "  --fix-all     attempt to fix all unknown and trolled instructions\n"
	 << "  --no-fix      do not fix any instructions\n"
	 << "  -h, --help    display usage message and exit\n"
	 << "\n";

    exit(1);
}

// Command-line:  [options] ...  filename
void
getOptions(int argc, char **argv, Options & opts)
{
    int n = 1;

    while (n < argc) {
	string arg(argv[n]);

	if (arg == "-h" || arg == "-help" || arg == "--help") {
	    usage("");
	}
	else if (arg == "-j") {
	    if (n + 1 >= argc) {
	        usage("missing arg for -j");
	    }
	    opts.jobs = atoi(argv[n + 1]);
	    if (opts.jobs <= 0 || opts.jobs > 550) {
	        usage(string("bad arg for -j: ") + argv[n + 1]);
	    }
	    n += 2;
	}
	else if (arg == "-q") {
	    opts.quiet = true;
	    n++;
	}
	else if (arg == "-v") {
	    opts.verbose = true;
	    n++;
	}
	else if (arg == "-fix" || arg == "--fix") {
	    opts.fix_valid = true;
	    opts.fix_troll = false;
	    n++;
	}
	else if (arg == "-fix-all" || arg == "--fix-all") {
	    opts.fix_valid = true;
	    opts.fix_troll = true;
	    n++;
	}
	else if (arg == "-no-fix" || arg == "--no-fix") {
	    opts.fix_valid = false;
	    opts.fix_troll = false;
	    n++;
	}
	else if (arg[0] == '-') {
	    usage("invalid option: " + arg);
	}
	else {
	    break;
	}
    }

    // filename (required)
    if (n < argc) {
	opts.filename = argv[n];
    }
    else {
	usage("missing file name");
    }
}

//----------------------------------------------------------------------

// Verify invalid Dyninst buffers for valid XED instructions.
// Three possibilities:
//
//  1. XED says valid instruction at beginning of buffer.
//    This is an instruction that dyninst doesn't know about.
//
//  2. XED says invalid, but skip ahead a few bytes (troll) and XED
//    says valid.  Likely dyninst has the length wrong on the previous
//    instruction.
//
//  3. XED says error and trolling doesn't find anything.
//
// If Dyninst doesn't recognize an op code, it should show up as (1).
// If it thinks it does but gets it wrong, it will show up as (2) or (3).
//
#define MY_BUF_SIZE (XED_MAX_INSTRUCTION_BYTES + 4)

static int num_xed_errors = 0;

InstructionAPI::Instruction
myXedCallback(InstructionDecoder::buffer seqn)
{
    uint8_t buf[MY_BUF_SIZE];
    Instruction ret;

    // copy into array of uint8_t for xed
    int buf_len = 0;
    for (auto p = seqn.start; p != seqn.end; ++p) {
	buf[buf_len] = (uint8_t) *p;
	buf_len++;

	if (buf_len >= MY_BUF_SIZE) {
	    break;
	}
    }

    xed_decoded_inst_t xedd;
    xed_state_t dstate;
    unsigned int xed_len = 0;
    unsigned int start = 0;
    bool is_valid = false, is_troll = false;

    // test beginning of buffer
    xed_state_zero(&dstate);
    dstate.mmode = XED_MACHINE_MODE_LONG_64;
    xed_decoded_inst_zero_set_mode(&xedd, &dstate);
    int xed_error = xed_decode(&xedd, buf, buf_len);

    if (xed_error == XED_ERROR_NONE) {
	//
	// case 1 - valid instruction at beginning of buffer
	// return a dyninst fake no-op, all we care about is the length,
	// since we don't expect any control flow here
	//
	xed_len = xed_decoded_inst_get_length(&xedd);
	is_valid = true;
	if (opts.fix_valid) {
	    ret = Instruction {
		{ e_nop, "nop", Arch_x86_64 },
		xed_len,
		seqn.start,
		Arch_x86_64
	    };
	} else {
	    ret = Instruction{};
	}
    }
    else {
	// try trolling
	for (start = 1; start < buf_len; start++) {
	    xed_state_zero(&dstate);
	    dstate.mmode = XED_MACHINE_MODE_LONG_64;
	    xed_decoded_inst_zero_set_mode(&xedd, &dstate);
	    xed_error = xed_decode(&xedd, buf + start, buf_len - start);

	    if (xed_error == XED_ERROR_NONE) {
		//
		// case 2 -- out of sync instn starting at buf[start].
		// return fake no-op and let dyninst get back in sync
		//
		xed_len = xed_decoded_inst_get_length(&xedd);
		is_troll = true;
		if (opts.fix_troll) {
		    ret = Instruction {
			{ e_nop, "nop", Arch_x86_64 },
			start,
			seqn.start,
			Arch_x86_64
		    };
		} else {
		    ret = Instruction{};
		}
		break;
	    }
	}
	if (xed_error != XED_ERROR_NONE) {
	    //
	    // case 3 -- not a valid instruction, trolling failed
	    // return an invalid instruction
	    //
	    ret = Instruction{};
	}
    }

    // sometime fixing trolls is dangerous, don't allow an infinite
    // string of errors
    if (xed_error != XED_ERROR_NONE) {
	num_xed_errors++;

	if (num_xed_errors > 20) {
	    cout << "\nexceeded num xed errors: " << num_xed_errors << "\n" << endl;
	    exit(1);
	}
    }
    else {
	num_xed_errors = 0;
    }

    // serialize the output to allow for multiple threads
    // we could sprintf() to a buffer and then dump all at once
    print_mutex.lock();

    // only count and report errors on initial parse.  splitting a
    // block into instructions causes duplicate calls here.
    if (initial_parse && ! opts.quiet) {
	printf("unknown: ");

	for (int i = 0; i < buf_len; i++) {
	    printf(" %02x", buf[i]);
	}
	if (is_valid) {
	    printf("  valid: %d%s\n", xed_len,
		   opts.fix_valid ? "  (fix)" : "");

	}
	else if (is_troll) {
	    printf("  troll: %d  len: %d%s\n", start, xed_len,
		   opts.fix_troll ? "  (fix)" : "");
	}
	else {
	    printf("  error\n");
	}
    }

    if (initial_parse) {
	num_unknown++;
	if (is_valid) { num_unknown_valid++; }
	else if (is_troll) { num_unknown_troll++; }
	else { num_unknown_error++; }
    }

    print_mutex.unlock();

    return ret;
}

//----------------------------------------------------------------------

// Iterate the instructions in a block and compare the length of each
// instruction with xed's length.  Also, make sure there are no gaps
// between instructions (rarely happens, but dyninst error if it does).
//
// Note: we only report one error per block.  After that, we consider
// the block to be corrupted and not worth testing any further.
//
void
doBlock(Block * block)
{
    Address block_start = block->start();
    long block_size = block->size();
    num_bytes += block_size;

    //
    // step 1 -- malloc buffer for entire block plus one instruction
    // in case xed length is longer than dyninst length.
    //
    long buf_size = block_size + 20;
    uint8_t * buf = (uint8_t *) malloc(buf_size);

    if (buf == NULL) {
	err(1, "malloc buffer in doBlock failed, size: %ld", buf_size);
    }
    memset(buf, 0, buf_size);

    //
    // step 2 -- iterate instructions and fill in buffer,
    // check instructions are all adjacent.
    //
    Block::Insns imap;
    block->getInsns(imap);
    num_instns += imap.size();

    long pos = 0;
    for (auto iit = imap.begin(); iit != imap.end(); ++iit) {
	Address addr = iit->first;
	Offset dyn_len = iit->second.size();

	if (block_start + pos != addr) {
	    if (! opts.quiet) {
		printf("block error (align): 0x%lx  offset: 0x%lx  next: 0x%lx\n",
		       block_start, pos, addr);
	    }
	    num_block_align_errors++;
	    goto end_block;
	}
	if (pos + dyn_len > block_size) {
	    if (! opts.quiet) {
		printf("block error (too long): 0x%lx  offset: 0x%lx  size: 0x%lx  len: 0x%lx\n",
		       block_start, pos, dyn_len, block_size);
	    }
	    num_block_length_errors++;
	    goto end_block;
	}

	for (int n = 0; n < dyn_len; n++) {
	    buf[pos + n] = (uint8_t) iit->second.rawByte(n);
	}
	pos += dyn_len;
    }

    //
    // step 3 -- iterate instructions and compare length with xed
    //
    for (auto iit = imap.begin(); iit != imap.end(); ++iit) {
	Address addr = iit->first;
	Offset dyn_len = iit->second.size();
	xed_decoded_inst_t xedd;
	xed_state_t dstate;

	xed_state_zero(&dstate);
	dstate.mmode = XED_MACHINE_MODE_LONG_64;
	xed_decoded_inst_zero_set_mode(&xedd, &dstate);
	int xed_error = xed_decode(&xedd, &buf[addr - block_start], 16);

	long xed_len =
	    (xed_error == XED_ERROR_NONE) ? xed_decoded_inst_get_length(&xedd) : 0;

	if (xed_error != XED_ERROR_NONE || dyn_len != xed_len) {
	    if (! opts.quiet) {
		printf("bad length at 0x%lx: ", addr);
		for (int i = 0; i < 16; i++) {
		    printf(" %02x", buf[addr - block_start + i]);
		}
		printf("  dyn: %ld  xed: %ld\n", dyn_len, xed_len);
	    }
	    num_bad_length++;
	    goto end_block;
	}
    }

 end_block:
    free(buf);

    return;
}

//----------------------------------------------------------------------

void
doFunction(ParseAPI::Function * func)
{
    // get map of visited blocks and convert to vector
    const ParseAPI::Function::blocklist & blist = func->blocks();
    vector <Block *> blockVec;

    for (auto bit = blist.begin(); bit != blist.end(); ++bit) {
	Block * block = *bit;
	blockVec.push_back(block);
    }
    num_blocks += blockVec.size();

    // sort by block start address
    std::sort(blockVec.begin(), blockVec.end(), BlockLessThan);

    for (long n = 0; n < blockVec.size(); n++) {
	Block * block = blockVec[n];
	doBlock(block);
    }
}

//----------------------------------------------------------------------

// Search for unclaimed regions (gaps) between basic blocks.  Some
// compilers insert cold regions inside other functions, so we need to
// analyze all blocks together.
//
void
doGaps(vector <ParseAPI::Function *> & funcVec)
{
    // get list of all blocks and sort by start address
    vector <Block *> blockVec;

    for (auto fit = funcVec.begin(); fit != funcVec.end(); ++fit) {
	ParseAPI::Function * func = *fit;
	const ParseAPI::Function::blocklist & blist = func->blocks();

	for (auto bit = blist.begin(); bit != blist.end(); ++bit) {
	    Block * block = *bit;
	    blockVec.push_back(block);
	}
    }

    std::sort(blockVec.begin(), blockVec.end(), BlockLessThan);

    //
    // compare adjacent blocks
    //
    Block * prev_block = blockVec[0];

    for (long n = 1; n < blockVec.size(); n++) {
	Block * block = blockVec[n];
	long size = block->start() - prev_block->end();

	if (size > 0) {
	    if (! opts.quiet) {
		cout << "gap: prev block: 0x" << hex << prev_block->start()
		     << "  end: 0x" << prev_block->end()
		     << "  next: 0x" << block->start()
		     << "  size: 0x" << size
		     << dec << " (" << size << ")\n";
	    }
	    num_gaps++;
	    size_gaps += size;

	    if (size < 16) {
		num_gaps_16++;
		size_gaps_16 += size;
	    }
	    else if (size < 64) {
		num_gaps_64++;
		size_gaps_64 += size;
	    }
	    else if (size < 256) {
		num_gaps_256++;
		size_gaps_256 += size;
	    }
	    else {
		num_gaps_other++;
		size_gaps_other += size;
	    }
	}
	else if (size < 0) {
	    //
	    // overlap or duplicate blocks
	    //
	    if (! opts.quiet) {
		cout << "overlap: prev end: 0x" << hex << prev_block->end()
		     << "  begin: 0x" << block->start()
		     << "  end: 0x" << block->end() << dec << "\n";
	    }
	    num_overlap++;
	}

	prev_block = block;
    }
}

//----------------------------------------------------------------------

int
main(int argc, char **argv)
{
    getOptions(argc, argv, opts);

    const char * nl = (! opts.quiet) ? "\n" : "";

    cout << "file: " << opts.filename << "\n"
	 << "threads: " << opts.jobs
	 << "  fix valid: " << opts.fix_valid
	 << "  fix troll: " << opts.fix_troll << endl;

    xed_tables_init();

    // this is only for the dyninst parse() phase
    omp_set_num_threads(opts.jobs);

    cout << "\nreading file: " << opts.filename << " ..." << endl;

    if (! Symtab::openFile(the_symtab, opts.filename)) {
	errx(1, "Symtab::openFile (on disk) failed: %s", opts.filename);
    }

    // ------------------------------------------------------------
    // Phase 1 -- test for unknown instructions
    // ------------------------------------------------------------
    cout << nl << "phase 1 -- parse binary and test for unknown instructions ..."
	 << nl << endl;

    // enable callback
    InstructionDecoder::unknown_instruction::register_callback(&myXedCallback);
    initial_parse = 1;

    the_symtab->parseTypesNow();
    the_symtab->parseFunctionRanges();

    SymtabCodeSource * code_src = new SymtabCodeSource(the_symtab);
    CodeObject * code_obj = new CodeObject(code_src);

    code_obj->parse();

    // ------------------------------------------------------------
    // Phase 2 -- test for "known" instructions with wrong length
    // ------------------------------------------------------------
    cout << nl << "phase 2 -- test known instructions for bad length ..." << nl << endl;

    // we have to keep the callback in place to be consistent for
    // fixed instructions, but turn off counting unknown instructions
    initial_parse = 0;

    // put function list into vector and sort by entry address
    const CodeObject::funclist & funcList = code_obj->funcs();
    vector <ParseAPI::Function *> funcVec;

    for (auto fit = funcList.begin(); fit != funcList.end(); ++fit) {
	ParseAPI::Function * func = *fit;
	funcVec.push_back(func);
    }

    std::sort(funcVec.begin(), funcVec.end(), FuncLessThan);

    for (long n = 0; n < funcVec.size(); n++) {
	ParseAPI::Function * func = funcVec[n];
	doFunction(func);
    }

    // ------------------------------------------------------------
    // Phase 3 -- test for gaps between basic blocks
    // ------------------------------------------------------------
    cout << nl << "phase 3 -- test for gaps between blocks ..." << nl << endl;

    doGaps(funcVec);

    // ------------------------------------------------------------
    // Summary of results
    // ------------------------------------------------------------
    printf("\nSummary:\n");

    printf("\nfile: %s\n"
	   "threads: %d  fix valid: %d  fix troll: %d\n",
	   opts.filename, opts.jobs, opts.fix_valid, opts.fix_troll);

    printf("\nfuncs: %ld  blocks: %ld  instns: %ld  bytes: %ld\n",
	   funcVec.size(), num_blocks, num_instns, num_bytes);

    printf("\nunknown: %ld  valid: %ld  troll: %ld  error: %ld\n",
	   num_unknown, num_unknown_valid, num_unknown_troll, num_unknown_error);

    printf("\nnum bad length: %ld\n", num_bad_length);
    if (num_block_align_errors > 0 || num_block_length_errors > 0) {
	printf("num align errors: %ld   num length errors: %ld\n",
	       num_block_align_errors, num_block_length_errors);
    }

    printf("\nnum gaps: %8ld    size: %10ld\n"
	   "under 16: %8ld    size: %10ld\n"
	   "under 64: %8ld    size: %10ld\n"
	   "under 256: %7ld    size: %10ld\n"
	   "other:    %8ld    size: %10ld\n"
	   "num blocks overlap:  %ld\n",
	   num_gaps, size_gaps, num_gaps_16, size_gaps_16,
	   num_gaps_64, size_gaps_64, num_gaps_256, size_gaps_256,
	   num_gaps_other, size_gaps_other, num_overlap);

    cout << endl;

    return 0;
}
