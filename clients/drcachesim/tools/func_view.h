/* **********************************************************
 * Copyright (c) 2020-2024 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#ifndef _FUNC_VIEW_H_
#define _FUNC_VIEW_H_ 1

#include <stddef.h>
#include <stdint.h>

#include <mutex>
#include <set>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "analysis_tool.h"
#include "dr_api.h"
#include "memref.h"
#include "raw2trace.h"
#include "raw2trace_directory.h"
#include "trace_entry.h"

namespace dynamorio {
namespace drmemtrace {

class func_view_t : public analysis_tool_t {
public:
    func_view_t(const std::string &funclist_file_path, bool full_trace,
                unsigned int verbose = 0);
    virtual ~func_view_t();
    std::string
    initialize_shard_type(shard_type_t shard_type) override;
    std::string
    initialize_stream(memtrace_stream_t *serial_stream) override;
    bool
    process_memref(const memref_t &memref) override;
    bool
    print_results() override;
    bool
    parallel_shard_supported() override;
    void *
    parallel_shard_init_stream(int shard_index, void *worker_data,
                               memtrace_stream_t *stream) override;
    bool
    parallel_shard_exit(void *shard_data) override;
    bool
    parallel_shard_memref(void *shard_data, const memref_t &memref) override;
    std::string
    parallel_shard_error(void *shard_data) override;

protected:
    struct func_stats_t {
        func_stats_t &
        operator+=(const func_stats_t &rhs)
        {
            num_calls += rhs.num_calls;
            num_returns += rhs.num_returns;
            return *this;
        }
        int64_t num_calls = 0;
        int64_t num_returns = 0;
        // TODO i#4083: Record the arg and retval distributions.
    };
    struct shard_data_t {
        memref_tid_t tid = 0; // We only support SHARD_BY_THREAD.
        std::unordered_map<int, func_stats_t> func_map;
        std::string error;
        // We use the function markers to record arguments and return
        // values in the trace also for some system calls like futex.
        // func_view skips printing details for such system calls,
        // because these are not specified by the user.
        bool last_was_syscall = false;
        int last_func_id = -1;
        int nesting_level = 0;
        int arg_idx = -1;
        bool prev_was_arg = false;
        addr_t prev_pc = 0;
        app_pc last_trace_module_start = nullptr;
        size_t last_trace_module_size = 0;
        bool prev_noret = false;
        std::string last_trace_module_name;
    };
    struct traced_info_t {
        std::set<std::string> names;
        int num_args = -1; // Illegal value to mark uninitialized info structs.
        bool noret = false;
    };

    // Lookup the information for the shard's last_func_id and return its traced_info_t
    // struct.  Sets shard.error if the ID is not found (but still returns a traced_info_t
    // reference).
    traced_info_t &
    get_info_for_last_func_id(shard_data_t *shard);

    static bool
    cmp_func_stats(const std::pair<int, func_stats_t> &l,
                   const std::pair<int, func_stats_t> &r);
    // Process markers, return true on success.
    bool
    process_memref_for_markers(void *shard_data, const memref_t &memref);
    std::unordered_map<int, func_stats_t>
    compute_totals();

    static const std::string TOOL_NAME;
    bool knob_full_trace_;
    unsigned int knob_verbose_;

    std::unordered_map<int, traced_info_t> id2info_;

    std::string funclist_file_path_;

    std::unordered_map<int, shard_data_t *> shard_map_;
    // This mutex is only needed in parallel_shard_init.  In all other accesses to
    // shard_map (process_memref, print_results) we are single-threaded.
    std::mutex shard_map_mutex_;
    memtrace_stream_t *serial_stream_ = nullptr;
};

} // namespace drmemtrace
} // namespace dynamorio

#endif /* _FUNC_VIEW_H_ */
