#!/bin/bash
function echo_green() {  echo -e "\e[0;32m$*\e[0m"; }
function GUSLI() {
	if [ $# -eq 0 ]; then
		echo "params: show / clean / name / "
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
