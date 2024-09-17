/* **********************************************************
 * Copyright (c) 2023-2024 Google, Inc.  All rights reserved.
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

#include "scheduler.h"

#include <inttypes.h>
#include <stdint.h>

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "memref.h"
#include "memtrace_stream.h"
#include "mutex_dbg_owned.h"
#include "reader.h"
#include "record_file_reader.h"
#include "trace_entry.h"
#ifdef HAS_LZ4
#    include "lz4_file_reader.h"
#endif
#ifdef HAS_ZLIB
#    include "compressed_file_reader.h"
#endif
#ifdef HAS_ZIP
#    include "zipfile_file_reader.h"
#else
#    include "file_reader.h"
#endif
#ifdef HAS_SNAPPY
#    include "snappy_file_reader.h"
#endif
#include "directory_iterator.h"
#include "utils.h"

#undef VPRINT
// We make logging available in release build to help in diagnosing issues
// and understanding scheduler behavior.
// We assume the extra branches do not add undue overhead.
#define VPRINT(obj, level, ...)                            \
    do {                                                   \
        if ((obj)->verbosity_ >= (level)) {                \
            fprintf(stderr, "%s ", (obj)->output_prefix_); \
            fprintf(stderr, __VA_ARGS__);                  \
        }                                                  \
    } while (0)
#define VDO(obj, level, statement)        \
    do {                                  \
        if ((obj)->verbosity_ >= (level)) \
            statement                     \
    } while (0)

namespace dynamorio {
namespace drmemtrace {

#ifdef HAS_ZLIB
// Even if the file is uncompressed, zlib's gzip interface is faster than
// file_reader_t's fstream in our measurements, so we always use it when
// available.
typedef compressed_file_reader_t default_file_reader_t;
typedef compressed_record_file_reader_t default_record_file_reader_t;
#else
typedef file_reader_t<std::ifstream *> default_file_reader_t;
typedef dynamorio::drmemtrace::record_file_reader_t<std::ifstream>
    default_record_file_reader_t;
#endif

std::string
replay_file_checker_t::check(archive_istream_t *infile)
{
    // Ensure we don't have repeated idle records, which balloon the file size.
    scheduler_t::schedule_record_t record;
    bool prev_was_idle = false;
    while (infile->read(reinterpret_cast<char *>(&record), sizeof(record))) {
        if (record.type == scheduler_t::schedule_record_t::IDLE) {
            if (prev_was_idle)
                return "Error: consecutive idle records";
            prev_was_idle = true;
        } else
            prev_was_idle = false;
    }
    return "";
}

/****************************************************************
 * Specializations for scheduler_tmpl_t<reader_t>, aka scheduler_t.
 */

template <>
std::unique_ptr<reader_t>
scheduler_tmpl_t<memref_t, reader_t>::get_default_reader()
{
    return std::unique_ptr<default_file_reader_t>(new default_file_reader_t());
}

template <>
std::unique_ptr<reader_t>
scheduler_tmpl_t<memref_t, reader_t>::get_reader(const std::string &path, int verbosity)
{
#if defined(HAS_SNAPPY) || defined(HAS_ZIP) || defined(HAS_LZ4)
#    ifdef HAS_LZ4
    if (ends_with(path, ".lz4")) {
        return std::unique_ptr<reader_t>(new lz4_file_reader_t(path, verbosity));
    }
#    endif
#    ifdef HAS_SNAPPY
    if (ends_with(path, ".sz"))
        return std::unique_ptr<reader_t>(new snappy_file_reader_t(path, verbosity));
#    endif
#    ifdef HAS_ZIP
    if (ends_with(path, ".zip"))
        return std::unique_ptr<reader_t>(new zipfile_file_reader_t(path, verbosity));
#    endif
    // If path is a directory, and any file in it ends in .sz, return a snappy reader.
    if (directory_iterator_t::is_directory(path)) {
        directory_iterator_t end;
        directory_iterator_t iter(path);
        if (!iter) {
            error_string_ =
                "Failed to list directory " + path + ": " + iter.error_string() + ". ";
            return nullptr;
        }
        for (; iter != end; ++iter) {
            const std::string fname = *iter;
            if (fname == "." || fname == ".." ||
                starts_with(fname, DRMEMTRACE_SERIAL_SCHEDULE_FILENAME) ||
                fname == DRMEMTRACE_CPU_SCHEDULE_FILENAME)
                continue;
            // Skip the auxiliary files.
            if (fname == DRMEMTRACE_MODULE_LIST_FILENAME ||
                fname == DRMEMTRACE_FUNCTION_LIST_FILENAME ||
                fname == DRMEMTRACE_ENCODING_FILENAME)
                continue;
#    ifdef HAS_SNAPPY
            if (ends_with(*iter, ".sz")) {
                return std::unique_ptr<reader_t>(
                    new snappy_file_reader_t(path, verbosity));
            }
#    endif
#    ifdef HAS_ZIP
            if (ends_with(*iter, ".zip")) {
                return std::unique_ptr<reader_t>(
                    new zipfile_file_reader_t(path, verbosity));
            }
#    endif
#    ifdef HAS_LZ4
            if (ends_with(path, ".lz4")) {
                return std::unique_ptr<reader_t>(new lz4_file_reader_t(path, verbosity));
            }
#    endif
        }
    }
#endif
    // No snappy/zlib support, or didn't find a .sz/.zip file.
    return std::unique_ptr<reader_t>(new default_file_reader_t(path, verbosity));
}

template <>
bool
scheduler_tmpl_t<memref_t, reader_t>::record_type_has_tid(memref_t record,
                                                          memref_tid_t &tid)
{
    if (record.marker.tid == INVALID_THREAD_ID)
        return false;
    tid = record.marker.tid;
    return true;
}

template <>
bool
scheduler_tmpl_t<memref_t, reader_t>::record_type_has_pid(memref_t record,
                                                          memref_pid_t &pid)
{
    if (record.marker.pid == INVALID_PID)
        return false;
    pid = record.marker.pid;
    return true;
}

template <>
void
scheduler_tmpl_t<memref_t, reader_t>::record_type_set_tid(memref_t &record,
                                                          memref_tid_t tid)
{
    record.marker.tid = tid;
}

template <>
bool
scheduler_tmpl_t<memref_t, reader_t>::record_type_is_instr(memref_t record)
{
    return type_is_instr(record.instr.type);
}

template <>
bool
scheduler_tmpl_t<memref_t, reader_t>::record_type_is_encoding(memref_t record)
{
    // There are no separate memref_t encoding records: encoding info is
    // inside instruction records.
    return false;
}

template <>
bool
scheduler_tmpl_t<memref_t, reader_t>::record_type_is_instr_boundary(memref_t record,
                                                                    memref_t prev_record)
{
    return record_type_is_instr(record);
}

template <>
bool
scheduler_tmpl_t<memref_t, reader_t>::record_type_is_marker(memref_t record,
                                                            trace_marker_type_t &type,
                                                            uintptr_t &value)
{
    if (record.marker.type != TRACE_TYPE_MARKER)
        return false;
    type = record.marker.marker_type;
    value = record.marker.marker_value;
    return true;
}

template <>
bool
scheduler_tmpl_t<memref_t, reader_t>::record_type_is_non_marker_header(memref_t record)
{
    // Non-marker trace_entry_t headers turn into markers or are
    // hidden, so there are none in a memref_t stream.
    return false;
}

template <>
bool
scheduler_tmpl_t<memref_t, reader_t>::record_type_is_timestamp(memref_t record,
                                                               uintptr_t &value)
{
    if (record.marker.type != TRACE_TYPE_MARKER ||
        record.marker.marker_type != TRACE_MARKER_TYPE_TIMESTAMP)
        return false;
    value = record.marker.marker_value;
    return true;
}

template <>
bool
scheduler_tmpl_t<memref_t, reader_t>::record_type_is_invalid(memref_t record)
{
    return record.instr.type == TRACE_TYPE_INVALID;
}

template <>
memref_t
scheduler_tmpl_t<memref_t, reader_t>::create_region_separator_marker(memref_tid_t tid,
                                                                     uintptr_t value)
{
    memref_t record = {};
    record.marker.type = TRACE_TYPE_MARKER;
    record.marker.marker_type = TRACE_MARKER_TYPE_WINDOW_ID;
    record.marker.marker_value = value;
    // XXX i#5843: We have .pid as 0 for now; worth trying to fill it in?
    record.marker.tid = tid;
    return record;
}

template <>
memref_t
scheduler_tmpl_t<memref_t, reader_t>::create_thread_exit(memref_tid_t tid)
{
    memref_t record = {};
    record.exit.type = TRACE_TYPE_THREAD_EXIT;
    // XXX i#5843: We have .pid as 0 for now; worth trying to fill it in?
    record.exit.tid = tid;
    return record;
}

template <>
memref_t
scheduler_tmpl_t<memref_t, reader_t>::create_invalid_record()
{
    memref_t record = {};
    record.instr.type = TRACE_TYPE_INVALID;
    return record;
}

template <>
void
scheduler_tmpl_t<memref_t, reader_t>::print_record(const memref_t &record)
{
    fprintf(stderr, "tid=%" PRId64 " type=%d", record.instr.tid, record.instr.type);
    if (type_is_instr(record.instr.type))
        fprintf(stderr, " pc=0x%zx size=%zu", record.instr.addr, record.instr.size);
    else if (record.marker.type == TRACE_TYPE_MARKER) {
        fprintf(stderr, " marker=%d val=%zu", record.marker.marker_type,
                record.marker.marker_value);
    }
    fprintf(stderr, "\n");
}

template <>
void
scheduler_tmpl_t<memref_t, reader_t>::insert_switch_tid_pid(input_info_t &info)
{
    // We do nothing, as every record has a tid from the separate inputs.
}

/******************************************************************************
 * Specializations for scheduler_tmpl_t<record_reader_t>, aka record_scheduler_t.
 */

template <>
std::unique_ptr<dynamorio::drmemtrace::record_reader_t>
scheduler_tmpl_t<trace_entry_t, record_reader_t>::get_default_reader()
{
    return std::unique_ptr<default_record_file_reader_t>(
        new default_record_file_reader_t());
}

template <>
std::unique_ptr<dynamorio::drmemtrace::record_reader_t>
scheduler_tmpl_t<trace_entry_t, record_reader_t>::get_reader(const std::string &path,
                                                             int verbosity)
{
    // TODO i#5675: Add support for other file formats.
    if (ends_with(path, ".sz"))
        return nullptr;
#ifdef HAS_ZIP
    if (ends_with(path, ".zip")) {
        return std::unique_ptr<dynamorio::drmemtrace::record_reader_t>(
            new zipfile_record_file_reader_t(path, verbosity));
    }
#endif
    return std::unique_ptr<dynamorio::drmemtrace::record_reader_t>(
        new default_record_file_reader_t(path, verbosity));
}

template <>
bool
scheduler_tmpl_t<trace_entry_t, record_reader_t>::record_type_has_tid(
    trace_entry_t record, memref_tid_t &tid)
{
    if (record.type != TRACE_TYPE_THREAD)
        return false;
    tid = static_cast<memref_tid_t>(record.addr);
    return true;
}

template <>
bool
scheduler_tmpl_t<trace_entry_t, record_reader_t>::record_type_has_pid(
    trace_entry_t record, memref_pid_t &pid)
{
    if (record.type != TRACE_TYPE_PID)
        return false;
    pid = static_cast<memref_pid_t>(record.addr);
    return true;
}

template <>
void
scheduler_tmpl_t<trace_entry_t, record_reader_t>::record_type_set_tid(
    trace_entry_t &record, memref_tid_t tid)
{
    if (record.type != TRACE_TYPE_THREAD)
        return;
    record.addr = static_cast<addr_t>(tid);
}

template <>
bool
scheduler_tmpl_t<trace_entry_t, record_reader_t>::record_type_is_instr(
    trace_entry_t record)
{
    return type_is_instr(static_cast<trace_type_t>(record.type));
}

template <>
bool
scheduler_tmpl_t<trace_entry_t, record_reader_t>::record_type_is_encoding(
    trace_entry_t record)
{
    return static_cast<trace_type_t>(record.type) == TRACE_TYPE_ENCODING;
}

template <>
typename scheduler_tmpl_t<trace_entry_t, record_reader_t>::stream_status_t
scheduler_tmpl_t<trace_entry_t, record_reader_t>::unread_last_record(
    output_ordinal_t output, trace_entry_t &record, input_info_t *&input)
{
    // See the general unread_last_record() below: we don't support this as
    // we can't provide the prev-prev record for record_type_is_instr_boundary().
    return STATUS_NOT_IMPLEMENTED;
}

template <>
bool
scheduler_tmpl_t<trace_entry_t, record_reader_t>::record_type_is_marker(
    trace_entry_t record, trace_marker_type_t &type, uintptr_t &value)
{
    if (record.type != TRACE_TYPE_MARKER)
        return false;
    type = static_cast<trace_marker_type_t>(record.size);
    value = record.addr;
    return true;
}

template <>
bool
scheduler_tmpl_t<trace_entry_t, record_reader_t>::record_type_is_non_marker_header(
    trace_entry_t record)
{
    return record.type == TRACE_TYPE_HEADER || record.type == TRACE_TYPE_THREAD ||
        record.type == TRACE_TYPE_PID;
}

template <>
bool
scheduler_tmpl_t<trace_entry_t, record_reader_t>::record_type_is_instr_boundary(
    trace_entry_t record, trace_entry_t prev_record)
{
    // Don't advance past encodings or target markers and split them from their
    // associated instr.
    return (record_type_is_instr(record) ||
            record_reader_t::record_is_pre_instr(&record)) &&
        !record_reader_t::record_is_pre_instr(&prev_record);
}

template <>
bool
scheduler_tmpl_t<trace_entry_t, record_reader_t>::record_type_is_timestamp(
    trace_entry_t record, uintptr_t &value)
{
    if (record.type != TRACE_TYPE_MARKER ||
        static_cast<trace_marker_type_t>(record.size) != TRACE_MARKER_TYPE_TIMESTAMP)
        return false;
    value = record.addr;
    return true;
}

template <>
bool
scheduler_tmpl_t<trace_entry_t, record_reader_t>::record_type_is_invalid(
    trace_entry_t record)
{
    return static_cast<trace_type_t>(record.type) == TRACE_TYPE_INVALID;
}

template <>
trace_entry_t
scheduler_tmpl_t<trace_entry_t, record_reader_t>::create_region_separator_marker(
    memref_tid_t tid, uintptr_t value)
{
    // We ignore the tid.
    trace_entry_t record;
    record.type = TRACE_TYPE_MARKER;
    record.size = TRACE_MARKER_TYPE_WINDOW_ID;
    record.addr = value;
    return record;
}

template <>
trace_entry_t
scheduler_tmpl_t<trace_entry_t, record_reader_t>::create_thread_exit(memref_tid_t tid)
{
    trace_entry_t record;
    record.type = TRACE_TYPE_THREAD_EXIT;
    record.size = sizeof(tid);
    record.addr = static_cast<addr_t>(tid);
    return record;
}

template <>
trace_entry_t
scheduler_tmpl_t<trace_entry_t, record_reader_t>::create_invalid_record()
{
    trace_entry_t record;
    record.type = TRACE_TYPE_INVALID;
    record.size = 0;
    record.addr = 0;
    return record;
}

template <>
void
scheduler_tmpl_t<trace_entry_t, record_reader_t>::print_record(
    const trace_entry_t &record)
{
    fprintf(stderr, "type=%d size=%d addr=0x%zx\n", record.type, record.size,
            record.addr);
}

template <>
void
scheduler_tmpl_t<trace_entry_t, record_reader_t>::insert_switch_tid_pid(
    input_info_t &input)
{
    // We need explicit tid,pid records so reader_t will see the new context.
    // We insert at the front, so we have reverse order.
    trace_entry_t pid;
    pid.type = TRACE_TYPE_PID;
    pid.size = 0;
    pid.addr = static_cast<addr_t>(input.pid);

    trace_entry_t tid;
    tid.type = TRACE_TYPE_THREAD;
    tid.size = 0;
    tid.addr = static_cast<addr_t>(input.tid);

    input.queue.push_front(pid);
    input.queue.push_front(tid);
}

