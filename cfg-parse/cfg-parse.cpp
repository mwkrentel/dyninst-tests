//
//  SPDX-License-Identifier: MIT
//
//  Copyright (c) 2017-2025, Rice University.
//  All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all
//  copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//  SOFTWARE.
//
// ----------------------------------------------------------------------
//
//  This program is a proxy for the queries that hpcstruct makes of
//  Dyninst SymtabAPI and ParseAPI.  It does two main things:
//
//    1. It dumps the raw, unprocessed data that Dyninst presents to
//    hpcstruct.  This includes functions, basic blocks, outgoing
//    edges, statements (instructions), inline sequences and line map
//    info.
//
//    2. It checks for non-deterministic output.  We sort the output
//    by VMA addr, so it should appear deterministic.  If two runs
//    produce different output, there's a good chance that one of them
//    is incorrect.
//
//  Doesn't include loops (not yet), but irreducible loops would
//  likely be non-det unless they are always broken in a consistent,
//  deterministic manner.
//
//  Mark W. Krentel
//  Rice University
//  November 2025
//
// ----------------------------------------------------------------------
//
//  Build me as a Dyninst app with -fopenmp with the mk-test.sh
//  script.
//
//  Usage:
//    cfg-parse  [-j num-threads]  [+/-options] ...  filename
//
//  There are several options for including or excluding blocks,
//  statements, inline, etc to focus on one area of non-det.  See the
//  usage message with 'cfg-parse -h' for the full list.
//
// ----------------------------------------------------------------------

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <omp.h>

#include <algorithm>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>
// #include <mutex>

#include <CFG.h>
#include <CodeObject.h>
#include <CodeSource.h>
#include <Function.h>
#include <Symtab.h>
#include <Instruction.h>
#include <LineInformation.h>

#define MAX_THREADS  550

using namespace Dyninst;
using namespace ParseAPI;
using namespace SymtabAPI;
using namespace InstructionAPI;
using namespace std;

typedef vector <Statement *> StatementVector;
typedef unsigned int uint;
typedef unsigned long ulong;

// used inside <<, so no outer parens
#define HEX(val)  "0x" << std::hex << val << std::dec

Symtab * the_symtab = NULL;

//----------------------------------------------------------------------

// Command-line options
class Options {
public:
    const char *filename;
    int	  jobs_symtab;
    int	  jobs_parse;
    int	  jobs_struct;
    bool  show_blocks;
    bool  show_stmts;
    bool  show_inline;
    bool  show_linemap;
    bool  show_time;

  Options() {
	filename = NULL;
	jobs_symtab = 4;
	jobs_parse  = 4;
	jobs_struct = 1;
	show_blocks = true;
	show_stmts = true;
	show_inline = true;
	show_linemap = true;
	show_time = false;
    }
};

Options opts;

//----------------------------------------------------------------------

// FLP: file name, line number, proc name for inline sequence
class InlineNode {
public:
    std::string	 filenm;
    std::string	 procnm;
    ulong  line;

    InlineNode(std::string file, std::string proc, ulong ln)
    {
	filenm = file;
	procnm = proc;
	line = ln;
    }
};

//----------------------------------------------------------------------

// Order Blocks by start address, low to high.
static bool
BlockLessThan(Block * b1, Block * b2)
{
    return b1->start() < b2->start();
}

// Order Edges by target address, low to high.
static bool
EdgeLessThan(Edge * e1, Edge * e2)
{
    return e1->trg()->start() < e2->trg()->start();
}

// Order Functions by entry address, low to high.
static bool
FuncLessThan(ParseAPI::Function * f1, ParseAPI::Function * f2)
{
    return f1->addr() < f2->addr();
}

//----------------------------------------------------------------------

static string
edgeType(int type)
{
    if (type == ParseAPI::CALL)            { return "call"; }
    if (type == ParseAPI::COND_TAKEN)      { return "cond-take"; }
    if (type == ParseAPI::COND_NOT_TAKEN)  { return "cond-not"; }
    if (type == ParseAPI::INDIRECT)        { return "indirect"; }
    if (type == ParseAPI::DIRECT)          { return "direct"; }
    if (type == ParseAPI::FALLTHROUGH)     { return "fallthr"; }
    if (type == ParseAPI::CATCH)           { return "catch"; }
    if (type == ParseAPI::CALL_FT)         { return "call-ft"; }
    if (type == ParseAPI::RET)             { return "return"; }
    return "unknown";
}

//----------------------------------------------------------------------

