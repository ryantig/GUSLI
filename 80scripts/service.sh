#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) <year> NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

function echo_green() {  echo -e "\e[0;32m$*\e[0m"; }
function GUSLI() {
	if [ $# -eq 0 ]; then
		echo "params: show / clean "
	elif [[ $1 == sh* ]]; then
		cmd="ps -eLo pid,ppid,comm,user | grep gusli"; echo_green $cmd; eval $cmd;
		cmd="ps -ef | grep gusli"; echo_green $cmd; eval $cmd;
		cmd='ll /dev/shm/gs*'; echo_green $cmd; eval $cmd;
	elif [[ $1 == cl* ]]; then
		ps_prg=`pgrep -f gusli`; [ ! -z "$ps_prg" ] && sudo kill -9 ${ps_prg};
		sudo rm /dev/shm/gs*;
		GUSLI show;
	fi
}
