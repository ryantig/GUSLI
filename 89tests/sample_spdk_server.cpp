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
#ifdef SUPPORT_SPDK
#include "../07examples/server/spdk_app_1server.hpp"
#else
	#include "07examples/client/io_submittion_example.hpp"
	class empty_app {
	 public:
		empty_app(const int argc, const char *argv[]) { (void)argc; (void)argv; }
		int get_rv(void) const {
			log("\x1b[1;31mCould not find spdk framework, spdk server unitest abort, rv=%d\x1b[0;0m\n", -ENOEXEC);
			return -ENOEXEC;
		}
	};
	typedef class empty_app server_spdk_app_1_gusli_server;
#endif

int main(int argc, const char **argv) {
	server_spdk_app_1_gusli_server app(argc, argv);
	return app.get_rv();
}