// Returns: true if block belongs to a noreturn function.
static bool
isNoreturn(Block * block)
{
    vector <ParseAPI::Function *> Funcs;
    block->getFuncs(Funcs);

    bool ans = false;
    for (ulong i = 0; i < Funcs.size(); i++) {
	if (Funcs[i]->retstatus() == ParseAPI::NORETURN) {
	    ans = true;
	    break;
	}
    }

    return ans;
}

//----------------------------------------------------------------------

//
// Line Map Info -- use only the module containing 'addr'.  Sometimes
// other modules have other, bogus entries for the same address
//
void
getLineMapInfo(StatementVector & svec, Offset addr)
{
    //
    // hpcstruct looks up the Symtab function containing 'addr' and
    // then module, which might be slightly different.
    //
    Module * mod = the_symtab->getContainingModule(addr);
    svec.clear();

    if (mod != NULL) {
	mod->getSourceLines(svec, addr);
    }
}

//----------------------------------------------------------------------

void
doInstruction(Offset addr, Instruction instn)
{
    cout << "stmt:  " << HEX(addr) << " (" << instn.size() << ")";

    // terminal linemap info
    if (opts.show_linemap) {
	StatementVector svec;
	string filenm = "";
	int line = 0;

	getLineMapInfo(svec, addr);

	if (! svec.empty()) {
	    filenm = svec[0]->getFile();
	    line = svec[0]->getLine();
	}
	cout << "  l=" << line << "  f='" << filenm << "'";
    }
    cout << "\n";

    // inline sequence
    if (opts.show_inline) {
	SymtabAPI::FunctionBase *func, *parent;

	if (the_symtab->getContainingInlinedFunction(addr, func) && func != NULL)
	{
	    // we get the inline sequence inside-out (bottom-up) but
	    // we present it in top-down call order like hpcstruct
	    //
	    list <InlineNode> inlineSeqn;

	    parent = func->getInlinedParent();
	    while (parent != NULL) {
		//
		// func is inlined iff it has a parent
		//
		InlinedFunction * ifunc = static_cast <InlinedFunction *> (func);
		pair <string, Offset> callsite = ifunc->getCallsite();

		inlineSeqn.push_front(
		    InlineNode(callsite.first, func->getName(), callsite.second));

		func = parent;
		parent = func->getInlinedParent();
	    }

	    // present the sequence top-down
	    for (auto nit = inlineSeqn.begin(); nit != inlineSeqn.end(); ++nit) {
		cout << "    inline:  l=" << nit->line
		     << "  f='" << nit->filenm << "'"
		     << "  p='" << nit->procnm << "'\n";
	    }
	}
    }
}

//----------------------------------------------------------------------

void
doBlock(Block * block)
{
    Block::Insns imap;
    block->getInsns(imap);

    // basic blocks
    if (opts.show_blocks) {
	int num_funcs = block->containingFuncs();

	cout << "\nblock: " << HEX(block->start()) << "--" << HEX(block->end())
	     << " (" << imap.size() << ", " << block->size() << ")";

	if (num_funcs > 1) {
	    vector <ParseAPI::Function *> Funcs;
	    block->getFuncs(Funcs);

	    std::sort(Funcs.begin(), Funcs.end(), FuncLessThan);

	    cout << "  funcs: (" << num_funcs << ")";

	    for (int i = 0; i < num_funcs; i++) {
		ParseAPI::Function * func = Funcs[i];
		cout << "  " << HEX(func->addr());
	    }
	}
	cout << "\n";
    }

    // statements (instructions)
    if (opts.show_stmts) {
	for (auto iit = imap.begin(); iit != imap.end(); ++iit) {
	    Offset addr = iit->first;
	    Instruction instn = iit->second;
	    doInstruction(addr, instn);
	}
    }

    // out edges
    if (opts.show_blocks) {
	const Block::edgelist & outEdges = block->targets();
	vector <Edge *> edgeVec;

	for (auto eit = outEdges.begin(); eit != outEdges.end(); ++eit) {
	    edgeVec.push_back(*eit);
	}
	std::sort(edgeVec.begin(), edgeVec.end(), EdgeLessThan);

	cout << "out edges: " << HEX(block->last())
	     << " (" << edgeVec.size() << ")";

	for (auto eit = edgeVec.begin(); eit != edgeVec.end(); ++eit) {
	    Edge * edge = *eit;
	    Block * target = edge->trg();

	    cout << "  " << HEX(target->start())
		 << " (" << edgeType(edge->type());

	    if (edge->interproc()) {
		cout << ", interproc";
	    }
	    if (edge->type() == ParseAPI::CALL && isNoreturn(target)) {
		cout << ", noreturn";
	    }
	    cout << ")";
	}
	cout << "\n";
    }
}

//----------------------------------------------------------------------

