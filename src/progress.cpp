////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2018, Lawrence Livermore National Security, LLC.  Produced at the
// Lawrence Livermore National Laboratory in collaboration with University of
// Illinois Urbana-Champaign.
//
// Written by the LBANN Research Team (N. Dryden, N. Maruyama, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-756777.
// All rights reserved.
//
// This file is part of Aluminum GPU-aware Communication Library. For details, see
// http://software.llnl.gov/Aluminum or https://github.com/LLNL/Aluminum.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
////////////////////////////////////////////////////////////////////////////////

#include "aluminum/progress.hpp"

#include <iostream>
#include <numeric>
#include <string>

#include <hwloc.h>
#include <mpi.h>

#include <Al_config.hpp>
#include "aluminum/base.hpp"
#include "aluminum/tuning_params.hpp"
#include "aluminum/state.hpp"
#include "aluminum/mpi/communicator.hpp"
#include "aluminum/profiling.hpp"
#ifdef AL_HAS_CUDA
#include "aluminum/cuda/cuda.hpp"
#endif
#ifdef AL_TRACE
#include "aluminum/trace.hpp"
#endif

#if defined AL_HAS_ROCM
#include <hip/hip_runtime.h>
#include <rocm_smi/rocm_smi.h>
#include <hwloc/rsmi.h>
#elif defined AL_HAS_CUDA
#include <cuda_runtime.h>
#include <hwloc/cudart.h>
#endif

