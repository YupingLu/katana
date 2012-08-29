/** Maximal independent set application -*- C++ -*-
 * @file
 *
 * A simple spanning tree algorithm to demostrate the Galois system.
 *
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2012, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 */
#include "Galois/Galois.h"
#include "Galois/Bag.h"
#include "Galois/Statistic.h"
#include "Galois/Graphs/LCGraph.h"
#ifdef GALOIS_USE_EXP
#include "Galois/Runtime/ParallelWorkInline.h"
#endif
#include "llvm/Support/CommandLine.h"

#include "Lonestar/BoilerPlate.h"

#include "boost/optional.hpp"

#include <utility>
#include <vector>
#include <algorithm>
#include <iostream>

const char* name = "Maximal Independent Set";
const char* desc = "Compute a maximal independent set (not maximum) of nodes in a graph";
const char* url = NULL;

enum MISAlgo {
  serial,
  parallel,
};

enum DetAlgo {
  nondet,
  detBase,
  detPrefix,
  detDisjoint
};

namespace cll = llvm::cl;
static cll::opt<std::string> filename(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<MISAlgo> algo(cll::desc("Choose an algorithm:"),
    cll::values(
      clEnumVal(serial, "Serial"),
      clEnumVal(parallel, "Parallel using Galois"),
      clEnumValEnd), cll::init(parallel));
#ifdef GALOIS_USE_DET
static cll::opt<DetAlgo> detAlgo(cll::desc("Deterministic algorithm:"),
    cll::values(
      clEnumVal(nondet, "Non-deterministic"),
      clEnumVal(detBase, "Base execution"),
      clEnumVal(detPrefix, "Prefix execution"),
      clEnumVal(detDisjoint, "Disjoint execution"),
      clEnumValEnd), cll::init(nondet));
#endif

enum MatchFlag {
  UNMATCHED, OTHER_MATCHED, MATCHED
};

struct Node {
  unsigned int id;
  MatchFlag flag; 
  MatchFlag pendingFlag; 
  Node() : flag(UNMATCHED), pendingFlag(UNMATCHED) { }
};

#ifdef GALOIS_USE_NUMA
typedef Galois::Graph::LC_Linear2_Graph<Node,void> Graph;
#else
typedef Galois::Graph::LC_CSR_Graph<Node,void> Graph;
#endif

typedef Graph::GraphNode GNode;

Graph graph;

//! Basic operator for any scheduling
template<int Version=detBase>
struct Process {
  typedef int tt_does_not_need_parallel_push;
  typedef int tt_needs_per_iter_alloc; // For LocalState

  struct LocalState {
    bool mod;
    LocalState(Process<Version>& self, Galois::PerIterAllocTy& alloc): mod(false) { }
  };

  template<Galois::MethodFlag Flag>
  bool build(GNode src) {
    Node& me = graph.getData(src, Flag);
    if (me.flag != UNMATCHED)
      return false;

    for (Graph::edge_iterator ii = graph.edge_begin(src, Galois::NONE),
        ei = graph.edge_end(src, Galois::NONE); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      Node& data = graph.getData(dst, Flag);
      if (data.flag == MATCHED)
        return false;
    }

    return true;
  }

  void modify(GNode src) {
    Node& me = graph.getData(src, Galois::NONE);
    for (Graph::edge_iterator ii = graph.edge_begin(src, Galois::NONE),
        ei = graph.edge_end(src, Galois::NONE); ii != ei; ++ii) {
      GNode dst = graph.getEdgeDst(ii);
      Node& data = graph.getData(dst, Galois::NONE);
      data.flag = OTHER_MATCHED;
    }

    me.flag = MATCHED;
  }

  //! Serial operator
  void operator()(GNode src) {
    if (build<Galois::NONE>(src))
      modify(src);
  }

  void operator()(GNode src, Galois::UserContext<GNode>& ctx) {
    bool* modp;
#ifdef GALOIS_USE_DET
    if (Version == detDisjoint) {
      bool used;
      LocalState* localState = (LocalState*) ctx.getLocalState(used);
      modp = &localState->mod;
      if (used) {
        if (*modp)
          modify(src);
        return;
      }
    }
#endif

    if (Version == detDisjoint) {
#ifdef GALOIS_USE_DET
      *modp = build<Galois::ALL>(src);
#endif
    } else {
      bool mod = build<Galois::ALL>(src);
      if (Version == detPrefix)
        return;
      else
        graph.getData(src, Galois::WRITE); // Failsafe point
      if (mod)
        modify(src);
    }
  }
};

struct GaloisAlgo {
  void operator()() {
#ifdef GALOIS_USE_EXP
    typedef GaloisRuntime::WorkList::BulkSynchronousInline<false> WL;
#else
    typedef GaloisRuntime::WorkList::dChunkedFIFO<256> WL;
#endif

#ifdef GALOIS_USE_DET
    switch (detAlgo) {
      case nondet: 
        Galois::for_each<WL>(graph.begin(), graph.end(), Process<>()); break;
      case detBase:
        Galois::for_each_det(graph.begin(), graph.end(), Process<>()); break;
      case detPrefix:
        Galois::for_each_det(graph.begin(), graph.end(), Process<detPrefix>(), Process<>());
        break;
      case detDisjoint:
        Galois::for_each_det(graph.begin(), graph.end(), Process<detDisjoint>()); break;
      default: std::cerr << "Unknown algorithm" << detAlgo << "\n"; abort();
    }
#else
    Galois::for_each<WL>(graph.begin(), graph.end(), Process<>());
#endif
  }
};

struct SerialAlgo {
  void operator()() {
    std::for_each(graph.begin(), graph.end(), Process<>());
  }
};

struct is_bad {
  bool operator()(GNode n) const {
    Node& me = graph.getData(n);
    if (me.flag == MATCHED) {
      for (Graph::edge_iterator ii = graph.edge_begin(n),
          ei = graph.edge_end(n); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        Node& data = graph.getData(dst);
        if (dst != n && data.flag == MATCHED) {
          std::cerr << "double match\n";
          return true;
        }
      }
    } else if (me.flag == UNMATCHED) {
      bool ok = false;
      for (Graph::edge_iterator ii = graph.edge_begin(n),
          ei = graph.edge_end(n); ii != ei; ++ii) {
        GNode dst = graph.getEdgeDst(ii);
        Node& data = graph.getData(dst);
        if (data.flag != UNMATCHED) {
          ok = true;
        }
      }
      if (!ok) {
        std::cerr << "not maximal\n";
        return true;
      }
    }
    return false;
  }
};

struct is_matched {
  bool operator()(const GNode& n) const {
    return graph.getData(n).flag == MATCHED;
  }
};

bool verify() {
  return Galois::find_if(graph.begin(), graph.end(), is_bad()) == graph.end();
}

int main(int argc, char** argv) {
  Galois::StatManager statManager;
  LonestarStart(argc, argv, name, desc, url);

  graph.structureFromFile(filename);

  unsigned int id = 0;
  for (Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii, ++id)
    graph.getData(*ii).id = id;
  
  Galois::preAlloc(numThreads + (graph.size() * sizeof(Node) * numThreads / 8) / GaloisRuntime::MM::pageSize);
  Galois::Statistic("MeminfoPre", GaloisRuntime::MM::pageAllocInfo());
  Galois::StatTimer T;
  T.start();
  switch (algo) {
    case serial: SerialAlgo()(); break;
    case parallel: GaloisAlgo()(); break;
    default: std::cerr << "Unknown algorithm" << algo << "\n"; abort();
  }
  T.stop();
  Galois::Statistic("MeminfoPost", GaloisRuntime::MM::pageAllocInfo());

  std::cout << "Cardinality of maximal independent set: " 
    << Galois::count_if(graph.begin(), graph.end(), is_matched()) 
    << "\n";

  if (!skipVerify && !verify()) {
    std::cerr << "verification failed\n";
    assert(0 && "verification failed");
    abort();
  }

  return 0;
}

