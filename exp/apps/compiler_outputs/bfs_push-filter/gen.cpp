/** BFS -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
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
 * @section Description
 *
 * Compute BFS on distributed Galois using worklist.
 *
 * @author Gurbinder Gill <gurbinder533@gmail.com>
 */

#include <iostream>
#include <limits>
#include "Galois/Galois.h"
#include "Galois/gstl.h"
#include "Lonestar/BoilerPlate.h"
#include "Galois/Runtime/CompilerHelperFunctions.h"

#ifdef __GALOIS_VERTEX_CUT_GRAPH__
#include "Galois/Runtime/vGraph.h"
#else
#include "Galois/Runtime/hGraph.h"
#endif
#include "Galois/DistAccumulator.h"
#include "Galois/Runtime/Tracer.h"

#ifdef __GALOIS_HET_CUDA__
#include "Galois/Runtime/Cuda/cuda_device.h"
#include "gen_cuda.h"
struct CUDA_Context *cuda_ctx;

enum Personality {
   CPU, GPU_CUDA, GPU_OPENCL
};
std::string personality_str(Personality p) {
   switch (p) {
   case CPU:
      return "CPU";
   case GPU_CUDA:
      return "GPU_CUDA";
   case GPU_OPENCL:
      return "GPU_OPENCL";
   }
   assert(false&& "Invalid personality");
   return "";
}
#endif

static const char* const name = "BFS - Distributed Heterogeneous with worklist.";
static const char* const desc = "BFS on Distributed Galois.";
static const char* const url = 0;

namespace cll = llvm::cl;
static cll::opt<std::string> inputFile(cll::Positional, cll::desc("<input file>"), cll::Required);
#ifdef __GALOIS_VERTEX_CUT_GRAPH__
static cll::opt<std::string> partFolder("partFolder", cll::desc("path to partitionFolder"), cll::init(""));
#endif
static cll::opt<unsigned int> maxIterations("maxIterations", cll::desc("Maximum iterations: Default 10000"), cll::init(10000));
static cll::opt<unsigned int> src_node("srcNodeId", cll::desc("ID of the source node"), cll::init(0));
static cll::opt<bool> verify("verify", cll::desc("Verify ranks by printing to 'page_ranks.#hid.csv' file"), cll::init(false));
#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
static cll::opt<unsigned> comm_mode("comm_mode", cll::desc("Communication mode: 0 - original, 1 - simulated net, 2 - simulated bare MPI"), cll::init(0));
#endif
#endif
#ifdef __GALOIS_HET_CUDA__
static cll::opt<int> gpudevice("gpu", cll::desc("Select GPU to run on, default is to choose automatically"), cll::init(-1));
static cll::opt<Personality> personality("personality", cll::desc("Personality"),
      cll::values(clEnumValN(CPU, "cpu", "Galois CPU"), clEnumValN(GPU_CUDA, "gpu/cuda", "GPU/CUDA"), clEnumValN(GPU_OPENCL, "gpu/opencl", "GPU/OpenCL"), clEnumValEnd),
      cll::init(CPU));
static cll::opt<std::string> personality_set("pset", cll::desc("String specifying personality for each host. 'c'=CPU,'g'=GPU/CUDA and 'o'=GPU/OpenCL"), cll::init(""));
static cll::opt<unsigned> scalegpu("scalegpu", cll::desc("Scale GPU workload w.r.t. CPU, default is proportionally equal workload to CPU and GPU (1)"), cll::init(1));
static cll::opt<unsigned> scalecpu("scalecpu", cll::desc("Scale CPU workload w.r.t. GPU, default is proportionally equal workload to CPU and GPU (1)"), cll::init(1));
static cll::opt<int> num_nodes("num_nodes", cll::desc("Num of physical nodes with devices (default = num of hosts): detect GPU to use for each host automatically"), cll::init(-1));
#endif

const unsigned int infinity = std::numeric_limits<unsigned int>::max()/4;


struct NodeData {
  std::atomic<unsigned int> dist_current;
  unsigned int dist_old;
};

#ifdef __GALOIS_VERTEX_CUT_GRAPH__
typedef vGraph<NodeData, void> Graph;
#else
typedef hGraph<NodeData, void> Graph;
#endif
typedef typename Graph::GraphNode GNode;

