/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <stdio.h>

namespace gusli {
/************************* Generic datapath logging **************************/
// IO logging
#define PRINT_IO_BUF_FMT   "BUF{0x%lx[b]=%p}"
#define PRINT_IO_BUF_ARGS(c) (c).byte_len, (c).ptr
#define PRINT_IO_REQ_FMT   "IO{%c:ofs=0x%lx, " PRINT_IO_BUF_FMT ", #rng=%d}"
#define PRINT_IO_REQ_ARGS(c) (c).op(), (c).map().offset_lba_bytes, PRINT_IO_BUF_ARGS(c.map().data), (c).num_ranges()
#define PRINT_IO_SQE_ELEM_FMT   "sqe[%03u]"
#define PRINT_IO_CQE_ELEM_FMT   "cqe[%03u]"
#define PRINT_CLNT_IO_PTR_FMT   ".clnt_io_ptr[%p]"

// Memory registration logging
#define PRINT_MMAP_PREFIX     "MMAP: "
#define PRINT_REG_BUF_FMT    PRINT_MMAP_PREFIX "Register[%d%c] " PRINT_IO_BUF_FMT ", n_blocks=0x%x"
#define PRINT_UNR_BUF_FMT    PRINT_MMAP_PREFIX "UnRegist[%d%c] " PRINT_IO_BUF_FMT ", n_blocks=0x%x"
#define PRINT_REG_IDX_FMT    "[%di]"
#define PRINT_REG_BUF_ARGS(pr, buf) pr->get_buf_idx(), pr->get_buf_type(), PRINT_IO_BUF_ARGS(buf), pr->num_blocks
#define PRINT_BDEV_UUID_FMT  "bdev{uuid=%.16s}"
#define PRINT_BDEV_ID_FMT    "bdev{uuid=%.16s, %s[%c].addr[%s]}"
#define PRINT_BDEV_ID_ARGS(bdev) (bdev).conf.id.uuid, (bdev).b.info.name, (bdev).conf.type, (bdev).conf.conn.any
#define PRINT_EXECUTOR      "exec[%p].o[%p]."

#if !defined(LIB_NAME) || !defined(LIB_COLOR)
	#error: Including file must define the above to use generic client/server logging system
#endif
#define pr_info1(fmt, ...) ({ pr_info( LIB_COLOR LIB_NAME ": " fmt NV_COL_R,    ##__VA_ARGS__); pr_flush(); })
#define pr_err1( fmt, ...) ({ pr_err(            LIB_NAME ": " fmt         ,    ##__VA_ARGS__); })
#define pr_note1(fmt, ...) ({ pr_note(           LIB_NAME ": " fmt         ,    ##__VA_ARGS__); })
#define pr_verb1(fmt, ...) ({ pr_verbs(LIB_COLOR LIB_NAME ": " fmt NV_COL_R,    ##__VA_ARGS__); pr_flush(); })

} // namespace gusli
