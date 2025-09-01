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

#include "../07examples/server/spdk_app_1server.hpp"
int main(int argc, const char **argv) {
	log_line("!!! Important !!!");
	log_unitest("Launching spdk server, waiting for client to connect, press Ctrl+C to quit\n");
	log_line("!!! Important !!!");
	server_spdk_app_1_gusli_server app(argc, argv);
	return app.get_rv();
}