struct InitializeGraph {
  const unsigned int &local_infinity;
  cll::opt<unsigned int> &local_src_node;
  Graph *graph;

  InitializeGraph(cll::opt<unsigned int> &_src_node, const unsigned int &_infinity, Graph* _graph) : local_src_node(_src_node), local_infinity(_infinity), graph(_graph){}

  void static go(Graph& _graph){
    	struct SyncerPull_0 {
    		static unsigned int extract(uint32_t node_id, const struct NodeData & node) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) return get_node_dist_current_cuda(cuda_ctx, node_id);
    			assert (personality == CPU);
    		#endif
    			return node.dist_current;
    		}
        static bool extract_batch(unsigned from_id, unsigned int *y) {
        #ifdef __GALOIS_HET_CUDA__
          if (personality == GPU_CUDA) {
            batch_get_node_dist_current_cuda(cuda_ctx, from_id, y);
            return true;
          }
          assert (personality == CPU);
        #endif
          return false;
        }
    		static void setVal (uint32_t node_id, struct NodeData & node, unsigned int y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) set_node_dist_current_cuda(cuda_ctx, node_id, y);
    			else if (personality == CPU)
    		#endif
    				node.dist_current = y;
    		}
        static bool setVal_batch(unsigned from_id, unsigned int *y) {
        #ifdef __GALOIS_HET_CUDA__
          if (personality == GPU_CUDA) {
            batch_set_node_dist_current_cuda(cuda_ctx, from_id, y);
            return true;
          } 
          assert (personality == CPU);
        #endif
            return false;
        }
    		typedef unsigned int ValTy;
    	};
    #ifdef __GALOIS_HET_CUDA__
    	if (personality == GPU_CUDA) {
    		InitializeGraph_cuda(infinity, src_node, cuda_ctx);
    	} else if (personality == CPU)
    #endif
    Galois::do_all(_graph.begin(), _graph.end(), InitializeGraph {src_node, infinity, &_graph}, Galois::loopname("Init"), Galois::write_set("sync_pull", "this->graph", "struct NodeData &", "struct NodeData &", "dist_current" , "unsigned int"));
    _graph.sync_pull<SyncerPull_0>("InitializeGraph");
    
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.dist_current = (graph->getGID(src) == local_src_node) ? 0 : local_infinity;
    sdata.dist_old = (graph->getGID(src) == local_src_node) ? 0 : local_infinity;
  }
};

struct FirstItr_BFS{
Graph * graph;
FirstItr_BFS(Graph * _graph):graph(_graph){}
void static go(Graph& _graph) {
	struct Syncer_0 {
		static unsigned int extract(uint32_t node_id, const struct NodeData & node) {
		#ifdef __GALOIS_HET_CUDA__
			if (personality == GPU_CUDA) return get_node_dist_current_cuda(cuda_ctx, node_id);
			assert (personality == CPU);
		#endif
			return node.dist_current;
		}
    static bool extract_reset_batch(unsigned from_id, unsigned int *y) {
    #ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) {
        batch_get_reset_node_dist_current_cuda(cuda_ctx, from_id, y, std::numeric_limits<unsigned int>::max());
        return true;
      }
      assert (personality == CPU);
    #endif
      return false;
    }
		static void reduce (uint32_t node_id, struct NodeData & node, unsigned int y) {
		#ifdef __GALOIS_HET_CUDA__
			if (personality == GPU_CUDA) min_node_dist_current_cuda(cuda_ctx, node_id, y);
			else if (personality == CPU)
		#endif
				{ Galois::min(node.dist_current, y); }
		}
    static bool reduce_batch(unsigned from_id, unsigned int *y) {
    #ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) {
        batch_min_node_dist_current_cuda(cuda_ctx, from_id, y);
        return true;
      } 
      assert (personality == CPU);
    #endif
        return false;
    }
		static void reset (uint32_t node_id, struct NodeData & node ) {
		#ifdef __GALOIS_HET_CUDA__
			if (personality == GPU_CUDA) set_node_dist_current_cuda(cuda_ctx, node_id, std::numeric_limits<unsigned int>::max());
			else if (personality == CPU)
		#endif
				{ node.dist_current = std::numeric_limits<unsigned int>::max(); }
		}
		typedef unsigned int ValTy;
	};
	struct SyncerPull_0 {
		static unsigned int extract(uint32_t node_id, const struct NodeData & node) {
		#ifdef __GALOIS_HET_CUDA__
			if (personality == GPU_CUDA) return get_node_dist_current_cuda(cuda_ctx, node_id);
			assert (personality == CPU);
		#endif
			return node.dist_current;
		}
    static bool extract_batch(unsigned from_id, unsigned int *y) {
    #ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) {
        batch_get_node_dist_current_cuda(cuda_ctx, from_id, y);
        return true;
      }
      assert (personality == CPU);
    #endif
      return false;
    }
		static void setVal (uint32_t node_id, struct NodeData & node, unsigned int y) {
		#ifdef __GALOIS_HET_CUDA__
			if (personality == GPU_CUDA) set_node_dist_current_cuda(cuda_ctx, node_id, y);
			else if (personality == CPU)
		#endif
				node.dist_current = y;
		}
    static bool setVal_batch(unsigned from_id, unsigned int *y) {
    #ifdef __GALOIS_HET_CUDA__
      if (personality == GPU_CUDA) {
        batch_set_node_dist_current_cuda(cuda_ctx, from_id, y);
        return true;
      } 
      assert (personality == CPU);
    #endif
        return false;
    }
		typedef unsigned int ValTy;
	};