void
doFunction(ParseAPI::Function * func)
{
    // vector of blocks, sort by address
    const ParseAPI::Function::blocklist & blist = func->blocks();
    vector <Block *> blockVec;
    long bytes = 0;

    for (auto bit = blist.begin(); bit != blist.end(); ++bit) {
	Block * block = *bit;
	blockVec.push_back(block);
	bytes += block->size();
    }

    cout << "\n--------------------------------------------------\n"
	 << "func:  " << HEX(func->addr());

    if (opts.show_blocks) {
	cout << "  (" << blockVec.size() << ", " << bytes;
	if (func->retstatus() == ParseAPI::NORETURN) {
	    cout << ", noreturn";
	}
	cout << ")";
    }
    cout << "  " << func->name() << "\n";

    // adjust blank lines, depending on output
    if (opts.show_stmts && ! opts.show_blocks) {
	cout << "\n";
    }

    std::sort(blockVec.begin(), blockVec.end(), BlockLessThan);

    for (ulong n = 0; n < blockVec.size(); n++) {
	doBlock(blockVec[n]);
    }
}

//----------------------------------------------------------------------

void
usage(string mesg)
{
    if (! mesg.empty()) {
	cout << "error: " << mesg << "\n\n";
    }

    cout << "usage:  cfg-parse  [options]...  filename\n\n"
	 << "options:\n"
	 << "  -j, --jobs num          num omp threads for all phases\n"
	 << "  --jobs-symtab num       num threads for symtab and line map\n"
	 << "  --jobs-parse num        num threads for parse phase\n"
	 << "  -A, +A                  disable (enable) all optional output\n"
	 << "  -B, +B                  omit (show) basic blocks and out edges\n"
	 << "  -S, +S                  omit (show) statements (instructions)\n"
	 << "  -I, +I                  omit (show) inline sequences\n"
	 << "  -L, +L                  omit (show) line map info\n"
	 << "  --time                  display time and memory usage\n"
	 << "  -h, --help              display usage message and exit\n"
	 << "\noptions are processed left to right.\n"
	 << "default is to have all output turned on.\n"
	 << "\n";

    exit(1);
}

static int
getNumJobs(int index, int argc, char **argv, string opt)
{
    if (index >= argc) {
	usage("missing arg for " + opt);
    }
    int num = atoi(argv[index]);

    if (num <= 0 || num > MAX_THREADS) {
	errx(1, "bad arg for %s: %s", opt.c_str(), argv[index]);
    }

    return num;
}

// Command-line:  [options] ...  filename
void
getOptions(int argc, char **argv, Options & opts)
{
    int n = 1;
    while (n < argc) {
	string arg(argv[n]);
	int indx = n;
	n++;

	if (arg == "-h" || arg == "-help" || arg == "--help") {
	    usage("");
	}

	// jobs per phase
	else if (arg == "-j" || arg == "--jobs") {
	    opts.jobs_symtab = getNumJobs(n, argc, argv, "--jobs");
	    opts.jobs_parse = opts.jobs_symtab;
	    n++;
	}
	else if (arg == "--jobs-symtab") {
	    opts.jobs_symtab = getNumJobs(n, argc, argv, "--jobs-symtab");
	    n++;
	}
	else if (arg == "--jobs-parse") {
	    opts.jobs_parse = getNumJobs(n, argc, argv, "--jobs-parse");
	    n++;
	}
	else if (arg == "--jobs-struct") {
	    warnx("--jobs-struct is ignored");
	    n++;
	}

	// blocks, edges, stmts
	else if (arg == "-A") {
	    opts.show_blocks = false;
	    opts.show_stmts = false;
	    opts.show_inline = false;
	    opts.show_linemap = false;
	}
	else if (arg == "+A") {
	    opts.show_blocks = true;
	    opts.show_stmts = true;
	    opts.show_inline = true;
	    opts.show_linemap = true;
	}
	else if (arg == "-B") {
	    opts.show_blocks = false;
	}
	else if (arg == "+B") {
	    opts.show_blocks = true;
	}
	else if (arg == "-S") {
	    opts.show_stmts = false;
	    opts.show_inline = false;
	    opts.show_linemap = false;
	}
	else if (arg == "+S") {
	    opts.show_stmts = true;
	    opts.show_inline = true;
	    opts.show_linemap = true;
	}

	// inline, line map, these imply stmts
	else if (arg == "-I") {
	    opts.show_inline = false;
	}
	else if (arg == "+I") {
	    opts.show_inline = true;
	    opts.show_stmts = true;
	}
	else if (arg == "-L") {
	    opts.show_linemap = false;
	}
	else if (arg == "+L") {
	    opts.show_linemap = true;
	    opts.show_stmts = true;
	}

	// other
	else if (arg == "--time") {
	    opts.show_time = true;
	}
	else if (arg[0] == '-' || arg[0] == '+') {
	    usage(string("invalid option: ") + arg);
	}
	else {
	    n = indx;
	    break;
	}
    }

    // filename (required)
    if (n < argc) {
	opts.filename = argv[n];
	if (access(opts.filename, R_OK) != 0) {
	    usage(string("unable to read: ") + opts.filename);
	}
    }
    else {
	usage("missing file name");
    }
}

