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

function echo_red() {    echo -e "\e[0;31m$*\e[0m"; }
function echo_green() {  echo -e "\e[0;32m$*\e[0m"; }
function echo_yellow() { echo -e "\e[0;33m$*\e[0m"; }
function echo_title() {  echo -e "~~~~~~~~~~~~~ \e[16;34m$*\e[0;39m:"; }

function GUSLI() {
	if [ $# -eq 0 ]; then
		echo "params: show / clean / huge_pages_setup";
	elif [[ $1 == sh* ]]; then
		cmd="ps -eLo pid,ppid,comm,user | grep gusli"; echo_title "Processes"; eval $cmd;
		#cmd="ps -ef | grep gusli"; echo_green $cmd; eval $cmd;
		cmd="pgrep -fl gusli"; echo_green $cmd; eval $cmd;
		echo_title "SHM";
		df -h /dev/shm;
		cmd='ll /dev/shm/gs* 2>/dev/null'; #echo_green $cmd;
		eval $cmd;
		ps_prg=$(pgrep -f gusli);
		echo_title "Shared mem bufs";
		for process in $ps_prg; do
			echo_green "* Process $(cat /proc/$process/comm)[ $process ]:";
			sudo grep -e /dev/shm/gs -e heap -e 'rw-p 00000000 00:00' /proc/$process/maps;
		done
		for process in $ps_prg; do
			echo_title "Process $(cat /proc/$process/comm)[ $process ]: stack";
			sudo cat /proc/$process/stack;
		done
	elif [[ $1 == cl* ]]; then
		ps_prg=`pgrep -f gusli`; [ ! -z "$ps_prg" ] && sudo kill -9 ${ps_prg};
		sudo rm /dev/shm/gs*;
		GUSLI show;
	elif [[ $1 == huge_pages* ]]; then
		# Set Up Huge pages (required for DPDK):
		if [ "`cat /proc/sys/vm/nr_hugepages`" == "0" ]; then
			echo_green "Configuring huge pages";
			sudo mkdir -p /mnt/huge;
			sudo mount -t hugetlbfs nodev /mnt/huge;
			sudo bash -c 'echo 4096 > /proc/sys/vm/nr_hugepages';
		else
			echo_green "huge pages already configured";
		fi
	fi
}

echo_green "Thank you. Type GUSLI to see service options";