#ifdef __GALOIS_HET_CUDA__
	if (personality == GPU_CUDA) {
    std::string comp_str("CUDA_IMPL_BFS_" + std::to_string(_graph.get_run_num()));
    Galois::StatTimer StatTimer_comp(comp_str.c_str());
    StatTimer_comp.start();
		FirstItr_BFS_cuda(cuda_ctx);
    StatTimer_comp.stop();
	} else if (personality == CPU)
#endif
Galois::do_all(_graph.begin(), _graph.end(), FirstItr_BFS{&_graph}, Galois::loopname("bfs"), Galois::write_set("sync_push", "this->graph", "struct NodeData &", "struct NodeData &" , "dist_current", "unsigned int" , "min",  "std::numeric_limits<unsigned int>::max()"), Galois::write_set("sync_pull", "this->graph", "struct NodeData &", "struct NodeData &", "dist_current" , "unsigned int"));
_graph.sync_push<Syncer_0>("FirstItr_BFS");

_graph.sync_pull<SyncerPull_0>("FirstItr_BFS");

}
void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);
    snode.dist_old = snode.dist_current;

    for (auto jj = graph->edge_begin(src), ee = graph->edge_end(src); jj != ee; ++jj) {
      GNode dst = graph->getEdgeDst(jj);
      auto& dnode = graph->getData(dst);
      unsigned int new_dist = 1 + snode.dist_current;
      Galois::atomicMin(dnode.dist_current, new_dist);
      
    }
  }

};
struct BFS {
  Graph* graph;