/***************************************************************************
 * Scheduled stream.
 */

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::stream_t::next_record(RecordType &record)
{
    return next_record(record, 0);
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::stream_t::next_record(RecordType &record,
                                                                uint64_t cur_time)
{
    if (max_ordinal_ > 0) {
        ++ordinal_;
        if (ordinal_ >= max_ordinal_)
            ordinal_ = 0;
    }
    input_info_t *input = nullptr;
    sched_type_t::stream_status_t res =
        scheduler_->next_record(ordinal_, record, input, cur_time);
    if (res != sched_type_t::STATUS_OK)
        return res;

    // Update our memtrace_stream_t state.
    std::lock_guard<mutex_dbg_owned> guard(*input->lock);
    if (!input->reader->is_record_synthetic())
        ++cur_ref_count_;
    if (scheduler_->record_type_is_instr_boundary(record, prev_record_))
        ++cur_instr_count_;
    VPRINT(scheduler_, 4,
           "stream record#=%" PRId64 ", instr#=%" PRId64 " (cur input %" PRId64
           " record#=%" PRId64 ", instr#=%" PRId64 ")\n",
           cur_ref_count_, cur_instr_count_, input->tid,
           input->reader->get_record_ordinal(), input->reader->get_instruction_ordinal());

    // Update our header state.
    // If we skipped over these, advance_region_of_interest() sets them.
    // TODO i#5843: Check that all inputs have the same top-level headers here.
    // A possible exception is allowing warmup-phase-filtered traces to be mixed
    // with regular traces.
    trace_marker_type_t marker_type;
    uintptr_t marker_value;
    if (scheduler_->record_type_is_marker(record, marker_type, marker_value)) {
        switch (marker_type) {
        case TRACE_MARKER_TYPE_TIMESTAMP:
            last_timestamp_ = marker_value;
            if (first_timestamp_ == 0)
                first_timestamp_ = last_timestamp_;
            break;

        case TRACE_MARKER_TYPE_VERSION: version_ = marker_value; break;
        case TRACE_MARKER_TYPE_FILETYPE: filetype_ = marker_value; break;
        case TRACE_MARKER_TYPE_CACHE_LINE_SIZE: cache_line_size_ = marker_value; break;
        case TRACE_MARKER_TYPE_CHUNK_INSTR_COUNT:
            chunk_instr_count_ = marker_value;
            break;
        case TRACE_MARKER_TYPE_PAGE_SIZE: page_size_ = marker_value; break;
        default: // No action needed.
            break;
        }
    }
    prev_record_ = record;
    return sched_type_t::STATUS_OK;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::stream_t::unread_last_record()
{
    RecordType record;
    input_info_t *input = nullptr;
    auto status = scheduler_->unread_last_record(ordinal_, record, input);
    if (status != sched_type_t::STATUS_OK)
        return status;
    // Restore state.  We document that get_last_timestamp() is not updated.
    std::lock_guard<mutex_dbg_owned> guard(*input->lock);
    if (!input->reader->is_record_synthetic())
        --cur_ref_count_;
    if (scheduler_->record_type_is_instr(record))
        --cur_instr_count_;
    return status;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::stream_t::start_speculation(
    addr_t start_address, bool queue_current_record)
{
    return scheduler_->start_speculation(ordinal_, start_address, queue_current_record);
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::stream_t::stop_speculation()
{
    return scheduler_->stop_speculation(ordinal_);
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::stream_t::set_active(bool active)
{
    return scheduler_->set_output_active(ordinal_, active);
}

/***************************************************************************
 * Scheduler.
 */

template <typename RecordType, typename ReaderType>
scheduler_tmpl_t<RecordType, ReaderType>::~scheduler_tmpl_t()
{
    for (unsigned int i = 0; i < outputs_.size(); ++i) {
        VPRINT(this, 1, "Stats for output #%d\n", i);
        VPRINT(this, 1, "  %-25s: %9" PRId64 "\n", "Switch input->input",
               outputs_[i].stats[memtrace_stream_t::SCHED_STAT_SWITCH_INPUT_TO_INPUT]);
        VPRINT(this, 1, "  %-25s: %9" PRId64 "\n", "Switch input->idle",
               outputs_[i].stats[memtrace_stream_t::SCHED_STAT_SWITCH_INPUT_TO_IDLE]);
        VPRINT(this, 1, "  %-25s: %9" PRId64 "\n", "Switch idle->input",
               outputs_[i].stats[memtrace_stream_t::SCHED_STAT_SWITCH_IDLE_TO_INPUT]);
        VPRINT(this, 1, "  %-25s: %9" PRId64 "\n", "Switch nop",
               outputs_[i].stats[memtrace_stream_t::SCHED_STAT_SWITCH_NOP]);
        VPRINT(this, 1, "  %-25s: %9" PRId64 "\n", "Quantum preempts",
               outputs_[i].stats[memtrace_stream_t::SCHED_STAT_QUANTUM_PREEMPTS]);
        VPRINT(this, 1, "  %-25s: %9" PRId64 "\n", "Direct switch attempts",
               outputs_[i].stats[memtrace_stream_t::SCHED_STAT_DIRECT_SWITCH_ATTEMPTS]);
        VPRINT(this, 1, "  %-25s: %9" PRId64 "\n", "Direct switch successes",
               outputs_[i].stats[memtrace_stream_t::SCHED_STAT_DIRECT_SWITCH_SUCCESSES]);
        VPRINT(this, 1, "  %-25s: %9" PRId64 "\n", "Migrations",
               outputs_[i].stats[memtrace_stream_t::SCHED_STAT_MIGRATIONS]);
    }
#ifndef NDEBUG
    VPRINT(this, 1, "%-27s: %9" PRId64 "\n", "Schedule lock acquired",
           sched_lock_.get_count_acquired());
    VPRINT(this, 1, "%-27s: %9" PRId64 "\n", "Schedule lock contended",
           sched_lock_.get_count_contended());
#endif
}

template <typename RecordType, typename ReaderType>
bool
scheduler_tmpl_t<RecordType, ReaderType>::check_valid_input_limits(
    const input_workload_t &workload, input_reader_info_t &reader_info)
{
    if (!workload.only_shards.empty()) {
        for (input_ordinal_t ord : workload.only_shards) {
            if (ord < 0 || ord >= static_cast<input_ordinal_t>(reader_info.input_count)) {
                error_string_ = "only_shards entry " + std::to_string(ord) +
                    " out of bounds for a shard ordinal";
                return false;
            }
        }
    }
    if (!workload.only_threads.empty()) {
        for (memref_tid_t tid : workload.only_threads) {
            if (reader_info.unfiltered_tids.find(tid) ==
                reader_info.unfiltered_tids.end()) {
                error_string_ = "only_threads entry " + std::to_string(tid) +
                    " not found in workload inputs";
                return false;
            }
        }
    }
    return true;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::init(
    std::vector<input_workload_t> &workload_inputs, int output_count,
    scheduler_options_t options)
{
    options_ = std::move(options);
    verbosity_ = options_.verbosity;
    // workload_inputs is not const so we can std::move readers out of it.
    std::unordered_map<int, std::vector<int>> workload2inputs(workload_inputs.size());
    for (int workload_idx = 0; workload_idx < static_cast<int>(workload_inputs.size());
         ++workload_idx) {
        auto &workload = workload_inputs[workload_idx];
        if (workload.struct_size != sizeof(input_workload_t))
            return STATUS_ERROR_INVALID_PARAMETER;
        if (!workload.only_threads.empty() && !workload.only_shards.empty())
            return STATUS_ERROR_INVALID_PARAMETER;
        input_reader_info_t reader_info;
        reader_info.only_threads = workload.only_threads;
        reader_info.only_shards = workload.only_shards;
        if (workload.path.empty()) {
            if (workload.readers.empty())
                return STATUS_ERROR_INVALID_PARAMETER;
            reader_info.input_count = workload.readers.size();
            for (int i = 0; i < static_cast<int>(workload.readers.size()); ++i) {
                auto &reader = workload.readers[i];
                if (!reader.reader || !reader.end)
                    return STATUS_ERROR_INVALID_PARAMETER;
                reader_info.unfiltered_tids.insert(reader.tid);
                if (!workload.only_threads.empty() &&
                    workload.only_threads.find(reader.tid) == workload.only_threads.end())
                    continue;
                if (!workload.only_shards.empty() &&
                    workload.only_shards.find(i) == workload.only_shards.end())
                    continue;
                int index = static_cast<input_ordinal_t>(inputs_.size());
                inputs_.emplace_back();
                input_info_t &input = inputs_.back();
                input.index = index;
                input.workload = workload_idx;
                workload2inputs[workload_idx].push_back(index);
                input.tid = reader.tid;
                input.reader = std::move(reader.reader);
                input.reader_end = std::move(reader.end);
                input.needs_init = true;
                reader_info.tid2input[input.tid] = input.index;
                tid2input_[workload_tid_t(workload_idx, input.tid)] = index;
            }
        } else {
            if (!workload.readers.empty())
                return STATUS_ERROR_INVALID_PARAMETER;
            sched_type_t::scheduler_status_t res =
                open_readers(workload.path, reader_info);
            if (res != STATUS_SUCCESS)
                return res;
            for (const auto &it : reader_info.tid2input) {
                inputs_[it.second].workload = workload_idx;
                workload2inputs[workload_idx].push_back(it.second);
                tid2input_[workload_tid_t(workload_idx, it.first)] = it.second;
            }
        }
        if (!check_valid_input_limits(workload, reader_info))
            return STATUS_ERROR_INVALID_PARAMETER;
        if (!workload.times_of_interest.empty()) {
            for (const auto &modifiers : workload.thread_modifiers) {
                if (!modifiers.regions_of_interest.empty()) {
                    // We do not support mixing with other ROI specifiers.
                    return STATUS_ERROR_INVALID_PARAMETER;
                }
            }
            sched_type_t::scheduler_status_t status =
                create_regions_from_times(reader_info.tid2input, workload);
            if (status != sched_type_t::STATUS_SUCCESS)
                return STATUS_ERROR_INVALID_PARAMETER;
        }
        for (const auto &modifiers : workload.thread_modifiers) {
            if (modifiers.struct_size != sizeof(input_thread_info_t))
                return STATUS_ERROR_INVALID_PARAMETER;
            const std::vector<memref_tid_t> *which_tids;
            std::vector<memref_tid_t> workload_tid_vector;
            if (modifiers.tids.empty()) {
                // Apply to all tids that have not already been modified.
                for (const auto entry : reader_info.tid2input) {
                    if (!inputs_[entry.second].has_modifier)
                        workload_tid_vector.push_back(entry.first);
                }
                which_tids = &workload_tid_vector;
            } else
                which_tids = &modifiers.tids;
            // We assume the overhead of copying the modifiers for every thread is
            // not high and the simplified code is worthwhile.
            for (memref_tid_t tid : *which_tids) {
                if (reader_info.tid2input.find(tid) == reader_info.tid2input.end())
                    return STATUS_ERROR_INVALID_PARAMETER;
                int index = reader_info.tid2input[tid];
                input_info_t &input = inputs_[index];
                input.has_modifier = true;
                input.binding = modifiers.output_binding;
                input.priority = modifiers.priority;
                for (size_t i = 0; i < modifiers.regions_of_interest.size(); ++i) {
                    const auto &range = modifiers.regions_of_interest[i];
                    VPRINT(this, 3, "ROI #%zu for input %d: [%" PRIu64 ", %" PRIu64 ")\n",
                           i, index, range.start_instruction, range.stop_instruction);
                    if (range.start_instruction == 0 ||
                        (range.stop_instruction < range.start_instruction &&
                         range.stop_instruction != 0))
                        return STATUS_ERROR_INVALID_PARAMETER;
                    if (i == 0)
                        continue;
                    if (range.start_instruction <=
                        modifiers.regions_of_interest[i - 1].stop_instruction) {
                        error_string_ = "gap required between regions of interest";
                        return STATUS_ERROR_INVALID_PARAMETER;
                    }
                }
                input.regions_of_interest = modifiers.regions_of_interest;
            }
        }
    }

    // Legacy field support.
    sched_type_t::scheduler_status_t res = legacy_field_support();
    if (res != sched_type_t::STATUS_SUCCESS)
        return res;

    if (TESTANY(sched_type_t::SCHEDULER_USE_SINGLE_INPUT_ORDINALS, options_.flags) &&
        inputs_.size() == 1 && output_count == 1) {
        options_.flags = static_cast<scheduler_flags_t>(
            static_cast<int>(options_.flags) |
            static_cast<int>(sched_type_t::SCHEDULER_USE_INPUT_ORDINALS));
    }

    // TODO i#5843: Once the speculator supports more options, change the
    // default.  For now we hardcode nops as the only supported option.
    options_.flags = static_cast<scheduler_flags_t>(
        static_cast<int>(options_.flags) |
        static_cast<int>(sched_type_t::SCHEDULER_SPECULATE_NOPS));

    outputs_.reserve(output_count);
    if (options_.single_lockstep_output) {
        global_stream_ = std::unique_ptr<sched_type_t::stream_t>(
            new sched_type_t::stream_t(this, 0, verbosity_, output_count));
    }
    for (int i = 0; i < output_count; ++i) {
        outputs_.emplace_back(this, i,
                              TESTANY(SCHEDULER_SPECULATE_NOPS, options_.flags)
                                  ? spec_type_t::USE_NOPS
                                  // TODO i#5843: Add more flags for other options.
                                  : spec_type_t::LAST_FROM_TRACE,
                              create_invalid_record(), verbosity_);
        if (options_.single_lockstep_output)
            outputs_.back().stream = global_stream_.get();
        if (options_.schedule_record_ostream != nullptr) {
            sched_type_t::stream_status_t status = record_schedule_segment(
                i, schedule_record_t::VERSION, schedule_record_t::VERSION_CURRENT, 0, 0);
            if (status != sched_type_t::STATUS_OK) {
                error_string_ = "Failed to add version to recorded schedule";
                return STATUS_ERROR_FILE_WRITE_FAILED;
            }
        }
    }
    VPRINT(this, 1, "%zu inputs\n", inputs_.size());
    live_input_count_.store(static_cast<int>(inputs_.size()), std::memory_order_release);

    res = read_switch_sequences();
    if (res != sched_type_t::STATUS_SUCCESS)
        return STATUS_ERROR_INVALID_PARAMETER;

    return set_initial_schedule(workload2inputs);
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::legacy_field_support()
{
    if (options_.time_units_per_us == 0) {
        error_string_ = "time_units_per_us must be > 0";
        return STATUS_ERROR_INVALID_PARAMETER;
    }
    if (options_.quantum_duration > 0) {
        if (options_.struct_size > offsetof(scheduler_options_t, quantum_duration_us)) {
            error_string_ = "quantum_duration is deprecated; use quantum_duration_us and "
                            "time_units_per_us or quantum_duration_instrs";
            return STATUS_ERROR_INVALID_PARAMETER;
        }
        if (options_.quantum_unit == QUANTUM_INSTRUCTIONS) {
            options_.quantum_duration_instrs = options_.quantum_duration;
        } else {
            options_.quantum_duration_us =
                static_cast<uint64_t>(static_cast<double>(options_.quantum_duration) /
                                      options_.time_units_per_us);
            VPRINT(this, 2,
                   "Legacy support: setting quantum_duration_us to %" PRIu64 "\n",
                   options_.quantum_duration_us);
        }
    }
    if (options_.quantum_duration_us == 0) {
        error_string_ = "quantum_duration_us must be > 0";
        return STATUS_ERROR_INVALID_PARAMETER;
    }
    if (options_.block_time_scale > 0) {
        if (options_.struct_size > offsetof(scheduler_options_t, block_time_multiplier)) {
            error_string_ = "quantum_duration is deprecated; use block_time_multiplier "
                            "and time_units_per_us";
            return STATUS_ERROR_INVALID_PARAMETER;
        }
        options_.block_time_multiplier =
            static_cast<double>(options_.block_time_scale) / options_.time_units_per_us;
        VPRINT(this, 2, "Legacy support: setting block_time_multiplier to %6.3f\n",
               options_.block_time_multiplier);
    }
    if (options_.block_time_multiplier == 0) {
        error_string_ = "block_time_multiplier must != 0";
        return STATUS_ERROR_INVALID_PARAMETER;
    }
    if (options_.block_time_max > 0) {
        if (options_.struct_size > offsetof(scheduler_options_t, block_time_max_us)) {
            error_string_ = "quantum_duration is deprecated; use block_time_max_us "
                            "and time_units_per_us";
            return STATUS_ERROR_INVALID_PARAMETER;
        }
        options_.block_time_max_us = static_cast<uint64_t>(
            static_cast<double>(options_.block_time_max) / options_.time_units_per_us);
        VPRINT(this, 2, "Legacy support: setting block_time_max_us to %" PRIu64 "\n",
               options_.block_time_max_us);
    }
    if (options_.block_time_max_us == 0) {
        error_string_ = "block_time_max_us must be > 0";
        return STATUS_ERROR_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::set_initial_schedule(
    std::unordered_map<int, std::vector<int>> &workload2inputs)
{
    bool need_lock;
    auto scoped_lock = acquire_scoped_sched_lock_if_necessary(need_lock);
    // Determine whether we need to read ahead in the inputs.  There are cases where we
    // do not want to do that as it would block forever if the inputs are not available
    // (e.g., online analysis IPC readers); it also complicates ordinals so we avoid it
    // if we can and enumerate all the cases that do need it.
    bool gather_timestamps = false;
    if (((options_.mapping == MAP_AS_PREVIOUSLY ||
          options_.mapping == MAP_TO_ANY_OUTPUT) &&
         options_.deps == DEPENDENCY_TIMESTAMPS) ||
        (options_.mapping == MAP_TO_RECORDED_OUTPUT &&
         options_.replay_as_traced_istream == nullptr && inputs_.size() > 1)) {
        gather_timestamps = true;
        if (!options_.read_inputs_in_init) {
            error_string_ = "Timestamp dependencies require read_inputs_in_init";
            return STATUS_ERROR_INVALID_PARAMETER;
        }
    }
    // The filetype, if present, is before the first timestamp.  If we only need the
    // filetype we avoid going as far as the timestamp.
    bool gather_filetype = options_.read_inputs_in_init;
    if (gather_filetype || gather_timestamps) {
        sched_type_t::scheduler_status_t res =
            get_initial_input_content(gather_timestamps);
        if (res != STATUS_SUCCESS) {
            error_string_ = "Failed to read initial input contents for filetype";
            if (gather_timestamps)
                error_string_ += " and initial timestamps";
            return res;
        }
    }

    if (options_.mapping == MAP_AS_PREVIOUSLY) {
        live_replay_output_count_.store(static_cast<int>(outputs_.size()),
                                        std::memory_order_release);
        if (options_.schedule_replay_istream == nullptr ||
            options_.schedule_record_ostream != nullptr)
            return STATUS_ERROR_INVALID_PARAMETER;
        sched_type_t::scheduler_status_t status = read_recorded_schedule();
        if (status != sched_type_t::STATUS_SUCCESS)
            return STATUS_ERROR_INVALID_PARAMETER;
        if (options_.deps == DEPENDENCY_TIMESTAMPS) {
            // Match the ordinals from the original run by pre-reading the timestamps.
            assert(gather_timestamps);
        }
    } else if (options_.schedule_replay_istream != nullptr) {
        return STATUS_ERROR_INVALID_PARAMETER;
    } else if (options_.mapping == MAP_TO_CONSISTENT_OUTPUT) {
        // Assign the inputs up front to avoid locks once we're in parallel mode.
        // We use a simple round-robin static assignment for now.
        for (int i = 0; i < static_cast<input_ordinal_t>(inputs_.size()); ++i) {
            size_t index = i % outputs_.size();
            if (outputs_[index].input_indices.empty())
                set_cur_input(static_cast<input_ordinal_t>(index), i);
            outputs_[index].input_indices.push_back(i);
            VPRINT(this, 2, "Assigning input #%d to output #%zd\n", i, index);
        }
    } else if (options_.mapping == MAP_TO_RECORDED_OUTPUT) {
        if (options_.replay_as_traced_istream != nullptr) {
            // Even for just one output we honor a request to replay the schedule
            // (although it should match the analyzer serial mode so there's no big
            // benefit to reading the schedule file.  The analyzer serial mode or other
            // special cases of one output don't set the replay_as_traced_istream
            // field.)
            sched_type_t::scheduler_status_t status =
                read_and_instantiate_traced_schedule();
            if (status != sched_type_t::STATUS_SUCCESS)
                return STATUS_ERROR_INVALID_PARAMETER;
            // Now leverage the regular replay code.
            options_.mapping = MAP_AS_PREVIOUSLY;
        } else if (outputs_.size() > 1) {
            return STATUS_ERROR_INVALID_PARAMETER;
        } else if (inputs_.size() == 1) {
            set_cur_input(0, 0);
        } else {
            // The old file_reader_t interleaving would output the top headers for every
            // thread first and then pick the oldest timestamp once it reached a
            // timestamp. We instead queue those headers so we can start directly with the
            // oldest timestamp's thread.
            assert(gather_timestamps);
            uint64_t min_time = std::numeric_limits<uint64_t>::max();
            input_ordinal_t min_input = -1;
            for (int i = 0; i < static_cast<input_ordinal_t>(inputs_.size()); ++i) {
                if (inputs_[i].next_timestamp < min_time) {
                    min_time = inputs_[i].next_timestamp;
                    min_input = i;
                }
            }
            if (min_input < 0)
                return STATUS_ERROR_INVALID_PARAMETER;
            set_cur_input(0, static_cast<input_ordinal_t>(min_input));
        }
    } else {
        // Assign initial inputs.
        if (options_.deps == DEPENDENCY_TIMESTAMPS) {
            assert(gather_timestamps);
            // Compute the min timestamp (==base_timestamp) per workload and sort
            // all inputs by relative time from the base.
            for (int workload_idx = 0;
                 workload_idx < static_cast<int>(workload2inputs.size());
                 ++workload_idx) {
                uint64_t min_time = std::numeric_limits<uint64_t>::max();
                input_ordinal_t min_input = -1;
                for (int input_idx : workload2inputs[workload_idx]) {
                    if (inputs_[input_idx].next_timestamp < min_time) {
                        min_time = inputs_[input_idx].next_timestamp;
                        min_input = input_idx;
                    }
                }
                if (min_input < 0)
                    return STATUS_ERROR_INVALID_PARAMETER;
                for (int input_idx : workload2inputs[workload_idx]) {
                    VPRINT(this, 4,
                           "workload %d: setting input %d base_timestamp to %" PRIu64
                           " vs next_timestamp %zu\n",
                           workload_idx, input_idx, min_time,
                           inputs_[input_idx].next_timestamp);
                    inputs_[input_idx].base_timestamp = min_time;
                    inputs_[input_idx].order_by_timestamp = true;
                }
            }
            // We'll pick the starting inputs below by sorting by relative time from
            // each workload's base_timestamp, which our queue does for us.
        }
        // We need to honor output bindings and possibly time ordering, which our queue
        // does for us.  We want the rest of the inputs in the queue in any case so it is
        // simplest to insert all and remove the first N.
        for (int i = 0; i < static_cast<input_ordinal_t>(inputs_.size()); ++i) {
            add_to_ready_queue(&inputs_[i]);
        }
        for (int i = 0; i < static_cast<output_ordinal_t>(outputs_.size()); ++i) {
            input_info_t *queue_next;
#ifndef NDEBUG
            sched_type_t::stream_status_t status =
#endif
                pop_from_ready_queue(i, queue_next);
            assert(status == STATUS_OK || status == STATUS_IDLE);
            if (queue_next == nullptr)
                set_cur_input(i, INVALID_INPUT_ORDINAL);
            else
                set_cur_input(i, queue_next->index);
        }
    }
    return STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
std::string
scheduler_tmpl_t<RecordType, ReaderType>::recorded_schedule_component_name(
    output_ordinal_t output)
{
    static const char *const SCHED_CHUNK_PREFIX = "output.";
    std::ostringstream name;
    name << SCHED_CHUNK_PREFIX << std::setfill('0') << std::setw(4) << output;
    return name.str();
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::write_recorded_schedule()
{
    if (options_.schedule_record_ostream == nullptr)
        return STATUS_ERROR_INVALID_PARAMETER;
    std::lock_guard<mutex_dbg_owned> guard(sched_lock_);
    for (int i = 0; i < static_cast<int>(outputs_.size()); ++i) {
        sched_type_t::stream_status_t status =
            record_schedule_segment(i, schedule_record_t::FOOTER, 0, 0, 0);
        if (status != sched_type_t::STATUS_OK)
            return STATUS_ERROR_FILE_WRITE_FAILED;
        std::string name = recorded_schedule_component_name(i);
        std::string err = options_.schedule_record_ostream->open_new_component(name);
        if (!err.empty()) {
            VPRINT(this, 1, "Failed to open component %s in record file: %s\n",
                   name.c_str(), err.c_str());
            return STATUS_ERROR_FILE_WRITE_FAILED;
        }
        if (!options_.schedule_record_ostream->write(
                reinterpret_cast<char *>(outputs_[i].record.data()),
                outputs_[i].record.size() * sizeof(outputs_[i].record[0])))
            return STATUS_ERROR_FILE_WRITE_FAILED;
    }
    return STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::read_recorded_schedule()
{
    if (options_.schedule_replay_istream == nullptr)
        return STATUS_ERROR_INVALID_PARAMETER;

    schedule_record_t record;
    // We assume we can easily fit the whole context switch sequence in memory.
    // If that turns out not to be the case for very long traces, we deliberately
    // used an archive format so we could do parallel incremental reads.
    // (Conversely, if we want to commit to storing in memory, we could use a
    // non-archive format and store the output ordinal in the version record.)
    for (int i = 0; i < static_cast<int>(outputs_.size()); ++i) {
        std::string err = options_.schedule_replay_istream->open_component(
            recorded_schedule_component_name(i));
        if (!err.empty()) {
            error_string_ = "Failed to open schedule_replay_istream component " +
                recorded_schedule_component_name(i) + ": " + err;
            return STATUS_ERROR_INVALID_PARAMETER;
        }
        // XXX: This could be made more efficient if we stored the record count
        // in the version field's stop_instruction field or something so we can
        // size the vector up front.  As this only happens once we do not bother
        // and live with a few vector resizes.
        bool saw_footer = false;
        while (options_.schedule_replay_istream->read(reinterpret_cast<char *>(&record),
                                                      sizeof(record))) {
            if (record.type == schedule_record_t::VERSION) {
                if (record.key.version != schedule_record_t::VERSION_CURRENT)
                    return STATUS_ERROR_INVALID_PARAMETER;
            } else if (record.type == schedule_record_t::FOOTER) {
                saw_footer = true;
                break;
            } else
                outputs_[i].record.push_back(record);
        }
        if (!saw_footer) {
            error_string_ = "Record file missing footer";
            return STATUS_ERROR_INVALID_PARAMETER;
        }
        VPRINT(this, 1, "Read %zu recorded records for output #%d\n",
               outputs_[i].record.size(), i);
    }
    // See if there was more data in the file (we do this after reading to not
    // mis-report i/o or path errors as this error).
    std::string err = options_.schedule_replay_istream->open_component(
        recorded_schedule_component_name(static_cast<output_ordinal_t>(outputs_.size())));
    if (err.empty()) {
        error_string_ = "Not enough output streams for recorded file";
        return STATUS_ERROR_INVALID_PARAMETER;
    }
    for (int i = 0; i < static_cast<output_ordinal_t>(outputs_.size()); ++i) {
        if (outputs_[i].record.empty()) {
            // XXX i#6630: We should auto-set the output count and avoid
            // having extra outputs; these complicate idle computations, etc.
            VPRINT(this, 1, "output %d empty: returning eof up front\n", i);
            set_cur_input(i, INVALID_INPUT_ORDINAL);
            outputs_[i].at_eof = true;
        } else if (outputs_[i].record[0].type == schedule_record_t::IDLE) {
            set_cur_input(i, INVALID_INPUT_ORDINAL);
            outputs_[i].waiting = true;
            outputs_[i].wait_start_time = 0; // Updated on first next_record().
            VPRINT(this, 3, "output %d starting out idle\n", i);
        } else {
            assert(outputs_[i].record[0].type == schedule_record_t::DEFAULT);
            set_cur_input(i, outputs_[i].record[0].key.input);
        }
    }
    return STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::read_and_instantiate_traced_schedule()
{
    std::vector<std::set<uint64_t>> start2stop(inputs_.size());
    // We also want to collapse same-cpu consecutive records so we start with
    // a temporary local vector.
    std::vector<std::vector<schedule_output_tracker_t>> all_sched(outputs_.size());
    // Work around i#6107 by tracking counts sorted by timestamp for each input.
    std::vector<std::vector<schedule_input_tracker_t>> input_sched(inputs_.size());
    // These hold entries added in the on-disk (unsorted) order.
    std::vector<output_ordinal_t> disk_ord2index; // Initially [i] holds i.
    std::vector<uint64_t> disk_ord2cpuid;         // [i] holds cpuid for entry i.
    sched_type_t::scheduler_status_t res = read_traced_schedule(
        input_sched, start2stop, all_sched, disk_ord2index, disk_ord2cpuid);
    if (res != sched_type_t::STATUS_SUCCESS)
        return res;
    // Sort by cpuid to get a more natural ordering.
    // Probably raw2trace should do this in the first place, but we have many
    // schedule files already out there so we still need a sort here.
    // If we didn't have cross-indices pointing at all_sched from input_sched, we
    // would just sort all_sched: but instead we have to construct a separate
    // ordering structure.
    std::sort(disk_ord2index.begin(), disk_ord2index.end(),
              [disk_ord2cpuid](const output_ordinal_t &l, const output_ordinal_t &r) {
                  return disk_ord2cpuid[l] < disk_ord2cpuid[r];
              });
    // disk_ord2index[i] used to hold i; now after sorting it holds the ordinal in
    // the disk file that has the ith largest cpuid.  We need to turn that into
    // the output_idx ordinal for the cpu at ith ordinal in the disk file, for
    // which we use a new vector disk_ord2output.
    // E.g., if the original file was in this order disk_ord2cpuid = {6,2,3,7},
    // disk_ord2index after sorting would hold {1,2,0,3}, which we want to turn
    // into disk_ord2output = {2,0,1,3}.
    std::vector<output_ordinal_t> disk_ord2output(disk_ord2index.size());
    for (size_t i = 0; i < disk_ord2index.size(); ++i) {
        disk_ord2output[disk_ord2index[i]] = static_cast<output_ordinal_t>(i);
    }
    for (int disk_idx = 0; disk_idx < static_cast<output_ordinal_t>(outputs_.size());
         ++disk_idx) {
        if (disk_idx >= static_cast<int>(disk_ord2index.size())) {
            // XXX i#6630: We should auto-set the output count and avoid
            // having extra ouputs; these complicate idle computations, etc.
            VPRINT(this, 1, "Output %d empty: returning eof up front\n", disk_idx);
            outputs_[disk_idx].at_eof = true;
            set_cur_input(disk_idx, INVALID_INPUT_ORDINAL);
            continue;
        }
        output_ordinal_t output_idx = disk_ord2output[disk_idx];
        VPRINT(this, 1, "Read %zu as-traced records for output #%d\n",
               all_sched[disk_idx].size(), output_idx);
        outputs_[output_idx].as_traced_cpuid = disk_ord2cpuid[disk_idx];
        VPRINT(this, 1, "Output #%d is as-traced CPU #%" PRId64 "\n", output_idx,
               outputs_[output_idx].as_traced_cpuid);
        // Update the stop_instruction field and collapse consecutive entries while
        // inserting into the final location.
        int start_consec = -1;
        for (int sched_idx = 0; sched_idx < static_cast<int>(all_sched[disk_idx].size());
             ++sched_idx) {
            auto &segment = all_sched[disk_idx][sched_idx];
            if (!segment.valid)
                continue;
            auto find = start2stop[segment.input].find(segment.start_instruction);
            ++find;
            if (find == start2stop[segment.input].end())
                segment.stop_instruction = std::numeric_limits<uint64_t>::max();
            else
                segment.stop_instruction = *find;
            VPRINT(this, 4,
                   "as-read segment #%d: input=%d start=%" PRId64 " stop=%" PRId64
                   " time=%" PRId64 "\n",
                   sched_idx, segment.input, segment.start_instruction,
                   segment.stop_instruction, segment.timestamp);
            if (sched_idx + 1 < static_cast<int>(all_sched[disk_idx].size()) &&
                segment.input == all_sched[disk_idx][sched_idx + 1].input &&
                segment.stop_instruction >
                    all_sched[disk_idx][sched_idx + 1].start_instruction) {
                // A second sanity check.
                error_string_ = "Invalid decreasing start field in schedule file";
                return STATUS_ERROR_INVALID_PARAMETER;
            } else if (sched_idx + 1 < static_cast<int>(all_sched[disk_idx].size()) &&
                       segment.input == all_sched[disk_idx][sched_idx + 1].input &&
                       segment.stop_instruction ==
                           all_sched[disk_idx][sched_idx + 1].start_instruction) {
                // Collapse into next.
                if (start_consec == -1)
                    start_consec = sched_idx;
            } else {
                schedule_output_tracker_t &toadd = start_consec >= 0
                    ? all_sched[disk_idx][start_consec]
                    : all_sched[disk_idx][sched_idx];
                outputs_[output_idx].record.emplace_back(
                    schedule_record_t::DEFAULT, toadd.input, toadd.start_instruction,
                    all_sched[disk_idx][sched_idx].stop_instruction, toadd.timestamp);
                start_consec = -1;
                VDO(this, 3, {
                    auto &added = outputs_[output_idx].record.back();
                    VPRINT(this, 3,
                           "segment #%zu: input=%d start=%" PRId64 " stop=%" PRId64
                           " time=%" PRId64 "\n",
                           outputs_[output_idx].record.size() - 1, added.key.input,
                           added.value.start_instruction, added.stop_instruction,
                           added.timestamp);
                });
            }
        }
        VPRINT(this, 1, "Collapsed duplicates for %zu as-traced records for output #%d\n",
               outputs_[output_idx].record.size(), output_idx);
        if (outputs_[output_idx].record.empty()) {
            error_string_ = "Empty as-traced schedule";
            return STATUS_ERROR_INVALID_PARAMETER;
        }
        if (outputs_[output_idx].record[0].value.start_instruction != 0) {
            VPRINT(this, 1, "Initial input for output #%d is: wait state\n", output_idx);
            set_cur_input(output_idx, INVALID_INPUT_ORDINAL);
            outputs_[output_idx].waiting = true;
            outputs_[output_idx].record_index = -1;
        } else {
            VPRINT(this, 1, "Initial input for output #%d is %d\n", output_idx,
                   outputs_[output_idx].record[0].key.input);
            set_cur_input(output_idx, outputs_[output_idx].record[0].key.input);
        }
    }
    return STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::create_regions_from_times(
    const std::unordered_map<memref_tid_t, int> &workload_tids,
    input_workload_t &workload)
{
    // First, read from the as-traced schedule file into data structures shared with
    // replay-as-traced.
    std::vector<std::vector<schedule_input_tracker_t>> input_sched(inputs_.size());
    // These are all unused.
    std::vector<std::set<uint64_t>> start2stop(inputs_.size());
    std::vector<std::vector<schedule_output_tracker_t>> all_sched;
    std::vector<output_ordinal_t> disk_ord2index;
    std::vector<uint64_t> disk_ord2cpuid;
    sched_type_t::scheduler_status_t res = read_traced_schedule(
        input_sched, start2stop, all_sched, disk_ord2index, disk_ord2cpuid);
    if (res != sched_type_t::STATUS_SUCCESS)
        return res;
    // Do not allow a replay mode to start later.
    options_.replay_as_traced_istream = nullptr;

    // Now create an interval tree of timestamps (with instr ordinals as payloads)
    // for each input. As our intervals do not overlap and have no gaps we need
    // no size, just the start address key.
    std::vector<std::map<uint64_t, uint64_t>> time_tree(inputs_.size());
    for (int input_idx = 0; input_idx < static_cast<input_ordinal_t>(inputs_.size());
         ++input_idx) {
        for (int sched_idx = 0;
             sched_idx < static_cast<int>(input_sched[input_idx].size()); ++sched_idx) {
            schedule_input_tracker_t &sched = input_sched[input_idx][sched_idx];
            VPRINT(this, 4, "as-read: input=%d start=%" PRId64 " time=%" PRId64 "\n",
                   input_idx, sched.start_instruction, sched.timestamp);
            time_tree[input_idx][sched.timestamp] = sched.start_instruction;
        }
    }

    // Finally, convert the requested time ranges into instr ordinal ranges.
    for (const auto &tid_it : workload_tids) {
        std::vector<range_t> instr_ranges;
        bool entire_tid = false;
        for (const auto &times : workload.times_of_interest) {
            uint64_t instr_start = 0, instr_end = 0;
            bool has_start = time_tree_lookup(time_tree[tid_it.second],
                                              times.start_timestamp, instr_start);
            bool has_end;
            if (times.stop_timestamp == 0)
                has_end = true;
            else {
                has_end = time_tree_lookup(time_tree[tid_it.second], times.stop_timestamp,
                                           instr_end);
            }
            if (has_start && has_end && instr_start == instr_end) {
                if (instr_start == 0 && instr_end == 0) {
                    entire_tid = true;
                } else {
                    ++instr_end;
                }
            }
            // If !has_start we'll include from 0.  The start timestamp will make it be
            // scheduled last but there will be no delay if no other thread is available.
            // If !has_end, instr_end will still be 0 which means the end of the trace.
            if (instr_start > 0 || instr_end > 0) {
                if (!instr_ranges.empty() &&
                    (instr_ranges.back().stop_instruction >= instr_start ||
                     instr_ranges.back().stop_instruction == 0)) {
                    error_string_ =
                        "times_of_interest are too close together: "
                        "corresponding instruction ordinals are overlapping or adjacent";
                    return STATUS_ERROR_INVALID_PARAMETER;
                }
                instr_ranges.emplace_back(instr_start, instr_end);
                VPRINT(this, 2,
                       "tid %" PRIu64 " overlaps with times_of_interest [%" PRIu64
                       ", %" PRIu64 ") @ [%" PRIu64 ", %" PRIu64 ")\n",
                       tid_it.first, times.start_timestamp, times.stop_timestamp,
                       instr_start, instr_end);
            }
        }
        if (!entire_tid && instr_ranges.empty()) {
            // Exclude this thread completely.  We've already created its
            // inputs_ entry with cross-indices stored in other structures
            // so instead of trying to erase it we give it a max start point.
            VPRINT(this, 2,
                   "tid %" PRIu64 " has no overlap with any times_of_interest entry\n",
                   tid_it.first);
            instr_ranges.emplace_back(std::numeric_limits<uint64_t>::max(), 0);
        }
        if (entire_tid) {
            // No range is needed.
        } else {
            workload.thread_modifiers.emplace_back(instr_ranges);
            workload.thread_modifiers.back().tids.emplace_back(tid_it.first);
        }
    }
    return sched_type_t::STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
bool
scheduler_tmpl_t<RecordType, ReaderType>::time_tree_lookup(
    const std::map<uint64_t, uint64_t> &tree, uint64_t time, uint64_t &ordinal)
{
    auto it = tree.upper_bound(time);
    if (it == tree.begin() || it == tree.end()) {
        // We do not have a timestamp in the footer, so we assume any time
        // past the final known timestamp is too far and do not try to
        // fit into the final post-last-timestamp sequence.
        return false;
    }
    uint64_t upper_time = it->first;
    uint64_t upper_ord = it->second;
    it--;
    uint64_t lower_time = it->first;
    uint64_t lower_ord = it->second;
    double fraction = (time - lower_time) / static_cast<double>(upper_time - lower_time);
    double interpolate = lower_ord + fraction * (upper_ord - lower_ord);
    // We deliberately round down to ensure we include a system call that spans
    // the start time, so we'll get the right starting behavior for a thread that
    // should be blocked or unscheduled at this point in time (though the blocked
    // time might be too long as it starts before this target time).
    ordinal = static_cast<uint64_t>(interpolate);
    VPRINT(this, 3,
           "time2ordinal: time %" PRIu64 " => times [%" PRIu64 ", %" PRIu64
           ") ords [%" PRIu64 ", %" PRIu64 ") => interpolated %" PRIu64 "\n",
           time, lower_time, upper_time, lower_ord, upper_ord, ordinal);
    return true;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::read_traced_schedule(
    std::vector<std::vector<schedule_input_tracker_t>> &input_sched,
    std::vector<std::set<uint64_t>> &start2stop,
    std::vector<std::vector<schedule_output_tracker_t>> &all_sched,
    std::vector<output_ordinal_t> &disk_ord2index, std::vector<uint64_t> &disk_ord2cpuid)
{
    if (options_.replay_as_traced_istream == nullptr) {
        error_string_ = "Missing as-traced istream";
        return STATUS_ERROR_INVALID_PARAMETER;
    }

    schedule_entry_t entry(0, 0, 0, 0);
    // See comment in read_recorded_schedule() on our assumption that we can
    // easily fit the whole context switch sequence in memory.  This cpu_schedule
    // file has an entry per timestamp, though, even for consecutive ones on the same
    // core, so it uses more memory.
    // We do not have a subfile listing feature in archive_istream_t, but we can
    // read sequentially as each record has a cpu field.
    // This schedule_entry_t format doesn't have the stop instruction ordinal (as it was
    // designed for skip targets only), so we take two passes to get that information.
    // If we do find memory is an issue we could add a stop field to schedule_entry_t
    // and collapse as we go, saving memory.
    // We also need to translate the thread and cpu id values into 0-based ordinals.
    std::unordered_map<memref_tid_t, input_ordinal_t> tid2input;
    for (int i = 0; i < static_cast<input_ordinal_t>(inputs_.size()); ++i) {
        tid2input[inputs_[i].tid] = i;
    }
    // We initially number the outputs according to their order in the file, and then
    // sort by the stored cpuid below.
    // XXX i#6726: Should we support some direction from the user on this?  Simulation
    // may want to preserve the NUMA relationships and may need to set up its simulated
    // cores at init time, so it would prefer to partition by output stream identifier.
    // Maybe we could at least add the proposed memtrace_stream_t query for cpuid and
    // let it be called even before reading any records at all?
    output_ordinal_t cur_output = 0;
    uint64_t cur_cpu = std::numeric_limits<uint64_t>::max();
    while (options_.replay_as_traced_istream->read(reinterpret_cast<char *>(&entry),
                                                   sizeof(entry))) {
        if (entry.cpu != cur_cpu) {
            // This is a zipfile component boundary: one conmponent per cpu.
            if (cur_cpu != std::numeric_limits<uint64_t>::max()) {
                ++cur_output;
                if (options_.mapping == MAP_TO_RECORDED_OUTPUT && !outputs_.empty() &&
                    cur_output >= static_cast<int>(outputs_.size())) {
                    error_string_ = "replay_as_traced_istream cpu count != output count";
                    return STATUS_ERROR_INVALID_PARAMETER;
                }
            }
            cur_cpu = entry.cpu;
            disk_ord2cpuid.push_back(cur_cpu);
            disk_ord2index.push_back(cur_output);
        }
        input_ordinal_t input = tid2input[entry.thread];
        // The caller must fill in the stop ordinal in a second pass.
        uint64_t start = entry.start_instruction;
        uint64_t timestamp = entry.timestamp;
        // Some entries have no instructions (there is an entry for each timestamp, and
        // a signal can come in after a prior timestamp with no intervening instrs).
        if (all_sched.size() < static_cast<size_t>(cur_output + 1))
            all_sched.resize(cur_output + 1);
        if (!all_sched[cur_output].empty() &&
            input == all_sched[cur_output].back().input &&
            start == all_sched[cur_output].back().start_instruction) {
            VPRINT(this, 3,
                   "Output #%d: as-read segment #%zu has no instructions: skipping\n",
                   cur_output, all_sched[cur_output].size() - 1);
            continue;
        }
        all_sched[cur_output].emplace_back(true, input, start, timestamp);
        start2stop[input].insert(start);
        input_sched[input].emplace_back(cur_output, all_sched[cur_output].size() - 1,
                                        start, timestamp);
    }
    sched_type_t::scheduler_status_t res =
        check_and_fix_modulo_problem_in_schedule(input_sched, start2stop, all_sched);
    if (res != sched_type_t::STATUS_SUCCESS)
        return res;
    return remove_zero_instruction_segments(input_sched, all_sched);
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::remove_zero_instruction_segments(
    std::vector<std::vector<schedule_input_tracker_t>> &input_sched,
    std::vector<std::vector<schedule_output_tracker_t>> &all_sched)

{
    // For a cpuid pair with no instructions in between, our
    // instruction-ordinal-based control points cannot model both sides.
    // For example:
    //    5   0:  1294139 <marker: page size 4096>
    //    6   0:  1294139 <marker: timestamp 13344214879969223>
    //    7   0:  1294139 <marker: tid 1294139 on core 2>
    //    8   0:  1294139 <marker: function==syscall #202>
    //    9   0:  1294139 <marker: function return value 0xffffffffffffff92>
    //   10   0:  1294139 <marker: system call failed: 110>
    //   11   0:  1294139 <marker: timestamp 13344214880209404>
    //   12   0:  1294139 <marker: tid 1294139 on core 2>
    //   13   1:  1294139 ifetch 3 byte(s) @ 0x0000563642cc5e75 8d 50 0b  lea...
    // That sequence has 2 different cpu_schedule file entries for that input
    // starting at instruction 0, which causes confusion when determining endpoints.
    // We just drop the older entry and keep the later one, which is the one bundled
    // with actual instructions.
    //
    // Should we not have instruction-based control points? The skip and
    // region-of-interest features were designed thinking about instructions, the more
    // natural unit for microarchitectural simulators.  It seemed like that was much more
    // usable for a user, and translated to other venues like PMU counts.  The scheduler
    // replay features were also designed that way.  But, that makes the infrastructure
    // messy as the underlying records are not built that way.  Xref i#6716 on an
    // instruction-based iterator.
    for (int input_idx = 0; input_idx < static_cast<input_ordinal_t>(inputs_.size());
         ++input_idx) {
        std::sort(
            input_sched[input_idx].begin(), input_sched[input_idx].end(),
            [](const schedule_input_tracker_t &l, const schedule_input_tracker_t &r) {
                return l.timestamp < r.timestamp;
            });
        uint64_t prev_start = 0;
        for (size_t i = 0; i < input_sched[input_idx].size(); ++i) {
            uint64_t start = input_sched[input_idx][i].start_instruction;
            assert(start >= prev_start);
            if (i > 0 && start == prev_start) {
                // Keep the newer one.
                VPRINT(this, 1, "Dropping same-input=%d same-start=%" PRIu64 " entry\n",
                       input_idx, start);
                all_sched[input_sched[input_idx][i - 1].output]
                         [static_cast<size_t>(
                              input_sched[input_idx][i - 1].output_array_idx)]
                             .valid = false;
                // If code after this used input_sched we would want to erase the
                // entry, but we have no further use so we leave it.
            }
            prev_start = start;
        }
    }
    return STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::check_and_fix_modulo_problem_in_schedule(
    std::vector<std::vector<schedule_input_tracker_t>> &input_sched,
    std::vector<std::set<uint64_t>> &start2stop,
    std::vector<std::vector<schedule_output_tracker_t>> &all_sched)

{
    // Work around i#6107 where the counts in the file are incorrectly modulo the chunk
    // size.  Unfortunately we need to construct input_sched and sort it for each input
    // in order to even detect this issue; we could bump the trace version to let us
    // know it's not present if these steps become overhead concerns.

    // We store the actual instruction count for each timestamp, for each input, keyed
    // by timestamp so we can look it up when iterating over the per-cpu schedule.  We
    // do not support consecutive identical timestamps in one input for this workaround.
    std::vector<std::unordered_map<uint64_t, uint64_t>> timestamp2adjust(inputs_.size());

    // We haven't read into the trace far enough to find the actual chunk size, so for
    // this workaround we only support what was the default in raw2trace up to this
    // point, 10M.
    static constexpr uint64_t DEFAULT_CHUNK_SIZE = 10 * 1000 * 1000;

    // For each input, sort and walk the schedule and look for decreasing counts.
    // Construct timestamp2adjust so we can fix the other data structures if necessary.
    bool found_i6107 = false;
    for (int input_idx = 0; input_idx < static_cast<input_ordinal_t>(inputs_.size());
         ++input_idx) {
        std::sort(
            input_sched[input_idx].begin(), input_sched[input_idx].end(),
            [](const schedule_input_tracker_t &l, const schedule_input_tracker_t &r) {
                return l.timestamp < r.timestamp;
            });
        uint64_t prev_start = 0;
        uint64_t add_to_start = 0;
        bool in_order = true;
        for (schedule_input_tracker_t &sched : input_sched[input_idx]) {
            if (sched.start_instruction < prev_start) {
                // If within 50% of the end of the chunk we assume it's i#6107.
                if (prev_start * 2 > DEFAULT_CHUNK_SIZE) {
                    add_to_start += DEFAULT_CHUNK_SIZE;
                    if (in_order) {
                        VPRINT(this, 2, "Working around i#6107 for input #%d\n",
                               input_idx);
                        in_order = false;
                        found_i6107 = true;
                    }
                } else {
                    error_string_ = "Invalid decreasing start field in schedule file";
                    return STATUS_ERROR_INVALID_PARAMETER;
                }
            }
            // We could save space by not storing the early ones but we do need to
            // include all duplicates.
            if (timestamp2adjust[input_idx].find(sched.timestamp) !=
                timestamp2adjust[input_idx].end()) {
                error_string_ = "Same timestamps not supported for i#6107 workaround";
                return STATUS_ERROR_INVALID_PARAMETER;
            }
            prev_start = sched.start_instruction;
            timestamp2adjust[input_idx][sched.timestamp] =
                sched.start_instruction + add_to_start;
            sched.start_instruction += add_to_start;
        }
    }
    if (!found_i6107)
        return STATUS_SUCCESS;
    // Rebuild start2stop.
    for (int input_idx = 0; input_idx < static_cast<input_ordinal_t>(inputs_.size());
         ++input_idx) {
        start2stop[input_idx].clear();
        for (auto &keyval : timestamp2adjust[input_idx]) {
            start2stop[input_idx].insert(keyval.second);
        }
    }
    // Update all_sched.
    for (int output_idx = 0; output_idx < static_cast<output_ordinal_t>(outputs_.size());
         ++output_idx) {
        for (int sched_idx = 0;
             sched_idx < static_cast<int>(all_sched[output_idx].size()); ++sched_idx) {
            auto &segment = all_sched[output_idx][sched_idx];
            if (!segment.valid)
                continue;
            auto it = timestamp2adjust[segment.input].find(segment.timestamp);
            if (it == timestamp2adjust[segment.input].end()) {
                error_string_ = "Failed to find timestamp for i#6107 workaround";
                return STATUS_ERROR_INVALID_PARAMETER;
            }
            assert(it->second >= segment.start_instruction);
            assert(it->second % DEFAULT_CHUNK_SIZE == segment.start_instruction);
            if (it->second != segment.start_instruction) {
                VPRINT(this, 2,
                       "Updating all_sched[%d][%d] input %d from %" PRId64 " to %" PRId64
                       "\n",
                       output_idx, sched_idx, segment.input, segment.start_instruction,
                       it->second);
            }
            segment.start_instruction = it->second;
        }
    }
    return STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::read_switch_sequences()
{
    std::unique_ptr<ReaderType> reader, reader_end;
    if (!options_.kernel_switch_trace_path.empty()) {
        reader = get_reader(options_.kernel_switch_trace_path, verbosity_);
        if (!reader || !reader->init()) {
            error_string_ +=
                "Failed to open kernel switch file " + options_.kernel_switch_trace_path;
            return STATUS_ERROR_FILE_OPEN_FAILED;
        }
        reader_end = get_default_reader();
    } else if (!options_.kernel_switch_reader) {
        // No switch data provided.
        return STATUS_SUCCESS;
    } else {
        if (!options_.kernel_switch_reader_end) {
            error_string_ += "Provided kernel switch reader but no end";
            return STATUS_ERROR_INVALID_PARAMETER;
        }
        reader = std::move(options_.kernel_switch_reader);
        reader_end = std::move(options_.kernel_switch_reader_end);
        // We own calling init() as it can block.
        if (!reader->init()) {
            error_string_ += "Failed to init kernel switch reader";
            return STATUS_ERROR_INVALID_PARAMETER;
        }
    }
    // We assume these sequences are small and we can easily read them all into
    // memory and don't need to stream them on every use.
    // We read a single stream, even if underneath these are split into subfiles
    // in an archive.
    sched_type_t::switch_type_t switch_type = SWITCH_INVALID;
    while (*reader != *reader_end) {
        RecordType record = **reader;
        // Only remember the records between the markers.
        trace_marker_type_t marker_type = TRACE_MARKER_TYPE_RESERVED_END;
        uintptr_t marker_value = 0;
        if (record_type_is_marker(record, marker_type, marker_value) &&
            marker_type == TRACE_MARKER_TYPE_CONTEXT_SWITCH_START) {
            switch_type = static_cast<sched_type_t::switch_type_t>(marker_value);
            if (!switch_sequence_[switch_type].empty()) {
                error_string_ += "Duplicate context switch sequence type found";
                return STATUS_ERROR_INVALID_PARAMETER;
            }
        }
        if (switch_type != SWITCH_INVALID)
            switch_sequence_[switch_type].push_back(record);
        if (record_type_is_marker(record, marker_type, marker_value) &&
            marker_type == TRACE_MARKER_TYPE_CONTEXT_SWITCH_END) {
            if (static_cast<sched_type_t::switch_type_t>(marker_value) != switch_type) {
                error_string_ += "Context switch marker values mismatched";
                return STATUS_ERROR_INVALID_PARAMETER;
            }
            VPRINT(this, 1, "Read %zu kernel context switch records for type %d\n",
                   switch_sequence_[switch_type].size(), switch_type);
            switch_type = SWITCH_INVALID;
        }
        ++(*reader);
    }
    return STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
bool
scheduler_tmpl_t<RecordType, ReaderType>::process_next_initial_record(
    input_info_t &input, RecordType record, bool &found_filetype, bool &found_timestamp)
{
    // We want to identify threads that should start out unscheduled as
    // we attached in the middle of an _UNSCHEDULE system call.
    // That marker *before* any instruction indicates the initial
    // exit from such a syscall (the markers anywhere else are added on
    // entry to a syscall, after the syscall instruction fetch record).
    trace_marker_type_t marker_type;
    uintptr_t marker_value;
    if (record_type_is_invalid(record)) // Sentinel on first call.
        return true;                    // Keep reading.
    if (record_type_is_non_marker_header(record))
        return true; // Keep reading.
    if (!record_type_is_marker(record, marker_type, marker_value)) {
        VPRINT(this, 3, "Stopping initial readahead at non-marker\n");
        return false; // Stop reading.
    }
    uintptr_t timestamp;
    if (marker_type == TRACE_MARKER_TYPE_FILETYPE) {
        found_filetype = true;
        VPRINT(this, 2, "Input %d filetype %zu\n", input.index, marker_value);
    } else if (record_type_is_timestamp(record, timestamp)) {
        if (!found_timestamp) {
            // next_timestamp must be the first timestamp, even when we read ahead.
            input.next_timestamp = timestamp;
            found_timestamp = true;
        } else {
            // Stop at a 2nd timestamp to avoid interval count issues.
            VPRINT(this, 3, "Stopping initial readahead at 2nd timestamp\n");
            return false;
        }
    } else if (marker_type == TRACE_MARKER_TYPE_SYSCALL_UNSCHEDULE) {
        if (options_.honor_direct_switches && options_.mapping != MAP_AS_PREVIOUSLY) {
            input.unscheduled = true;
            // Ignore this marker during regular processing.
            input.skip_next_unscheduled = true;
        }
        return false; // Stop reading.
    }
    return true; // Keep reading.
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::get_initial_input_content(
    bool gather_timestamps)
{
    // For every mode, read ahead until we see a filetype record so the user can
    // examine it prior to retrieving any records.
    VPRINT(this, 1, "Reading headers from inputs to find filetypes%s\n",
           gather_timestamps ? " and timestamps" : "");
    assert(options_.read_inputs_in_init);
    // Read ahead in each input until we find a timestamp record.
    // Queue up any skipped records to ensure we present them to the
    // output stream(s).
    for (size_t i = 0; i < inputs_.size(); ++i) {
        input_info_t &input = inputs_[i];
        std::lock_guard<mutex_dbg_owned> lock(*input.lock);

        // If the input jumps to the middle immediately, do that now so we'll have
        // the proper start timestamp.
        if (!input.regions_of_interest.empty() &&
            // The docs say for replay we allow the user to pass ROI but ignore it.
            // Maybe we should disallow it so we don't need checks like this?
            options_.mapping != MAP_AS_PREVIOUSLY) {
            RecordType record = create_invalid_record();
            sched_type_t::stream_status_t res =
                advance_region_of_interest(/*output=*/-1, record, input);
            if (res == sched_type_t::STATUS_SKIPPED) {
                input.next_timestamp =
                    static_cast<uintptr_t>(input.reader->get_last_timestamp());
                // We can skip the rest of the loop here (the filetype will be there
                // in the stream).
                continue;
            }
            if (res != sched_type_t::STATUS_OK) {
                VPRINT(this, 1, "Failed to advance initial ROI with status %d\n", res);
                return sched_type_t::STATUS_ERROR_RANGE_INVALID;
            }
        }

        bool found_filetype = false;
        bool found_timestamp = !gather_timestamps || input.next_timestamp > 0;
        if (process_next_initial_record(input, create_invalid_record(), found_filetype,
                                        found_timestamp)) {
            // First, check any queued records in the input.
            // XXX: Can we create a helper to iterate the queue and then the
            // reader, and avoid the duplicated loops here?  The challenge is
            // the non-consuming queue loop vs the consuming and queue-pushback
            // reader loop.
            for (const auto &record : input.queue) {
                if (!process_next_initial_record(input, record, found_filetype,
                                                 found_timestamp))
                    break;
            }
        }
        if (input.next_timestamp > 0)
            found_timestamp = true;
        if (process_next_initial_record(input, create_invalid_record(), found_filetype,
                                        found_timestamp)) {
            // If we didn't find our targets in the queue, request new records.
            if (input.needs_init) {
                input.reader->init();
                input.needs_init = false;
            }
            while (*input.reader != *input.reader_end) {
                RecordType record = **input.reader;
                if (record_type_is_instr(record)) {
                    ++input.instrs_pre_read;
                }
                trace_marker_type_t marker_type;
                uintptr_t marker_value;
                if (!process_next_initial_record(input, record, found_filetype,
                                                 found_timestamp))
                    break;
                // Don't go too far if only looking for filetype, to avoid reaching
                // the first instruction, which causes problems with ordinals when
                // there is no filetype as happens in legacy traces (and unit tests).
                // Just exit with a 0 filetype.
                if (!found_filetype &&
                    (record_type_is_timestamp(record, marker_value) ||
                     (record_type_is_marker(record, marker_type, marker_value) &&
                      marker_type == TRACE_MARKER_TYPE_PAGE_SIZE))) {
                    VPRINT(this, 2, "No filetype found: assuming unit test input.\n");
                    found_filetype = true;
                    if (!gather_timestamps)
                        break;
                }
                // If we see an instruction, there may be no timestamp (a malformed
                // synthetic trace in a test) or we may have to read thousands of records
                // to find it if it were somehow missing, which we do not want to do.  We
                // assume our queued records are few and do not include instructions when
                // we skip (see skip_instructions()).  Thus, we abort with an error.
                if (record_type_is_instr(record))
                    break;
                input.queue.push_back(record);
                ++(*input.reader);
            }
        }
        if (gather_timestamps && input.next_timestamp <= 0)
            return STATUS_ERROR_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::open_reader(const std::string &path,
                                                      input_ordinal_t input_ordinal,
                                                      input_reader_info_t &reader_info)
{
    if (path.empty() || directory_iterator_t::is_directory(path))
        return STATUS_ERROR_INVALID_PARAMETER;
    std::unique_ptr<ReaderType> reader = get_reader(path, verbosity_);
    if (!reader || !reader->init()) {
        error_string_ += "Failed to open " + path;
        return STATUS_ERROR_FILE_OPEN_FAILED;
    }
    int index = static_cast<input_ordinal_t>(inputs_.size());
    inputs_.emplace_back();
    input_info_t &input = inputs_.back();
    input.index = index;
    // We need the tid up front.  Rather than assume it's still part of the filename,
    // we read the first record (we generalize to read until we find the first but we
    // expect it to be the first after PR #5739 changed the order file_reader_t passes
    // them to reader_t) to find it.
    // XXX: For core-sharded-on-disk traces, this tid is just the first one for
    // this core; it would be better to read the filetype and not match any tid
    // for such files?  Should we call get_initial_input_content() to do that?
    std::unique_ptr<ReaderType> reader_end = get_default_reader();
    memref_tid_t tid = INVALID_THREAD_ID;
    while (*reader != *reader_end) {
        RecordType record = **reader;
        if (record_type_has_tid(record, tid))
            break;
        input.queue.push_back(record);
        ++(*reader);
    }
    if (tid == INVALID_THREAD_ID) {
        error_string_ = "Failed to read " + path;
        return STATUS_ERROR_FILE_READ_FAILED;
    }
    // For core-sharded inputs that start idle the tid might be IDLE_THREAD_ID.
    // That means the size of unfiltered_tids will not be the total input
    // size, which is why we have a separate input_count.
    reader_info.unfiltered_tids.insert(tid);
    ++reader_info.input_count;
    if (!reader_info.only_threads.empty() &&
        reader_info.only_threads.find(tid) == reader_info.only_threads.end()) {
        inputs_.pop_back();
        return sched_type_t::STATUS_SUCCESS;
    }
    if (!reader_info.only_shards.empty() &&
        reader_info.only_shards.find(input_ordinal) == reader_info.only_shards.end()) {
        inputs_.pop_back();
        return sched_type_t::STATUS_SUCCESS;
    }
    VPRINT(this, 1, "Opened reader for tid %" PRId64 " %s\n", tid, path.c_str());
    input.tid = tid;
    input.reader = std::move(reader);
    input.reader_end = std::move(reader_end);
    reader_info.tid2input[tid] = index;
    return sched_type_t::STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::scheduler_status_t
scheduler_tmpl_t<RecordType, ReaderType>::open_readers(const std::string &path,
                                                       input_reader_info_t &reader_info)
{
    if (!directory_iterator_t::is_directory(path)) {
        return open_reader(path, 0, reader_info);
    }
    directory_iterator_t end;
    directory_iterator_t iter(path);
    if (!iter) {
        error_string_ = "Failed to list directory " + path + ": " + iter.error_string();
        return sched_type_t::STATUS_ERROR_FILE_OPEN_FAILED;
    }
    std::vector<std::string> files;
    for (; iter != end; ++iter) {
        const std::string fname = *iter;
        if (fname == "." || fname == ".." ||
            starts_with(fname, DRMEMTRACE_SERIAL_SCHEDULE_FILENAME) ||
            fname == DRMEMTRACE_CPU_SCHEDULE_FILENAME)
            continue;
        // Skip the auxiliary files.
        if (fname == DRMEMTRACE_MODULE_LIST_FILENAME ||
            fname == DRMEMTRACE_FUNCTION_LIST_FILENAME ||
            fname == DRMEMTRACE_ENCODING_FILENAME)
            continue;
        const std::string file = path + DIRSEP + fname;
        files.push_back(file);
    }
    // Sort so we can have reliable shard ordinals for only_shards.
    // We assume leading 0's are used for important numbers embedded in the path,
    // so that a regular sort keeps numeric order.
    std::sort(files.begin(), files.end());
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        sched_type_t::scheduler_status_t res = open_reader(files[i], i, reader_info);
        if (res != sched_type_t::STATUS_SUCCESS)
            return res;
    }
    return sched_type_t::STATUS_SUCCESS;
}

template <typename RecordType, typename ReaderType>
std::string
scheduler_tmpl_t<RecordType, ReaderType>::get_input_name(output_ordinal_t output)
{
    int index = outputs_[output].cur_input;
    if (index < 0)
        return "";
    return inputs_[index].reader->get_stream_name();
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::input_ordinal_t
scheduler_tmpl_t<RecordType, ReaderType>::get_input_ordinal(output_ordinal_t output)
{
    return outputs_[output].cur_input;
}

template <typename RecordType, typename ReaderType>
int64_t
scheduler_tmpl_t<RecordType, ReaderType>::get_tid(output_ordinal_t output)
{
    int index = outputs_[output].cur_input;
    if (index < 0)
        return -1;
    if (inputs_[index].is_combined_stream() ||
        TESTANY(OFFLINE_FILE_TYPE_CORE_SHARDED, inputs_[index].reader->get_filetype()))
        return inputs_[index].last_record_tid;
    return inputs_[index].tid;
}

template <typename RecordType, typename ReaderType>
int
scheduler_tmpl_t<RecordType, ReaderType>::get_shard_index(output_ordinal_t output)
{
    if (output < 0 || output >= static_cast<output_ordinal_t>(outputs_.size()))
        return -1;
    if (TESTANY(sched_type_t::SCHEDULER_USE_INPUT_ORDINALS |
                    sched_type_t::SCHEDULER_USE_SINGLE_INPUT_ORDINALS,
                options_.flags)) {
        if (inputs_.size() == 1 && inputs_[0].is_combined_stream()) {
            int index;
            memref_tid_t tid = get_tid(output);
            auto exists = tid2shard_.find(tid);
            if (exists == tid2shard_.end()) {
                index = static_cast<int>(tid2shard_.size());
                tid2shard_[tid] = index;
            } else
                index = exists->second;
            return index;
        }
        return get_input_ordinal(output);
    }
    return output;
}

template <typename RecordType, typename ReaderType>
int
scheduler_tmpl_t<RecordType, ReaderType>::get_workload_ordinal(output_ordinal_t output)
{
    if (output < 0 || output >= static_cast<output_ordinal_t>(outputs_.size()))
        return -1;
    if (outputs_[output].cur_input < 0)
        return -1;
    return inputs_[outputs_[output].cur_input].workload;
}

template <typename RecordType, typename ReaderType>
bool
scheduler_tmpl_t<RecordType, ReaderType>::is_record_synthetic(output_ordinal_t output)
{
    int index = outputs_[output].cur_input;
    if (index < 0)
        return false;
    if (outputs_[output].in_context_switch_code)
        return true;
    return inputs_[index].reader->is_record_synthetic();
}

template <typename RecordType, typename ReaderType>
int64_t
scheduler_tmpl_t<RecordType, ReaderType>::get_output_cpuid(output_ordinal_t output) const
{
    if (options_.replay_as_traced_istream != nullptr)
        return outputs_[output].as_traced_cpuid;
    int index = outputs_[output].cur_input;
    if (index >= 0 &&
        TESTANY(OFFLINE_FILE_TYPE_CORE_SHARDED, inputs_[index].reader->get_filetype()))
        return outputs_[output].cur_input;
    return output;
}

template <typename RecordType, typename ReaderType>
memtrace_stream_t *
scheduler_tmpl_t<RecordType, ReaderType>::get_input_stream(output_ordinal_t output)
{
    if (output < 0 || output >= static_cast<output_ordinal_t>(outputs_.size()))
        return nullptr;
    int index = outputs_[output].cur_input;
    if (index < 0)
        return nullptr;
    return inputs_[index].reader.get();
}

template <typename RecordType, typename ReaderType>
uint64_t
scheduler_tmpl_t<RecordType, ReaderType>::get_input_record_ordinal(
    output_ordinal_t output)
{
    if (output < 0 || output >= static_cast<output_ordinal_t>(outputs_.size()))
        return 0;
    int index = outputs_[output].cur_input;
    if (index < 0)
        return 0;
    uint64_t ord = inputs_[index].reader->get_record_ordinal();
    if (get_instr_ordinal(inputs_[index]) == 0) {
        // Account for get_initial_input_content() readahead for filetype/timestamp.
        // If this gets any more complex, the scheduler stream should track its
        // own counts for every input and just ignore the input stream's tracking.
        ord -= inputs_[index].queue.size() + (inputs_[index].cur_from_queue ? 1 : 0);
    }
    return ord;
}

template <typename RecordType, typename ReaderType>
uint64_t
scheduler_tmpl_t<RecordType, ReaderType>::get_instr_ordinal(input_info_t &input)
{
    uint64_t reader_cur = input.reader->get_instruction_ordinal();
    assert(reader_cur >= static_cast<uint64_t>(input.instrs_pre_read));
    VPRINT(this, 5, "get_instr_ordinal: %" PRId64 " - %d\n", reader_cur,
           input.instrs_pre_read);
    return reader_cur - input.instrs_pre_read;
}

template <typename RecordType, typename ReaderType>
uint64_t
scheduler_tmpl_t<RecordType, ReaderType>::get_input_first_timestamp(
    output_ordinal_t output)
{
    if (output < 0 || output >= static_cast<output_ordinal_t>(outputs_.size()))
        return 0;
    int index = outputs_[output].cur_input;
    if (index < 0)
        return 0;
    uint64_t res = inputs_[index].reader->get_first_timestamp();
    if (get_instr_ordinal(inputs_[index]) == 0 &&
        (!inputs_[index].queue.empty() || inputs_[index].cur_from_queue)) {
        // Account for get_initial_input_content() readahead for filetype/timestamp.
        res = 0;
    }
    return res;
}

template <typename RecordType, typename ReaderType>
uint64_t
scheduler_tmpl_t<RecordType, ReaderType>::get_input_last_timestamp(
    output_ordinal_t output)
{
    if (output < 0 || output >= static_cast<output_ordinal_t>(outputs_.size()))
        return 0;
    int index = outputs_[output].cur_input;
    if (index < 0)
        return 0;
    uint64_t res = inputs_[index].reader->get_last_timestamp();
    if (get_instr_ordinal(inputs_[index]) == 0 &&
        (!inputs_[index].queue.empty() || inputs_[index].cur_from_queue)) {
        // Account for get_initial_input_content() readahead for filetype/timestamp.
        res = 0;
    }
    return res;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::advance_region_of_interest(
    output_ordinal_t output, RecordType &record, input_info_t &input)
{
    assert(input.lock->owned_by_cur_thread());
    uint64_t cur_instr = get_instr_ordinal(input);
    uint64_t cur_reader_instr = input.reader->get_instruction_ordinal();
    assert(input.cur_region >= 0 &&
           input.cur_region < static_cast<int>(input.regions_of_interest.size()));
    auto &cur_range = input.regions_of_interest[input.cur_region];
    // Look for the end of the current range.
    if (input.in_cur_region && cur_range.stop_instruction != 0 &&
        cur_instr > cur_range.stop_instruction) {
        ++input.cur_region;
        input.in_cur_region = false;
        VPRINT(this, 2, "at %" PRId64 " instrs: advancing to ROI #%d\n", cur_instr,
               input.cur_region);
        if (input.cur_region >= static_cast<int>(input.regions_of_interest.size())) {
            if (input.at_eof)
                return eof_or_idle(output, /*hold_sched_lock=*/false, input.index);
            else {
                // We let the user know we're done.
                if (options_.schedule_record_ostream != nullptr) {
                    sched_type_t::stream_status_t status =
                        close_schedule_segment(output, input);
                    if (status != sched_type_t::STATUS_OK)
                        return status;
                    // Indicate we need a synthetic thread exit on replay.
                    status =
                        record_schedule_segment(output, schedule_record_t::SYNTHETIC_END,
                                                input.index, cur_instr, 0);
                    if (status != sched_type_t::STATUS_OK)
                        return status;
                }
                input.queue.push_back(create_thread_exit(input.tid));
                mark_input_eof(input);
                return sched_type_t::STATUS_SKIPPED;
            }
        }
        cur_range = input.regions_of_interest[input.cur_region];
    }

    if (!input.in_cur_region && cur_instr >= cur_range.start_instruction) {
        // We're already there (back-to-back regions).
        input.in_cur_region = true;
        // Even though there's no gap we let the user know we're on a new region.
        if (input.cur_region > 0) {
            VPRINT(this, 3, "skip_instructions input=%d: inserting separator marker\n",
                   input.index);
            input.queue.push_back(record);
            record = create_region_separator_marker(input.tid, input.cur_region);
        }
        return sched_type_t::STATUS_OK;
    }
    // If we're within one and already skipped, just exit to avoid re-requesting a skip
    // and making no progress (we're on the inserted timetamp + cpuid and our cur instr
    // count isn't yet the target).
    if (input.in_cur_region && cur_instr >= cur_range.start_instruction - 1)
        return sched_type_t::STATUS_OK;

    VPRINT(this, 2,
           "skipping from %" PRId64 " to %" PRIu64 " instrs (%" PRIu64
           " in reader) for ROI\n",
           cur_instr, cur_range.start_instruction,
           cur_range.start_instruction - cur_reader_instr - 1);
    if (options_.schedule_record_ostream != nullptr) {
        if (output >= 0) {
            record_schedule_skip(output, input.index, cur_instr,
                                 cur_range.start_instruction);
        } // Else, will be done in set_cur_input once assigned to an output.
    }
    if (cur_range.start_instruction < cur_reader_instr) {
        // We do not support skipping without skipping over the pre-read: we would
        // need to extract from the queue.
        return sched_type_t::STATUS_INVALID;
    }
    return skip_instructions(input, cur_range.start_instruction - cur_reader_instr - 1);
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::record_schedule_skip(output_ordinal_t output,
                                                               input_ordinal_t input,
                                                               uint64_t start_instruction,
                                                               uint64_t stop_instruction)
{
    assert(inputs_[input].lock->owned_by_cur_thread());
    if (options_.schedule_record_ostream == nullptr)
        return sched_type_t::STATUS_INVALID;
    sched_type_t::stream_status_t status;
    // Close any prior default record for this input.  If we switched inputs,
    // we'll already have closed the prior in set_cur_input().
    if (outputs_[output].record.back().type == schedule_record_t::DEFAULT &&
        outputs_[output].record.back().key.input == input) {
        status = close_schedule_segment(output, inputs_[input]);
        if (status != sched_type_t::STATUS_OK)
            return status;
    }
    if (outputs_[output].record.size() == 1) {
        // Replay doesn't handle starting out with a skip record: we need a
        // start=0,stop=0 dummy entry to get things rolling at the start of
        // an output's records, if we're the first record after the version.
        assert(outputs_[output].record.back().type == schedule_record_t::VERSION);
        status = record_schedule_segment(output, schedule_record_t::DEFAULT, input, 0, 0);
        if (status != sched_type_t::STATUS_OK)
            return status;
    }
    status = record_schedule_segment(output, schedule_record_t::SKIP, input,
                                     start_instruction, stop_instruction);
    if (status != sched_type_t::STATUS_OK)
        return status;
    status = record_schedule_segment(output, schedule_record_t::DEFAULT, input,
                                     stop_instruction);
    if (status != sched_type_t::STATUS_OK)
        return status;
    return sched_type_t::STATUS_OK;
}

template <typename RecordType, typename ReaderType>
void
scheduler_tmpl_t<RecordType, ReaderType>::clear_input_queue(input_info_t &input)
{
    // We assume the queue contains no instrs other than the single candidate record we
    // ourselves read but did not pass to the user (else our query of input.reader's
    // instr ordinal would include them and so be incorrect) and that we should thus
    // skip it all when skipping ahead in the input stream.
    int i = 0;
    while (!input.queue.empty()) {
        assert(i == 0 ||
               (!record_type_is_instr(input.queue.front()) &&
                !record_type_is_encoding(input.queue.front())));
        ++i;
        input.queue.pop_front();
    }
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::skip_instructions(input_info_t &input,
                                                            uint64_t skip_amount)
{
    assert(input.lock->owned_by_cur_thread());
    // reader_t::at_eof_ is true until init() is called.
    if (input.needs_init) {
        input.reader->init();
        input.needs_init = false;
    }
    // For a skip of 0 we still need to clear non-instrs from the queue, but
    // should not have an instr in there.
    assert(skip_amount > 0 || input.queue.empty() ||
           (!record_type_is_instr(input.queue.front()) &&
            !record_type_is_encoding(input.queue.front())));
    clear_input_queue(input);
    input.reader->skip_instructions(skip_amount);
    VPRINT(this, 3, "skip_instructions: input=%d amount=%" PRIu64 "\n", input.index,
           skip_amount);
    if (input.instrs_pre_read > 0) {
        // We do not support skipping without skipping over the pre-read: we would
        // need to extract from the queue.
        input.instrs_pre_read = 0;
    }
    if (*input.reader == *input.reader_end) {
        mark_input_eof(input);
        // Raise error because the input region is out of bounds, unless the max
        // was used which we ourselves use internally for times_of_interest.
        if (skip_amount >= std::numeric_limits<uint64_t>::max() - 2) {
            VPRINT(this, 2, "skip_instructions: input=%d skip to eof\n", input.index);
            return sched_type_t::STATUS_SKIPPED;
        } else {
            VPRINT(this, 2, "skip_instructions: input=%d skip out of bounds\n",
                   input.index);
            return sched_type_t::STATUS_REGION_INVALID;
        }
    }
    input.in_cur_region = true;

    // We've documented that an output stream's ordinals ignore skips in its input
    // streams, so we do not need to remember the input's ordinals pre-skip and increase
    // our output's ordinals commensurately post-skip.

    // We let the user know we've skipped.  There's no discontinuity for the
    // first one so we do not insert a marker there (if we do want to insert one,
    // we need to update the view tool to handle a window marker as the very
    // first entry).
    if (input.cur_region > 0) {
        VPRINT(this, 3, "skip_instructions input=%d: inserting separator marker\n",
               input.index);
        input.queue.push_back(
            create_region_separator_marker(input.tid, input.cur_region));
    }
    return sched_type_t::STATUS_SKIPPED;
}

template <typename RecordType, typename ReaderType>
uint64_t
scheduler_tmpl_t<RecordType, ReaderType>::get_time_micros()
{
    return get_microsecond_timestamp();
}

template <typename RecordType, typename ReaderType>
uint64_t
scheduler_tmpl_t<RecordType, ReaderType>::get_output_time(output_ordinal_t output)
{
    return outputs_[output].cur_time;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::record_schedule_segment(
    output_ordinal_t output, typename schedule_record_t::record_type_t type,
    input_ordinal_t input, uint64_t start_instruction, uint64_t stop_instruction)
{
    assert(type == schedule_record_t::VERSION || type == schedule_record_t::FOOTER ||
           type == schedule_record_t::IDLE || inputs_[input].lock->owned_by_cur_thread());
    // We always use the current wall-clock time, as the time stored in the prior
    // next_record() call can be out of order across outputs and lead to deadlocks.
    uint64_t timestamp = get_time_micros();
    if (type == schedule_record_t::IDLE &&
        outputs_[output].record.back().type == schedule_record_t::IDLE) {
        // Merge.  We don't need intermediate timestamps when idle, and consecutive
        // idle records quickly balloon the file.
        return sched_type_t::STATUS_OK;
    }
    VPRINT(this, 4,
           "recording out=%d type=%d input=%d start=%" PRIu64 " stop=%" PRIu64
           " time=%" PRIu64 "\n",
           output, type, input, start_instruction, stop_instruction, timestamp);
    outputs_[output].record.emplace_back(type, input, start_instruction, stop_instruction,
                                         timestamp);
    // The stop is typically updated later in close_schedule_segment().
    return sched_type_t::STATUS_OK;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::close_schedule_segment(output_ordinal_t output,
                                                                 input_info_t &input)
{
    assert(output >= 0 && output < static_cast<output_ordinal_t>(outputs_.size()));
    assert(!outputs_[output].record.empty());
    assert(outputs_[output].record.back().type == schedule_record_t::VERSION ||
           outputs_[output].record.back().type == schedule_record_t::FOOTER ||
           outputs_[output].record.back().type == schedule_record_t::IDLE ||
           input.lock->owned_by_cur_thread());
    if (outputs_[output].record.back().type == schedule_record_t::SKIP) {
        // Skips already have a final stop value.
        return sched_type_t::STATUS_OK;
    }
    if (outputs_[output].record.back().type == schedule_record_t::IDLE) {
        // Just like in record_schedule_segment() we use wall-clock time for recording
        // replay timestamps.
        uint64_t end = get_time_micros();
        assert(end >= outputs_[output].record.back().timestamp);
        outputs_[output].record.back().value.idle_duration =
            end - outputs_[output].record.back().timestamp;
        VPRINT(this, 3,
               "close_schedule_segment: idle duration %" PRIu64 " = %" PRIu64
               " - %" PRIu64 "\n",
               outputs_[output].record.back().value.idle_duration, end,
               outputs_[output].record.back().timestamp);
        return sched_type_t::STATUS_OK;
    }
    uint64_t instr_ord = get_instr_ordinal(input);
    if (input.at_eof || *input.reader == *input.reader_end) {
        // The end is exclusive, so use the max int value.
        instr_ord = std::numeric_limits<uint64_t>::max();
    }
    if (input.switching_pre_instruction) {
        input.switching_pre_instruction = false;
        // We aren't switching after reading a new instruction that we do not pass
        // to the consumer, so to have an exclusive stop instr ordinal we need +1.
        VPRINT(
            this, 3,
            "set_cur_input: +1 to instr_ord for not-yet-processed instr for input=%d\n",
            input.index);
        ++instr_ord;
    }
    VPRINT(this, 3,
           "close_schedule_segment: input=%d type=%d start=%" PRIu64 " stop=%" PRIu64
           "\n",
           input.index, outputs_[output].record.back().type,
           outputs_[output].record.back().value.start_instruction, instr_ord);
    // Check for empty default entries, except the starter 0,0 ones.
    assert(outputs_[output].record.back().type != schedule_record_t::DEFAULT ||
           outputs_[output].record.back().value.start_instruction < instr_ord ||
           instr_ord == 0);
    outputs_[output].record.back().stop_instruction = instr_ord;
    return sched_type_t::STATUS_OK;
}

template <typename RecordType, typename ReaderType>
bool
scheduler_tmpl_t<RecordType, ReaderType>::ready_queue_empty()
{
    assert(!need_sched_lock() || sched_lock_.owned_by_cur_thread());
    return ready_priority_.empty();
}

template <typename RecordType, typename ReaderType>
void
scheduler_tmpl_t<RecordType, ReaderType>::add_to_unscheduled_queue(input_info_t *input)
{
    assert(!need_sched_lock() || sched_lock_.owned_by_cur_thread());
    assert(input->unscheduled &&
           input->blocked_time == 0); // Else should be in regular queue.
    VPRINT(this, 4, "add_to_unscheduled_queue (pre-size %zu): input %d priority %d\n",
           unscheduled_priority_.size(), input->index, input->priority);
    input->queue_counter = ++unscheduled_counter_;
    unscheduled_priority_.push(input);
}

template <typename RecordType, typename ReaderType>
void
scheduler_tmpl_t<RecordType, ReaderType>::add_to_ready_queue(input_info_t *input)
{
    assert(!need_sched_lock() || sched_lock_.owned_by_cur_thread());
    if (input->unscheduled && input->blocked_time == 0) {
        add_to_unscheduled_queue(input);
        return;
    }
    VPRINT(
        this, 4,
        "add_to_ready_queue (pre-size %zu): input %d priority %d timestamp delta %" PRIu64
        " block time %" PRIu64 " start time %" PRIu64 "\n",
        ready_priority_.size(), input->index, input->priority,
        input->reader->get_last_timestamp() - input->base_timestamp, input->blocked_time,
        input->blocked_start_time);
    if (input->blocked_time > 0)
        ++num_blocked_;
    input->queue_counter = ++ready_counter_;
    ready_priority_.push(input);
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::pop_from_ready_queue(
    output_ordinal_t for_output, input_info_t *&new_input)
{
    assert(!need_sched_lock() || sched_lock_.owned_by_cur_thread());
    std::set<input_info_t *> skipped;
    std::set<input_info_t *> blocked;
    input_info_t *res = nullptr;
    sched_type_t::stream_status_t status = STATUS_OK;
    uint64_t cur_time = (num_blocked_ > 0) ? get_output_time(for_output) : 0;
    while (!ready_priority_.empty()) {
        if (options_.randomize_next_input) {
            res = ready_priority_.get_random_entry();
            ready_priority_.erase(res);
        } else {
            res = ready_priority_.top();
            ready_priority_.pop();
        }
        assert(!res->unscheduled ||
               res->blocked_time > 0); // Should be in unscheduled_priority_.
        if (res->binding.empty() || res->binding.find(for_output) != res->binding.end()) {
            // For blocked inputs, as we don't have interrupts or other regular
            // control points we only check for being unblocked when an input
            // would be chosen to run.  We thus keep blocked inputs in the ready queue.
            if (res->blocked_time > 0) {
                assert(cur_time > 0);
                --num_blocked_;
            }
            if (res->blocked_time > 0 &&
                cur_time - res->blocked_start_time < res->blocked_time) {
                VPRINT(this, 4, "pop queue: %d still blocked for %" PRIu64 "\n",
                       res->index,
                       res->blocked_time - (cur_time - res->blocked_start_time));
                // We keep searching for a suitable input.
                blocked.insert(res);
            } else
                break;
        } else {
            // We keep searching for a suitable input.
            skipped.insert(res);
        }
        res = nullptr;
    }
    if (res == nullptr && !blocked.empty()) {
        // Do not hand out EOF thinking we're done: we still have inputs blocked
        // on i/o, so just wait and retry.
        status = STATUS_IDLE;
    }
    // Re-add the ones we skipped, but without changing their counters so we preserve
    // the prior FIFO order.
    for (input_info_t *save : skipped)
        ready_priority_.push(save);
    // Re-add the blocked ones to the back.
    for (input_info_t *save : blocked)
        add_to_ready_queue(save);
    VDO(this, 1, {
        static int heartbeat;
        // We are ok with races as the cadence is approximate.
        if (++heartbeat % 2000 == 0) {
            VPRINT(this, 1,
                   "heartbeat[%d] %zd in queue; %d blocked; %zd unscheduled => %d %d\n",
                   for_output, ready_priority_.size(), num_blocked_,
                   unscheduled_priority_.size(), res == nullptr ? -1 : res->index,
                   status);
        }
    });
    if (res != nullptr) {
        VPRINT(this, 4,
               "pop_from_ready_queue[%d] (post-size %zu): input %d priority %d timestamp "
               "delta %" PRIu64 "\n",
               for_output, ready_priority_.size(), res->index, res->priority,
               res->reader->get_last_timestamp() - res->base_timestamp);
        res->blocked_time = 0;
        res->unscheduled = false;
    }
    new_input = res;
    return status;
}

template <typename RecordType, typename ReaderType>
uint64_t
scheduler_tmpl_t<RecordType, ReaderType>::scale_blocked_time(uint64_t initial_time) const
{
    uint64_t scaled_us = static_cast<uint64_t>(static_cast<double>(initial_time) *
                                               options_.block_time_multiplier);
    if (scaled_us > options_.block_time_max_us) {
        // We have a max to avoid outlier latencies that are already a second or
        // more from scaling up to tens of minutes.  We assume a cap is representative
        // as the outliers likely were not part of key dependence chains.  Without a
        // cap the other threads all finish and the simulation waits for tens of
        // minutes further for a couple of outliers.
        scaled_us = options_.block_time_max_us;
    }
    return static_cast<uint64_t>(scaled_us * options_.time_units_per_us);
}

template <typename RecordType, typename ReaderType>
bool
scheduler_tmpl_t<RecordType, ReaderType>::syscall_incurs_switch(input_info_t *input,
                                                                uint64_t &blocked_time)
{
    assert(input->lock->owned_by_cur_thread());
    uint64_t post_time = input->reader->get_last_timestamp();
    assert(input->processing_syscall || input->processing_maybe_blocking_syscall);
    if (input->reader->get_version() < TRACE_ENTRY_VERSION_FREQUENT_TIMESTAMPS) {
        // This is a legacy trace that does not have timestamps bracketing syscalls.
        // We switch on every maybe-blocking syscall in this case and have a simplified
        // blocking model.
        blocked_time = options_.blocking_switch_threshold;
        return input->processing_maybe_blocking_syscall;
    }
    assert(input->pre_syscall_timestamp > 0);
    assert(input->pre_syscall_timestamp <= post_time);
    uint64_t latency = post_time - input->pre_syscall_timestamp;
    uint64_t threshold = input->processing_maybe_blocking_syscall
        ? options_.blocking_switch_threshold
        : options_.syscall_switch_threshold;
    blocked_time = scale_blocked_time(latency);
    VPRINT(this, 3,
           "input %d %ssyscall latency %" PRIu64 " * scale %6.3f => blocked time %" PRIu64
           "\n",
           input->index,
           input->processing_maybe_blocking_syscall ? "maybe-blocking " : "", latency,
           options_.block_time_multiplier, blocked_time);
    return latency >= threshold;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::set_cur_input(output_ordinal_t output,
                                                        input_ordinal_t input)
{
    assert(!need_sched_lock() || sched_lock_.owned_by_cur_thread());
    // XXX i#5843: Merge tracking of current inputs with ready_priority_ to better manage
    // the possible 3 states of each input (a live cur_input for an output stream, in
    // the ready_queue_, or at EOF) (4 states once we add i/o wait times).
    assert(output >= 0 && output < static_cast<output_ordinal_t>(outputs_.size()));
    // 'input' might be INVALID_INPUT_ORDINAL.
    assert(input < static_cast<input_ordinal_t>(inputs_.size()));
    int prev_input = outputs_[output].cur_input;
    if (prev_input >= 0) {
        if (options_.mapping == MAP_TO_ANY_OUTPUT && prev_input != input &&
            !inputs_[prev_input].at_eof) {
            add_to_ready_queue(&inputs_[prev_input]);
        }
        if (prev_input != input && options_.schedule_record_ostream != nullptr) {
            input_info_t &prev_info = inputs_[prev_input];
            std::lock_guard<mutex_dbg_owned> lock(*prev_info.lock);
            sched_type_t::stream_status_t status =
                close_schedule_segment(output, prev_info);
            if (status != sched_type_t::STATUS_OK)
                return status;
        }
    } else if (options_.schedule_record_ostream != nullptr &&
               outputs_[output].record.back().type == schedule_record_t::IDLE) {
        input_info_t unused;
        sched_type_t::stream_status_t status = close_schedule_segment(output, unused);
        if (status != sched_type_t::STATUS_OK)
            return status;
    }
    if (outputs_[output].cur_input >= 0)
        outputs_[output].prev_input = outputs_[output].cur_input;
    outputs_[output].cur_input = input;
    if (input < 0)
        return STATUS_OK;
    if (prev_input == input)
        return STATUS_OK;

    int prev_workload = -1;
    if (outputs_[output].prev_input >= 0 && outputs_[output].prev_input != input) {
        std::lock_guard<mutex_dbg_owned> lock(*inputs_[outputs_[output].prev_input].lock);
        prev_workload = inputs_[outputs_[output].prev_input].workload;
    }

    std::lock_guard<mutex_dbg_owned> lock(*inputs_[input].lock);

    if (inputs_[input].prev_output != INVALID_OUTPUT_ORDINAL &&
        inputs_[input].prev_output != output) {
        VPRINT(this, 3, "output[%d] migrating input %d from output %d\n", output, input,
               inputs_[input].prev_output);
        ++outputs_[output].stats[memtrace_stream_t::SCHED_STAT_MIGRATIONS];
    }
    inputs_[input].prev_output = output;

    if (prev_input < 0 && outputs_[output].stream->version_ == 0) {
        // Set the version and filetype up front, to let the user query at init time
        // as documented.  Also set the other fields in case we did a skip for ROI.
        auto *stream = outputs_[output].stream;
        stream->version_ = inputs_[input].reader->get_version();
        stream->last_timestamp_ = inputs_[input].reader->get_last_timestamp();
        stream->first_timestamp_ = inputs_[input].reader->get_first_timestamp();
        stream->filetype_ = inputs_[input].reader->get_filetype();
        stream->cache_line_size_ = inputs_[input].reader->get_cache_line_size();
        stream->chunk_instr_count_ = inputs_[input].reader->get_chunk_instr_count();
        stream->page_size_ = inputs_[input].reader->get_page_size();
    }

    if (inputs_[input].pid != INVALID_PID) {
        insert_switch_tid_pid(inputs_[input]);
    }

    if (!switch_sequence_.empty() &&
        outputs_[output].stream->get_instruction_ordinal() > 0) {
        sched_type_t::switch_type_t switch_type = SWITCH_INVALID;
        if (prev_workload != inputs_[input].workload)
            switch_type = SWITCH_PROCESS;
        else
            switch_type = SWITCH_THREAD;
        // Inject kernel context switch code.  Since the injected records belong to this
        // input (the kernel is acting on behalf of this input) we insert them into the
        // input's queue, but ahead of any prior queued items.  This is why we walk in
        // reverse, for the push_front calls to the deque.  We update the tid of the
        // records here to match.  They are considered as is_record_synthetic() and do
        // not affect input stream ordinals.
        // XXX: These will appear before the top headers of a new thread which is slightly
        // odd to have regular records with the new tid before the top headers.
        if (!switch_sequence_[switch_type].empty()) {
            for (int i = static_cast<int>(switch_sequence_[switch_type].size()) - 1;
                 i >= 0; --i) {
                RecordType record = switch_sequence_[switch_type][i];
                record_type_set_tid(record, inputs_[input].tid);
                inputs_[input].queue.push_front(record);
            }
            VPRINT(this, 3,
                   "Inserted %zu switch records for type %d from %d.%d to %d.%d\n",
                   switch_sequence_[switch_type].size(), switch_type, prev_workload,
                   outputs_[output].prev_input, inputs_[input].workload, input);
        }
    }

    inputs_[input].prev_time_in_quantum = outputs_[output].cur_time;

    if (options_.schedule_record_ostream != nullptr) {
        uint64_t instr_ord = get_instr_ordinal(inputs_[input]);
        VPRINT(this, 3, "set_cur_input: recording input=%d start=%" PRId64 "\n", input,
               instr_ord);
        if (!inputs_[input].regions_of_interest.empty() &&
            inputs_[input].cur_region == 0 && inputs_[input].in_cur_region &&
            (instr_ord == inputs_[input].regions_of_interest[0].start_instruction ||
             // The ord may be 1 less because we're still on the inserted timestamp.
             instr_ord + 1 == inputs_[input].regions_of_interest[0].start_instruction)) {
            // We skipped during init but didn't have an output for recording the skip:
            // record it now.
            record_schedule_skip(output, input, 0,
                                 inputs_[input].regions_of_interest[0].start_instruction);
        } else {
            sched_type_t::stream_status_t status = record_schedule_segment(
                output, schedule_record_t::DEFAULT, input, instr_ord);
            if (status != sched_type_t::STATUS_OK)
                return status;
        }
    }
    return STATUS_OK;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::pick_next_input_as_previously(
    output_ordinal_t output, input_ordinal_t &index)
{
    assert(!need_sched_lock() || sched_lock_.owned_by_cur_thread());
    if (outputs_[output].record_index + 1 >=
        static_cast<int>(outputs_[output].record.size())) {
        if (!outputs_[output].at_eof) {
            outputs_[output].at_eof = true;
            live_replay_output_count_.fetch_add(-1, std::memory_order_release);
        }
        return eof_or_idle(output, need_sched_lock(), outputs_[output].cur_input);
    }
    const schedule_record_t &segment =
        outputs_[output].record[outputs_[output].record_index + 1];
    if (segment.type == schedule_record_t::IDLE) {
        outputs_[output].waiting = true;
        outputs_[output].wait_start_time = get_output_time(output);
        ++outputs_[output].record_index;
        return sched_type_t::STATUS_IDLE;
    }
    index = segment.key.input;
    VPRINT(this, 5,
           "pick_next_input_as_previously[%d]: next replay segment in=%d (@%" PRId64
           ") type=%d start=%" PRId64 " end=%" PRId64 "\n",
           output, index, get_instr_ordinal(inputs_[index]), segment.type,
           segment.value.start_instruction, segment.stop_instruction);
    {
        std::lock_guard<mutex_dbg_owned> lock(*inputs_[index].lock);
        if (get_instr_ordinal(inputs_[index]) > segment.value.start_instruction) {
            VPRINT(this, 1,
                   "WARNING: next_record[%d]: input %d wants instr #%" PRId64
                   " but it is already at #%" PRId64 "\n",
                   output, index, segment.value.start_instruction,
                   get_instr_ordinal(inputs_[index]));
        }
        if (get_instr_ordinal(inputs_[index]) < segment.value.start_instruction &&
            // Don't wait for an ROI that starts at the beginning.
            segment.value.start_instruction > 1 &&
            // The output may have begun in the wait state.
            (outputs_[output].record_index == -1 ||
             // When we skip our separator+timestamp markers are at the
             // prior instr ord so do not wait for that.
             (outputs_[output].record[outputs_[output].record_index].type !=
                  schedule_record_t::SKIP &&
              // Don't wait if we're at the end and just need the end record.
              segment.type != schedule_record_t::SYNTHETIC_END))) {
            // Some other output stream has not advanced far enough, and we do
            // not support multiple positions in one input stream: we wait.
            // XXX i#5843: We may want to provide a kernel-mediated wait
            // feature so a multi-threaded simulator doesn't have to do a
            // spinning poll loop.
            // XXX i#5843: For replaying a schedule as it was traced with
            // MAP_TO_RECORDED_OUTPUT there may have been true idle periods during
            // tracing where some other process than the traced workload was
            // scheduled on a core.  If we could identify those, we should return
            // STATUS_IDLE rather than STATUS_WAIT.
            VPRINT(this, 3, "next_record[%d]: waiting for input %d instr #%" PRId64 "\n",
                   output, index, segment.value.start_instruction);
            // Give up this input and go into a wait state.
            // We'll come back here on the next next_record() call.
            set_cur_input(output, INVALID_INPUT_ORDINAL);
            outputs_[output].waiting = true;
            return sched_type_t::STATUS_WAIT;
        }
    }
    // Also wait if this segment is ahead of the next-up segment on another
    // output.  We only have a timestamp per context switch so we can't
    // enforce finer-grained timing replay.
    if (options_.deps == DEPENDENCY_TIMESTAMPS) {
        for (int i = 0; i < static_cast<output_ordinal_t>(outputs_.size()); ++i) {
            if (i != output &&
                outputs_[i].record_index + 1 <
                    static_cast<int>(outputs_[i].record.size()) &&
                segment.timestamp >
                    outputs_[i].record[outputs_[i].record_index + 1].timestamp) {
                VPRINT(this, 3,
                       "next_record[%d]: waiting because timestamp %" PRIu64
                       " is ahead of output %d\n",
                       output, segment.timestamp, i);
                // Give up this input and go into a wait state.
                // We'll come back here on the next next_record() call.
                // XXX: We should add a timeout just in case some timestamps are out of
                // order due to using prior values, to avoid hanging.  We try to avoid
                // this by using wall-clock time in record_schedule_segment() rather than
                // the stored output time.
                set_cur_input(output, INVALID_INPUT_ORDINAL);
                outputs_[output].waiting = true;
                return sched_type_t::STATUS_WAIT;
            }
        }
    }
    if (segment.type == schedule_record_t::SYNTHETIC_END) {
        std::lock_guard<mutex_dbg_owned> lock(*inputs_[index].lock);
        // We're past the final region of interest and we need to insert
        // a synthetic thread exit record.  We need to first throw out the
        // queued candidate record, if any.
        clear_input_queue(inputs_[index]);
        inputs_[index].queue.push_back(create_thread_exit(inputs_[index].tid));
        mark_input_eof(inputs_[index]);
        VPRINT(this, 2, "early end for input %d\n", index);
        // We're done with this entry but we need the queued record to be read,
        // so we do not move past the entry.
        ++outputs_[output].record_index;
        return sched_type_t::STATUS_SKIPPED;
    } else if (segment.type == schedule_record_t::SKIP) {
        std::lock_guard<mutex_dbg_owned> lock(*inputs_[index].lock);
        uint64_t cur_reader_instr = inputs_[index].reader->get_instruction_ordinal();
        VPRINT(this, 2,
               "next_record[%d]: skipping from %" PRId64 " to %" PRId64
               " in %d for schedule\n",
               output, cur_reader_instr, segment.stop_instruction, index);
        auto status = skip_instructions(inputs_[index],
                                        segment.stop_instruction - cur_reader_instr -
                                            1 /*exclusive*/);
        // Increment the region to get window id markers with ordinals.
        inputs_[index].cur_region++;
        if (status != sched_type_t::STATUS_SKIPPED)
            return sched_type_t::STATUS_INVALID;
        // We're done with the skip so move to and past it.
        outputs_[output].record_index += 2;
        return sched_type_t::STATUS_SKIPPED;
    } else {
        VPRINT(this, 2, "next_record[%d]: advancing to input %d instr #%" PRId64 "\n",
               output, index, segment.value.start_instruction);
    }
    ++outputs_[output].record_index;
    return sched_type_t::STATUS_OK;
}

template <typename RecordType, typename ReaderType>
bool
scheduler_tmpl_t<RecordType, ReaderType>::need_sched_lock()
{
    return options_.mapping == MAP_TO_ANY_OUTPUT || options_.mapping == MAP_AS_PREVIOUSLY;
}

template <typename RecordType, typename ReaderType>
std::unique_lock<mutex_dbg_owned>
scheduler_tmpl_t<RecordType, ReaderType>::acquire_scoped_sched_lock_if_necessary(
    bool &need_lock)
{
    need_lock = need_sched_lock();
    auto scoped_lock = need_lock ? std::unique_lock<mutex_dbg_owned>(sched_lock_)
                                 : std::unique_lock<mutex_dbg_owned>();
    return scoped_lock;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::pick_next_input(output_ordinal_t output,
                                                          uint64_t blocked_time)
{
    sched_type_t::stream_status_t res = sched_type_t::STATUS_OK;
    bool need_lock;
    auto scoped_lock = acquire_scoped_sched_lock_if_necessary(need_lock);
    input_ordinal_t prev_index = outputs_[output].cur_input;
    input_ordinal_t index = INVALID_INPUT_ORDINAL;
    int iters = 0;
    while (true) {
        ++iters;
        if (index < 0) {
            // XXX i#6831: Refactor to use subclasses or templates to specialize
            // scheduler code based on mapping options, to avoid these top-level
            // conditionals in many functions?
            if (options_.mapping == MAP_AS_PREVIOUSLY) {
                res = pick_next_input_as_previously(output, index);
                VDO(this, 2, {
                    if (outputs_[output].record_index >= 0 &&
                        outputs_[output].record_index <
                            static_cast<int>(outputs_[output].record.size())) {
                        const schedule_record_t &segment =
                            outputs_[output].record[outputs_[output].record_index];
                        int input = segment.key.input;
                        VPRINT(this,
                               (res == sched_type_t::STATUS_IDLE ||
                                res == sched_type_t::STATUS_WAIT)
                                   ? 3
                                   : 2,
                               "next_record[%d]: replay segment in=%d (@%" PRId64
                               ") type=%d start=%" PRId64 " end=%" PRId64 "\n",
                               output, input, get_instr_ordinal(inputs_[input]),
                               segment.type, segment.value.start_instruction,
                               segment.stop_instruction);
                    }
                });
                if (res == sched_type_t::STATUS_SKIPPED)
                    break;
                if (res != sched_type_t::STATUS_OK)
                    return res;
            } else if (options_.mapping == MAP_TO_ANY_OUTPUT) {
                if (blocked_time > 0 && prev_index != INVALID_INPUT_ORDINAL) {
                    std::lock_guard<mutex_dbg_owned> lock(*inputs_[prev_index].lock);
                    if (inputs_[prev_index].blocked_time == 0) {
                        VPRINT(this, 2, "next_record[%d]: blocked time %" PRIu64 "\n",
                               output, blocked_time);
                        inputs_[prev_index].blocked_time = blocked_time;
                        inputs_[prev_index].blocked_start_time = get_output_time(output);
                    }
                }
                if (prev_index != INVALID_INPUT_ORDINAL &&
                    inputs_[prev_index].switch_to_input != INVALID_INPUT_ORDINAL) {
                    input_info_t *target = &inputs_[inputs_[prev_index].switch_to_input];
                    inputs_[prev_index].switch_to_input = INVALID_INPUT_ORDINAL;
                    std::lock_guard<mutex_dbg_owned> lock(*target->lock);
                    // XXX i#5843: Add an invariant check that the next timestamp of the
                    // target is later than the pre-switch-syscall timestamp?
                    if (ready_priority_.find(target)) {
                        VPRINT(this, 2,
                               "next_record[%d]: direct switch from input %d to input %d "
                               "@%" PRIu64 "\n",
                               output, prev_index, target->index,
                               inputs_[prev_index].reader->get_last_timestamp());
                        ready_priority_.erase(target);
                        index = target->index;
                        // Erase any remaining wait time for the target.
                        if (target->blocked_time > 0) {
                            VPRINT(this, 3,
                                   "next_record[%d]: direct switch erasing blocked time "
                                   "for input %d\n",
                                   output, target->index);
                            --num_blocked_;
                            target->blocked_time = 0;
                            target->unscheduled = false;
                        }
                        if (target->prev_output != INVALID_OUTPUT_ORDINAL &&
                            target->prev_output != output) {
                            ++outputs_[output]
                                  .stats[memtrace_stream_t::SCHED_STAT_MIGRATIONS];
                        }
                        ++outputs_[output].stats
                              [memtrace_stream_t::SCHED_STAT_DIRECT_SWITCH_SUCCESSES];
                    } else if (unscheduled_priority_.find(target)) {
                        target->unscheduled = false;
                        unscheduled_priority_.erase(target);
                        index = target->index;
                        VPRINT(this, 2,
                               "next_record[%d]: direct switch from input %d to "
                               "was-unscheduled input %d "
                               "@%" PRIu64 "\n",
                               output, prev_index, target->index,
                               inputs_[prev_index].reader->get_last_timestamp());
                        if (target->prev_output != INVALID_OUTPUT_ORDINAL &&
                            target->prev_output != output) {
                            ++outputs_[output]
                                  .stats[memtrace_stream_t::SCHED_STAT_MIGRATIONS];
                        }
                        ++outputs_[output].stats
                              [memtrace_stream_t::SCHED_STAT_DIRECT_SWITCH_SUCCESSES];
                    } else {
                        // We assume that inter-input dependencies are captured in
                        // the _DIRECT_THREAD_SWITCH, _UNSCHEDULE, and _SCHEDULE markers
                        // and that if a switch request targets a thread running elsewhere
                        // that means there isn't a dependence and this is really a
                        // dynamic switch to whoever happens to be available (and
                        // different timing between tracing and analysis has caused this
                        // miss).
                        VPRINT(this, 1,
                               "Direct switch (from %d) target input #%d is running "
                               "elsewhere; picking a different target @%" PRIu64 "\n",
                               prev_index, target->index,
                               inputs_[prev_index].reader->get_last_timestamp());
                        // We do ensure the missed target doesn't wait indefinitely.
                        // XXX i#6822: It's not clear this is always the right thing to
                        // do.
                        target->skip_next_unscheduled = true;
                    }
                }
                if (index != INVALID_INPUT_ORDINAL) {
                    // We found a direct switch target above.
                } else if (ready_queue_empty() && blocked_time == 0) {
                    if (prev_index == INVALID_INPUT_ORDINAL)
                        return eof_or_idle(output, need_lock, prev_index);
                    auto lock =
                        std::unique_lock<mutex_dbg_owned>(*inputs_[prev_index].lock);
                    // If we can't go back to the current input, we're EOF or idle.
                    // TODO i#6959: We should go the EOF/idle route if
                    // inputs_[prev_index].unscheduled as otherwise we're ignoring its
                    // unscheduled transition: although if there are no other threads at
                    // all (not just an empty queue) this turns into the eof_or_idle()
                    // all-unscheduled scenario.  Once we have some kind of early exit
                    // option we'll add the unscheduled check here.
                    if (inputs_[prev_index].at_eof) {
                        lock.unlock();
                        return eof_or_idle(output, need_lock, prev_index);
                    } else
                        index = prev_index; // Go back to prior.
                } else {
                    // Give up the input before we go to the queue so we can add
                    // ourselves to the queue.  If we're the highest priority we
                    // shouldn't switch.  The queue preserves FIFO for same-priority
                    // cases so we will switch if someone of equal priority is
                    // waiting.
                    set_cur_input(output, INVALID_INPUT_ORDINAL);
                    input_info_t *queue_next = nullptr;
                    sched_type_t::stream_status_t status =
                        pop_from_ready_queue(output, queue_next);
                    if (status != STATUS_OK) {
                        if (status == STATUS_IDLE) {
                            outputs_[output].waiting = true;
                            if (options_.schedule_record_ostream != nullptr) {
                                sched_type_t::stream_status_t record_status =
                                    record_schedule_segment(
                                        output, schedule_record_t::IDLE, 0, 0, 0);
                                if (record_status != sched_type_t::STATUS_OK)
                                    return record_status;
                            }
                            if (prev_index != INVALID_INPUT_ORDINAL) {
                                ++outputs_[output]
                                      .stats[memtrace_stream_t::
                                                 SCHED_STAT_SWITCH_INPUT_TO_IDLE];
                            }
                        }
                        return status;
                    }
                    if (queue_next == nullptr) {
                        assert(blocked_time == 0 || prev_index == INVALID_INPUT_ORDINAL);
                        return eof_or_idle(output, need_lock, prev_index);
                    }
                    index = queue_next->index;
                }
            } else if (options_.deps == DEPENDENCY_TIMESTAMPS) {
                uint64_t min_time = std::numeric_limits<uint64_t>::max();
                for (size_t i = 0; i < inputs_.size(); ++i) {
                    std::lock_guard<mutex_dbg_owned> lock(*inputs_[i].lock);
                    if (!inputs_[i].at_eof && inputs_[i].next_timestamp > 0 &&
                        inputs_[i].next_timestamp < min_time) {
                        min_time = inputs_[i].next_timestamp;
                        index = static_cast<int>(i);
                    }
                }
                if (index < 0)
                    return eof_or_idle(output, need_lock, prev_index);
                VPRINT(this, 2,
                       "next_record[%d]: advancing to timestamp %" PRIu64
                       " == input #%d\n",
                       output, min_time, index);
            } else if (options_.mapping == MAP_TO_CONSISTENT_OUTPUT) {
                // We're done with the prior thread; take the next one that was
                // pre-allocated to this output (pre-allocated to avoid locks). Invariant:
                // the same output will not be accessed by two different threads
                // simultaneously in this mode, allowing us to support a lock-free
                // parallel-friendly increment here.
                int indices_index = ++outputs_[output].input_indices_index;
                if (indices_index >=
                    static_cast<int>(outputs_[output].input_indices.size())) {
                    VPRINT(this, 2, "next_record[%d]: all at eof\n", output);
                    return sched_type_t::STATUS_EOF;
                }
                index = outputs_[output].input_indices[indices_index];
                VPRINT(this, 2,
                       "next_record[%d]: advancing to local index %d == input #%d\n",
                       output, indices_index, index);
            } else
                return sched_type_t::STATUS_INVALID;
            // reader_t::at_eof_ is true until init() is called.
            std::lock_guard<mutex_dbg_owned> lock(*inputs_[index].lock);
            if (inputs_[index].needs_init) {
                inputs_[index].reader->init();
                inputs_[index].needs_init = false;
            }
        }
        std::lock_guard<mutex_dbg_owned> lock(*inputs_[index].lock);
        if (inputs_[index].at_eof ||
            *inputs_[index].reader == *inputs_[index].reader_end) {
            VPRINT(this, 2, "next_record[%d]: input #%d at eof\n", output, index);
            if (!inputs_[index].at_eof)
                mark_input_eof(inputs_[index]);
            index = INVALID_INPUT_ORDINAL;
            // Loop and pick next thread.
            continue;
        }
        break;
    }
    // We can't easily place these stats inside set_cur_input() as we call that to
    // temporarily give up our input.
    if (prev_index == index)
        ++outputs_[output].stats[memtrace_stream_t::SCHED_STAT_SWITCH_NOP];
    else if (prev_index != INVALID_INPUT_ORDINAL && index != INVALID_INPUT_ORDINAL)
        ++outputs_[output].stats[memtrace_stream_t::SCHED_STAT_SWITCH_INPUT_TO_INPUT];
    else if (index == INVALID_INPUT_ORDINAL)
        ++outputs_[output].stats[memtrace_stream_t::SCHED_STAT_SWITCH_INPUT_TO_IDLE];
    else
        ++outputs_[output].stats[memtrace_stream_t::SCHED_STAT_SWITCH_IDLE_TO_INPUT];
    set_cur_input(output, index);
    return res;
}

template <typename RecordType, typename ReaderType>
void
scheduler_tmpl_t<RecordType, ReaderType>::process_marker(input_info_t &input,
                                                         output_ordinal_t output,
                                                         trace_marker_type_t marker_type,
                                                         uintptr_t marker_value)
{
    assert(input.lock->owned_by_cur_thread());
    switch (marker_type) {
    case TRACE_MARKER_TYPE_SYSCALL:
        input.processing_syscall = true;
        input.pre_syscall_timestamp = input.reader->get_last_timestamp();
        break;
    case TRACE_MARKER_TYPE_MAYBE_BLOCKING_SYSCALL:
        input.processing_maybe_blocking_syscall = true;
        // Generally we should already have the timestamp from a just-prior
        // syscall marker, but we support tests and other synthetic sequences
        // with just a maybe-blocking.
        input.pre_syscall_timestamp = input.reader->get_last_timestamp();
        break;
    case TRACE_MARKER_TYPE_CONTEXT_SWITCH_START:
        outputs_[output].in_context_switch_code = true;
        ANNOTATE_FALLTHROUGH;
    case TRACE_MARKER_TYPE_SYSCALL_TRACE_START:
        outputs_[output].in_kernel_code = true;
        break;
    case TRACE_MARKER_TYPE_CONTEXT_SWITCH_END:
        // We have to delay until the next record.
        outputs_[output].hit_switch_code_end = true;
        ANNOTATE_FALLTHROUGH;
    case TRACE_MARKER_TYPE_SYSCALL_TRACE_END:
        outputs_[output].in_kernel_code = false;
        break;
    case TRACE_MARKER_TYPE_DIRECT_THREAD_SWITCH: {
        if (!options_.honor_direct_switches)
            break;
        ++outputs_[output].stats[memtrace_stream_t::SCHED_STAT_DIRECT_SWITCH_ATTEMPTS];
        memref_tid_t target_tid = marker_value;
        auto it = tid2input_.find(workload_tid_t(input.workload, target_tid));
        if (it == tid2input_.end()) {
            VPRINT(this, 1, "Failed to find input for target switch thread %" PRId64 "\n",
                   target_tid);
        } else {
            input.switch_to_input = it->second;
        }
        // Trigger a switch either indefinitely or until timeout.
        if (input.skip_next_unscheduled) {
            // The underlying kernel mechanism being modeled only supports a single
            // request: they cannot accumulate.  Timing differences in the trace could
            // perhaps result in multiple lining up when the didn't in the real app;
            // but changing the scheme here could also push representatives in the
            // other direction.
            input.skip_next_unscheduled = false;
            VPRINT(this, 3,
                   "input %d unschedule request ignored due to prior schedule request "
                   "@%" PRIu64 "\n",
                   input.index, input.reader->get_last_timestamp());
            break;
        }
        input.unscheduled = true;
        if (input.syscall_timeout_arg > 0) {
            input.blocked_time = scale_blocked_time(input.syscall_timeout_arg);
            input.blocked_start_time = get_output_time(output);
            VPRINT(this, 3, "input %d unscheduled for %" PRIu64 " @%" PRIu64 "\n",
                   input.index, input.blocked_time, input.reader->get_last_timestamp());
        } else {
            VPRINT(this, 3, "input %d unscheduled indefinitely @%" PRIu64 "\n",
                   input.index, input.reader->get_last_timestamp());
        }
        break;
    }
    case TRACE_MARKER_TYPE_SYSCALL_ARG_TIMEOUT:
        // This is cleared at the post-syscall instr.
        input.syscall_timeout_arg = static_cast<uint64_t>(marker_value);
        break;
    case TRACE_MARKER_TYPE_SYSCALL_UNSCHEDULE:
        if (!options_.honor_direct_switches)
            break;
        if (input.skip_next_unscheduled) {
            input.skip_next_unscheduled = false;
            VPRINT(this, 3,
                   "input %d unschedule request ignored due to prior schedule request "
                   "@%" PRIu64 "\n",
                   input.index, input.reader->get_last_timestamp());
            break;
        }
        // Trigger a switch either indefinitely or until timeout.
        input.unscheduled = true;
        if (input.syscall_timeout_arg > 0) {
            input.blocked_time = scale_blocked_time(input.syscall_timeout_arg);
            input.blocked_start_time = get_output_time(output);
            VPRINT(this, 3, "input %d unscheduled for %" PRIu64 " @%" PRIu64 "\n",
                   input.index, input.blocked_time, input.reader->get_last_timestamp());
        } else {
            VPRINT(this, 3, "input %d unscheduled indefinitely @%" PRIu64 "\n",
                   input.index, input.reader->get_last_timestamp());
        }
        break;
    case TRACE_MARKER_TYPE_SYSCALL_SCHEDULE: {
        if (!options_.honor_direct_switches)
            break;
        memref_tid_t target_tid = marker_value;
        auto it = tid2input_.find(workload_tid_t(input.workload, target_tid));
        if (it == tid2input_.end()) {
            VPRINT(this, 1,
                   "Failed to find input for switchto::resume target tid %" PRId64 "\n",
                   target_tid);
            return;
        }
        input_ordinal_t target_idx = it->second;
        VPRINT(this, 3, "input %d re-scheduling input %d @%" PRIu64 "\n", input.index,
               target_idx, input.reader->get_last_timestamp());
        // Release the input lock before acquiring sched_lock, to meet our lock
        // ordering convention to avoid deadlocks.
        input.lock->unlock();
        {
            bool need_sched_lock;
            auto scoped_sched_lock =
                acquire_scoped_sched_lock_if_necessary(need_sched_lock);
            input_info_t *target = &inputs_[target_idx];
            std::lock_guard<mutex_dbg_owned> lock(*target->lock);
            if (target->unscheduled) {
                target->unscheduled = false;
                if (unscheduled_priority_.find(target)) {
                    add_to_ready_queue(target);
                    unscheduled_priority_.erase(target);
                } else if (ready_priority_.find(target)) {
                    // We assume blocked_time is from _ARG_TIMEOUT and is not from
                    // regularly-blocking i/o.  We assume i/o getting into the mix is
                    // rare enough or does not matter enough to try to have separate
                    // timeouts.
                    if (target->blocked_time > 0) {
                        VPRINT(
                            this, 3,
                            "switchto::resume erasing blocked time for target input %d\n",
                            target->index);
                        --num_blocked_;
                        target->blocked_time = 0;
                    }
                }
            } else {
                VPRINT(this, 3, "input %d will skip next unschedule\n", target_idx);
                target->skip_next_unscheduled = true;
            }
        }
        input.lock->lock();
        break;
    }
    default: // Nothing to do.
        break;
    }
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::next_record(output_ordinal_t output,
                                                      RecordType &record,
                                                      input_info_t *&input,
                                                      uint64_t cur_time)
{
    // We do not enforce a globally increasing time to avoid the synchronization cost; we
    // do return an error on a time smaller than an input's current start time when we
    // check for quantum end.
    if (cur_time == 0) {
        // It's more efficient for QUANTUM_INSTRUCTIONS to get the time here instead of
        // in get_output_time().  This also makes the two more similarly behaved with
        // respect to blocking system calls.
        // TODO i#6971: Use INSTRS_PER_US to replace .cur_time completely
        // with a counter-based time, weighted appropriately for STATUS_IDLE.
        cur_time = get_time_micros();
    }
    outputs_[output].cur_time = cur_time; // Invalid values are checked below.
    if (!outputs_[output].active)
        return sched_type_t::STATUS_IDLE;
    if (outputs_[output].waiting) {
        if (options_.mapping == MAP_AS_PREVIOUSLY &&
            outputs_[output].wait_start_time > 0) {
            uint64_t duration = outputs_[output]
                                    .record[outputs_[output].record_index]
                                    .value.idle_duration;
            uint64_t now = get_output_time(output);
            if (now - outputs_[output].wait_start_time < duration) {
                VPRINT(this, 4,
                       "next_record[%d]: elapsed %" PRIu64 " < duration %" PRIu64 "\n",
                       output, now - outputs_[output].wait_start_time, duration);
                return sched_type_t::STATUS_WAIT;
            } else
                outputs_[output].wait_start_time = 0;
        }
        VPRINT(this, 5, "next_record[%d]: need new input (cur=waiting)\n", output);
        sched_type_t::stream_status_t res = pick_next_input(output, 0);
        if (res != sched_type_t::STATUS_OK && res != sched_type_t::STATUS_SKIPPED)
            return res;
        outputs_[output].waiting = false;
    }
    if (outputs_[output].cur_input < 0) {
        // This happens with more outputs than inputs.  For non-empty outputs we
        // require cur_input to be set to >=0 during init().
        return eof_or_idle(output, /*hold_sched_lock=*/false, outputs_[output].cur_input);
    }
    input = &inputs_[outputs_[output].cur_input];
    auto lock = std::unique_lock<mutex_dbg_owned>(*input->lock);
    // Since we do not ask for a start time, we have to check for the first record from
    // each input and set the time here.
    if (input->prev_time_in_quantum == 0)
        input->prev_time_in_quantum = cur_time;
    if (!outputs_[output].speculation_stack.empty()) {
        outputs_[output].prev_speculate_pc = outputs_[output].speculate_pc;
        error_string_ = outputs_[output].speculator.next_record(
            outputs_[output].speculate_pc, record);
        if (!error_string_.empty()) {
            VPRINT(this, 1, "next_record[%d]: speculation failed: %s\n", output,
                   error_string_.c_str());
            return sched_type_t::STATUS_INVALID;
        }
        // Leave the cur input where it is: the ordinals will remain unchanged.
        // Also avoid the context switch checks below as we cannot switch in the
        // middle of speculating (we also don't count speculated instructions toward
        // QUANTUM_INSTRUCTIONS).
        return sched_type_t::STATUS_OK;
    }
    while (true) {
        input->cur_from_queue = false;
        if (input->needs_init) {
            // We pay the cost of this conditional to support ipc_reader_t::init() which
            // blocks and must be called right before reading its first record.
            // The user can't call init() when it accesses the output streams because
            // it std::moved the reader to us; we can't call it between our own init()
            // and here as we have no control point in between, and our init() is too
            // early as the user may have other work after that.
            input->reader->init();
            input->needs_init = false;
        }
        if (!input->queue.empty()) {
            record = input->queue.front();
            input->queue.pop_front();
            input->cur_from_queue = true;
        } else {
            // We again have a flag check because reader_t::init() does an initial ++
            // and so we want to skip that on the first record but perform a ++ prior
            // to all subsequent records.  We do not want to ++ after reading as that
            // messes up memtrace_stream_t queries on ordinals while the user examines
            // the record.
            if (input->needs_advance && !input->at_eof) {
                ++(*input->reader);
            } else {
                input->needs_advance = true;
            }
            if (input->at_eof || *input->reader == *input->reader_end) {
                if (!input->at_eof)
                    mark_input_eof(*input);
                lock.unlock();
                VPRINT(this, 5, "next_record[%d]: need new input (cur=%d eof)\n", output,
                       input->index);
                sched_type_t::stream_status_t res = pick_next_input(output, 0);
                if (res != sched_type_t::STATUS_OK && res != sched_type_t::STATUS_SKIPPED)
                    return res;
                input = &inputs_[outputs_[output].cur_input];
                lock = std::unique_lock<mutex_dbg_owned>(*input->lock);
                if (res == sched_type_t::STATUS_SKIPPED) {
                    // Like for the ROI below, we need the queue or a de-ref.
                    input->needs_advance = false;
                }
                continue;
            } else {
                record = **input->reader;
            }
        }
        VPRINT(this, 5,
               "next_record[%d]: candidate record from %d (@%" PRId64 "): ", output,
               input->index, get_instr_ordinal(*input));
        if (input->instrs_pre_read > 0 && record_type_is_instr(record))
            --input->instrs_pre_read;
        VDO(this, 5, print_record(record););
        bool need_new_input = false;
        bool preempt = false;
        uint64_t blocked_time = 0;
        uint64_t prev_time_in_quantum = 0;
        // XXX i#6831: Refactor to use subclasses or templates to specialize
        // scheduler code based on mapping options, to avoid these top-level
        // conditionals in many functions?
        if (options_.mapping == MAP_AS_PREVIOUSLY) {
            assert(outputs_[output].record_index >= 0);
            if (outputs_[output].record_index >=
                static_cast<int>(outputs_[output].record.size())) {
                // We're on the last record.
                VPRINT(this, 4, "next_record[%d]: on last record\n", output);
            } else if (outputs_[output].record[outputs_[output].record_index].type ==
                       schedule_record_t::SKIP) {
                VPRINT(this, 5, "next_record[%d]: need new input after skip\n", output);
                need_new_input = true;
            } else if (outputs_[output].record[outputs_[output].record_index].type ==
                       schedule_record_t::SYNTHETIC_END) {
                VPRINT(this, 5, "next_record[%d]: at synthetic end\n", output);
            } else {
                const schedule_record_t &segment =
                    outputs_[output].record[outputs_[output].record_index];
                assert(segment.type == schedule_record_t::DEFAULT);
                uint64_t start = segment.value.start_instruction;
                uint64_t stop = segment.stop_instruction;
                // The stop is exclusive.  0 does mean to do nothing (easiest
                // to have an empty record to share the next-entry for a start skip
                // or other cases).
                // Only check for stop when we've exhausted the queue, or we have
                // a starter schedule with a 0,0 entry prior to a first skip entry
                // (as just mentioned, it is easier to have a seemingly-redundant entry
                // to get into the trace reading loop and then do something like a skip
                // from the start rather than adding logic into the setup code).
                if (get_instr_ordinal(*input) >= stop &&
                    (!input->cur_from_queue || (start == 0 && stop == 0))) {
                    VPRINT(this, 5,
                           "next_record[%d]: need new input: at end of segment in=%d "
                           "stop=%" PRId64 "\n",
                           output, input->index, stop);
                    need_new_input = true;
                }
            }
        } else if (options_.mapping == MAP_TO_ANY_OUTPUT) {
            trace_marker_type_t marker_type;
            uintptr_t marker_value;
            // While regular traces typically always have a syscall marker when
            // there's a maybe-blocking marker, some tests and synthetic traces have
            // just the maybe so we check both.
            if (input->processing_syscall || input->processing_maybe_blocking_syscall) {
                // Wait until we're past all the markers associated with the syscall.
                // XXX: We may prefer to stop before the return value marker for
                // futex, or a kernel xfer marker, but our recorded format is on instr
                // boundaries so we live with those being before the switch.
                // XXX: Once we insert kernel traces, we may have to try harder
                // to stop before the post-syscall records.
                if (record_type_is_instr_boundary(record, outputs_[output].last_record)) {
                    if (input->switch_to_input != INVALID_INPUT_ORDINAL) {
                        // The switch request overrides any latency threshold.
                        need_new_input = true;
                        VPRINT(this, 3,
                               "next_record[%d]: direct switch on low-latency "
                               "syscall in "
                               "input %d\n",
                               output, input->index);
                    } else if (input->blocked_time > 0) {
                        // If we've found out another way that this input should
                        // block, use that time and do a switch.
                        need_new_input = true;
                        blocked_time = input->blocked_time;
                        VPRINT(this, 3,
                               "next_record[%d]: blocked time set for input %d\n", output,
                               input->index);
                    } else if (input->unscheduled) {
                        need_new_input = true;
                        VPRINT(this, 3, "next_record[%d]: input %d going unscheduled\n",
                               output, input->index);
                    } else if (syscall_incurs_switch(input, blocked_time)) {
                        // Model as blocking and should switch to a different input.
                        need_new_input = true;
                        VPRINT(this, 3,
                               "next_record[%d]: hit blocking syscall in input %d\n",
                               output, input->index);
                    }
                    input->processing_syscall = false;
                    input->processing_maybe_blocking_syscall = false;
                    input->pre_syscall_timestamp = 0;
                    input->syscall_timeout_arg = 0;
                }
            }
            if (outputs_[output].hit_switch_code_end) {
                // We have to delay so the end marker is still in_context_switch_code.
                outputs_[output].in_context_switch_code = false;
                outputs_[output].hit_switch_code_end = false;
                // We're now back "on the clock".
                if (options_.quantum_unit == QUANTUM_TIME)
                    input->prev_time_in_quantum = cur_time;
                // XXX: If we add a skip feature triggered on the output stream,
                // we'll want to make sure skipping while in these switch and kernel
                // sequences is handled correctly.
            }
            if (record_type_is_marker(record, marker_type, marker_value)) {
                process_marker(*input, output, marker_type, marker_value);
            }
            if (options_.quantum_unit == QUANTUM_INSTRUCTIONS &&
                record_type_is_instr_boundary(record, outputs_[output].last_record) &&
                !outputs_[output].in_kernel_code) {
                ++input->instrs_in_quantum;
                if (input->instrs_in_quantum > options_.quantum_duration_instrs) {
                    // We again prefer to switch to another input even if the current
                    // input has the oldest timestamp, prioritizing context switches
                    // over timestamp ordering.
                    VPRINT(this, 4,
                           "next_record[%d]: input %d hit end of instr quantum\n", output,
                           input->index);
                    preempt = true;
                    need_new_input = true;
                    input->instrs_in_quantum = 0;
                    ++outputs_[output]
                          .stats[memtrace_stream_t::SCHED_STAT_QUANTUM_PREEMPTS];
                }
            } else if (options_.quantum_unit == QUANTUM_TIME) {
                if (cur_time == 0 || cur_time < input->prev_time_in_quantum) {
                    VPRINT(this, 1,
                           "next_record[%d]: invalid time %" PRIu64 " vs start %" PRIu64
                           "\n",
                           output, cur_time, input->prev_time_in_quantum);
                    return sched_type_t::STATUS_INVALID;
                }
                input->time_spent_in_quantum += cur_time - input->prev_time_in_quantum;
                prev_time_in_quantum = input->prev_time_in_quantum;
                input->prev_time_in_quantum = cur_time;
                double elapsed_micros =
                    static_cast<double>(input->time_spent_in_quantum) /
                    options_.time_units_per_us;
                if (elapsed_micros >= options_.quantum_duration_us &&
                    // We only switch on instruction boundaries.  We could possibly switch
                    // in between (e.g., scatter/gather long sequence of reads/writes) by
                    // setting input->switching_pre_instruction.
                    record_type_is_instr_boundary(record, outputs_[output].last_record)) {
                    VPRINT(
                        this, 4,
                        "next_record[%d]: input %d hit end of time quantum after %" PRIu64
                        "\n",
                        output, input->index, input->time_spent_in_quantum);
                    preempt = true;
                    need_new_input = true;
                    input->time_spent_in_quantum = 0;
                    ++outputs_[output]
                          .stats[memtrace_stream_t::SCHED_STAT_QUANTUM_PREEMPTS];
                }
            }
        }
        if (options_.deps == DEPENDENCY_TIMESTAMPS &&
            options_.mapping != MAP_AS_PREVIOUSLY &&
            // For MAP_TO_ANY_OUTPUT with timestamps: enforcing asked-for context switch
            // rates is more important that honoring precise trace-buffer-based
            // timestamp inter-input dependencies so we do not end a quantum early due
            // purely to timestamps.
            options_.mapping != MAP_TO_ANY_OUTPUT &&
            record_type_is_timestamp(record, input->next_timestamp))
            need_new_input = true;
        if (need_new_input) {
            int prev_input = outputs_[output].cur_input;
            VPRINT(this, 5, "next_record[%d]: need new input (cur=%d)\n", output,
                   prev_input);
            // We have to put the candidate record in the queue before we release
            // the lock since another output may grab this input.
            VPRINT(this, 5, "next_record[%d]: queuing candidate record\n", output);
            input->queue.push_back(record);
            lock.unlock();
            sched_type_t::stream_status_t res = pick_next_input(output, blocked_time);
            if (res != sched_type_t::STATUS_OK && res != sched_type_t::STATUS_WAIT &&
                res != sched_type_t::STATUS_SKIPPED)
                return res;
            if (outputs_[output].cur_input != prev_input) {
                // TODO i#5843: Queueing here and in a few other places gets the stream
                // record and instruction ordinals off: we need to undo the ordinal
                // increases to avoid over-counting while queued and double-counting
                // when we resume.
                // In some cases we need to undo this on the output stream too.
                // So we should set suppress_ref_count_ in the input to get
                // is_record_synthetic() (and have our stream class check that
                // for instr count too) -- but what about output during speculation?
                // Decrement counts instead to undo?
                lock.lock();
                VPRINT(this, 5, "next_record_mid[%d]: switching from %d to %d\n", output,
                       prev_input, outputs_[output].cur_input);
                // We need to offset the {instrs,time_spent}_in_quantum values from
                // overshooting during dynamic scheduling, unless this is a preempt when
                // we've already reset to 0.
                if (!preempt && options_.mapping == MAP_TO_ANY_OUTPUT) {
                    if (options_.quantum_unit == QUANTUM_INSTRUCTIONS &&
                        record_type_is_instr_boundary(record,
                                                      outputs_[output].last_record)) {
                        assert(inputs_[prev_input].instrs_in_quantum > 0);
                        --inputs_[prev_input].instrs_in_quantum;
                    } else if (options_.quantum_unit == QUANTUM_TIME) {
                        assert(inputs_[prev_input].time_spent_in_quantum >=
                               cur_time - prev_time_in_quantum);
                        inputs_[prev_input].time_spent_in_quantum -=
                            (cur_time - prev_time_in_quantum);
                    }
                }
                if (res == sched_type_t::STATUS_WAIT)
                    return res;
                input = &inputs_[outputs_[output].cur_input];
                lock = std::unique_lock<mutex_dbg_owned>(*input->lock);
                continue;
            } else {
                lock.lock();
                if (res != sched_type_t::STATUS_SKIPPED) {
                    // Get our candidate record back.
                    record = input->queue.back();
                    input->queue.pop_back();
                }
            }
            if (res == sched_type_t::STATUS_SKIPPED) {
                // Like for the ROI below, we need the queue or a de-ref.
                input->needs_advance = false;
                continue;
            }
        }
        if (input->needs_roi && options_.mapping != MAP_AS_PREVIOUSLY &&
            !input->regions_of_interest.empty()) {
            sched_type_t::stream_status_t res =
                advance_region_of_interest(output, record, *input);
            if (res == sched_type_t::STATUS_SKIPPED) {
                // We need either the queue or to re-de-ref the reader so we loop,
                // but we do not want to come back here.
                input->needs_roi = false;
                input->needs_advance = false;
                continue;
            } else if (res != sched_type_t::STATUS_OK)
                return res;
        } else {
            input->needs_roi = true;
        }
        break;
    }
    VPRINT(this, 4, "next_record[%d]: from %d @%" PRId64 ": ", output, input->index,
           cur_time);
    VDO(this, 4, print_record(record););

    outputs_[output].last_record = record;
    record_type_has_tid(record, input->last_record_tid);
    record_type_has_pid(record, input->pid);
    return sched_type_t::STATUS_OK;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::unread_last_record(output_ordinal_t output,
                                                             RecordType &record,
                                                             input_info_t *&input)
{
    auto &outinfo = outputs_[output];
    if (record_type_is_invalid(outinfo.last_record))
        return sched_type_t::STATUS_INVALID;
    if (!outinfo.speculation_stack.empty())
        return sched_type_t::STATUS_INVALID;
    record = outinfo.last_record;
    input = &inputs_[outinfo.cur_input];
    std::lock_guard<mutex_dbg_owned> lock(*input->lock);
    VPRINT(this, 4, "next_record[%d]: unreading last record, from %d\n", output,
           input->index);
    input->queue.push_back(outinfo.last_record);
    // XXX: This should be record_type_is_instr_boundary() but we don't have the pre-prev
    // record.  For now we don't support unread_last_record() for record_reader_t,
    // enforced in a specialization of unread_last_record().
    if (options_.quantum_unit == QUANTUM_INSTRUCTIONS && record_type_is_instr(record))
        --input->instrs_in_quantum;
    outinfo.last_record = create_invalid_record();
    return sched_type_t::STATUS_OK;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::start_speculation(output_ordinal_t output,
                                                            addr_t start_address,
                                                            bool queue_current_record)
{
    auto &outinfo = outputs_[output];
    if (outinfo.speculation_stack.empty()) {
        if (queue_current_record) {
            if (record_type_is_invalid(outinfo.last_record))
                return sched_type_t::STATUS_INVALID;
            inputs_[outinfo.cur_input].queue.push_back(outinfo.last_record);
        }
        // The store address for the outer layer is not used since we have the
        // actual trace storing our resumption context, so we store a sentinel.
        static constexpr addr_t SPECULATION_OUTER_ADDRESS = 0;
        outinfo.speculation_stack.push(SPECULATION_OUTER_ADDRESS);
    } else {
        if (queue_current_record) {
            // XXX i#5843: We'll re-call the speculator so we're assuming a repeatable
            // response with the same instruction returned.  We should probably save the
            // precise record either here or in the speculator.
            outinfo.speculation_stack.push(outinfo.prev_speculate_pc);
        } else
            outinfo.speculation_stack.push(outinfo.speculate_pc);
    }
    // Set the prev in case another start is called before reading a record.
    outinfo.prev_speculate_pc = outinfo.speculate_pc;
    outinfo.speculate_pc = start_address;
    VPRINT(this, 2, "start_speculation layer=%zu pc=0x%zx\n",
           outinfo.speculation_stack.size(), start_address);
    return sched_type_t::STATUS_OK;
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::stop_speculation(output_ordinal_t output)
{
    auto &outinfo = outputs_[output];
    if (outinfo.speculation_stack.empty())
        return sched_type_t::STATUS_INVALID;
    if (outinfo.speculation_stack.size() > 1) {
        // speculate_pc is only used when exiting inner layers.
        outinfo.speculate_pc = outinfo.speculation_stack.top();
    }
    VPRINT(this, 2, "stop_speculation layer=%zu (resume=0x%zx)\n",
           outinfo.speculation_stack.size(), outinfo.speculate_pc);
    outinfo.speculation_stack.pop();
    return sched_type_t::STATUS_OK;
}

template <typename RecordType, typename ReaderType>
void
scheduler_tmpl_t<RecordType, ReaderType>::mark_input_eof(input_info_t &input)
{
    assert(input.lock->owned_by_cur_thread());
    if (input.at_eof)
        return;
    input.at_eof = true;
    assert(live_input_count_.load(std::memory_order_acquire) > 0);
    live_input_count_.fetch_add(-1, std::memory_order_release);
    VPRINT(this, 2, "input %d at eof; %d live inputs left\n", input.index,
           live_input_count_.load(std::memory_order_acquire));
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::eof_or_idle(output_ordinal_t output,
                                                      bool hold_sched_lock,
                                                      input_ordinal_t prev_input)
{
    // XXX i#6831: Refactor to use subclasses or templates to specialize
    // scheduler code based on mapping options, to avoid these top-level
    // conditionals in many functions?
    if (options_.mapping == MAP_TO_CONSISTENT_OUTPUT ||
        live_input_count_.load(std::memory_order_acquire) == 0 ||
        // While a full schedule recorded should have each input hit either its
        // EOF or ROI end, we have a fallback to avoid hangs for possible recorded
        // schedules that end an input early deliberately without an ROI.
        (options_.mapping == MAP_AS_PREVIOUSLY &&
         live_replay_output_count_.load(std::memory_order_acquire) == 0)) {
        assert(options_.mapping != MAP_AS_PREVIOUSLY || outputs_[output].at_eof);
        return sched_type_t::STATUS_EOF;
    } else {
        bool need_lock;
        auto scoped_lock = hold_sched_lock
            ? std::unique_lock<mutex_dbg_owned>()
            : acquire_scoped_sched_lock_if_necessary(need_lock);
        if (options_.mapping == MAP_TO_ANY_OUTPUT) {
            // Workaround to avoid hangs when _SCHEDULE and/or _DIRECT_THREAD_SWITCH
            // directives miss their targets (due to running with a subset of the
            // original threads, or other scenarios) and we end up with no scheduled
            // inputs but a set of unscheduled inputs who will never be scheduled.
            VPRINT(this, 4,
                   "eof_or_idle output=%d live=%d unsched=%zu runq=%zu blocked=%d\n",
                   output, live_input_count_.load(std::memory_order_acquire),
                   unscheduled_priority_.size(), ready_priority_.size(), num_blocked_);
            if (ready_priority_.empty() && !unscheduled_priority_.empty()) {
                if (outputs_[output].wait_start_time == 0) {
                    outputs_[output].wait_start_time = get_output_time(output);
                } else {
                    uint64_t now = get_output_time(output);
                    double elapsed_micros = (now - outputs_[output].wait_start_time) *
                        options_.time_units_per_us;
                    if (elapsed_micros > options_.block_time_max_us) {
                        // XXX i#6822: We may want some other options here for what to
                        // do.  We could release just one input at a time, which would be
                        // the same scheduling order (as we have FIFO in
                        // unscheduled_priority_) but may take a long time at
                        // block_time_max_us each; we could declare we're done and just
                        // exit, maybe under a flag or if we could see what % of total
                        // records we've processed.
                        VPRINT(this, 1,
                               "eof_or_idle moving entire unscheduled queue to ready "
                               "queue\n");
                        while (!unscheduled_priority_.empty()) {
                            input_info_t *tomove = unscheduled_priority_.top();
                            std::lock_guard<mutex_dbg_owned> lock(*tomove->lock);
                            tomove->unscheduled = false;
                            ready_priority_.push(tomove);
                            unscheduled_priority_.pop();
                        }
                        outputs_[output].wait_start_time = 0;
                    }
                }
            } else {
                outputs_[output].wait_start_time = 0;
            }
        }
        outputs_[output].waiting = true;
        if (prev_input != INVALID_INPUT_ORDINAL)
            ++outputs_[output].stats[memtrace_stream_t::SCHED_STAT_SWITCH_INPUT_TO_IDLE];
        set_cur_input(output, INVALID_INPUT_ORDINAL);
        return sched_type_t::STATUS_IDLE;
    }
}

template <typename RecordType, typename ReaderType>
bool
scheduler_tmpl_t<RecordType, ReaderType>::is_record_kernel(output_ordinal_t output)
{
    int index = outputs_[output].cur_input;
    if (index < 0)
        return false;
    return inputs_[index].reader->is_record_kernel();
}

template <typename RecordType, typename ReaderType>
double
scheduler_tmpl_t<RecordType, ReaderType>::get_statistic(
    output_ordinal_t output, memtrace_stream_t::schedule_statistic_t stat) const
{
    if (stat >= memtrace_stream_t::SCHED_STAT_TYPE_COUNT)
        return -1;
    return static_cast<double>(outputs_[output].stats[stat]);
}

template <typename RecordType, typename ReaderType>
typename scheduler_tmpl_t<RecordType, ReaderType>::stream_status_t
scheduler_tmpl_t<RecordType, ReaderType>::set_output_active(output_ordinal_t output,
                                                            bool active)
{
    if (options_.mapping != MAP_TO_ANY_OUTPUT)
        return sched_type_t::STATUS_INVALID;
    if (outputs_[output].active == active)
        return sched_type_t::STATUS_OK;
    outputs_[output].active = active;
    VPRINT(this, 2, "Output stream %d is now %s\n", output,
           active ? "active" : "inactive");
    std::lock_guard<mutex_dbg_owned> guard(sched_lock_);
    if (!active) {
        // Make the now-inactive output's input available for other cores.
        // This will reset its quantum too.
        // We aren't switching on a just-read instruction not passed to the consumer,
        // if the queue is empty.
        if (inputs_[outputs_[output].cur_input].queue.empty())
            inputs_[outputs_[output].cur_input].switching_pre_instruction = true;
        set_cur_input(output, INVALID_INPUT_ORDINAL);
    } else {
        outputs_[output].waiting = true;
    }
    return sched_type_t::STATUS_OK;
}

template class scheduler_tmpl_t<memref_t, reader_t>;
template class scheduler_tmpl_t<trace_entry_t, dynamorio::drmemtrace::record_reader_t>;

} // namespace drmemtrace
} // namespace dynamorio