//----------------------------------------------------------------------

//
// Write to stderr so that stdout remains deterministic.
//
static void
printTime(const char *label, struct timeval *tv_prev, struct timeval *tv_now,
          struct rusage *ru_prev, struct rusage *ru_now)
{
    float delta = (float)(tv_now->tv_sec - tv_prev->tv_sec)
        + ((float)(tv_now->tv_usec - tv_prev->tv_usec))/1000000.0;

    fprintf(stderr, "%s  %8.1f sec  %8ld meg  %8ld meg\n", label, delta,
	    (ru_now->ru_maxrss - ru_prev->ru_maxrss)/1024,
	    ru_now->ru_maxrss/1024);
}

//----------------------------------------------------------------------

int
main(int argc, char **argv)
{
    struct timeval tv_init, tv_symtab, tv_parse, tv_fini;
    struct rusage  ru_init, ru_symtab, ru_parse, ru_fini;

    getOptions(argc, argv, opts);

    cout << "--------------------------------------------------\n"
	 << "file:  " << opts.filename << "\n";

    gettimeofday(&tv_init, NULL);
    getrusage(RUSAGE_SELF, &ru_init);

    //--------------------------------------------------
    // Phase 1 -- Open Symtab, compute Line Map Info
    //--------------------------------------------------

    omp_set_num_threads(opts.jobs_symtab);

    if (! Symtab::openFile(the_symtab, opts.filename)) {
        errx(1, "Symtab::openFile failed: %s", opts.filename);
    }

    the_symtab->parseTypesNow();
    the_symtab->parseFunctionRanges();

    // pre-compute line map info
    vector <Module *> modVec;
    the_symtab->getAllModules(modVec);

#pragma omp parallel  shared(modVec)
    {
#pragma omp for  schedule(dynamic, 1)
	for (uint i = 0; i < modVec.size(); i++) {
	    modVec[i]->parseLineInformation();
	}
    }  // end parallel

    gettimeofday(&tv_symtab, NULL);
    getrusage(RUSAGE_SELF, &ru_symtab);

    //--------------------------------------------------
    // Phase 2 -- Parse CFG into Blocks and Edges
    //--------------------------------------------------

    omp_set_num_threads(opts.jobs_parse);

    SymtabCodeSource * code_src = new SymtabCodeSource(the_symtab);
    CodeObject * code_obj = new CodeObject(code_src);

    code_obj->parse();

    gettimeofday(&tv_parse, NULL);
    getrusage(RUSAGE_SELF, &ru_parse);

    //--------------------------------------------------
    // Phase 3 -- Iterate Functions and Dump Results
    //--------------------------------------------------

    omp_set_num_threads(1);

    // get function list and convert to vector
    const CodeObject::funclist & funcList = code_obj->funcs();
    vector <ParseAPI::Function *> funcVec;

    for (auto fit = funcList.begin(); fit != funcList.end(); ++fit) {
	ParseAPI::Function * func = *fit;
	funcVec.push_back(func);
    }

    // sort funcVec to ensure deterministic output
    std::sort(funcVec.begin(), funcVec.end(), FuncLessThan);

    for (ulong n = 0; n < funcVec.size(); n++) {
        ParseAPI::Function * func = funcVec[n];
	doFunction(func);
    }

    cout << "\nnum funcs:  " << funcVec.size() << "\n" << endl;

    gettimeofday(&tv_fini, NULL);
    getrusage(RUSAGE_SELF, &ru_fini);

    if (opts.show_time) {
        cerr << "\nfile: " << opts.filename << "\n"
	     << "symtab threads: " << opts.jobs_symtab
	     << "  parse threads: " << opts.jobs_parse
	     << "  struct threads: " << opts.jobs_struct << "\n\n";

	printTime("init:  ", &tv_init, &tv_init, &ru_init, &ru_init);
	printTime("symtab:", &tv_init, &tv_symtab, &ru_init, &ru_symtab);
	printTime("parse: ", &tv_symtab, &tv_parse, &ru_symtab, &ru_parse);
	printTime("struct:", &tv_parse, &tv_fini, &ru_parse, &ru_fini);
	printTime("total: ", &tv_init, &tv_fini, &ru_init, &ru_fini);
	cerr << endl;
    }

    return 0;
}