  BFS(Graph* _graph) : graph(_graph){}
  void static go(Graph& _graph){
    using namespace Galois::WorkList;
    typedef dChunkedFIFO<64> dChunk;
    FirstItr_BFS::go(_graph);
    
     do { 
    DGAccumulator_accum.reset();
    	struct Syncer_0 {
    		static unsigned int extract(uint32_t node_id, const struct NodeData & node) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) return get_node_dist_current_cuda(cuda_ctx, node_id);
    			assert (personality == CPU);
    		#endif
    			return node.dist_current;
    		}
        static bool extract_reset_batch(unsigned from_id, unsigned int *y) {
        #ifdef __GALOIS_HET_CUDA__
          if (personality == GPU_CUDA) {
            batch_get_reset_node_dist_current_cuda(cuda_ctx, from_id, y, std::numeric_limits<unsigned int>::max());
            return true;
          }
          assert (personality == CPU);
        #endif
          return false;
        }
    		static void reduce (uint32_t node_id, struct NodeData & node, unsigned int y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) min_node_dist_current_cuda(cuda_ctx, node_id, y);
    			else if (personality == CPU)
    		#endif
    				{ Galois::min(node.dist_current, y); }
    		}
        static bool reduce_batch(unsigned from_id, unsigned int *y) {
        #ifdef __GALOIS_HET_CUDA__
          if (personality == GPU_CUDA) {
            batch_min_node_dist_current_cuda(cuda_ctx, from_id, y);
            return true;
          } 
          assert (personality == CPU);
        #endif
            return false;
        }
    		static void reset (uint32_t node_id, struct NodeData & node ) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) set_node_dist_current_cuda(cuda_ctx, node_id, std::numeric_limits<unsigned int>::max());
    			else if (personality == CPU)
    		#endif
    				{ node.dist_current = std::numeric_limits<unsigned int>::max(); }
    		}
    		typedef unsigned int ValTy;
    	};
    	struct SyncerPull_0 {
    		static unsigned int extract(uint32_t node_id, const struct NodeData & node) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) return get_node_dist_current_cuda(cuda_ctx, node_id);
    			assert (personality == CPU);
    		#endif
    			return node.dist_current;
    		}
        static bool extract_batch(unsigned from_id, unsigned int *y) {
        #ifdef __GALOIS_HET_CUDA__
          if (personality == GPU_CUDA) {
            batch_get_node_dist_current_cuda(cuda_ctx, from_id, y);
            return true;
          }
          assert (personality == CPU);
        #endif
          return false;
        }
    		static void setVal (uint32_t node_id, struct NodeData & node, unsigned int y) {
    		#ifdef __GALOIS_HET_CUDA__
    			if (personality == GPU_CUDA) set_node_dist_current_cuda(cuda_ctx, node_id, y);
    			else if (personality == CPU)
    		#endif
    				node.dist_current = y;
    		}
        static bool setVal_batch(unsigned from_id, unsigned int *y) {
        #ifdef __GALOIS_HET_CUDA__
          if (personality == GPU_CUDA) {
            batch_set_node_dist_current_cuda(cuda_ctx, from_id, y);
            return true;
          } 
          assert (personality == CPU);
        #endif
            return false;
        }
    		typedef unsigned int ValTy;
    	};
    #ifdef __GALOIS_HET_CUDA__
    	if (personality == GPU_CUDA) {
    		int __retval = 0;
        std::string comp_str("CUDA_IMPL_BFS_" + std::to_string(_graph.get_run_num()));
        Galois::StatTimer StatTimer_comp(comp_str.c_str());
        StatTimer_comp.start();
        BFS_cuda(__retval, cuda_ctx);
        StatTimer_comp.stop();
    		DGAccumulator_accum += __retval;
    	} else if (personality == CPU)
    #endif
    Galois::do_all(_graph.begin(), _graph.end(), BFS {&_graph}, Galois::loopname("bfs"), Galois::write_set("sync_push", "this->graph", "struct NodeData &", "struct NodeData &" , "dist_current", "unsigned int" , "min",  "std::numeric_limits<unsigned int>::max()"), Galois::write_set("sync_pull", "this->graph", "struct NodeData &", "struct NodeData &", "dist_current" , "unsigned int"));
    _graph.sync_push<Syncer_0>("BFS");
    
    _graph.sync_pull<SyncerPull_0>("BFS");
    
    }while(DGAccumulator_accum.reduce());
    
  }

  static Galois::DGAccumulator<int> DGAccumulator_accum;
void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);

    if( snode.dist_old > snode.dist_current){
        snode.dist_old = snode.dist_current;

    
DGAccumulator_accum+= 1;
for (auto jj = graph->edge_begin(src), ee = graph->edge_end(src); jj != ee; ++jj) {
      GNode dst = graph->getEdgeDst(jj);
      auto& dnode = graph->getData(dst);
      unsigned int new_dist = 1 + snode.dist_current;
      Galois::atomicMin(dnode.dist_current, new_dist);
      
    }
      }
  }
};
Galois::DGAccumulator<int>  BFS::DGAccumulator_accum;