namespace Al {
namespace internal {

namespace {

std::string print_hwloc_version(unsigned long ver) {
  return std::to_string(ver >> 16) + "." + std::to_string((ver & 0x00ff00) >> 8);
}

// Adapted from
// https://github.com/open-mpi/hwloc/blob/master/utils/hwloc/misc.h
void check_hwloc_api_version() {
  if ((hwloc_get_api_version() >> 16) != (HWLOC_API_VERSION >> 16)) {
    throw_al_exception("HWLOC runtime library version "
                       + print_hwloc_version(hwloc_get_api_version())
                       + " does not match the version Aluminum was compiled with "
                       + print_hwloc_version(HWLOC_API_VERSION));
  }
}

// Implement these manually because we often don't have a new enough hwloc
// version.

// Equivalent of hwloc_bitmap_nr_ulongs.
int get_bitmap_len(hwloc_const_bitmap_t bitmap) {
  int last = hwloc_bitmap_last(bitmap);
  if (last == -1 && !hwloc_bitmap_iszero(bitmap)) {
    // hwloc internally handles this better, but this should disambiguate
    // between infinite bitmaps (an error) and empty bitmaps.
    throw_al_exception("Tried to exchange infinite bitmap");
  }
  constexpr int bits_per_ulong = sizeof(unsigned long) * 8;
  return (last + bits_per_ulong - 1) / bits_per_ulong;
}

// Equivalent of hwloc_bitmap_to_ulongs.
void bitmap_to_ulongs(hwloc_const_bitmap_t bitmap, unsigned nr,
                            unsigned long* masks) {
  for (unsigned i = 0; i < nr; ++i) {
    masks[i] = hwloc_bitmap_to_ith_ulong(bitmap, i);
  }
}

// Equivalent of hwloc_bitmap_from_ulongs.
void bitmap_from_ulongs(hwloc_bitmap_t bitmap, unsigned nr,
                        const unsigned long* masks) {
  for (unsigned i = 0; i < nr; ++i) {
    hwloc_bitmap_set_ith_ulong(bitmap, i, masks[i]);
  }
}

// Exchange hwloc bitmaps among processes in the local communicator.
// Returns one bitmap for each processor, in their local rank order.
// Bitmaps must be freed by the caller.
std::vector<hwloc_bitmap_t> local_exchange_hwloc_bitmaps(
  const mpi::MPICommunicator& comm, hwloc_const_bitmap_t bitmap) {
  // Extract the bitmap into longs for exchanging.
  int len = get_bitmap_len(bitmap);
  if (len == -1) {
    throw_al_exception("Tried to exchange infinite bitmap");
  }
  std::vector<unsigned long> ul_bitmap = std::vector<unsigned long>(len);
  bitmap_to_ulongs(bitmap, len, ul_bitmap.data());

  // Exchange bitmap sizes (in case they are different lengths).
  MPI_Comm local_comm = comm.get_local_comm();
  std::vector<int> bitmap_lens = std::vector<int>(comm.local_size());
  MPI_Allgather(&len, 1, MPI_INT,
                bitmap_lens.data(), 1, MPI_INT,
                local_comm);

  // Collect all the bitmaps.
  size_t total_len = std::accumulate(bitmap_lens.begin(),
                                     bitmap_lens.end(), size_t{0});
  std::vector<unsigned long> gathered_bitmaps = std::vector<unsigned long>(total_len);
  // Compute displacements (TODO: Should generalize excl_prefix_sum).
  std::vector<int> displs = std::vector<int>(bitmap_lens.size(), 0);
  for (size_t i = 1; i < bitmap_lens.size(); ++i) {
    displs[i] = bitmap_lens[i-1] + displs[i-1];
  }
  MPI_Allgatherv(ul_bitmap.data(), len, MPI_UNSIGNED_LONG,
                 gathered_bitmaps.data(), bitmap_lens.data(), displs.data(),
                 MPI_UNSIGNED_LONG, local_comm);

  // Extract back to real bitmaps.
  std::vector<hwloc_bitmap_t> bitmaps = std::vector<hwloc_bitmap_t>(comm.local_size());
  for (int i = 0; i < comm.local_size(); ++i) {
    bitmaps[i] = hwloc_bitmap_alloc();
    bitmap_from_ulongs(bitmaps[i], bitmap_lens[i],
                       gathered_bitmaps.data() + displs[i]);
  }
  return bitmaps;
}

// Return a vector marking which indices have bitmaps that are the same as the
// one for the local rank.
// This will include the current local rank.
std::vector<bool> get_same_indices(
  const std::vector<hwloc_bitmap_t>& bitmaps,
  const mpi::MPICommunicator& comm) {
  std::vector<bool> marks = std::vector<bool>(bitmaps.size());
  size_t local_rank = static_cast<size_t>(comm.local_rank());
  for (size_t i = 0; i < bitmaps.size(); ++i) {
    if (i == local_rank ||
        hwloc_bitmap_isequal(bitmaps[local_rank], bitmaps[i])) {
      marks[i] = true;
    } else {
      marks[i] = false;
    }
  }
  return marks;
}

// Return the offset to be used for assinging the local rank to a core.
// This assumes that if bitmaps are different, there is no overlap.
// If two ranks have the same bitmap, their offsets are ordered by rank.
int get_hwloc_offset(
  const std::vector<hwloc_bitmap_t>& bitmaps,
  const mpi::MPICommunicator& comm) {
  std::vector<bool> marks = get_same_indices(bitmaps, comm);
  int offset = 0;
  for (int i = 0; i < comm.local_rank(); ++i) {
    if (marks[i]) {
      ++offset;
    }
  }
  return offset;
}

// Get the local CPU set we want to use for binding.
bool get_hwloc_cpuset(hwloc_cpuset_t& cpuset, hwloc_topology_t& topo) {
  #ifdef AL_HAS_ROCM
  {
    // We need RSMI to be initialized for the hwloc_rsmi call.
    // We only use it here, so initialize it and shut it down.
    rsmi_init(0);
    int device;
    AL_CHECK_CUDA(hipGetDevice(&device));
    hwloc_rsmi_get_device_cpuset(topo, device, cpuset);
    rsmi_shut_down();
  }
#elif defined AL_HAS_CUDA
  {
    // If we have CUDA support, always assume we're using GPUs.
    // This also assumes the CUDA device has already been set.
    // Get the locality domain for the current GPU.
    int device;
    AL_CHECK_CUDA(cudaGetDevice(&device));
    hwloc_cudart_get_device_cpuset(topo, device, cpuset);
  }
#else
  {
    // Use the NUMA node we're currently on.
    hwloc_get_cpubind(topo, cpuset, 0);
    hwloc_nodeset_t nodeset = hwloc_bitmap_alloc();
    hwloc_cpuset_to_nodeset(topo, cpuset, nodeset);
    hwloc_bitmap_singlify(nodeset);
    hwloc_cpuset_from_nodeset(topo, cpuset, nodeset);
    hwloc_bitmap_free(nodeset);
  }
#endif
  if (hwloc_bitmap_iszero(cpuset)) {
    return false;
  }
  return true;
}

}  // anonymous namespace

#ifdef AL_PE_STREAM_QUEUE_CACHE
#ifdef AL_THREAD_MULTIPLE
thread_local std::unordered_map<void*, ProgressEngine::InputQueue*> ProgressEngine::stream_to_queue;
#else
std::unordered_map<void*, ProgressEngine::InputQueue*> ProgressEngine::stream_to_queue;
#endif
#endif

ProgressEngine::ProgressEngine() {
  stop_flag = false;
  started_flag = false;
#ifdef AL_PE_START_ON_DEMAND
  doing_start_flag = false;
#endif
#ifdef AL_PE_ADD_DEFAULT_STREAM
  // Initialze with the default stream.
  num_input_streams = 1;
#else
  num_input_streams = 0;
#endif
#ifdef AL_HAS_CUDA
  // Capture the current CUDA device for the progress engine.
  int device;
  AL_CHECK_CUDA(AlGpuGetDevice(&device));
  cur_device = device;
#endif
  bind_init();
}

ProgressEngine::~ProgressEngine() {}

void ProgressEngine::run() {
  // Wait for the progress engine to start.
  std::unique_lock<std::mutex> lock(startup_mutex);
#ifdef AL_PE_START_ON_DEMAND
  // Ensure only one thread starts this.
  if (doing_start_flag.load()) {
    // Another thread is already starting the progress engine.
    startup_cv.wait(lock, [this] {return started_flag.load();});
    return;
  }
  doing_start_flag = true;
#endif
  thread = std::thread(&ProgressEngine::engine, this);
  profiling::name_thread(thread.native_handle(), "al-progress");
  startup_cv.wait(lock, [this] {return started_flag.load() ;});
}

void ProgressEngine::stop() {
  if (!started_flag.load()) {
    return;  // Progress engine never started.
  }
  if (stop_flag.load()) {
    throw_al_exception("Stop called twice on progress engine");
  }
  stop_flag.store(true, std::memory_order_release);
  thread.join();
}

void ProgressEngine::enqueue(AlState* state) {
#ifdef AL_PE_START_ON_DEMAND
  if (!started_flag.load()) {
    run();
  }
#endif
#ifdef AL_PE_STREAM_QUEUE_CACHE
  // Check the thread-local queue cache.
  auto iter = ProgressEngine::stream_to_queue.find(state->get_compute_stream());
  if (iter != ProgressEngine::stream_to_queue.end()) {
    iter->second->q.push(state);
    return;
  }
  const size_t local_num_input_streams = num_input_streams.load();
#else
  // Linear search to find the queue.
  const size_t local_num_input_streams = num_input_streams.load();
  for (size_t i = 0; i < local_num_input_streams; ++i) {
    if (request_queues[i].compute_stream == state->get_compute_stream()) {
      // Appropriate queue found, we can just enqueue.
      request_queues[i].q.push(state);
      return;
    }
  }
#endif
  // Queue was not found, so we need to create it.
#ifdef AL_THREAD_MULTIPLE
  add_queue_mutex.lock();
  // Check if some other thread added the queue.
  const size_t locked_local_num_input_streams = num_input_streams.load();
  for (size_t i = local_num_input_streams;
       i < locked_local_num_input_streams;
       ++i) {
    if (request_queues[i].compute_stream == state->get_compute_stream()) {
      // Queue was added.
      add_queue_mutex.unlock();
      request_queues[i].q.push(state);
#ifdef AL_PE_STREAM_QUEUE_CACHE
      // Update cache.
      ProgressEngine::stream_to_queue[state->get_compute_stream()] = &request_queues[i];
#endif
      return;
    }
  }
#else
  // When there is only one thread, if the queue was not found earlier,
  // we always have to create it.
  const size_t locked_local_num_input_streams = local_num_input_streams;
#endif
#ifdef AL_DEBUG
  // Ensure we do not try to use too many queues.
  if (locked_local_num_input_streams >= AL_PE_NUM_STREAMS) {
    throw_al_exception(
      "Trying to create more progress engine streams than supported");
  }
#endif
  // Add the new queue.
  request_queues[locked_local_num_input_streams].compute_stream = state->get_compute_stream();
  ++num_input_streams;  // Make new queue visible.
#ifdef AL_THREAD_MULTIPLE
  add_queue_mutex.unlock();
#endif
  request_queues[locked_local_num_input_streams].q.push(state);
#ifdef AL_PE_STREAM_QUEUE_CACHE
  ProgressEngine::stream_to_queue[state->get_compute_stream()] = &request_queues[locked_local_num_input_streams];
#endif
}

std::ostream& ProgressEngine::dump_state(std::ostream& ss) {
  // Note: This pulls *directly from internal state*.
  // This is *not* thread safe, and stuff might blow up.
  // You should only be dumping state where you don't care about that anyway.
  for (auto&& stream_pipeline_pair : run_queues) {
    ss << "Pipelined run queue for stream " << stream_pipeline_pair.first << ":\n";
    auto&& pipeline = stream_pipeline_pair.second;
    for (size_t stage = 0; stage < AL_PE_NUM_PIPELINE_STAGES; ++stage) {
      const size_t stage_queue_size = pipeline[stage].size();
      ss << "Stage " << stage << " run queue (" << stage_queue_size << "):\n";
      for (size_t i = 0; i < stage_queue_size; ++i) {
        ss << i << ": ";
        if (pipeline[stage][i]) {
          ss << pipeline[stage][i]->get_name() << " "
             << pipeline[stage][i]->get_desc() << "\n";
        } else {
          ss << "(unknown)\n";
        }
      }
    }
  }
  return ss;
}

void ProgressEngine::bind_init() {
  check_hwloc_api_version();
  // Determine topology information.
  hwloc_topology_t topo;
  hwloc_topology_init(&topo);
  hwloc_topology_load(topo);
  // cpuset will be filled out with the set of CPUs we might want to
  // bind this rank to.
  hwloc_cpuset_t cpuset = hwloc_bitmap_alloc();
  if (!get_hwloc_cpuset(cpuset, topo)) {
    std::cerr << mpi::get_world_comm().rank()
              << ": Could not get starting cpuset; not binding progress thread"
              << std::endl;
    hwloc_bitmap_free(cpuset);
    hwloc_topology_destroy(topo);
    return;
  }
  // Now identify how we want to share the CPU among local ranks and compute
  // appropriate offsets.
  std::vector<hwloc_bitmap_t> local_cpusets = local_exchange_hwloc_bitmaps(
    mpi::get_world_comm(), cpuset);
  int offset = get_hwloc_offset(local_cpusets, mpi::get_world_comm());
  // Free local_cpusets.
  for (auto& local_cpuset : local_cpusets) {
    hwloc_bitmap_free(local_cpuset);
  }

  // Figure out how many cores we have.
  int num_cores = hwloc_get_nbobjs_inside_cpuset_by_type(
    topo, cpuset, HWLOC_OBJ_CORE);
  if (num_cores <= 0) {
    std::cerr << mpi::get_world_comm().rank()
              << ": Could not get cores for cpuset; not binding progress thread"
              << std::endl;
    hwloc_bitmap_free(cpuset);
    hwloc_topology_destroy(topo);
    return;
  }
  if (offset >= num_cores) {
    std::cerr << mpi::get_world_comm().rank()
              << ": computed cores offset of "
              << offset
              << " but have only "
              << num_cores
              << " available; not binding progress thread"
              << std::endl;
    hwloc_bitmap_free(cpuset);
    hwloc_topology_destroy(topo);
    return;
  }

  core_to_bind = num_cores - offset - 1;

  hwloc_bitmap_free(cpuset);
  hwloc_topology_destroy(topo);
}

void ProgressEngine::bind() {
  if (core_to_bind < 0) {
    std::cerr << mpi::get_world_comm().rank()
              << ": progress engine binding not initialized"
              << std::endl;
    return;
  }

  hwloc_topology_t topo;
  hwloc_topology_init(&topo);
  hwloc_topology_load(topo);
  hwloc_cpuset_t cpuset = hwloc_bitmap_alloc();
  if (!get_hwloc_cpuset(cpuset, topo)) {
    std::cerr << mpi::get_world_comm().rank()
              << ": Could not get starting cpuset; not binding progress thread"
              << std::endl;
    hwloc_bitmap_free(cpuset);
    hwloc_topology_destroy(topo);
    return;
  }

  hwloc_obj_t core = hwloc_get_obj_inside_cpuset_by_type(
    topo, cpuset, HWLOC_OBJ_CORE, core_to_bind);
  if (core == NULL) {
    std::cerr << mpi::get_world_comm().rank()
              << ": could not get core "
              << core_to_bind
              << "; not binding progress thread"
              << std::endl;
    hwloc_bitmap_free(cpuset);
    hwloc_topology_destroy(topo);
    return;
  }
  hwloc_cpuset_t coreset = hwloc_bitmap_dup(core->cpuset);
  hwloc_bitmap_singlify(coreset);
  if (hwloc_set_cpubind(topo, coreset, HWLOC_CPUBIND_THREAD) == -1) {
    std::cerr << mpi::get_world_comm().rank()
              << ": failed to bind progress thread"
              << std::endl;
  }

  hwloc_bitmap_free(coreset);
  hwloc_bitmap_free(cpuset);
  hwloc_topology_destroy(topo);
}

void ProgressEngine::engine() {
#ifdef AL_HAS_CUDA
  // Set the current CUDA device for the thread.
  AL_CHECK_CUDA_NOSYNC(AlGpuSetDevice(cur_device.load()));
#endif
  bind();
  // Notify the main thread we're now running.
  {
    std::unique_lock<std::mutex> lock(startup_mutex);
    started_flag = true;
  }
#ifdef AL_PE_START_ON_DEMAND
  startup_cv.notify_all();
#else
  startup_cv.notify_one();
#endif
  while (!stop_flag.load(std::memory_order_acquire)) {
    // Check for newly-submitted requests.
    size_t cur_input_streams = num_input_streams.load();
    for (size_t i = 0; i < cur_input_streams; ++i) {
      AlState* req = request_queues[i].q.peek();
      if (req != nullptr) {
        // Add to the correct run queue if one is available.
        bool do_start = false;
        switch (req->get_run_type()) {
        case RunType::bounded:
          // Move to the run queue if any of the following hold:
          //   1. num_bounded < AL_PE_NUM_CONCURRENT_OPS.
          //   2. The run_queue for this stream doesn't exist.
          //   3. The run_queue for this stream's first stage is empty.
          if (num_bounded < AL_PE_NUM_CONCURRENT_OPS
              || !run_queues.count(req->get_compute_stream())
              || !run_queues[req->get_compute_stream()][0].size()) {
            ++num_bounded;
            do_start = true;
          }
          break;
        case RunType::unbounded:
          do_start = true;
          break;
        }
        if (do_start) {
          // Add to end of first pipeline stage.
          // Create run queues if needed.
          if (!run_queues.count(req->get_compute_stream())) {
            run_queues.emplace(req->get_compute_stream(),
                               decltype(run_queues)::mapped_type{});
          }
          run_queues[req->get_compute_stream()][0].push_back(req);
          req->start();
#ifdef AL_DEBUG_HANG_CHECK
          req->start_time = get_time();
#endif
#ifdef AL_TRACE
          trace::record_pe_start(*req);
#endif
          request_queues[i].q.pop_always();
        }
      }
    }
    // Process one step of each in-progress request.
    for (auto&& stream_pipeline_pair : run_queues) {
      auto&& pipeline = stream_pipeline_pair.second;
      for (size_t stage = 0; stage < AL_PE_NUM_PIPELINE_STAGES; ++stage) {
        // Process this stage of the pipeline.
        for (auto i = pipeline[stage].begin(); i != pipeline[stage].end();) {
          AlState* req = *i;
          // Simply skip over paused states.
          if (req->paused_for_advance) {
            ++i;
          } else {
            PEAction action = req->step();
            switch (action) {
            case PEAction::cont:
              // Nothing to do here.
#ifdef AL_DEBUG_HANG_CHECK
              // Check whether we have hung.
              if (!req->hang_reported) {
                double t = get_time();
                if (t - req->start_time > 10.0 + world_comm->rank()) {
                  std::cout << world_comm->rank()
                            << ": Progress engine detected a possible hang"
                            << " state=" << req << " " << req->get_name()
                            << " compute_stream=" << req->get_compute_stream()
                            << " run_type="
                            << (req->get_run_type() == RunType::bounded ? "bounded" : "unbounded")
                            << std::endl;
                  req->hang_reported = true;
                }
              }
#endif
              ++i;
              break;
            case PEAction::advance:
#ifdef AL_DEBUG
              // Ensure we don't advance too far.
              if (stage + 1 >= AL_PE_NUM_PIPELINE_STAGES) {
                throw_al_exception("Trying to advance pipeline stage too far");
              }
#endif
              // Only move if this is the head of the pipeline stage.
              if (i == pipeline[stage].begin()) {
                pipeline[stage+1].push_back(req);
                i = pipeline[stage].erase(i);
              } else {
                req->paused_for_advance = true;
                ++i;
              }
              break;
            case PEAction::complete:
              if (req->get_run_type() == RunType::bounded) {
                --num_bounded;
              }
#ifdef AL_TRACE
              trace::record_pe_done(*req);
#endif
              delete req;
              i = pipeline[stage].erase(i);
              break;
            default:
              throw_al_exception("Unknown PEAction");
              break;
            }
          }
        }
        // Check whether we can advance paused states.
        for (auto i = pipeline[stage].begin(); i != pipeline[stage].end();) {
          AlState* req = *i;
          if (req->paused_for_advance) {
            // Move to the next stage.
            req->paused_for_advance = false;
            pipeline[stage+1].push_back(req);
            i = pipeline[stage].erase(i);
          } else {
            break;  // Nothing at the head to advance.
          }
        }
      }
    }
  }
}

}  // namespace internal
}  // namespace Al