int main(int argc, char** argv) {
  try {
    LonestarStart(argc, argv, name, desc, url);
    Galois::Runtime::reportStat("(NULL)", "Max Iterations", (unsigned long)maxIterations, 0);
    Galois::Runtime::reportStat("(NULL)", "Source Node ID", (unsigned long)src_node, 0);
    Galois::StatManager statManager;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    Galois::StatTimer StatTimer_init("TIMER_GRAPH_INIT"), StatTimer_total("TIMER_TOTAL"), StatTimer_hg_init("TIMER_HG_INIT");

    StatTimer_total.start();

    std::vector<unsigned> scalefactor;
#ifdef __GALOIS_HET_CUDA__
    const unsigned my_host_id = Galois::Runtime::getHostID();
    int gpu_device = gpudevice;
    //Parse arg string when running on multiple hosts and update/override personality
    //with corresponding value.
    if (personality_set.length() == Galois::Runtime::NetworkInterface::Num) {
      switch (personality_set.c_str()[my_host_id]) {
      case 'g':
        personality = GPU_CUDA;
        break;
      case 'o':
        assert(0);
        personality = GPU_OPENCL;
        break;
      case 'c':
      default:
        personality = CPU;
        break;
      }
      if ((personality == GPU_CUDA) && (gpu_device == -1)) {
        gpu_device = get_gpu_device_id(personality_set, num_nodes);
      }
      for (unsigned i=0; i<personality_set.length(); ++i) {
        if (personality_set.c_str()[i] == 'c') 
          scalefactor.push_back(scalecpu);
        else
          scalefactor.push_back(scalegpu);
      }
    }
#endif


    StatTimer_hg_init.start();
#ifdef __GALOIS_VERTEX_CUT_GRAPH__
    Graph hg(inputFile, partFolder, net.ID, net.Num, scalefactor);
#else
    Graph hg(inputFile, net.ID, net.Num, scalefactor);
#endif
#ifdef __GALOIS_SIMULATE_COMMUNICATION__
#ifdef __GALOIS_SIMULATE_COMMUNICATION_WITH_GRAPH_DATA__
    hg.set_comm_mode(comm_mode);
#endif
#endif
#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      cuda_ctx = get_CUDA_context(my_host_id);
      if (!init_CUDA_context(cuda_ctx, gpu_device))
        return -1;
      MarshalGraph m = hg.getMarshalGraph(my_host_id);
      load_graph_CUDA(cuda_ctx, m, net.Num);
    } else if (personality == GPU_OPENCL) {
      //Galois::OpenCL::cl_env.init(cldevice.Value);
    }
#endif
    StatTimer_hg_init.stop();

    std::cout << "[" << net.ID << "] InitializeGraph::go called\n";
    StatTimer_init.start();
      InitializeGraph::go(hg);
    StatTimer_init.stop();


    for(auto run = 0; run < numRuns; ++run){
      std::cout << "[" << net.ID << "] BFS::go run " << run << " called\n";
      std::string timer_str("TIMER_" + std::to_string(run));
      Galois::StatTimer StatTimer_main(timer_str.c_str());

      Galois::Runtime::getHostBarrier().wait();
      hg.reset_num_iter(run);

      Galois::Runtime::beginSampling();
      StatTimer_main.start();
        BFS::go(hg);
      StatTimer_main.stop();
      Galois::Runtime::endSampling();

      if((run + 1) != numRuns){
        hg.reset_num_iter(run);
        InitializeGraph::go(hg);
      }
    }

   StatTimer_total.stop();

    // Verify
    if(verify){
#ifdef __GALOIS_HET_CUDA__
      if (personality == CPU) { 
#endif
        for(auto ii = hg.begin(); ii != hg.end(); ++ii) {
          Galois::Runtime::printOutput("% %\n", hg.getGID(*ii), hg.getData(*ii).dist_current);
        }
#ifdef __GALOIS_HET_CUDA__
      } else if(personality == GPU_CUDA)  {
        for(auto ii = hg.begin(); ii != hg.end(); ++ii) {
          Galois::Runtime::printOutput("% %\n", hg.getGID(*ii), get_node_dist_current_cuda(cuda_ctx, *ii));
        }
      }
#endif
    }
    return 0;
  } catch(const char* c) {
    std::cerr << "Error: " << c << "\n";
      return 1;
  }
}
